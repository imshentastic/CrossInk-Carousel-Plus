#include "Section.h"

#include <Arduino.h>
#include <FontCacheManager.h>  // v18.9.9.13: prewarm scope for streamed render
#include <GfxRenderer.h>       // v18.9.9.13: full type for prewarm-scope integration
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include <algorithm>  // std::lower_bound for atlas lookup
#include <cstring>

#include "EmbeddedGlyphSubset.h"
#include "Epub/css/CssParser.h"
#include "Epub/parsers/ChapterHtmlSlimParserGuards.h"  // v18.9.6: force-simple table guard
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v38: added imagesSuppressed + buildMaxAlloc to the header so a chapter cached
// with images dropped under low heap can be rebuilt with images once memory
// recovers. The bump also invalidates any v37 cache that was silently cached
// imageless (it will rebuild fresh on next open).
//
// v39 (CrumBLE 4.3): added three trailer offsets at the end of the header
// for an OPTIONAL embedded glyph subset block:
//   embeddedGlyphSubsetOffset      uint32_t  file offset of the block, 0 = no block
//   embeddedGlyphSubsetSize        uint32_t  byte size of the block, 0 = no block
//   embeddedGlyphSubsetCpfontHash  uint32_t  matches SdCardFont::contentHash() for
//                                            the .cpfont this section was baked
//                                            against; 0 = no block. Used at load
//                                            time to validate that the user
//                                            didn't swap fonts since the bake.
// The block itself sits between the trailing LUTs and EOF (start offset is
// recorded in the header so the loader can seek directly to it). Sections
// without an embedded subset (device-built sections, or prebakes for built-in
// fonts where embedding adds no value) leave all three fields at 0 and the
// existing SD-font lazy-miss-handler path takes over for SD-font books, same
// as v4.2.x. The motivation for this format: NimBLE init shatters contiguous
// heap to ~13 KB after BT enable, below the ~14 KB miniData footprint of an
// SD font; embedding the section's working glyph set into the section file
// lets the runtime skip the miniData allocation entirely for that section.
// CrumBLE 4.4: v40 adds a 3-uint32 glyph atlas trailer (offset/size/hash)
// right after the v39 embedded-glyph-subset trailer. Sections with no atlas
// emit zeros for all three fields; the loader treats that as "no atlas
// available, fall back to the v39 subset / SD-font miss handler path".
// See lib/Epub/Epub/GlyphAtlas.h for the on-disk block format.
// v18.9.9.19: bump to 42. New emission adds a uint32_t payloadSize prefix
// between the tag byte and every TAG_PageTableFragment payload so the streamed
// reader (v18.9.9.20) can cheap-skip the whole fragment without deserialising
// its cell TextBlocks. v41 and earlier files stay readable via the file-
// version gate in Page::deserialize; those older files just don't get the
// skip benefit under compat mode.
// v18.9.9.24: bump to 43. Adds a uint8_t tableRendering field after the v41
// alt-atlas trailer so the fingerprint check can invalidate cached sections
// when the user toggles the Tables setting. v42 and earlier files default
// to TABLES_DISPLAY on load (that's how they were built, so it round-trips).
constexpr uint8_t SECTION_FILE_VERSION = 43;
// Oldest section file version this firmware can still read (forward-compat
// window). v38 sections produced by v4.2.x prebakes / live caches still load
// cleanly; their embedded glyph subset offsets default to 0 (no subset).
// v39 sections similarly default the v40 atlas trailer to 0/0/0. Older
// versions trigger a rebuild as before.
constexpr uint8_t MIN_READABLE_SECTION_FILE_VERSION = 38;
// How much the largest free block must have grown since a degraded build before
// we bother rebuilding it for images (avoids rebuild churn on tiny variations).
constexpr uint32_t SECTION_DEGRADED_REBUILD_MARGIN = 12 * 1024;

// CrumBLE 4.3 option 3: pre-allocated heap reserve for page-DOM deserialize.
//
// Holds ~12 KB of contiguous heap from boot. When loadPageFromSectionFile()
// detects heap pressure (typical: just after BT enable + connect on an
// SD-font book) it releases the reserve, which goes back to the heap as a
// fresh large free block. TextBlock::deserialize's vector<string>::resize
// and friends then find a contiguous slot to land in and don't bad_alloc.
// After Page::deserialize returns, we try to re-acquire the reserve
// (best-effort -- if the page DOM ate the freed region, the reserve stays
// nullptr until heap recovers and the next render's re-acquire succeeds).
//
// Why 12 KB: empirical peak vector::resize bytecount across a sweep of
// chapters in test books. Leave 1 KB margin above observed peak so a
// freshly-released reserve can absorb a worst-case page's allocations
// without re-fragmenting.
//
// MALLOC_CAP_8BIT puts the reserve in the standard internal SRAM heap so
// it competes for the same blocks the deserialize allocator pulls from.
// CrumBLE 4.3 fifth tuning: back to 18 KB but the BT-enable path
// releases it explicitly. The hold-through-BT approaches (14 KB, 10 KB)
// either starved NimBLE or left too little post-release for the page
// DOM deserialize. Releasing for BT gives NimBLE the full 85 KB +
// reserve, NimBLE allocates ~70 KB, leaving ~15 KB free + a fresh
// contiguous slot that the released reserve carved out -- which gives
// post-NimBLE MaxAlloc the ~13 KB that successfully landed the
// TextBlock deserialize allocations in the user-confirmed "page turns
// worked" test.
//
// CrumBLE 4.5.7 sixth tuning: tried 28 KB, reverted to 18 KB.
// Field log after the bump: reserve released 29 KB into NimBLE's clean
// slot -> NimBLE consumed ALL 75 KB anyway (not the ~60 KB it took with
// 18 KB reserve) -> post-BT free = 72 BYTES. NimBLE's dynamic
// allocations grow to fill the available heap; giving it more clean
// slot just lets it allocate more. Net gain for the reader: zero.
// Stays at 18 KB, matching the historical calibration.
constexpr size_t PAGE_HEAP_RESERVE_BYTES = 18 * 1024;
constexpr uint32_t PAGE_HEAP_RESERVE_TRIGGER_MAX_ALLOC = 16 * 1024;
void* pageHeapReserve_ = nullptr;

void* tryAcquirePageHeapReserve() {
  if (pageHeapReserve_) return pageHeapReserve_;
  pageHeapReserve_ = heap_caps_malloc(PAGE_HEAP_RESERVE_BYTES, MALLOC_CAP_8BIT);
  return pageHeapReserve_;
}

void releasePageHeapReserve() {
  if (pageHeapReserve_) {
    heap_caps_free(pageHeapReserve_);
    pageHeapReserve_ = nullptr;
  }
}
// v38 header byte count (all fields up to and including the liLut trailer
// offset). Kept as a named constant because v38 sections are still readable
// after the v39 bump; the loader uses this to know when to stop reading
// trailer offsets for legacy files.
constexpr uint32_t HEADER_SIZE_V38 = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                     sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) +
                                     sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                     sizeof(uint8_t) + sizeof(bool) + sizeof(bool) +
                                     sizeof(bool) /*imagesSuppressed*/ + sizeof(uint32_t) /*buildMaxAlloc*/ +
                                     sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
// v39 adds 3 uint32_t trailer fields after the liLutOffset (embedded glyph
// subset offset/size/hash). v40 adds 3 more (glyph atlas offset/size/hash).
constexpr uint32_t HEADER_SIZE_V39 = HEADER_SIZE_V38 + 3 * sizeof(uint32_t);
constexpr uint32_t HEADER_SIZE_V40 = HEADER_SIZE_V39 + 3 * sizeof(uint32_t);
// v42 didn't change the header layout (page-element format only).
constexpr uint32_t HEADER_SIZE_V42 = HEADER_SIZE_V40 + 3 * sizeof(uint32_t);  // +3 for v41 alt-atlas trailer
// v18.9.9.24: v43 adds one uint8_t (tableRendering) for fingerprint gating
// of the new Tables setting. Position: right after the v41 alt-atlas trailer.
constexpr uint32_t HEADER_SIZE = HEADER_SIZE_V42 + sizeof(uint8_t);

// The embedded glyph subset block format constants live in
// EmbeddedGlyphSubset.h (namespace embeddedGlyphSubset) so the on-device
// loader, the prebake CLI emitter, and any future tooling share a single
// source of truth. Section.cpp itself doesn't reference the magic / version
// directly -- the install path reads them through the same header it'll
// use to drive deserialisation.

}  // namespace

// v18.9.9.29 (v20 Phase C1): out-of-line ctor + dtor so
// unique_ptr<BuildContext> can be constructed/destroyed here where the
// forward-declared ChapterHtmlSlimParser is a complete type.
Section::Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin"),
      prebakeFilePath(epub->getCachePath() + "/sections-prebake/" + std::to_string(spineIndex) + ".bin"),
      activeFilePath(filePath) {}

Section::~Section() { abandonBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  // v18.9.9.19: emit at SECTION_FILE_VERSION (currently 42) so on-device
  // rebuilt sections carry the payloadSize prefix on table fragments.
  if (!page->serialize(file, SECTION_FILE_VERSION)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed (pos=%lu, free=%u, maxAlloc=%u)", pageCount, static_cast<unsigned long>(position),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  pageCount++;
  return position;
}

bool Section::writeSectionFileHeader(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                                     const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const bool embeddedStyle,
                                     const uint8_t imageRendering, const bool bionicReadingEnabled,
                                     const bool guideReadingEnabled, const uint8_t tableRendering) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(fontId) +
                                   sizeof(lineCompression) + sizeof(extraParagraphSpacing) +
                                   sizeof(forceParagraphIndents) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(bionicReadingEnabled) +
                                   sizeof(guideReadingEnabled) + sizeof(bool) /*imagesSuppressed*/ +
                                   sizeof(uint32_t) /*buildMaxAlloc*/ + sizeof(uint32_t) /*lutOffset*/ +
                                   sizeof(uint32_t) /*anchorMapOffset*/ + sizeof(uint32_t) /*paragraphLutOffset*/ +
                                   sizeof(uint32_t) /*liLutOffset*/ +
                                   sizeof(uint32_t) /*embeddedGlyphSubsetOffset (v39)*/ +
                                   sizeof(uint32_t) /*embeddedGlyphSubsetSize (v39)*/ +
                                   sizeof(uint32_t) /*embeddedGlyphSubsetCpfontHash (v39)*/ +
                                   sizeof(uint32_t) /*glyphAtlasOffset (v40)*/ +
                                   sizeof(uint32_t) /*glyphAtlasSize (v40)*/ +
                                   sizeof(uint32_t) /*glyphAtlasCpfontHash (v40)*/ +
                                   sizeof(uint32_t) /*glyphAtlasAltOffset (v41)*/ +
                                   sizeof(uint32_t) /*glyphAtlasAltSize (v41)*/ +
                                   sizeof(uint32_t) /*glyphAtlasAltCpfontHash (v41)*/ +
                                   sizeof(uint8_t) /*tableRendering (v43)*/,
                "Header size mismatch");
  return serialization::tryWritePod(file, SECTION_CACHE_MAGIC) &&
         serialization::tryWritePod(file, SECTION_FILE_VERSION) && serialization::tryWritePod(file, fontId) &&
         serialization::tryWritePod(file, lineCompression) && serialization::tryWritePod(file, extraParagraphSpacing) &&
         serialization::tryWritePod(file, forceParagraphIndents) &&
         serialization::tryWritePod(file, paragraphAlignment) && serialization::tryWritePod(file, viewportWidth) &&
         serialization::tryWritePod(file, viewportHeight) && serialization::tryWritePod(file, hyphenationEnabled) &&
         serialization::tryWritePod(file, embeddedStyle) && serialization::tryWritePod(file, imageRendering) &&
         serialization::tryWritePod(file, bionicReadingEnabled) &&
         serialization::tryWritePod(file, guideReadingEnabled) &&
         serialization::tryWritePod(file, static_cast<bool>(false)) &&     // imagesSuppressed (patched in finalize)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&     // buildMaxAlloc (patched in finalize)
         serialization::tryWritePod(file,
                                    pageCount) &&  // Placeholder for page count (will be initially 0, patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for LUT offset (patched later)
         serialization::tryWritePod(file,
                                    static_cast<uint32_t>(0)) &&  // Placeholder for anchor map offset (patched later)
         serialization::tryWritePod(
             file,
             static_cast<uint32_t>(0)) &&  // Placeholder for paragraph LUT offset (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for li LUT offset (patched later)
         // v39 trailer: three uint32_t fields for the optional embedded glyph
         // subset block. All zero on device-built sections (no embedding) and
         // patched by the prebake CLI when it emits a subset block at finalize.
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // embeddedGlyphSubsetOffset
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // embeddedGlyphSubsetSize
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // embeddedGlyphSubsetCpfontHash
         // v40 trailer: three uint32_t fields for the optional pre-rendered
         // glyph atlas block. All zero on device-built sections and on prebake
         // CLI runs that didn't emit an atlas (e.g. --emit-section-glyph-atlas
         // not passed). Patched by the prebake CLI when it emits an atlas
         // block in the buildGlyphAtlasBlock path.
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // glyphAtlasOffset
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // glyphAtlasSize
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // glyphAtlasCpfontHash
         // v41 trailer: three uint32_t fields for the alternate atlas slot
         // (the OTHER bit-depth of the same glyph set). All zero on bakes
         // that only produced one bit-depth. Patched by the prebake CLI's
         // atlas-emit path when --emit-section-glyph-subsets is set.
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // glyphAtlasAltOffset
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // glyphAtlasAltSize
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // glyphAtlasAltCpfontHash
         // v43 trailer: one uint8_t for the Tables setting the section was
         // built with. Fingerprint-compared on load so a toggle from Display
         // to Paragraphs (or back) invalidates the cached section.
         serialization::tryWritePod(file, tableRendering);
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                              const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                              const bool bionicReadingEnabled, const bool guideReadingEnabled,
                              const uint8_t tableRendering,
                              const bool prebakeFallbackEnabled, const bool forceSimpleRendering) {
  // Try the live cache (sections/) first. This is the path the device
  // writes to when it rebuilds a chapter under current settings, so it's
  // always going to fingerprint-match if the cache is still fresh.
  // v18.9.9.3a: pass forceSimpleRendering so a section written by an
  // earlier force-simple rebuild (embeddedStyle/imageRendering/bionic/guide
  // stored as override values) matches even when current SETTINGS still say
  // the unoverridden values. Without this, sidecar-seeded opens keep hitting
  // fingerprint mismatch and falling back to the prebake (full render).
  if (tryLoadFromPath(filePath, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
                      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle,
                      imageRendering, bionicReadingEnabled, guideReadingEnabled, tableRendering, forceSimpleRendering)) {
    return true;
  }
  // Fall through to the prebake fallback (sections-prebake/) when the user
  // has the "Optimize Chapter Indexing" toggle on. If the user just
  // reverted their reader settings back to the prebake'd layout via the
  // switch-back prompt, the live cache was deleted by the previous
  // mismatch's clearCache call BUT the prebake artifact is still on SD.
  // Fingerprint check inside tryLoadFromPath enforces that this only
  // succeeds when current settings actually match the prebake.
  //
  // When the toggle is off, we never touch sections-prebake/ -- the device
  // behaves exactly like stock 3.7.3 even if prebake files happen to be on
  // SD (e.g. left over from a previous opt-in session).
  if (prebakeFallbackEnabled &&
      tryLoadFromPath(prebakeFilePath, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
                      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle,
                      imageRendering, bionicReadingEnabled, guideReadingEnabled, tableRendering)) {
    LOG_INF("SCT", "Loaded section %d from prebake fallback (%s)", spineIndex, prebakeFilePath.c_str());
    return true;
  }
  // Both failed. Clear the live cache so createSectionFile writes fresh
  // bytes at filePath. NEVER clear the prebake artifact -- it has to
  // outlive any number of live-cache rebuilds so the user can always
  // switch back.
  if (Storage.exists(filePath.c_str())) {
    clearCache();
  }
  return false;
}

bool Section::tryLoadFromPath(const std::string& path, const int fontId, const float lineCompression,
                              const uint8_t extraParagraphSpacing, const bool forceParagraphIndents,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool bionicReadingEnabled,
                              const bool guideReadingEnabled, const uint8_t tableRendering,
                              const bool forceSimpleRendering) {
  if (!Storage.openFileForRead("SCT", path, file)) {
    return false;
  }

  // Match parameters
  {
    uint32_t magic;
    if (!serialization::tryReadPod(file, magic)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read cache magic (%s)", path.c_str());
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch (%s)", path.c_str());
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version (%s)", path.c_str());
      return false;
    }
    // CrumBLE 4.3: accept any version within the forward-compat window so v38
    // sections produced by older firmware / prebakes keep loading after the
    // v39 bump. Older versions still trigger a rebuild as before. fileVersion_
    // is cached so the trailer reader below knows whether to expect the v39
    // embedded-glyph-subset offsets.
    if (version < MIN_READABLE_SECTION_FILE_VERSION || version > SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u (%s)", version, path.c_str());
      return false;
    }
    fileVersion_ = version;

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    uint8_t fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileBionicReadingEnabled;
    bool fileGuideReadingEnabled;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileBionicReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header (%s)", path.c_str());
      return false;
    }

    // CrumBLE 4.5.7: allow small viewport drift across firmware updates.
    // freeink-sdk migration shifted computed viewportHeight by ~5px (theme
    // metric change). Exact match would invalidate every prebake made on
    // the prior firmware. A few pixels of extra whitespace at page bottom
    // is imperceptible; page break positions and word wrapping are
    // determined by width (unchanged) and stay identical.
    constexpr uint16_t kViewportTolerancePx = 8;
    const uint16_t viewportWidthDelta =
        (viewportWidth > fileViewportWidth) ? (viewportWidth - fileViewportWidth) : (fileViewportWidth - viewportWidth);
    const uint16_t viewportHeightDelta = (viewportHeight > fileViewportHeight)
                                             ? (viewportHeight - fileViewportHeight)
                                             : (fileViewportHeight - viewportHeight);
    // v18.9.9.3a: when this open is force-simple, the 4 fields that
    // createSectionFile's force-simple path overrides are stored on disk
    // as the OVERRIDE values.
    // v18.9.9.151: flipped from bypass to strict-require. Old bypass let
    // heavy pre-compat sections load unchallenged under force-simple, then
    // streamed render couldn't fit them in post-BT tight heap (~1 KB
    // maxAlloc). Now force-simple opens require the file's stored values
    // to BE the override values; heavy files invalidate and rebuild lazily
    // with actual force settings.
    const bool embeddedStyleMismatch = forceSimpleRendering ? fileEmbeddedStyle
                                                            : (embeddedStyle != fileEmbeddedStyle);
    const bool imageRenderingMismatch = forceSimpleRendering ? (fileImageRendering != 2)
                                                             : (imageRendering != fileImageRendering);
    const bool bionicMismatch = forceSimpleRendering ? fileBionicReadingEnabled
                                                     : (bionicReadingEnabled != fileBionicReadingEnabled);
    const bool guideMismatch = forceSimpleRendering ? fileGuideReadingEnabled
                                                    : (guideReadingEnabled != fileGuideReadingEnabled);
    // v18.9.9.24: v43 stores tableRendering as a trailer byte at HEADER_SIZE_V42.
    // Older files default to TABLES_DISPLAY (that's how they were built).
    // Peek without disturbing the current position -- rest of the header read
    // continues sequentially from where we left off after guideReadingEnabled.
    // 0 = TABLES_DISPLAY per CrossPointSettings::TABLE_RENDERING; hard-coded here
    // to keep the Epub library free of app-layer settings dependencies.
    uint8_t fileTableRendering = 0;
    if (version >= 43) {
      const uint32_t restorePos = file.position();
      if (!file.seek(HEADER_SIZE_V42) || !serialization::tryReadPod(file, fileTableRendering) ||
          !file.seek(restorePos)) {
        file.close();
        LOG_ERR("SCT", "Deserialization failed: could not read v43 tableRendering trailer (%s)", path.c_str());
        return false;
      }
    }
    // v18.9.9.151: mirror the strict-require flip for tableRendering. A
    // force-simple open expects PARAGRAPHS (1) on disk; anything else means
    // the file was built without force-simple and must be invalidated.
    const bool tableRenderingMismatch = forceSimpleRendering ? (fileTableRendering != 1)
                                                             : (tableRendering != fileTableRendering);
    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || forceParagraphIndents != fileForceParagraphIndents ||
        paragraphAlignment != fileParagraphAlignment || viewportWidthDelta > kViewportTolerancePx ||
        viewportHeightDelta > kViewportTolerancePx || hyphenationEnabled != fileHyphenationEnabled ||
        embeddedStyleMismatch || imageRenderingMismatch || bionicMismatch || guideMismatch ||
        tableRenderingMismatch) {
      file.close();
      // CrumBLE prebake debug: prebake'd sections are arriving but the device
      // refuses them with a fingerprint mismatch -- and the previous LOG_ERR
      // just said "Parameters do not match" without naming the offender. Log
      // both sides of all 12 fields so the optimizer-integration tests can
      // see exactly which one differs.
      LOG_ERR("SCT",
              "Fingerprint mismatch on %s:\n"
              "  fontId:                file=%d   device=%d   %s\n"
              "  lineCompression:       file=%.4f device=%.4f %s\n"
              "  extraParagraphSpacing: file=%d   device=%d   %s\n"
              "  forceParagraphIndents: file=%d   device=%d   %s\n"
              "  paragraphAlignment:    file=%u   device=%u   %s\n"
              "  viewportWidth:         file=%u   device=%u   %s\n"
              "  viewportHeight:        file=%u   device=%u   %s\n"
              "  hyphenationEnabled:    file=%d   device=%d   %s\n"
              "  embeddedStyle:         file=%d   device=%d   %s\n"
              "  imageRendering:        file=%u   device=%u   %s\n"
              "  bionicReadingEnabled:  file=%d   device=%d   %s\n"
              "  guideReadingEnabled:   file=%d   device=%d   %s",
              path.c_str(),
              fileFontId, fontId, (fileFontId == fontId ? "OK" : "MISMATCH"),
              fileLineCompression, lineCompression, (fileLineCompression == lineCompression ? "OK" : "MISMATCH"),
              fileExtraParagraphSpacing, extraParagraphSpacing,
              (fileExtraParagraphSpacing == extraParagraphSpacing ? "OK" : "MISMATCH"),
              fileForceParagraphIndents, forceParagraphIndents,
              (fileForceParagraphIndents == forceParagraphIndents ? "OK" : "MISMATCH"),
              fileParagraphAlignment, paragraphAlignment,
              (fileParagraphAlignment == paragraphAlignment ? "OK" : "MISMATCH"),
              fileViewportWidth, viewportWidth, (fileViewportWidth == viewportWidth ? "OK" : "MISMATCH"),
              fileViewportHeight, viewportHeight, (fileViewportHeight == viewportHeight ? "OK" : "MISMATCH"),
              fileHyphenationEnabled, hyphenationEnabled,
              (fileHyphenationEnabled == hyphenationEnabled ? "OK" : "MISMATCH"),
              fileEmbeddedStyle, embeddedStyle, (fileEmbeddedStyle == embeddedStyle ? "OK" : "MISMATCH"),
              fileImageRendering, imageRendering, (fileImageRendering == imageRendering ? "OK" : "MISMATCH"),
              fileBionicReadingEnabled, bionicReadingEnabled,
              (fileBionicReadingEnabled == bionicReadingEnabled ? "OK" : "MISMATCH"),
              fileGuideReadingEnabled, guideReadingEnabled,
              (fileGuideReadingEnabled == guideReadingEnabled ? "OK" : "MISMATCH"));
      return false;
    }
  }

  // CrumBLE: degraded-cache metadata, written right after the layout params.
  bool fileImagesSuppressed = false;
  uint32_t fileBuildMaxAlloc = 0;
  if (!serialization::tryReadPod(file, fileImagesSuppressed) ||
      !serialization::tryReadPod(file, fileBuildMaxAlloc)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing degraded-cache fields (%s)", path.c_str());
    return false;
  }

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count (%s)", path.c_str());
    return false;
  }

  // CrumBLE: the degraded-cache fields are still parsed (v38 format stays valid,
  // so existing caches don't all re-index), but we no longer auto-rebuild on load
  // when the heap looks "recovered". That rebuild-when-recovered re-indexed the
  // chapter on boot-resume into a book -- where the fresh-boot heap always reads
  // recovered -- right as the bonded BLE remote auto-connected, starving NimBLE
  // and dropping the link ("Bluetooth couldn't stay connected" on a book that
  // read fine when entered from home). Leaving the cache as-is keeps BLE reading
  // stable. Image-suppressed chapters restore their images on an explicit cache
  // clear / re-open instead of automatically. (Follow-up: a heap-safe, BLE-aware
  // image-recovery that doesn't re-index right as the remote connects.)
  (void)fileImagesSuppressed;
  (void)fileBuildMaxAlloc;

  // CrumBLE 4.3: read the v39 embedded-glyph-subset trailer fields. Seek
  // directly to HEADER_SIZE_V38 because the 4 LUT offsets that v38 wrote
  // (lutOffset, anchorMapOffset, paragraphLutOffset, liLutOffset) come
  // BEFORE these fields in the header; tryLoadFromPath doesn't need the
  // LUT offsets here (they're consumed by loadPageFromSectionFile via its
  // own seek), so skipping past them is fine. v38 sections fall through
  // with the embedded* fields at their default 0/0/0 -- "no subset".
  embeddedGlyphSubsetOffset_ = 0;
  embeddedGlyphSubsetSize_ = 0;
  embeddedGlyphSubsetCpfontHash_ = 0;
  glyphAtlasOffset_ = 0;
  glyphAtlasSize_ = 0;
  glyphAtlasCpfontHash_ = 0;
  glyphAtlasAltOffset_ = 0;
  glyphAtlasAltSize_ = 0;
  glyphAtlasAltCpfontHash_ = 0;
  if (fileVersion_ >= 39) {
    if (!file.seek(HEADER_SIZE_V38)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not seek to v39 trailer (%s)", path.c_str());
      return false;
    }
    if (!serialization::tryReadPod(file, embeddedGlyphSubsetOffset_) ||
        !serialization::tryReadPod(file, embeddedGlyphSubsetSize_) ||
        !serialization::tryReadPod(file, embeddedGlyphSubsetCpfontHash_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated v39 embedded-glyph-subset trailer (%s)", path.c_str());
      return false;
    }
  }
  if (fileVersion_ >= 40) {
    // v40 atlas trailer sits immediately after the v39 subset trailer.
    // File position is already there because the v39 reads above moved
    // it forward by exactly 3 * sizeof(uint32_t); we read the next three
    // uint32_t fields with no seek needed.
    if (!serialization::tryReadPod(file, glyphAtlasOffset_) ||
        !serialization::tryReadPod(file, glyphAtlasSize_) ||
        !serialization::tryReadPod(file, glyphAtlasCpfontHash_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated v40 glyph-atlas trailer (%s)", path.c_str());
      return false;
    }
  }
  if (fileVersion_ >= 41) {
    // v41 alternate atlas trailer sits immediately after the v40 trailer.
    // Same offset-progression pattern: file position advanced by the v40
    // reads, no seek needed. v40 files don't have these bytes, so we
    // leave the alt fields at 0 and the install path falls through to
    // the single (v40) atlas slot.
    if (!serialization::tryReadPod(file, glyphAtlasAltOffset_) ||
        !serialization::tryReadPod(file, glyphAtlasAltSize_) ||
        !serialization::tryReadPod(file, glyphAtlasAltCpfontHash_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated v41 glyph-atlas-alt trailer (%s)", path.c_str());
      return false;
    }
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  // CrumBLE: remember which slot we actually loaded from so subsequent
  // reads (loadPageFromSectionFile etc.) re-open the right file. Without
  // this, a section loaded from the prebake fallback could not read its
  // own page bytes because reads went to the non-existent live filePath.
  activeFilePath = path;
  LOG_DBG("SCT", "Deserialization succeeded: %d pages (from %s)", pageCount, path.c_str());
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                                const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyleIn,
                                const uint8_t imageRenderingIn, const bool bionicReadingEnabledIn,
                                const bool guideReadingEnabledIn, const uint8_t tableRenderingIn,
                                const std::function<void()>& popupFn,
                                bool* imagesWereSuppressed, bool* layoutAbortedForLowMemory,
                                const bool forceSimpleRendering, const bool suppressTablesOnly) {
  // v18.9.9.29 (v20 Phase C1): one-shot wrapper over the incremental API.
  // Behavior is identical to the pre-refactor version -- startBuild sets up
  // parser + tmp file + header, buildSomeMore(0) runs parseStep to
  // completion and calls finalizeBuild internally when the parser reports
  // Done.
  if (!startBuild(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                  viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyleIn, imageRenderingIn,
                  bionicReadingEnabledIn, guideReadingEnabledIn, tableRenderingIn, popupFn, imagesWereSuppressed,
                  layoutAbortedForLowMemory, forceSimpleRendering, suppressTablesOnly)) {
    return false;
  }
  if (!buildSomeMore(0)) {
    return false;
  }
  return buildComplete_;
}

bool Section::startBuild(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                         const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                         const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
                         const bool embeddedStyleIn, const uint8_t imageRenderingIn,
                         const bool bionicReadingEnabledIn, const bool guideReadingEnabledIn,
                         const uint8_t tableRenderingIn, const std::function<void()>& popupFn,
                         bool* imagesWereSuppressed, bool* layoutAbortedForLowMemory,
                         const bool forceSimpleRendering, const bool suppressTablesOnly) {
  if (build_) {
    LOG_ERR("SCT", "startBuild called with build already active");
    return false;
  }
  buildComplete_ = false;
  pageCount = 0;
  // v18.9.9.30: clear last-build outcome flags -- they're set by the current
  // build's finalize (success) or the Error branch (failure) so callers can
  // query them without holding onto stack-scoped out-params.
  lastBuildLayoutAbortedForLowMemory_ = false;
  lastBuildImagesWereSuppressed_ = false;

  // v18.9.6: force-simple retry overrides the styling knobs to their most
  // memory-frugal state. Every subsequent read in this function uses the
  // effective values.
  const bool embeddedStyle = forceSimpleRendering ? false : embeddedStyleIn;
  const uint8_t imageRendering = forceSimpleRendering ? 2 : imageRenderingIn;
  const bool bionicReadingEnabled = forceSimpleRendering ? false : bionicReadingEnabledIn;
  const bool guideReadingEnabled = forceSimpleRendering ? false : guideReadingEnabledIn;
  // v18.9.9.24: tableRendering baked into the header. force-simple stores 1
  // (TABLES_PARAGRAPHS) so a later mid-session force-simple continuation
  // matches. suppressTablesOnly also stores 1 -- the section content is
  // paragraphs, regardless of which knob got us there.
  const uint8_t tableRendering = (forceSimpleRendering || suppressTablesOnly) ? 1 : tableRenderingIn;
  // v18.9.9.6 Level 2: tables suppressed but nothing else. Piggybacks on the
  // same parser guard force-simple uses (setChapterParserSuppressTablesForSimple)
  // but leaves images/embedded style/bionic/guide untouched. Level 3 subsumes
  // Level 2, so only arm the tables-only guard when Level 3 is not.
  // v18.9.9.24: also arm when the user-facing tableRendering setting is
  // PARAGRAPHS (value 1) even outside compat -- same runtime parser guard.
  const bool userTablesParagraphs = tableRenderingIn == 1;
  const bool tablesGuardArmed = forceSimpleRendering || suppressTablesOnly || userTablesParagraphs;
  if (forceSimpleRendering) {
    LOG_INF("SCT", "createSectionFile forceSimpleRendering=1: images=suppressed embeddedStyle=0 bionic=0 guide=0 tables=paragraphs");
    setChapterParserSuppressTablesForSimple(true);
  } else if (suppressTablesOnly) {
    LOG_INF("SCT", "createSectionFile suppressTablesOnly=1: tables=paragraphs (images/style/bionic/guide preserved)");
    setChapterParserSuppressTablesForSimple(true);
  } else if (userTablesParagraphs) {
    LOG_INF("SCT", "createSectionFile user tables=paragraphs setting: tables=paragraphs");
    setChapterParserSuppressTablesForSimple(true);
  }
  // v18.9.9.29 (v20 Phase C1): the tables-suppress parser guard is now
  // owned by BuildContext.tablesGuardArmed (see finalizeBuild /
  // abandonBuild for the disarm). Replaces the stack-scoped SimpleFlagGuard
  // that used to sit here.
  const auto disarmGuardOnEarlyReturn = [tablesGuardArmed]() {
    if (tablesGuardArmed) setChapterParserSuppressTablesForSimple(false);
  };
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpSectionPath = filePath + ".tmp";
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  const uint32_t buildStartMaxAlloc = ESP.getMaxAllocHeap();
  // v20 Phase A (from PR #2452 Smart Indexing): persistent HTML cache.
  // The unzipped HTML is keyed only on the book, not on render settings, so
  // it survives the layout .bin invalidation that fires when font/margin/
  // orientation change -- rebuilds then skip zip inflation entirely.
  // Promoted atomically as soon as inflate succeeds, so even a build that
  // later aborts (OOM, user close mid-parse) still caches the HTML for the
  // next open.
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";
  // CrumBLE: snapshot the largest free block we're building with. If images end
  // up suppressed, this is stored in the cache header so a later load can tell
  // whether the heap has recovered enough (e.g. BLE disconnected) to be worth
  // rebuilding the chapter with images.
  LOG_DBG("SCT", "Create section start: spine=%d viewport=%ux%u image=%u bionic=%u guide=%u free=%u maxAlloc=%u",
          spineIndex, viewportWidth, viewportHeight, imageRendering, bionicReadingEnabled, guideReadingEnabled,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // v20 Phase A: check for a pre-existing cached HTML first. If htmlPath
  // exists, it was atomically renamed in a prior build after successful
  // inflate, so it's known-complete. Skip the multi-second zip inflate.
  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  // The path we point the parser at: cached htmlPath if reused OR just
  // atomically promoted, else the raw tmp (rename failed but stream
  // succeeded).
  std::string parsePath = htmlPath;
  uint32_t fileSize = 0;
  if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s (skipping zip inflate)", htmlPath.c_str());
  } else {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      FsFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      disarmGuardOnEarlyReturn();
      return false;
    }

    LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes, free=%u, maxAlloc=%u)", tmpHtmlPath.c_str(), fileSize,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // v20 Phase A: promote to the persistent cache immediately. The inflate
    // is complete and the bytes are valid regardless of whether the layout
    // build finishes below, so a future rebuild (even if this one aborts on
    // OOM) skips inflation. If rename fails we just parse from the temp;
    // the cleanup below removes it either way.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      parsePath = htmlPath;
      LOG_DBG("SCT", "Promoted HTML cache: %s", htmlPath.c_str());
    } else {
      parsePath = tmpHtmlPath;
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp %s", tmpHtmlPath.c_str());
    }
  }

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    if (!reusedHtml && parsePath == tmpHtmlPath) Storage.remove(tmpHtmlPath.c_str());
    disarmGuardOnEarlyReturn();
    return false;
  }
  // v18.9.9.39 Phase C3: seed fileVersion_ so loadPageFromSectionFile can
  // read pages from the in-progress build using the same Page::deserialize
  // version gate the finalized-file path uses. We write pages at
  // SECTION_FILE_VERSION (currently 43) via onPageComplete()'s serialize
  // call, so reads from the tmp file must decode at that same version.
  fileVersion_ = SECTION_FILE_VERSION;
  if (!writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                              bionicReadingEnabled, guideReadingEnabled, tableRendering)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (!reusedHtml && parsePath == tmpHtmlPath) Storage.remove(tmpHtmlPath.c_str());
    disarmGuardOnEarlyReturn();
    return false;
  }

  auto* rawCtx = new (std::nothrow) BuildContext();
  if (!rawCtx) {
    LOG_ERR("SCT", "OOM: BuildContext");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (!reusedHtml && parsePath == tmpHtmlPath) Storage.remove(tmpHtmlPath.c_str());
    disarmGuardOnEarlyReturn();
    return false;
  }
  std::unique_ptr<BuildContext> ctx(rawCtx);
  ctx->fontId = fontId;
  ctx->lineCompression = lineCompression;
  ctx->extraParagraphSpacing = extraParagraphSpacing;
  ctx->forceParagraphIndents = forceParagraphIndents;
  ctx->paragraphAlignment = paragraphAlignment;
  ctx->viewportWidth = viewportWidth;
  ctx->viewportHeight = viewportHeight;
  ctx->hyphenationEnabled = hyphenationEnabled;
  ctx->embeddedStyle = embeddedStyle;
  ctx->imageRendering = imageRendering;
  ctx->bionicReadingEnabled = bionicReadingEnabled;
  ctx->guideReadingEnabled = guideReadingEnabled;
  ctx->tableRendering = tableRendering;
  ctx->tmpSectionPath = tmpSectionPath;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->parsePath = parsePath;
  ctx->reusedHtml = reusedHtml;
  ctx->buildStartMaxAlloc = buildStartMaxAlloc;
  ctx->imagesWereSuppressedOut = imagesWereSuppressed;
  ctx->layoutAbortedForLowMemoryOut = layoutAbortedForLowMemory;
  ctx->tablesGuardArmed = tablesGuardArmed;
  ctx->popupFn = popupFn;
  ctx->lastPopupTickMs = millis();

  // Derive the content base directory and image cache path prefix for the parser.
  // These live in the BuildContext because ChapterHtmlSlimParser stores them by
  // reference and the parser outlives startBuild.
  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = ctx->cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, static_cast<unsigned>(ctx->cssParser->ruleCount()), cssHeapBefore.freeHeap,
              cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Parser holds references into the BuildContext (parsePath, contentBase,
  // imageBasePath). Capture ctx.get() so the completePageFn appends to the
  // context's LUT even when the caller mutates build_ later.
  BuildContext* ctxPtr = ctx.get();
  auto* rawParser = new (std::nothrow) ChapterHtmlSlimParser(
      epub, ctxPtr->parsePath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, bionicReadingEnabled, guideReadingEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        ctxPtr->lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, imageRendering, popupFn, ctxPtr->cssParser);
  if (!rawParser) {
    LOG_ERR("SCT", "OOM: ChapterHtmlSlimParser");
    if (ctx->cssParser) ctx->cssParser->clear();
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (!reusedHtml && parsePath == tmpHtmlPath) Storage.remove(tmpHtmlPath.c_str());
    disarmGuardOnEarlyReturn();
    return false;
  }
  ctx->parser.reset(rawParser);

  Hyphenator::setPreferredLanguage(epub->getLanguage());
  LOG_DBG("SCT", "Parser start: spine=%d free=%u maxAlloc=%u", spineIndex, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // From here on ownership belongs to build_; abandonBuild handles cleanup
  // (parser abortParse, css clear, file close+remove tmp, tables guard
  // disarm). Any failure below returns via abandonBuild instead of the
  // explicit early-return dance above.
  build_ = std::move(ctx);

  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    abandonBuild();
    return false;
  }
  // v18.9.9.76: snapshot parse file size for the byte-ratio estimate.
  // Parser's parseFile_ is opened by beginParse, so query after.
  build_->totalBytes = static_cast<uint32_t>(build_->parser->parseTotalBytes());
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore with no active build");
    return false;
  }
  const int startCount = pageCount;
  constexpr uint32_t kPopupTickMs = 250;
  for (;;) {
    // v18.9.9.30: fire the popup callback at ~250 ms cadence so the
    // "Indexing..." popup animates (and, from C2, shows live pageCount)
    // during long parses. Replaces the popup tick that used to live
    // inside parseAndBuildPages.
    if (build_->popupFn && (millis() - build_->lastPopupTickMs) >= kPopupTickMs) {
      build_->popupFn();
      build_->lastPopupTickMs = millis();
    }
    const auto status = build_->parser->parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Parse error during incremental build");
      // v18.9.9.30: snapshot parser outcome flags before abandonBuild
      // tears build_ down. Also forward to any stack-scoped out-params
      // the one-shot createSectionFile wrapper handed us -- callers of
      // the incremental API query the Section getters instead.
      lastBuildLayoutAbortedForLowMemory_ = build_->parser->wasLowMemoryAbortTriggered();
      lastBuildImagesWereSuppressed_ = build_->parser->wasLowMemoryFallbackTriggered();
      if (build_->layoutAbortedForLowMemoryOut) {
        *build_->layoutAbortedForLowMemoryOut = lastBuildLayoutAbortedForLowMemory_;
      }
      if (build_->imagesWereSuppressedOut) {
        *build_->imagesWereSuppressedOut = lastBuildImagesWereSuppressed_;
      }
      abandonBuild();
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      return finalizeBuild();
    }
    // ParseStatus::More: yield once we've laid out the requested pages.
    // maxPages <= 0 = run to completion (createSectionFile one-shot uses 0).
    if (maxPages > 0 && (pageCount - startCount) >= maxPages) {
      // v18.9.9.76: snapshot bytes consumed at yield so estimatedTotalPages()
      // has fresh input for the "page X of ~Y" popup between ticks.
      build_->bytesConsumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
      return true;
    }
  }
}

// v18.9.9.76: port of crosspoint/feat-smart-indexing's estimatedTotalPages.
// Returns pageCount when no build is active (finalized section — exact). During
// a build, extrapolates from byte ratio + EMA smoothing so the popup shows a
// stable "page X of ~Y" instead of jittering as long chapters have variable
// page density. ALPHA=0.25 matches crosspoint's tuning.
uint16_t Section::estimatedTotalPages() const {
  if (!build_) return pageCount;
  if (build_->totalBytes == 0 || build_->bytesConsumed == 0 || pageCount == 0) return pageCount;

  const float ratio = static_cast<float>(build_->bytesConsumed) / static_cast<float>(build_->totalBytes);
  if (ratio <= 0.0f) return pageCount;
  const float rawEstimate = static_cast<float>(pageCount) / ratio;

  // First sample seeds the EMA. Subsequent samples smooth with ALPHA=0.25.
  constexpr float kAlpha = 0.25f;
  auto& s = build_->smoothedEstimate;
  if (s <= 0.0f || build_->smoothedAtConsumed == 0) {
    s = rawEstimate;
  } else {
    s = kAlpha * rawEstimate + (1.0f - kAlpha) * s;
  }
  build_->smoothedAtConsumed = build_->bytesConsumed;

  // Never claim fewer pages than we already have — the popup would count backward.
  const uint32_t est = static_cast<uint32_t>(s + 0.5f);
  return static_cast<uint16_t>(est < pageCount ? pageCount : est);
}

bool Section::finalizeBuild() {
  if (!build_) return false;

  // Flush the trailing page (parser finishParse fires the completePageFn
  // one last time for whatever pending line-buffer state remained).
  const bool finishOk = build_->parser->finishParse();
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, finishOk ? 1U : 0U,
          pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  const bool builtImagesSuppressed = build_->parser->wasLowMemoryFallbackTriggered();
  // v18.9.9.30: snapshot on Section for incremental callers (their stack
  // out-param pointers would be dangling by now). Also propagate to any
  // stack out-params the one-shot createSectionFile wrapper handed us.
  lastBuildLayoutAbortedForLowMemory_ = build_->parser->wasLowMemoryAbortTriggered();
  lastBuildImagesWereSuppressed_ = builtImagesSuppressed;
  if (build_->imagesWereSuppressedOut) *build_->imagesWereSuppressedOut = builtImagesSuppressed;
  if (build_->layoutAbortedForLowMemoryOut) {
    *build_->layoutAbortedForLowMemoryOut = lastBuildLayoutAbortedForLowMemory_;
  }

  // v20 Phase A: clean up temp HTML if we didn't promote it. htmlPath itself
  // stays INTACT so future rebuilds reuse the inflated HTML.
  if (!build_->reusedHtml && build_->parsePath == build_->tmpHtmlPath) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }

  const auto failFinalize = [this]() {
    file.close();
    Storage.remove(build_->tmpSectionPath.c_str());
    if (build_->cssParser) build_->cssParser->clear();
    if (build_->tablesGuardArmed) setChapterParserSuppressTablesForSimple(false);
    build_.reset();
    return false;
  };

  if (!finishOk) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    return failFinalize();
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  for (const auto& entry : build_->lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    if (!serialization::tryWritePod(file, entry.fileOffset)) {
      hasFailedLutRecords = true;
      break;
    }
  }
  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    return failFinalize();
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) {
    return failFinalize();
  }
  for (const auto& [anchor, page] : anchors) {
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      return failFinalize();
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(build_->lut.size()))) {
    return failFinalize();
  }
  for (const auto& entry : build_->lut) {
    if (!serialization::tryWritePod(file, entry.paragraphIndex)) {
      return failFinalize();
    }
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    if (!serialization::tryWritePod(file, entry.listItemIndex)) {
      return failFinalize();
    }
  }

  // Patch header with final imagesSuppressed, buildMaxAlloc, pageCount, lutOffset,
  // anchorMapOffset, paragraphLutOffset, and liLutOffset (all written as
  // placeholders by writeSectionFileHeader, in this exact order).
  // CrumBLE 4.3: the seek arithmetic targets fields within the v38-shaped
  // portion of the header, so use HEADER_SIZE_V38 instead of HEADER_SIZE
  // (which now includes the v39 embedded-glyph-subset trailer fields).
  if (!file.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 4 - sizeof(pageCount) - sizeof(uint32_t) - sizeof(bool)) ||
      !serialization::tryWritePod(file, builtImagesSuppressed) ||
      !serialization::tryWritePod(file, build_->buildStartMaxAlloc) ||
      !serialization::tryWritePod(file, pageCount) || !serialization::tryWritePod(file, lutOffset) ||
      !serialization::tryWritePod(file, anchorMapOffset) ||
      !serialization::tryWritePod(file, paragraphLutOffset) ||
      !serialization::tryWritePod(file, liLutFileOffset) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    return failFinalize();
  }
  file.close();

  // v18.9.9.16: diagnostic + retry around the final promote. See the
  // pre-refactor comment for the rationale -- rename to filePath is what
  // makes a fresh build atomically visible.
  bool renamed = false;
  const std::string tmpPath = build_->tmpSectionPath;
  for (int attempt = 0; attempt < 3 && !renamed; attempt++) {
    const bool tmpExistsPre = Storage.exists(tmpPath.c_str());
    const bool destExistsPre = Storage.exists(filePath.c_str());
    if (destExistsPre) {
      Storage.remove(filePath.c_str());
    }
    if (attempt > 0) {
      LOG_INF("SCT", "Section cache rename retry %d (tmpExists=%u destExisted=%u free=%u maxAlloc=%u)", attempt + 1,
              tmpExistsPre ? 1U : 0U, destExistsPre ? 1U : 0U, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      delay(50);
    }
    renamed = Storage.rename(tmpPath.c_str(), filePath.c_str());
  }
  if (!renamed) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place (tmpExists=%u destExists=%u free=%u maxAlloc=%u)",
            Storage.exists(tmpPath.c_str()) ? 1U : 0U, Storage.exists(filePath.c_str()) ? 1U : 0U, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    Storage.remove(tmpPath.c_str());
    if (build_->cssParser) build_->cssParser->clear();
    if (build_->tablesGuardArmed) setChapterParserSuppressTablesForSimple(false);
    build_.reset();
    return false;
  }
  activeFilePath = filePath;
  if (build_->cssParser) build_->cssParser->clear();
  if (build_->tablesGuardArmed) setChapterParserSuppressTablesForSimple(false);
  build_.reset();
  buildComplete_ = true;
  LOG_DBG("SCT", "Create section done: spine=%d pages=%u free=%u maxAlloc=%u", spineIndex, pageCount, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return true;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (file) {
    file.close();
    if (!build_->tmpSectionPath.empty()) {
      Storage.remove(build_->tmpSectionPath.c_str());
    }
  }
  if (!build_->reusedHtml && !build_->tmpHtmlPath.empty() && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  if (build_->tablesGuardArmed) setChapterParserSuppressTablesForSimple(false);
  build_.reset();
  buildComplete_ = false;
  pageCount = 0;
}

bool Section::ensurePageHeapReserveAtBoot() {
  return tryAcquirePageHeapReserve() != nullptr;
}

bool Section::pageHeapReserveHeld() { return pageHeapReserve_ != nullptr; }

void Section::releasePageHeapReserveForBtEnable() {
  if (pageHeapReserve_) {
    releasePageHeapReserve();
  }
}

bool Section::tryReacquirePageHeapReserve() { return tryAcquirePageHeapReserve() != nullptr; }

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  SET_CHECKPOINT("section:loadPage");
  // CrumBLE 4.2: heap pre-flight. Page deserialization runs `std::vector
  // <std::string>::resize()` on the per-word arrays inside
  // TextBlock::deserialize (words, wordXpos, wordStyles,
  // wordBackgroundBlack). Under heap pressure that allocation throws
  // std::bad_alloc, which -- because the firmware is built with
  // -fno-exceptions -- terminates the device instead of being catchable.
  // Observed in the field: connecting Bluetooth on a page with a heavy
  // text block ate 58 KB for NimBLE, left ~10-15 KB maxAlloc, the next
  // render's loadPageFromSectionFile entered TextBlock::deserialize and
  // crashed inside vector<string>::_M_default_append.
  //
  // Returning nullptr here routes the caller (EpubReaderActivity render
  // loop) into its existing retry-then-error-screen path
  // (MAX_PAGE_LOAD_RETRIES + drawCenteredText(STR_PAGE_LOAD_ERROR)),
  // so the user sees "Page load error -- close + reopen" instead of a
  // hard reboot. 25 KB threshold covers a typical 150-300 word page
  // with overhead headroom; the original crash had maxAlloc well below
  // this floor.
  // CrumBLE 4.4 post-bisect: page-load floor lowered to 1.5 KB.
  // TextBlock::deserialize now allocates ONE compact data block per
  // TextBlock (~100-500 bytes each) instead of the old per-vector
  // pattern (which peaked at 5-8 KB contiguous). Empirically the new
  // pattern succeeds even at MaxAlloc=2036 bytes under post-BT pressure
  // (validated across 5 page-turns in the Option I test). The 8 KB
  // floor was set for the old peak and now refuses loads that would
  // actually succeed; with 2-bit atlas + post-BT pressure, MaxAlloc
  // typically lands at ~5-7 KB which was below the legacy gate. Floor
  // stays at 1500 to catch truly degenerate cases (largest single
  // TextBlock compact block) before they bad_alloc.
  //
  // v18.9.9.7: raised to 4000. Field crash showed page turn deserialize
  // entering at MaxAlloc=8180 (well above the 1500 floor), draining to
  // MaxAlloc=16 mid-loop as PageTableFragment cells consumed heap, then
  // panicking in the vector<unique_ptr<TextBlock>> unwind after TXB
  // refused the last alloc. The 1500 gate lets these pages start but not
  // finish. Refusing at 4000 upfront routes to the reader escalation
  // branch (Level 1 defrag -> Level 2 tables-only -> Level 3 Simple
  // Rendering) before any allocation has committed. Some borderline
  // pages that today succeed will now refuse -- the escalation path
  // handles them gracefully instead of crashing later.
  // v18.9.9.359: raised 4000 -> 12000. Field crash on Chinese book chapter
  // jump: streaming atlas install completed at maxAlloc=18420, entered
  // loadPageFromSectionFile with the 4000 gate passing, then vector<
  // unique_ptr<TextBlock>> allocation inside the deserialize path
  // consumed ~14 KB more heap and hit std::terminate at
  // free=4620/maxAlloc=1396. The 4000 gate was set for post-BT-pressure
  // pages where deserialize had ~1500-4000 B compact blocks; CJK books
  // with streaming atlas already burned the safety margin. 12000 gives
  // the deserialize + subsequent atlas per-glyph SD reads their needed
  // headroom.
  constexpr uint32_t PAGE_LOAD_MIN_MAX_ALLOC = 12000;

  // CrumBLE 4.3 option 3: opportunistic re-acquire of the reserve when it
  // was released earlier (BT enable path, or a prior page load that didn't
  // recover) and heap has since recovered enough to absorb the 18 KB
  // chunk without sliding back below the BT-enable pre-flight floor. Skip
  // the re-acquire if we're already tight on contiguous heap -- in that
  // case the deserialize needs the bytes more than the reserve does.
  if (!pageHeapReserve_ && ESP.getMaxAllocHeap() > 30 * 1024) {
    tryAcquirePageHeapReserve();
  }

  // CrumBLE 4.3 option 3: under heap pressure release the pre-allocated
  // reserve so the deserialize allocator has a contiguous 12 KB block to
  // pull from. Pressure threshold is set above the bare PAGE_LOAD floor so
  // the reserve drops EARLY enough to actually help (if we waited until
  // MaxAlloc < 8 KB the release happens too late -- vector::resize will
  // have already grabbed and re-fragmented whatever's available). Drop is
  // a no-op when the reserve was never acquired or was already released
  // by a prior page load that hasn't been able to re-acquire yet.
  const uint32_t maxAllocBeforeReserveDrop = ESP.getMaxAllocHeap();
  const bool releasedReserveForDeserialize =
      (maxAllocBeforeReserveDrop < PAGE_HEAP_RESERVE_TRIGGER_MAX_ALLOC) && pageHeapReserve_ != nullptr;
  if (releasedReserveForDeserialize) {
    releasePageHeapReserve();
    LOG_INF("SCT",
            "loadPageFromSectionFile: released page heap reserve under pressure (maxAlloc %u -> %u, free %u)",
            maxAllocBeforeReserveDrop, ESP.getMaxAllocHeap(), ESP.getFreeHeap());
  }

  if (ESP.getMaxAllocHeap() < PAGE_LOAD_MIN_MAX_ALLOC) {
    LOG_ERR("SCT", "loadPageFromSectionFile: maxAlloc=%u below %u, refusing load to avoid bad_alloc terminate",
            ESP.getMaxAllocHeap(), PAGE_LOAD_MIN_MAX_ALLOC);
    return nullptr;
  }

  // v18.9.9.39 Phase C3: if a build is in progress, always take the tmp-file
  // read path -- never fall through to Storage.openFileForRead(activeFilePath)
  // below because that call REUSES the member `file` handle, which during a
  // build is the append-mode write handle. Falling through would close the
  // write handle and destroy the build. Return nullptr for pages that haven't
  // been laid out yet; the reader interprets that as "wait for more pages" and
  // shows the indexing popup.
  if (build_) {
    if (currentPage < 0 || static_cast<size_t>(currentPage) >= build_->lut.size()) {
      // Target page not yet built. Not an error; loop() will lay it out.
      return nullptr;
    }
    file.flush();
    const uint32_t writePos = file.position();
    const uint32_t pagePos = build_->lut[static_cast<size_t>(currentPage)].fileOffset;
    if (!file.seek(pagePos)) {
      file.seek(writePos);
      LOG_ERR("SCT", "loadPageFromSectionFile: build-peek seek to %u failed", pagePos);
      return nullptr;
    }
    auto page = Page::deserialize(file, fileVersion_);
    // Restore write cursor even on deserialize failure so subsequent
    // onPageComplete writes don't clobber earlier pages.
    file.seek(writePos);
    if (releasedReserveForDeserialize) {
      const uint32_t maxAllocBeforeReacquire = ESP.getMaxAllocHeap();
      const void* reacquired = tryAcquirePageHeapReserve();
      LOG_INF("SCT", "loadPageFromSectionFile: build-peek reserve re-acquire %s (maxAlloc %u -> %u, free %u)",
              reacquired ? "OK" : "failed", maxAllocBeforeReacquire, ESP.getMaxAllocHeap(), ESP.getFreeHeap());
    }
    return page;
  }

  if (!Storage.openFileForRead("SCT", activeFilePath, file)) {
    return nullptr;
  }

  if (!file.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 4)) {
    file.close();
    return nullptr;
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(file, lutOffset) || !file.seek(lutOffset + sizeof(uint32_t) * currentPage)) {
    file.close();
    return nullptr;
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(file, pagePos) || !file.seek(pagePos)) {
    file.close();
    return nullptr;
  }

  // v18.9.9.19: pass fileVersion_ so v42+ files' table-fragment size prefix
  // gets consumed before PageTableFragment::deserialize runs. Older versions
  // skip the extra read.
  auto page = Page::deserialize(file, fileVersion_);
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // CrumBLE 4.3 option 3: best-effort reserve re-acquire. If the deserialize
  // consumed the freed reserve region the heap_caps_malloc will return null
  // and pageHeapReserve_ stays nullptr; the next render's pressure check
  // will try again once heap has recovered (e.g. after the post-render
  // cache drop frees the page DOM). Logged at INF so the BT-stability
  // diagnostic shows whether the reserve cycle is sustaining itself.
  if (releasedReserveForDeserialize) {
    const uint32_t maxAllocBeforeReacquire = ESP.getMaxAllocHeap();
    const void* reacquired = tryAcquirePageHeapReserve();
    LOG_INF("SCT", "loadPageFromSectionFile: reserve re-acquire %s (maxAlloc %u -> %u, free %u)",
            reacquired ? "OK" : "failed -- will retry next render", maxAllocBeforeReacquire, ESP.getMaxAllocHeap(),
            ESP.getFreeHeap());
  }
  return page;
}

bool Section::renderPageStreamed(GfxRenderer& renderer, const int fontId, const int xOffset,
                                 const int yOffset, const bool foregroundBlack, const bool btLinked,
                                 const bool pxcImagesSafe) {
  // Per-element peak is ~500 bytes (one TextBlock compact block). Refuse
  // if MaxAlloc can't cover that plus margin. Much lower gate than the
  // whole-DOM path so streamed render fits into post-BT tight heap.
  // v18.9.9.146: REVERTED v141's 1600 back to 800. v145 test showed
  // post-BT heap sits at maxAlloc=1012 -- the 1600 gate refused EVERY
  // render, immediately kicking us to v137's cycle-BT path which
  // silent-restarted to reader-without-BT. From the user's POV, BT never
  // connected (it did connect at 6050 ms, but the first render refused
  // instantly). 800 lets streamed render proceed at the observed 1012
  // maxAlloc; most Simple Rendering pages fit. Rare pages with heavier
  // library-alloc peaks may still bad_alloc -> terminate + panic reboot,
  // which is a worse edge case but a rare one; the failure mode of
  // "never connects" is a common one.
  constexpr uint32_t STREAMED_MIN_MAX_ALLOC = 800;
  if (ESP.getMaxAllocHeap() < STREAMED_MIN_MAX_ALLOC) {
    LOG_ERR("SCT", "renderPageStreamed: maxAlloc=%u below %u; refusing",
            ESP.getMaxAllocHeap(), STREAMED_MIN_MAX_ALLOC);
    return false;
  }
  LOG_INF("RPSD", "phase=entry maxAlloc=%u", ESP.getMaxAllocHeap());

  // v18.9.9.13 Optimization A: reuse the persistent Section-member file
  // handle across consecutive renders. Every open/close hits the SD
  // driver (~10-50 ms). Under BT the reader stays on the streamed path
  // continuously, so the open+close overhead accumulates. Skip the open
  // when the handle is already usable.
  const bool wasFileOpen = file.isOpen();
  if (!file.isOpen()) {
    if (!Storage.openFileForRead("SCT", activeFilePath, file)) {
      return false;
    }
  }
  LOG_INF("RPSD", "phase=file-open (was=%d) maxAlloc=%u", (int)wasFileOpen, ESP.getMaxAllocHeap());

  // Seek to the current page start via the offset LUT (same nav as
  // loadPageFromSectionFile).
  if (!file.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 4)) {
    file.close();
    return false;
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(file, lutOffset) ||
      !file.seek(lutOffset + sizeof(uint32_t) * currentPage)) {
    file.close();
    return false;
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(file, pagePos) || !file.seek(pagePos)) {
    file.close();
    return false;
  }
  LOG_INF("RPSD", "phase=seeked maxAlloc=%u", ESP.getMaxAllocHeap());

  const uint32_t entryMaxAlloc = ESP.getMaxAllocHeap();

  // v18.9.9.13 Optimization B: two-pass streamed render with prewarm.
  // Pass 1 walks all elements under a FontCacheManager PrewarmScope --
  // the scope intercepts every draw call in GfxRenderer and instead
  // records the glyph codepoints into a scan buffer. endScanAndPrewarm
  // then batch-loads all needed glyphs from the font file in one big
  // read, eliminating the per-glyph SD miss that dominates the
  // single-pass render. Pass 2 seeks back to page start and does the
  // actual draw with glyphs already resident. Double the section-file
  // I/O for a ~2x saving on font-file I/O -- the font file is the
  // bottleneck.
  //
  // Extracted per-element loop into a lambda so we can drive it twice
  // over the same file. streamedRenderElements reads count then
  // iterates -- caller is responsible for having already seeked to
  // pagePos.
  const auto streamedRenderElements = [&](const char* passLabel) -> bool {
    uint16_t count;
    if (!serialization::tryReadPod(file, count)) {
      return false;
    }
    // v18.9.9.156: per-element maxAlloc logging, gated on low heap. Fires
    // only when maxAlloc < 5000 (post-BT tight-heap scenario). Names which
    // element and tag causes the ~784 B contiguous heap drop we've been
    // hunting.
    const bool tightHeap = ESP.getMaxAllocHeap() < 5000;
    if (tightHeap) {
      LOG_INF("RPSD", "iter=%s start count=%u maxAlloc=%u", passLabel, count, ESP.getMaxAllocHeap());
    }
    for (uint16_t i = 0; i < count; i++) {
      uint8_t tag;
      if (!serialization::tryReadPod(file, tag)) {
        LOG_ERR("SCT", "renderPageStreamed: truncated tag at element %u", i);
        return false;
      }
      const uint32_t maxAllocBefore = tightHeap ? ESP.getMaxAllocHeap() : 0;
      // Per-element scope: deserialize -> render -> drop. The unique_ptr
      // destructs at the end of the switch block, returning all bytes
      // (compact block, shared_ptr ctrl block, wrapper struct) before
      // the next element's deserialize allocates. Peak = one element.
      switch (tag) {
        case TAG_PageLine: {
          auto pl = PageLine::deserialize(file);
          if (pl) pl->render(renderer, fontId, xOffset, yOffset, foregroundBlack);
          break;
        }
        case TAG_PageImage: {
          auto pi = PageImage::deserialize(file);
          // v18.9.9.11: skip image render under BT (whole-DOM path OOMs on
          // JPEG decoder).
          // v18.9.9.57: when the caller vouches for pxcImagesSafe -- prebake
          // fingerprint match + .pxc manifest loaded -- blit from cache
          // instead. Peak per-image ~64 B row buffer, no decoder fallback.
          if (pi) {
            if (pxcImagesSafe) {
              pi->renderIfCached(renderer, xOffset, yOffset);
            } else if (!btLinked) {
              pi->render(renderer, fontId, xOffset, yOffset, foregroundBlack);
            }
          }
          break;
        }
        case TAG_PageTableFragment: {
          // v18.9.9.19: v42+ files carry a payloadSize prefix here. Consume
          // it and pass through to the normal deserialize -- v18.9.9.20 will
          // use the size to file.seek() past the fragment under btLinked
          // instead of deserialising it, avoiding the cell-tree allocation
          // peak that OOMs post-BT.
          if (fileVersion_ >= 42) {
            uint32_t payloadSize;
            if (!serialization::tryReadPod(file, payloadSize)) {
              LOG_ERR("SCT", "renderPageStreamed: truncated table size prefix at element %u", i);
              return false;
            }
            (void)payloadSize;
          }
          auto pt = PageTableFragment::deserialize(file);
          if (pt) {
            if (btLinked) {
              pt->renderContentOnly(renderer, fontId, xOffset, yOffset, foregroundBlack);
            } else {
              pt->render(renderer, fontId, xOffset, yOffset, foregroundBlack);
            }
          }
          break;
        }
        case TAG_PageHorizontalRule: {
          auto ph = PageHorizontalRule::deserialize(file);
          if (ph) ph->render(renderer, fontId, xOffset, yOffset, foregroundBlack);
          break;
        }
        default:
          LOG_ERR("SCT", "renderPageStreamed: unknown tag %u at element %u", tag, i);
          return false;
      }
      if (tightHeap) {
        const uint32_t maxAllocAfter = ESP.getMaxAllocHeap();
        if (maxAllocAfter != maxAllocBefore) {
          LOG_INF("RPSD", "iter=%s i=%u tag=%u before=%u after=%u delta=%d",
                  passLabel, i, tag, maxAllocBefore, maxAllocAfter,
                  (int)maxAllocAfter - (int)maxAllocBefore);
        }
      }
    }
    return true;
  };

  FontCacheManager* fcm = renderer.getFontCacheManager();
  LOG_INF("RPSD", "phase=got-fcm (fcm=%d) maxAlloc=%u", (int)(fcm != nullptr), ESP.getMaxAllocHeap());

  // Pass 1: scan pass under prewarm scope. Draws are intercepted as
  // codepoint recording. Skip if no font cache manager attached (host
  // sim, tests) -- fall through to single-pass.
  if (fcm) {
    auto scope = fcm->createPrewarmScope();
    LOG_INF("RPSD", "phase=scope-created maxAlloc=%u", ESP.getMaxAllocHeap());
    if (!streamedRenderElements("pass1")) {
      return false;
    }
    LOG_INF("RPSD", "phase=pass1-done maxAlloc=%u", ESP.getMaxAllocHeap());
    scope.endScanAndPrewarm();
    LOG_INF("RPSD", "phase=prewarmed maxAlloc=%u", ESP.getMaxAllocHeap());

    // Rewind to page start for Pass 2 (real render).
    if (!file.seek(pagePos)) {
      return false;
    }
    if (!streamedRenderElements("pass2")) {
      return false;
    }
    LOG_INF("RPSD", "phase=pass2-done maxAlloc=%u", ESP.getMaxAllocHeap());
    // scope destructs here -> cache clears, matching renderContents.
  } else {
    if (!streamedRenderElements("solo")) {
      return false;
    }
  }

  // Footnotes trailer exists in the file (loadPageFromSectionFile reads
  // it into page->footnotes) but the streamed render path deliberately
  // skips it -- footnote overlay under BT is a future add.
  // v18.9.9.13: no file.close() here -- keep the handle open for the
  // next renderPageStreamed (Optimization A). It'll be reused via the
  // isOpen() check at entry. Section teardown or a non-streamed path
  // that re-opens `file` will replace it, which closes this handle
  // implicitly via HalFile assignment.
  LOG_DBG("SCT", "renderPageStreamed done: entry maxAlloc=%u exit maxAlloc=%u", entryMaxAlloc,
          ESP.getMaxAllocHeap());
  return true;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", activeFilePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(f, key) || !serialization::tryReadPod(f, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", activeFilePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::tryReadPod(f, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", activeFilePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", activeFilePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE_V38 - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek(HEADER_SIZE_V38 - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(f, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

// CrumBLE 4.3: read the embedded glyph subset block at
// embeddedGlyphSubsetOffset_ and populate embeddedStyles_. Block layout
// is fully owned by EmbeddedGlyphSubset.h (BlockHeader + per-style
// StyleHeader + intervals + glyphs + bitmaps); we validate the magic +
// cpfontContentHash here before allocating any of the per-style vectors.
//
// On any failure (no block, hash mismatch, truncated read, etc.),
// embeddedSubsetInstalled_ stays false and the renderer's lookup
// fast-path falls through to the existing SdCardFont miss-handler.
bool Section::tryInstallEmbeddedGlyphSubset(uint32_t cpfontContentHash) {
  SET_CHECKPOINT("section:glyph-atlas-install");
  embeddedSubsetInstalled_ = false;
  for (auto& slot : embeddedStyles_) {
    slot.styleId = 0xFF;
    slot.intervals.clear();
    slot.glyphs.clear();
    slot.bitmap.clear();
    slot.kernLeftClasses.clear();
    slot.kernRightClasses.clear();
    slot.kernMatrix.clear();
    slot.kernLeftClassCount = 0;
    slot.kernRightClassCount = 0;
    slot.ligaturePairs.clear();
    slot.fontData = EpdFontData{};
  }
  if (!hasEmbeddedGlyphSubset()) return false;
  if (embeddedGlyphSubsetCpfontHash_ != cpfontContentHash) {
    LOG_INF("SCT",
            "Embedded glyph subset hash mismatch: section baked against 0x%08x, loaded SD font is 0x%08x -- falling "
            "back to miss-handler",
            embeddedGlyphSubsetCpfontHash_, cpfontContentHash);
    return false;
  }
  if (!Storage.openFileForRead("SCT", activeFilePath, file)) {
    LOG_ERR("SCT", "Embedded glyph subset install: cannot open %s for read", activeFilePath.c_str());
    return false;
  }
  if (!file.seek(embeddedGlyphSubsetOffset_)) {
    LOG_ERR("SCT", "Embedded glyph subset install: seek to offset %u failed", embeddedGlyphSubsetOffset_);
    file.close();
    return false;
  }
  embeddedGlyphSubset::BlockHeader hdr{};
  static_assert(sizeof(hdr) == 16, "EmbeddedGlyphSubset BlockHeader size drift");
  if (file.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) != static_cast<int>(sizeof(hdr))) {
    LOG_ERR("SCT", "Embedded glyph subset install: truncated BlockHeader read");
    file.close();
    return false;
  }
  if (hdr.magic != embeddedGlyphSubset::BLOCK_MAGIC) {
    LOG_ERR("SCT", "Embedded glyph subset install: bad block magic 0x%08x (expected 0x%08x)", hdr.magic,
            embeddedGlyphSubset::BLOCK_MAGIC);
    file.close();
    return false;
  }
  if (hdr.version != embeddedGlyphSubset::BLOCK_VERSION) {
    LOG_INF("SCT", "Embedded glyph subset install: block version %u, runtime supports %u -- skipping",
            static_cast<unsigned>(hdr.version), static_cast<unsigned>(embeddedGlyphSubset::BLOCK_VERSION));
    file.close();
    return false;
  }
  if (hdr.cpfontContentHash != cpfontContentHash) {
    LOG_ERR("SCT", "Embedded glyph subset install: in-block cpfontHash 0x%08x != caller's 0x%08x", hdr.cpfontContentHash,
            cpfontContentHash);
    file.close();
    return false;
  }
  uint8_t populated = 0;
  for (uint8_t i = 0; i < hdr.styleCount; ++i) {
    embeddedGlyphSubset::StyleHeader sh{};
    static_assert(sizeof(sh) == 32, "EmbeddedGlyphSubset StyleHeader v2 size drift");
    if (file.read(reinterpret_cast<uint8_t*>(&sh), sizeof(sh)) != static_cast<int>(sizeof(sh))) {
      LOG_ERR("SCT", "Embedded glyph subset install: truncated StyleHeader at style %u", static_cast<unsigned>(i));
      file.close();
      return false;
    }
    if (sh.styleId >= embeddedStyles_.size()) {
      LOG_ERR("SCT", "Embedded glyph subset install: invalid styleId %u", static_cast<unsigned>(sh.styleId));
      file.close();
      return false;
    }
    // CrumBLE 4.3: preflight + alloc-order fix. The bitmap is the largest
    // single contiguous block (~5 KB). Under post-NimBLE fragmentation it
    // fails first. We allocate the bitmap FIRST so it gets the freshest
    // MaxAlloc; intervals/glyphs are small enough to fit afterward in the
    // fragmented remainder. Preflight checks the bitmap size + 512 bytes
    // margin (allocator overhead) -- earlier 2 KB margin was for the kerning
    // blobs that v2 baked, but the kerning data is now skipped by the
    // prebake CLI (search "EXPERIMENTAL: zero these"). Returning false
    // leaves the slot empty -- caller threads nullptr to the renderer
    // routing and per-glyph lookups fall through to SdCardFont onGlyphMiss.
    const uint32_t needed = sh.bitmapDataSize + 512;
    if (ESP.getMaxAllocHeap() < needed) {
      LOG_INF("SCT",
              "Embedded glyph subset install: skipping style %u under heap pressure (need ~%u bytes, maxAlloc=%u)",
              static_cast<unsigned>(sh.styleId), needed, ESP.getMaxAllocHeap());
      file.close();
      return false;
    }
    EmbeddedStyleSlot& slot = embeddedStyles_[sh.styleId];
    slot.styleId = sh.styleId;
    slot.flags = sh.flags;
    // Allocate bitmap first (largest). intervals/glyphs are smaller and
    // can land in whatever fragments remain.
    slot.bitmap.resize(sh.bitmapDataSize);
    slot.intervals.resize(sh.intervalCount);
    slot.glyphs.resize(sh.glyphCount);
    if (sh.intervalCount > 0) {
      const size_t bytes = sh.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(slot.intervals.data()), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated intervals at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    if (sh.glyphCount > 0) {
      const size_t bytes = sh.glyphCount * sizeof(EpdGlyph);
      if (file.read(reinterpret_cast<uint8_t*>(slot.glyphs.data()), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated glyphs at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    if (sh.bitmapDataSize > 0) {
      if (file.read(slot.bitmap.data(), sh.bitmapDataSize) != static_cast<int>(sh.bitmapDataSize)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated bitmap at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    // v2 additions: kerning (left entries, right entries, matrix) + ligatures.
    slot.kernLeftClassCount = sh.kernLeftClassCount;
    slot.kernRightClassCount = sh.kernRightClassCount;
    slot.kernLeftClasses.resize(sh.kernLeftEntryCount);
    slot.kernRightClasses.resize(sh.kernRightEntryCount);
    slot.kernMatrix.resize(static_cast<size_t>(sh.kernLeftClassCount) * sh.kernRightClassCount);
    slot.ligaturePairs.resize(sh.ligaturePairCount);
    if (sh.kernLeftEntryCount > 0) {
      const size_t bytes = sh.kernLeftEntryCount * sizeof(EpdKernClassEntry);
      if (file.read(reinterpret_cast<uint8_t*>(slot.kernLeftClasses.data()), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated kernLeft at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    if (sh.kernRightEntryCount > 0) {
      const size_t bytes = sh.kernRightEntryCount * sizeof(EpdKernClassEntry);
      if (file.read(reinterpret_cast<uint8_t*>(slot.kernRightClasses.data()), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated kernRight at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    if (!slot.kernMatrix.empty()) {
      const size_t bytes = slot.kernMatrix.size();
      if (file.read(reinterpret_cast<uint8_t*>(slot.kernMatrix.data()), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated kernMatrix at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    if (sh.ligaturePairCount > 0) {
      const size_t bytes = sh.ligaturePairCount * sizeof(EpdLigaturePair);
      if (file.read(reinterpret_cast<uint8_t*>(slot.ligaturePairs.data()), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Embedded glyph subset install: truncated ligaturePairs at style %u",
                static_cast<unsigned>(sh.styleId));
        file.close();
        return false;
      }
    }
    slot.fontData.advanceY = sh.advanceY;
    slot.fontData.ascender = sh.ascender;
    slot.fontData.descender = sh.descender;
    slot.fontData.is2Bit = (sh.flags & embeddedGlyphSubset::STYLE_FLAG_IS_2BIT) != 0;
    slot.fontData.intervalCount = sh.intervalCount;
    slot.fontData.kernLeftEntryCount = sh.kernLeftEntryCount;
    slot.fontData.kernRightEntryCount = sh.kernRightEntryCount;
    slot.fontData.kernLeftClassCount = sh.kernLeftClassCount;
    slot.fontData.kernRightClassCount = sh.kernRightClassCount;
    slot.fontData.ligaturePairCount = sh.ligaturePairCount;
    populated++;
  }
  file.close();
  if (populated == 0) {
    LOG_INF("SCT", "Embedded glyph subset install: block was empty (styleCount=0)");
    return false;
  }
  patchEmbeddedFontDataPointers();
  embeddedSubsetInstalled_ = true;
  LOG_INF("SCT", "Embedded glyph subset installed: %u style(s), cpfontHash=0x%08x", static_cast<unsigned>(populated),
          cpfontContentHash);
  return true;
}

void Section::patchEmbeddedFontDataPointers() {
  // Re-point each populated slot's EpdFontData at its std::vector storage.
  // Called once after install; the vectors don't move afterwards because
  // Section never resizes them again.
  for (auto& slot : embeddedStyles_) {
    if (slot.styleId == 0xFF) continue;
    slot.fontData.intervals = slot.intervals.empty() ? nullptr : slot.intervals.data();
    slot.fontData.glyph = slot.glyphs.empty() ? nullptr : slot.glyphs.data();
    slot.fontData.bitmap = slot.bitmap.empty() ? nullptr : slot.bitmap.data();
    // v2: kerning + ligatures embedded. Re-point the fontData kerning fields
    // at the slot's std::vector storage. Counts on fontData were set at
    // install time; we just re-point the data pointers here.
    slot.fontData.kernLeftClasses = slot.kernLeftClasses.empty() ? nullptr : slot.kernLeftClasses.data();
    slot.fontData.kernRightClasses = slot.kernRightClasses.empty() ? nullptr : slot.kernRightClasses.data();
    slot.fontData.kernMatrix = slot.kernMatrix.empty() ? nullptr : slot.kernMatrix.data();
    slot.fontData.ligaturePairs = slot.ligaturePairs.empty() ? nullptr : slot.ligaturePairs.data();
    slot.fontData.ligaturePairs = nullptr;
    slot.fontData.kernLeftEntryCount = 0;
    slot.fontData.kernRightEntryCount = 0;
    slot.fontData.kernLeftClassCount = 0;
    slot.fontData.kernRightClassCount = 0;
    slot.fontData.ligaturePairCount = 0;
    // Embedded subset is fully resident in RAM; no miss handler needed
    // for the covered codepoints. EpdFontFamily's section-aware router
    // (task #17) consults the embedded subset first and falls back to
    // the SD-font miss handler for codepoints NOT in the embedded set.
    slot.fontData.glyphMissHandler = nullptr;
    slot.fontData.glyphMissCtx = nullptr;
    // Uncompressed -- no FontDecompressor groups.
    slot.fontData.groups = nullptr;
    slot.fontData.groupCount = 0;
    slot.fontData.glyphToGroup = nullptr;
  }
}

void Section::dropEmbeddedGlyphSubset() {
  if (!embeddedSubsetInstalled_) return;
  for (auto& slot : embeddedStyles_) {
    slot.styleId = 0xFF;
    slot.flags = 0;
    // swap-with-empty guarantees vector capacity is freed (clear keeps it).
    std::vector<EpdUnicodeInterval>().swap(slot.intervals);
    std::vector<EpdGlyph>().swap(slot.glyphs);
    std::vector<uint8_t>().swap(slot.bitmap);
    std::vector<EpdKernClassEntry>().swap(slot.kernLeftClasses);
    std::vector<EpdKernClassEntry>().swap(slot.kernRightClasses);
    std::vector<int8_t>().swap(slot.kernMatrix);
    slot.kernLeftClassCount = 0;
    slot.kernRightClassCount = 0;
    std::vector<EpdLigaturePair>().swap(slot.ligaturePairs);
    slot.fontData = EpdFontData{};
  }
  embeddedSubsetInstalled_ = false;
}

const EpdFontData* Section::embeddedFontDataForStyle(uint8_t styleId) const {
  if (!embeddedSubsetInstalled_) return nullptr;
  if (styleId >= embeddedStyles_.size()) return nullptr;
  const auto& slot = embeddedStyles_[styleId];
  if (slot.styleId == 0xFF) return nullptr;
  return &slot.fontData;
}

// CrumBLE 4.4 task #35 step 4: read the v40 glyph atlas block from the
// section file into per-style slots + a shared bitmap buffer. The block
// format (see lib/Epub/Epub/GlyphAtlas.h) is:
//
//   BlockHeader { magic, version, bitDepth, styleMask, reserved,
//                 totalGlyphs, bitmapBytes }
//   For each style bit set in styleMask (low-to-high):
//     StyleHeader { styleId, reserved, glyphCount,
//                   ascender, descender, lineHeight, spaceWidth }
//     GlyphEntry[glyphCount]
//   uint8_t bitmapPayload[bitmapBytes]
//
// The GlyphEntry::bitmapOffset values are byte offsets into the shared
// payload that comes last; this loader reads them in order and resolves
// pointers when callers ask for them via glyphAtlasBitmapPtr().
bool Section::tryInstallGlyphAtlas(uint32_t cpfontContentHash, bool preferLowBitDepth) {
  // Clear any prior install state -- both successful (re-install) and
  // failed (partial) paths.
  glyphAtlasInstalled_ = false;
  for (auto& slot : glyphAtlasSlots_) {
    slot.styleId = 0xFF;
    slot.ascender = 0;
    slot.descender = 0;
    slot.lineHeight = 0;
    slot.spaceWidth = 0;
    std::vector<glyphatlas::GlyphEntry>().swap(slot.entries);
  }
  std::vector<uint8_t>().swap(glyphAtlasBitmap_);
  std::vector<uint8_t>().swap(glyphAtlasStreamScratch_);
  glyphAtlasStreaming_ = false;
  glyphAtlasStreamBitmapBase_ = 0;
  glyphAtlasBitDepth_ = 0;

  // CrumBLE 4.4 v41: dual-slot atlas selection. Primary slot is the
  // BT-friendly bake (1-bit on v41; can be either depth on legacy v40);
  // alt slot is the BT-cold upgrade (2-bit on v41 ≥16pt bakes; empty
  // otherwise). preferLowBitDepth comes from the caller's BT-enabled
  // check -- when true we install primary; when false (BT cold) we
  // prefer alt and fall through to primary if alt is empty.
  uint32_t chosenOffset = 0;
  uint32_t chosenSize = 0;
  uint32_t chosenHash = 0;
  const char* chosenLabel = "(none)";
  if (!preferLowBitDepth && hasGlyphAtlasAlt()) {
    chosenOffset = glyphAtlasAltOffset_;
    chosenSize = glyphAtlasAltSize_;
    chosenHash = glyphAtlasAltCpfontHash_;
    chosenLabel = "alt (2-bit, BT-cold)";
  } else if (hasGlyphAtlas()) {
    chosenOffset = glyphAtlasOffset_;
    chosenSize = glyphAtlasSize_;
    chosenHash = glyphAtlasCpfontHash_;
    chosenLabel = preferLowBitDepth ? "primary (BT-enabled)" : "primary (no alt available)";
  } else {
    return false;
  }
  if (chosenHash != cpfontContentHash) {
    LOG_INF("SCT",
            "Glyph atlas hash mismatch on %s slot: section baked against 0x%08x, loaded SD font is 0x%08x -- "
            "falling back to v39 subset / miss-handler",
            chosenLabel, chosenHash, cpfontContentHash);
    return false;
  }
  if (!Storage.openFileForRead("SCT", activeFilePath, file)) {
    LOG_ERR("SCT", "Glyph atlas install: cannot open %s for read", activeFilePath.c_str());
    return false;
  }
  if (!file.seek(chosenOffset)) {
    LOG_ERR("SCT", "Glyph atlas install: seek to %s offset %u failed", chosenLabel, chosenOffset);
    file.close();
    return false;
  }
  (void)chosenSize;  // currently unused -- size validation lives inside BlockHeader read

  // CrumBLE 4.5.5: dual-version BlockHeader read. v1 (12 B) is the legacy
  // uint16-bound layout; v2 (14 B) widens bitmapBytes to uint32_t. We
  // peek the version byte after the magic to pick the right struct, then
  // fan out to either parse path. The runtime hdr struct that drives
  // everything below is the v2 layout (uint32 bitmapBytes) so old v1
  // values just zero-extend.
  glyphatlas::BlockHeader hdr{};
  // Read enough to determine version: magic (4) + version (1) + bitDepth
  // (1) + styleMask (1) + reserved (1) + totalGlyphs (2) = 10 bytes,
  // which is the common prefix between v1 and v2.
  uint8_t prefix[10];
  if (file.read(prefix, sizeof(prefix)) != static_cast<int>(sizeof(prefix))) {
    LOG_ERR("SCT", "Glyph atlas install: truncated BlockHeader prefix read");
    file.close();
    return false;
  }
  memcpy(&hdr.magic, prefix + 0, 4);
  hdr.version = prefix[4];
  hdr.bitDepth = prefix[5];
  hdr.styleMask = prefix[6];
  hdr.reserved = prefix[7];
  memcpy(&hdr.totalGlyphs, prefix + 8, 2);
  if (hdr.magic != glyphatlas::MAGIC) {
    LOG_ERR("SCT", "Glyph atlas install: bad block magic 0x%08x (expected 0x%08x)", hdr.magic, glyphatlas::MAGIC);
    file.close();
    return false;
  }
  const bool isV1 = (hdr.version == glyphatlas::FORMAT_VERSION_V1);
  const bool isV2 = (hdr.version == glyphatlas::FORMAT_VERSION_V2);
  if (!isV1 && !isV2) {
    LOG_INF("SCT", "Glyph atlas install: block version %u not recognised (supported: 1, 2) -- skipping",
            static_cast<unsigned>(hdr.version));
    file.close();
    return false;
  }
  // Tail of header: bitmapBytes. v1 = uint16_t, v2 = uint32_t.
  if (isV1) {
    uint16_t bitmapBytes16 = 0;
    if (file.read(reinterpret_cast<uint8_t*>(&bitmapBytes16), sizeof(bitmapBytes16)) !=
        static_cast<int>(sizeof(bitmapBytes16))) {
      LOG_ERR("SCT", "Glyph atlas install: truncated v1 BlockHeader bitmapBytes read");
      file.close();
      return false;
    }
    hdr.bitmapBytes = static_cast<uint32_t>(bitmapBytes16);
  } else {
    if (file.read(reinterpret_cast<uint8_t*>(&hdr.bitmapBytes), sizeof(hdr.bitmapBytes)) !=
        static_cast<int>(sizeof(hdr.bitmapBytes))) {
      LOG_ERR("SCT", "Glyph atlas install: truncated v2 BlockHeader bitmapBytes read");
      file.close();
      return false;
    }
  }
  if (hdr.bitDepth != glyphatlas::BIT_DEPTH_1 && hdr.bitDepth != glyphatlas::BIT_DEPTH_2) {
    LOG_ERR("SCT", "Glyph atlas install: unsupported bit depth %u", static_cast<unsigned>(hdr.bitDepth));
    file.close();
    return false;
  }

  // Pre-flight the bitmap allocation. The bitmap is the single biggest chunk
  // in the atlas (per-style headers + GlyphEntry arrays are small in
  // comparison). Two-tier install:
  //   1) If the full bitmap fits contiguously, install fully resident
  //      (fast path: zero render-time disk I/O, current behaviour).
  //   2) Else if metadata + scratch fits, install in STREAMING mode: keep
  //      intervals/glyph tables resident, fetch per-glyph bitmap bytes from
  //      disk on demand at render time.
  //   3) Else skip -- caller falls through to v39 subset / SD-font handler.
  // Same allocator-overhead margin (512) used by the v39 subset install path
  // so the heap-pressure behaviour stays consistent across font formats.
  constexpr uint32_t kStreamScratchBytes = 2048;  // largest single glyph
  const uint32_t bitmapNeed = static_cast<uint32_t>(hdr.bitmapBytes) + 512u;
  const uint32_t streamNeed = kStreamScratchBytes + 512u;
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const bool canFullInstall = (maxAlloc >= bitmapNeed);
  const bool canStreamInstall = (maxAlloc >= streamNeed);
  if (!canFullInstall && !canStreamInstall) {
    LOG_INF("SCT",
            "Glyph atlas install: skipping under heap pressure (bitmap needs ~%u bytes, "
            "stream scratch needs ~%u bytes, maxAlloc=%u)",
            bitmapNeed, streamNeed, maxAlloc);
    file.close();
    return false;
  }
  glyphAtlasStreaming_ = !canFullInstall;
  if (glyphAtlasStreaming_) {
    LOG_INF("SCT",
            "Glyph atlas install: full bitmap (%u B) exceeds maxAlloc (%u B); falling back to "
            "STREAMING mode (per-glyph SD reads, %u B scratch)",
            static_cast<unsigned>(hdr.bitmapBytes), maxAlloc, kStreamScratchBytes);
  }

  // Per-style header + entry tables come BEFORE the bitmap. We have to
  // read the styles first (the file position is right after the
  // BlockHeader), capture all entry data, then read the bitmap payload
  // at the end.
  uint8_t populated = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    if ((hdr.styleMask & (1u << s)) == 0) continue;

    glyphatlas::StyleHeader sh{};
    static_assert(sizeof(sh) == 12, "GlyphAtlas StyleHeader size drift");
    if (file.read(reinterpret_cast<uint8_t*>(&sh), sizeof(sh)) != static_cast<int>(sizeof(sh))) {
      LOG_ERR("SCT", "Glyph atlas install: truncated StyleHeader at bit %u", static_cast<unsigned>(s));
      file.close();
      glyphAtlasInstalled_ = false;
      return false;
    }
    if (sh.styleId >= glyphAtlasSlots_.size()) {
      LOG_ERR("SCT", "Glyph atlas install: invalid styleId %u in style at bit %u",
              static_cast<unsigned>(sh.styleId), static_cast<unsigned>(s));
      file.close();
      return false;
    }
    GlyphAtlasSlot& slot = glyphAtlasSlots_[sh.styleId];
    slot.styleId = sh.styleId;
    slot.ascender = sh.ascender;
    slot.descender = sh.descender;
    slot.lineHeight = sh.lineHeight;
    slot.spaceWidth = sh.spaceWidth;
    slot.entries.resize(sh.glyphCount);
    if (sh.glyphCount > 0) {
      if (isV1) {
        // v1 wire entries are 12 B with uint16 bitmapOffset. Read into a
        // temp buffer of GlyphEntryV1[] then widen into the runtime
        // (v2-shaped) slot.entries[]. Pre-2024 prebakes go through here.
        std::vector<glyphatlas::GlyphEntryV1> v1entries(sh.glyphCount);
        const size_t v1bytes = static_cast<size_t>(sh.glyphCount) * sizeof(glyphatlas::GlyphEntryV1);
        if (file.read(reinterpret_cast<uint8_t*>(v1entries.data()), v1bytes) !=
            static_cast<int>(v1bytes)) {
          LOG_ERR("SCT", "Glyph atlas install: truncated v1 GlyphEntry table at style %u",
                  static_cast<unsigned>(sh.styleId));
          file.close();
          glyphAtlasInstalled_ = false;
          return false;
        }
        for (uint16_t i = 0; i < sh.glyphCount; ++i) {
          const auto& src = v1entries[i];
          glyphatlas::GlyphEntry& dst = slot.entries[i];
          dst.codepoint = src.codepoint;
          dst.bitmapOffset = static_cast<uint32_t>(src.bitmapOffset);  // zero-extend
          dst.width = src.width;
          dst.height = src.height;
          dst.left = src.left;
          dst.top = src.top;
          dst.advanceX = src.advanceX;
        }
      } else {
        // v2: 14 B wire entries match the runtime layout 1:1, slurp
        // straight into the vector.
        const size_t entriesBytes = static_cast<size_t>(sh.glyphCount) * sizeof(glyphatlas::GlyphEntry);
        if (file.read(reinterpret_cast<uint8_t*>(slot.entries.data()), entriesBytes) !=
            static_cast<int>(entriesBytes)) {
          LOG_ERR("SCT", "Glyph atlas install: truncated v2 GlyphEntry table at style %u",
                  static_cast<unsigned>(sh.styleId));
          file.close();
          glyphAtlasInstalled_ = false;
          return false;
        }
      }
    }
    ++populated;
  }

  // Now read the shared bitmap payload, which sits right after the last
  // style's entry table -- OR, in streaming mode, just record the file
  // offset so streamingAtlasFetch() can seek+read per-glyph at render time.
  if (glyphAtlasStreaming_) {
    glyphAtlasStreamBitmapBase_ = file.position();
    glyphAtlasStreamScratch_.assign(kStreamScratchBytes, 0);
    // Don't read the bitmap. file.close() below; subsequent fetches re-open
    // activeFilePath on demand (cheap on warm SD, kept simple to avoid
    // managing a long-lived file handle on a per-section basis).
  } else {
    glyphAtlasBitmap_.resize(hdr.bitmapBytes);
    if (hdr.bitmapBytes > 0) {
      if (file.read(glyphAtlasBitmap_.data(), hdr.bitmapBytes) != static_cast<int>(hdr.bitmapBytes)) {
        LOG_ERR("SCT", "Glyph atlas install: truncated bitmap payload (expected %u bytes)",
                static_cast<unsigned>(hdr.bitmapBytes));
        file.close();
        glyphAtlasInstalled_ = false;
        std::vector<uint8_t>().swap(glyphAtlasBitmap_);
        return false;
      }
    }
  }
  file.close();

  glyphAtlasBitDepth_ = hdr.bitDepth;

  // Step 5: synthesize EpdFontData per populated style so the existing
  // renderer (which speaks EpdFontData/EpdGlyph/EpdUnicodeInterval) can
  // consume atlas glyphs through the same setEmbeddedGlyphData slot the
  // v39 subset uses. Without this step the atlas would sit in RAM
  // unread by the draw path.
  for (auto& slot : glyphAtlasSlots_) {
    if (slot.styleId == 0xFF) continue;
    synthesizeAtlasFontData(slot);
  }

  glyphAtlasInstalled_ = (populated > 0);
  LOG_INF("SCT",
          "Glyph atlas installed: %u style(s), %u glyphs, %u-bit bitmap (%u bytes, mode=%s)",
          static_cast<unsigned>(populated), static_cast<unsigned>(hdr.totalGlyphs),
          static_cast<unsigned>(glyphAtlasBitDepth_), static_cast<unsigned>(hdr.bitmapBytes),
          glyphAtlasStreaming_ ? "streaming" : "resident");
  return glyphAtlasInstalled_;
}

void Section::synthesizeAtlasFontData(GlyphAtlasSlot& slot) {
  // Build the EpdGlyph[] in the same order as the GlyphEntry[]. For each
  // glyph, dataOffset is the atlas bitmap offset; dataLength is the
  // packed glyph byte count (rowBytes * height).
  slot.synthesizedGlyphs.clear();
  slot.synthesizedGlyphs.reserve(slot.entries.size());
  for (const auto& e : slot.entries) {
    EpdGlyph g{};
    g.width = e.width;
    g.height = e.height;
    g.advanceX = e.advanceX;
    g.left = static_cast<int16_t>(e.left);
    g.top = static_cast<int16_t>(e.top);
    g.dataOffset = e.bitmapOffset;
    g.dataLength = glyphatlas::glyphBytes(e.width, e.height, glyphAtlasBitDepth_);
    slot.synthesizedGlyphs.push_back(g);
  }

  // Build EpdUnicodeInterval[] by collapsing runs of consecutive
  // codepoints. The renderer uses intervals to map codepoint -> glyph
  // index via binary search; one interval per gap in the codepoint
  // sequence is optimal.
  slot.synthesizedIntervals.clear();
  if (!slot.entries.empty()) {
    EpdUnicodeInterval current{};
    current.first = slot.entries.front().codepoint;
    current.last = slot.entries.front().codepoint;
    current.offset = 0;
    for (size_t i = 1; i < slot.entries.size(); ++i) {
      const uint32_t cp = slot.entries[i].codepoint;
      if (cp == current.last + 1) {
        current.last = cp;
      } else {
        slot.synthesizedIntervals.push_back(current);
        current.first = cp;
        current.last = cp;
        current.offset = static_cast<uint32_t>(i);
      }
    }
    slot.synthesizedIntervals.push_back(current);
  }

  // Assemble the EpdFontData with pointers into the just-built vectors
  // and the section's shared bitmap buffer. is2Bit = (bitDepth == 2);
  // glyphMissHandler is null because atlas misses fall through to the
  // EpdFontFamily-level chain (v39 subset, then SD-font miss handler).
  //
  // Streaming mode (CrumBLE 4.5.5): the shared bitmap is NOT resident;
  // bitmap=nullptr and glyphBitmapFetch is wired so the renderer reads
  // each glyph's bytes on demand into glyphAtlasStreamScratch_.
  slot.fontData = EpdFontData{};
  slot.fontData.bitmap = glyphAtlasStreaming_ ? nullptr : glyphAtlasBitmap_.data();
  slot.fontData.glyph = slot.synthesizedGlyphs.data();
  slot.fontData.intervals = slot.synthesizedIntervals.data();
  slot.fontData.intervalCount = static_cast<uint32_t>(slot.synthesizedIntervals.size());
  slot.fontData.advanceY = static_cast<uint8_t>(slot.lineHeight);
  slot.fontData.ascender = slot.ascender;
  slot.fontData.descender = slot.descender;
  slot.fontData.is2Bit = (glyphAtlasBitDepth_ == glyphatlas::BIT_DEPTH_2);
  // Streaming mode wires the bitmap-fetch callback so GfxRenderer::getGlyphBitmap
  // routes each glyph through Section::streamingAtlasFetch (re-opens the
  // section file, seeks to glyphAtlasStreamBitmapBase_ + dataOffset, reads
  // dataLength bytes into glyphAtlasStreamScratch_). Resident mode leaves
  // these null so the renderer takes the fast direct-deref path.
  if (glyphAtlasStreaming_) {
    slot.fontData.glyphBitmapFetch = &Section::streamingAtlasFetch;
    slot.fontData.glyphBitmapCtx = this;
  }
  // groups / glyphToGroup / glyphMissHandler / kerning / ligatures all
  // stay null: atlas is a flat lookup table, no compression groups,
  // no kerning baked in (CrumBLE 4.3 dropped kerning from the embedded
  // subset to save bytes -- atlas follows the same policy).
}

const uint8_t* Section::streamingAtlasFetch(void* ctx, const EpdGlyph* glyph) {
  if (!ctx || !glyph) return nullptr;
  return static_cast<Section*>(ctx)->streamingAtlasFetchImpl(glyph);
}

const uint8_t* Section::streamingAtlasFetchImpl(const EpdGlyph* glyph) {
  // Zero-byte glyphs (e.g. SPACE) have no bitmap; render path handles
  // nullptr by drawing nothing for them.
  if (glyph->dataLength == 0) return nullptr;
  if (glyph->dataLength > glyphAtlasStreamScratch_.size()) {
    // Defensive: a glyph wider than the scratch budget shouldn't happen at
    // 14pt CJK (max ~250 B at 2-bit), but bail rather than overrun if a
    // future build raises point size or bit depth.
    LOG_ERR("SCT", "Streaming atlas fetch: glyph dataLength %u exceeds scratch %u",
            static_cast<unsigned>(glyph->dataLength),
            static_cast<unsigned>(glyphAtlasStreamScratch_.size()));
    return nullptr;
  }
  FsFile f;
  if (!Storage.openFileForRead("SCT", activeFilePath, f)) {
    return nullptr;
  }
  const uint32_t pos = glyphAtlasStreamBitmapBase_ + glyph->dataOffset;
  if (!f.seek(pos)) {
    f.close();
    return nullptr;
  }
  if (f.read(glyphAtlasStreamScratch_.data(), glyph->dataLength) !=
      static_cast<int>(glyph->dataLength)) {
    f.close();
    return nullptr;
  }
  f.close();
  return glyphAtlasStreamScratch_.data();
}

const EpdFontData* Section::glyphAtlasFontDataForStyle(uint8_t styleId) const {
  if (!glyphAtlasInstalled_) return nullptr;
  if (styleId >= glyphAtlasSlots_.size()) return nullptr;
  const auto& slot = glyphAtlasSlots_[styleId];
  if (slot.styleId == 0xFF || slot.synthesizedGlyphs.empty()) return nullptr;
  return &slot.fontData;
}

void Section::dropGlyphAtlas() {
  if (!glyphAtlasInstalled_) return;
  for (auto& slot : glyphAtlasSlots_) {
    slot.styleId = 0xFF;
    slot.ascender = 0;
    slot.descender = 0;
    slot.lineHeight = 0;
    slot.spaceWidth = 0;
    // swap-with-empty guarantees vector capacity is freed (clear keeps it).
    std::vector<glyphatlas::GlyphEntry>().swap(slot.entries);
    std::vector<EpdGlyph>().swap(slot.synthesizedGlyphs);
    std::vector<EpdUnicodeInterval>().swap(slot.synthesizedIntervals);
    slot.fontData = EpdFontData{};
  }
  std::vector<uint8_t>().swap(glyphAtlasBitmap_);
  std::vector<uint8_t>().swap(glyphAtlasStreamScratch_);
  glyphAtlasStreaming_ = false;
  glyphAtlasStreamBitmapBase_ = 0;
  glyphAtlasBitDepth_ = 0;
  glyphAtlasInstalled_ = false;
}

const glyphatlas::GlyphEntry* Section::lookupGlyphAtlasEntry(uint8_t styleId, uint32_t codepoint) const {
  if (!glyphAtlasInstalled_) return nullptr;
  if (styleId >= glyphAtlasSlots_.size()) return nullptr;
  const GlyphAtlasSlot& slot = glyphAtlasSlots_[styleId];
  if (slot.styleId == 0xFF || slot.entries.empty()) return nullptr;
  // Entries are sorted by codepoint ascending (prebake CLI emits in
  // interval order which is itself codepoint-ascending). Binary search.
  const auto* first = slot.entries.data();
  const auto* last = first + slot.entries.size();
  const auto it = std::lower_bound(first, last, codepoint,
                                   [](const glyphatlas::GlyphEntry& e, uint32_t cp) { return e.codepoint < cp; });
  if (it == last || it->codepoint != codepoint) return nullptr;
  return it;
}

