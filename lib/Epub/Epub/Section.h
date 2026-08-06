#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../EpdFont/EpdFontData.h"  // EpdFontData / EpdUnicodeInterval / EpdGlyph
#include "Epub.h"
#include "GlyphAtlas.h"  // CrumBLE 4.4: per-section glyph atlas types

class Page;
class GfxRenderer;
// v18.9.9.29 (v20 Phase C1): forward-declared so BuildContext can hold a
// unique_ptr without pulling the parser + CSS headers into every Section
// consumer. Full definitions live in the .cpp where startBuild constructs
// the parser and finalizeBuild tears it down.
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  // CrumBLE: read-only fallback path for prebake'd section files. The
  // optimizer/prebake CLI writes its sections here (sections-prebake/)
  // rather than into the regular sections/ slot so the live cache eviction
  // path (Section::clearCache, called on fingerprint mismatch) never eats
  // the prebake. If the user changes a reader setting, the live sections/
  // file gets eaten and rebuilt under the new fingerprint -- but
  // sections-prebake/ stays untouched, so the moment the user reverts
  // settings (via the device-side switch-back prompt), section loads can
  // fall back to the prebake artifact instead of rebuilding the chapter
  // from HTML again.
  std::string prebakeFilePath;
  // CrumBLE: which of filePath / prebakeFilePath actually contained the
  // most recently loaded section. tryLoadFromPath updates this on success.
  // All subsequent reads (loadPageFromSectionFile, getPageForAnchor,
  // getPageForParagraphIndex, etc.) MUST use this rather than filePath --
  // when a section was loaded from the prebake fallback, filePath
  // (sections/<n>.bin) doesn't exist on SD and re-opens fail.
  // Writes (clearCache, createSectionFile, rename) keep using filePath
  // since they target the live cache slot, never the prebake artifact.
  std::string activeFilePath;
  FsFile file;

  // CrumBLE 4.3: section file format version actually read from disk. Set
  // by tryLoadFromPath on success; 0 means "no section loaded yet". Used by
  // loadPageFromSectionFile and the embedded-glyph-subset accessors below
  // to know whether to expect v39's trailer fields. v38 sections always
  // report 0/0/0 for the subset triple.
  uint8_t fileVersion_ = 0;
  // CrumBLE 4.3: embedded glyph subset trailer (v39+). All three default to 0
  // and stay that way for v38 sections (no embedded subset) and for v39
  // sections written without a glyph block (device-built sections; prebakes
  // for built-in fonts where embedding adds no value). When non-zero the
  // block lives at embeddedGlyphSubsetOffset_ with byte size
  // embeddedGlyphSubsetSize_, and embeddedGlyphSubsetCpfontHash_ identifies
  // the .cpfont it was baked against -- runtime validates against
  // SdCardFont::contentHash() before installing the block, so a font swap
  // since bake falls back to the existing SD-font miss-handler path.
  uint32_t embeddedGlyphSubsetOffset_ = 0;
  uint32_t embeddedGlyphSubsetSize_ = 0;
  uint32_t embeddedGlyphSubsetCpfontHash_ = 0;

  // CrumBLE 4.4: v40 glyph atlas trailer fields. All zero on sections
  // that don't carry a pre-rendered atlas (legacy <v40 prebakes, device-
  // built sections, prebake CLI runs without --emit-section-glyph-atlas).
  // When non-zero, the atlas block lives at glyphAtlasOffset_ with byte
  // size glyphAtlasSize_, and glyphAtlasCpfontHash_ identifies the .cpfont
  // it was baked against -- runtime validates against SdCardFont's
  // contentHash() before installing, so a font swap since bake falls back
  // to the v39 embedded subset path or the SD-font miss handler.
  // See lib/Epub/Epub/GlyphAtlas.h for the block format.
  uint32_t glyphAtlasOffset_ = 0;
  uint32_t glyphAtlasSize_ = 0;
  uint32_t glyphAtlasCpfontHash_ = 0;

  // CrumBLE 4.4 v41: alternate glyph atlas slot. The primary slot above is
  // baked at the bit-depth chosen by --atlas-bit-depth or by the prebake's
  // auto-pick (1-bit for <16pt, 2-bit for ≥16pt). The alternate slot holds
  // the OTHER bit-depth for the same set of glyphs, so the reader can pick:
  //
  //   * BT cold        -> install the 2-bit atlas (better visual)
  //   * BT enabled     -> install the 1-bit atlas (smaller, fits tight heap)
  //
  // Either slot may be zero when the bake produced only one bit-depth (e.g.
  // small-size bake where 2-bit visual gain is not worth the section-file
  // bloat). The runtime install path falls back gracefully: prefer the slot
  // matching the BT-state preference, else use whichever slot is non-zero.
  // v40 files leave these at 0; their single atlas lives in the primary slot.
  uint32_t glyphAtlasAltOffset_ = 0;
  uint32_t glyphAtlasAltSize_ = 0;
  uint32_t glyphAtlasAltCpfontHash_ = 0;

  // CrumBLE 4.4: parsed in-RAM glyph atlas for one style. Populated by
  // tryInstallGlyphAtlas() when the section carries an atlas block and
  // its cpfontContentHash matches the active SdCardFont. The `entries`
  // vector is sorted by codepoint (the prebake CLI emits in interval
  // order which is codepoint-ascending), so the renderer can binary-
  // search by codepoint. styleId == 0xFF marks the slot as unused
  // (this style wasn't in the block's styleMask).
  struct GlyphAtlasSlot {
    uint8_t styleId = 0xFF;
    uint16_t ascender = 0;
    uint16_t descender = 0;
    uint16_t lineHeight = 0;
    uint16_t spaceWidth = 0;
    std::vector<glyphatlas::GlyphEntry> entries;

    // CrumBLE 4.4 step 5 renderer interop: synthesized representation
    // of the same atlas data in the EpdFontData/EpdGlyph/EpdUnicodeInterval
    // shape the existing renderer expects. Populated once at install
    // time so EpdFontFamily::setEmbeddedGlyphData can be reused unchanged
    // -- the renderer treats an atlas-backed style the same way it
    // treats a v39 embedded subset style, with the same 1-bit blit path
    // and the same per-glyph metadata lookups. Pointers inside fontData
    // point at the vectors here + the section's shared glyphAtlasBitmap_;
    // the entire structure is invalidated when dropGlyphAtlas() runs.
    std::vector<EpdGlyph> synthesizedGlyphs;
    std::vector<EpdUnicodeInterval> synthesizedIntervals;
    EpdFontData fontData{};
  };
  // Per-style slots indexed by styleId (REGULAR=0, BOLD=1, ITALIC=2,
  // BOLDITALIC=3). The bitmap payload is SHARED across all styles --
  // each GlyphEntry::bitmapOffset is a byte offset into the same
  // glyphAtlasBitmap_ buffer. This matches the prebake CLI's output
  // (single bitmap blob after all per-style headers) and saves on
  // per-style allocation overhead during install.
  std::array<GlyphAtlasSlot, 4> glyphAtlasSlots_;
  std::vector<uint8_t> glyphAtlasBitmap_;
  // Bit-depth of glyphAtlasBitmap_ payload. 1 today (atlas emits 1-bit
  // packed glyphs); 2 if a future builder produces 2-bit anti-aliased
  // glyphs. The renderer routes the bitmap through different blit paths
  // depending on this value.
  uint8_t glyphAtlasBitDepth_ = 0;
  bool glyphAtlasInstalled_ = false;

  // CrumBLE 4.5.5: hybrid streaming atlas. When the full atlas bitmap won't
  // fit contiguously in heap (e.g. CJK section with ~90 KB bitmap on a heap
  // fragmented to maxAlloc < 30 KB), tryInstallGlyphAtlas falls back to a
  // streaming install: all metadata stays resident (StyleHeaders + GlyphEntry
  // tables, ~3-6 KB total), and per-glyph bitmap bytes are read on demand
  // from the section file at render time. glyphAtlasStreaming_ flags this
  // mode; glyphAtlasStreamBitmapBase_ records the absolute file offset of
  // the bitmap payload (start of the shared blob after the last style's
  // entry table); glyphAtlasStreamScratch_ holds the most-recently-read
  // glyph's bytes, valid until the next streamingAtlasFetch() call.
  bool glyphAtlasStreaming_ = false;
  uint32_t glyphAtlasStreamBitmapBase_ = 0;
  std::vector<uint8_t> glyphAtlasStreamScratch_;
  // Callback wired into EpdFontData::glyphBitmapFetch for streamed slots.
  // Casts ctx back to Section* and dispatches to the instance method.
  static const uint8_t* streamingAtlasFetch(void* ctx, const EpdGlyph* glyph);
  const uint8_t* streamingAtlasFetchImpl(const EpdGlyph* glyph);

  // CrumBLE 4.3: parsed in-memory representation of one style's slice of the
  // embedded glyph subset block. Populated by tryInstallEmbeddedGlyphSubset()
  // when the block is present AND its cpfontHash matches the active
  // SdCardFont. The fontData member is a fully-populated EpdFontData whose
  // intervals / glyph / bitmap pointers reference the std::vectors below, so
  // it can be returned directly to the renderer. styleId == 0xFF marks the
  // slot as unused (this style wasn't in the block).
  struct EmbeddedStyleSlot {
    uint8_t styleId = 0xFF;     // 0/1/2/3 when populated; 0xFF = unused
    uint8_t flags = 0;          // bit 0: is2Bit
    std::vector<EpdUnicodeInterval> intervals;
    std::vector<EpdGlyph> glyphs;
    std::vector<uint8_t> bitmap;
    // CrumBLE 4.3 v2 embedded subset additions: kerning + ligatures.
    // patchEmbeddedFontDataPointers re-points the fontData kerning fields
    // at these vectors after install/move, mirroring the intervals/glyphs/
    // bitmap pointers.
    std::vector<EpdKernClassEntry> kernLeftClasses;
    std::vector<EpdKernClassEntry> kernRightClasses;
    std::vector<int8_t> kernMatrix;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    std::vector<EpdLigaturePair> ligaturePairs;
    EpdFontData fontData{};      // pointers patched to vectors above + metrics
  };
  // Slots indexed by styleId (REGULAR=0, BOLD=1, ITALIC=2, BOLDITALIC=3).
  // We use a fixed-size array (~270 bytes overhead when no subset is
  // installed) rather than a unique_ptr<vector> indirection so the
  // accessor path stays branch-light. embeddedSubsetInstalled_ flips true
  // when at least one slot was populated.
  std::array<EmbeddedStyleSlot, 4> embeddedStyles_;
  bool embeddedSubsetInstalled_ = false;
  // Re-patches each populated slot's fontData pointers (intervals / glyph /
  // bitmap) to point at the slot's std::vector storage. Called once after
  // tryInstallEmbeddedGlyphSubset() builds the slots, and any time the
  // slot vectors might move (in practice, only at install time -- the
  // vectors are stable after install).
  void patchEmbeddedFontDataPointers();

  // CrumBLE 4.4 step 5: convert one style's GlyphEntry table into the
  // EpdFontData + EpdGlyph[] + EpdUnicodeInterval[] shape the renderer
  // already knows how to consume. Populates the synthesizedGlyphs /
  // synthesizedIntervals / fontData fields of slot. Called once per
  // style at tryInstallGlyphAtlas time after entries[] is populated and
  // glyphAtlasBitmap_ is loaded -- bitmap base pointer in fontData is
  // taken from glyphAtlasBitmap_.data() so the pointer is stable for
  // the lifetime of the install.
  void synthesizeAtlasFontData(GlyphAtlasSlot& slot);

  bool writeSectionFileHeader(int fontId, float lineCompression, uint8_t extraParagraphSpacing, bool forceParagraphIndents,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                              bool bionicReadingEnabled, bool guideReadingEnabled,
                              uint8_t tableRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  // CrumBLE: shared implementation of loadSectionFile that works for either
  // the live filePath or the prebakeFilePath. Returns true on a clean load
  // (magic + version + 12 fingerprint fields all matching the args). On
  // mismatch / parse failure, returns false WITHOUT calling clearCache --
  // the caller (loadSectionFile) decides whether the live cache should be
  // cleared, so we never accidentally delete the prebake fallback.
  bool tryLoadFromPath(const std::string& path, int fontId, float lineCompression, uint8_t extraParagraphSpacing,
                       bool forceParagraphIndents, uint8_t paragraphAlignment, uint16_t viewportWidth,
                       uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                       bool bionicReadingEnabled, bool guideReadingEnabled, uint8_t tableRendering,
                       bool forceSimpleRendering = false);

  // v18.9.9.29 (v20 Phase C1): page-offset table entry held in RAM while an
  // incremental build is running so already-emitted pages can be located in
  // the partially-written .bin. Also used by the one-shot createSectionFile
  // wrapper (populated by onPageComplete, drained by finalizeBuild).
  struct PageLutEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
  };
  // Held only while an incremental build is in progress (see startBuild).
  // Carries every piece of state that has to survive across buildSomeMore
  // ticks: the parser (holds internal expat state), the strings it
  // references by pointer, the in-RAM LUT, the effective parse settings
  // (after force-simple / suppressTables overrides), and the CrumBLE-
  // specific out-params. When the build ends -- Done, Error, or abandoned
  // via the destructor -- finalizeBuild / abandonBuild release build_ and
  // disarm the tables-suppress parser guard.
  struct BuildContext {
    std::unique_ptr<ChapterHtmlSlimParser> parser;
    std::vector<PageLutEntry> lut;

    // Parse settings the parser was constructed with. Kept so we can patch
    // them into the section header at finalizeBuild time. "Effective"
    // means: after forceSimpleRendering / suppressTablesOnly overrides.
    int fontId = 0;
    float lineCompression = 0.0f;
    uint8_t extraParagraphSpacing = 0;
    bool forceParagraphIndents = false;
    uint8_t paragraphAlignment = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    bool hyphenationEnabled = false;
    bool embeddedStyle = false;
    uint8_t imageRendering = 0;
    bool bionicReadingEnabled = false;
    bool guideReadingEnabled = false;
    uint8_t tableRendering = 0;

    // File paths + build workspace. tmpSectionPath is where onPageComplete
    // writes; finalizeBuild atomic-renames to filePath at the end. The
    // parser references parsePath/contentBase/imageBasePath by reference,
    // so they must live in the context (which outlives the parser).
    std::string tmpSectionPath;
    std::string parsePath;
    std::string htmlPath;
    std::string tmpHtmlPath;
    std::string contentBase;
    std::string imageBasePath;
    bool reusedHtml = false;
    CssParser* cssParser = nullptr;

    // Snapshotted at startBuild for the imagesSuppressed sidecar rebuild
    // threshold that finalizeBuild patches into the header.
    uint32_t buildStartMaxAlloc = 0;

    // Out-param pointers from the createSectionFile caller. Nullable.
    bool* imagesWereSuppressedOut = nullptr;
    bool* layoutAbortedForLowMemoryOut = nullptr;

    // v18.9.9.6 Level 2 tables-suppress parser guard armed at startBuild
    // via setChapterParserSuppressTablesForSimple(true). finalizeBuild /
    // abandonBuild disarm it. (Replaces the SimpleFlagGuard RAII that was
    // stack-scoped inside the one-shot createSectionFile.)
    bool tablesGuardArmed = false;
    // v18.9.9.30: popup callback fired from buildSomeMore's parseStep loop
    // every ~250 ms. Restores the animated popup that used to live inside
    // parseAndBuildPages (which C1 replaced with a Section-driven parseStep
    // loop). Copied off the parser so buildSomeMore can call it without a
    // parser accessor. Optional -- one-shot createSectionFile passes its
    // dots-cycling lambda; C2 will pass a lambda that reads pageCount.
    std::function<void()> popupFn;
    // Last-fired timestamp so the 250 ms cadence is preserved across
    // buildSomeMore(N) yield boundaries (a C2 caller might return control
    // to the render loop between ticks -- keep the popup smooth anyway).
    uint32_t lastPopupTickMs = 0;

    // v18.9.9.76: byte-ratio page-count estimate (ported from crosspoint's
    // feat-smart-indexing). estimatedTotalPages() extrapolates from
    // bytesConsumed/totalBytes ratio with an EMA smoothing pass so the
    // "Indexing… page X of ~Y" popup doesn't jitter mid-parse. bytesConsumed
    // captured per buildSomeMore yield; totalBytes captured once at
    // startBuild. smoothedEstimate and smoothedAtConsumed are the EMA state.
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    float smoothedEstimate = 0.0f;
    uint32_t smoothedAtConsumed = 0;

    // v18.9.9.479: scratch buffer that onPageComplete wraps in a
    // BufferedFileWriter so page serialization coalesces its ~2000
    // per-field SdFat writes into a handful of block writes. Allocated
    // ONCE per build (not per page) and released with the BuildContext.
    // nullptr is a supported state: under heap pressure the allocation is
    // skipped and page writes fall back to the old unbuffered path.
    static constexpr size_t PAGE_WRITE_BUFFER_BYTES = 2048;
    std::unique_ptr<uint8_t[]> pageWriteBuffer;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;
  // v18.9.9.30 (v20 Phase C2): snapshotted at finalizeBuild time (success)
  // AND at buildSomeMore's Error branch (failure). Callers driving the
  // incremental build across render ticks can't hold on to stack-scoped
  // out-param pointers, so query these getters after isBuildComplete()
  // (success) or after buildSomeMore returns false (failure).
  bool lastBuildLayoutAbortedForLowMemory_ = false;
  bool lastBuildImagesWereSuppressed_ = false;

  // v18.9.9.29 (v20 Phase C1) internal: called by buildSomeMore when the
  // parser reports Done. Writes LUT / anchors / trailer, patches the
  // header, closes the file, and atomic-renames tmp -> live. Also drains
  // build_'s out-param pointers into the caller's bool*.
  bool finalizeBuild();

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Constructor and destructor are out-of-line so unique_ptr<BuildContext>
  // (which forward-declares ChapterHtmlSlimParser) can be
  // constructed/destroyed where the parser's full definition is visible.
  // The destructor calls abandonBuild so every section.reset() path tears
  // down an in-flight build cleanly and removes its partial tmp section
  // file.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  // CrumBLE: when prebakeFallbackEnabled is false, only the live sections/
  // file is consulted (matches stock 3.7.3 behaviour). When true, the live
  // file is tried first, then sections-prebake/ as a read-only fallback.
  // Default-false preserves call-site compatibility for any caller that
  // hasn't been updated to thread the SETTINGS toggle through.
  bool loadSectionFile(int fontId, float lineCompression, uint8_t extraParagraphSpacing, bool forceParagraphIndents,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                       bool guideReadingEnabled, uint8_t tableRendering, bool prebakeFallbackEnabled = false,
                       bool forceSimpleRendering = false);
  bool clearCache() const;
  // v18.9.6: forceSimpleRendering overrides the styling knobs to their most
  // memory-frugal state for a retry-after-abort parse: images suppressed
  // (imageRendering=2), embedded style skipped, bionic+guide reading off,
  // tables forced to paragraph flow (via
  // setChapterParserSuppressTablesForSimple). Caller passes the user's
  // real settings; when the flag is true, the function ignores them for
  // those five knobs.
  // v18.9.9.6 Level 2: suppressTablesOnly is orthogonal to forceSimpleRendering.
  // When true (and forceSimpleRendering false), the parser suppresses only
  // table fragments -- images/embedded style/bionic/guide follow the passed
  // values. When both false, everything follows the passed values (full
  // render). When forceSimpleRendering is true it subsumes suppressTablesOnly.
  bool createSectionFile(int fontId, float lineCompression, uint8_t extraParagraphSpacing, bool forceParagraphIndents,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                         bool guideReadingEnabled, uint8_t tableRendering,
                         const std::function<void()>& popupFn = nullptr,
                         bool* imagesWereSuppressed = nullptr, bool* layoutAbortedForLowMemory = nullptr,
                         bool forceSimpleRendering = false, bool suppressTablesOnly = false);

  // v18.9.9.29 (v20 Phase C1): incremental build API. Lay out a section a
  // few pages at a time so a large chapter can be rendered / scrolled
  // through while the rest is still being parsed.
  //
  //   if (!startBuild(...)) fail;
  //   each render tick: buildSomeMore(N);  // returns false on error
  //   check isBuildComplete(); if true, finalizeBuild has already committed.
  //
  // createSectionFile() is the one-shot wrapper: start + buildSomeMore(0)
  // (0 = build to completion in one call). Behaviour is identical to the
  // pre-refactor version -- C1 is a pure refactor. C2 (reader drives
  // buildSomeMore across ticks) and C3 (loadPageDuringBuild) land later.
  bool startBuild(int fontId, float lineCompression, uint8_t extraParagraphSpacing, bool forceParagraphIndents,
                  uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                  bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled, bool guideReadingEnabled,
                  uint8_t tableRendering, const std::function<void()>& popupFn = nullptr,
                  bool* imagesWereSuppressed = nullptr, bool* layoutAbortedForLowMemory = nullptr,
                  bool forceSimpleRendering = false, bool suppressTablesOnly = false);
  // Advance the in-progress build by up to maxPages more pages. maxPages
  // <= 0 means "run to completion". Returns false on parse error / cleanup
  // failure (build is torn down). When the parser reports Done, buildSomeMore
  // calls finalizeBuild internally and sets isBuildComplete().
  bool buildSomeMore(int maxPages);
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  // v18.9.9.30 (v20 Phase C2): last-build outcome flags. Set by the
  // buildSomeMore Error branch and by finalizeBuild's success path.
  // Callers that drive incremental builds across render ticks can't rely
  // on stack-scoped bool* out-params (they'd be dangling by the time the
  // parser reports Done or Error), so we snapshot the flags on Section
  // itself. lastBuildLayoutAbortedForLowMemory()  = parser aborted for
  // heap pressure; lastBuildImagesWereSuppressed() = parser fell back to
  // suppressing images. Both cleared on the next startBuild.
  bool lastBuildLayoutAbortedForLowMemory() const { return lastBuildLayoutAbortedForLowMemory_; }
  bool lastBuildImagesWereSuppressed() const { return lastBuildImagesWereSuppressed_; }
  // v18.9.9.76: byte-ratio page-count estimate for the "Indexing… page X of ~Y"
  // popup. Returns pageCount unmodified when there's no active build (already-
  // finalized sections need no estimate — pageCount is exact). During an active
  // build, extrapolates from bytesConsumed/totalBytes with an EMA smoothing pass
  // so the estimate doesn't jitter as long chapters have variable page density.
  // Ported from crosspoint/feat-smart-indexing; ALPHA=0.25 matches theirs.
  uint16_t estimatedTotalPages() const;
  // Drop an in-flight build without committing. Called from ~Section and
  // from every error path inside startBuild / buildSomeMore. Idempotent.
  void abandonBuild();

  std::unique_ptr<Page> loadPageFromSectionFile();
  // v18.9.9.10: streamed page render. Deserializes and renders ONE
  // PageElement at a time, dropping each before reading the next. Peak
  // heap footprint is ~500 bytes per page (one TextBlock compact block)
  // instead of the ~10 KB whole-DOM peak of loadPageFromSectionFile +
  // Page::render. Used by the reader whenever BT is linked -- the
  // guaranteed-fit compat-mode render path.
  //
  // v18.9.9.11: btLinked hint. When true, PageImage elements skip render
  // (image slot left blank -- JPEG decoder won't fit post-BT budget) and
  // PageTableFragment elements render via renderContentOnly (cell text
  // without borders/structure). Reader passes its own BT-state check.
  // Returns true on successful render, false on file open / seek /
  // deserialize error.
  // v18.9.9.57: pxcImagesSafe -- when true, TAG_PageImage elements attempt a
  // cache-only blit (ImageBlock::renderIfCached) instead of being skipped.
  // Reader passes true iff pxcManifest_.has_value() && wasLoadedFromPrebake()
  // -- both together guarantee any image referenced by this section has a
  // valid .pxc entry, so the JPEG-decoder fallback path (~53 KB, OOM under
  // BT) never fires.
  bool renderPageStreamed(GfxRenderer& renderer, int fontId, int xOffset, int yOffset,
                          bool foregroundBlack, bool btLinked, bool pxcImagesSafe = false);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // CrumBLE 4.3: embedded glyph subset accessors. Surface the trailer fields
  // read from the section file so the font system (EpdFontFamily +
  // SdCardFont) can install a section-scoped EpdFontData when one was baked
  // in. hasEmbeddedGlyphSubset() returns true iff the section actually
  // carries a block (offset and size both non-zero). The cpfontHash is the
  // SdCardFont::contentHash() of the .cpfont this section was baked
  // against; install code MUST compare against the currently-loaded
  // SdCardFont's hash before consuming the block (otherwise glyph indices
  // would point at the wrong font's bitmaps).
  bool hasEmbeddedGlyphSubset() const { return embeddedGlyphSubsetOffset_ != 0 && embeddedGlyphSubsetSize_ != 0; }
  uint32_t embeddedGlyphSubsetOffset() const { return embeddedGlyphSubsetOffset_; }
  uint32_t embeddedGlyphSubsetSize() const { return embeddedGlyphSubsetSize_; }
  uint32_t embeddedGlyphSubsetCpfontHash() const { return embeddedGlyphSubsetCpfontHash_; }
  // CrumBLE 4.4: v40 glyph atlas accessors. Same shape as the v39 subset
  // accessors above; hasGlyphAtlas() short-circuits the install path
  // when no atlas was baked (legacy sections, device-built sections,
  // built-in font books for the moment until phase 2 of atlas work).
  bool hasGlyphAtlas() const { return glyphAtlasOffset_ != 0 && glyphAtlasSize_ != 0; }
  uint32_t glyphAtlasOffset() const { return glyphAtlasOffset_; }
  uint32_t glyphAtlasSize() const { return glyphAtlasSize_; }
  uint32_t glyphAtlasCpfontHash() const { return glyphAtlasCpfontHash_; }
  // v41: alternate atlas slot (the OTHER bit-depth, when the bake emitted
  // both). See field comment above for the BT-aware install rationale.
  bool hasGlyphAtlasAlt() const { return glyphAtlasAltOffset_ != 0 && glyphAtlasAltSize_ != 0; }
  uint32_t glyphAtlasAltOffset() const { return glyphAtlasAltOffset_; }
  uint32_t glyphAtlasAltSize() const { return glyphAtlasAltSize_; }
  uint32_t glyphAtlasAltCpfontHash() const { return glyphAtlasAltCpfontHash_; }

  // CrumBLE 4.4: read the glyph atlas block from the section file and
  // populate glyphAtlasSlots_ + glyphAtlasBitmap_. Validates against
  // the caller's cpfontContentHash (from SdCardFont::contentHash())
  // before consuming the block; mismatch leaves the slots untouched and
  // returns false so the renderer falls back to the v39 embedded subset
  // or SD-font miss handler path. Returns true iff at least one style
  // slot was populated. Idempotent: a second call with the same hash
  // re-installs cleanly.
  // CrumBLE 4.4 v41: when preferLowBitDepth=true (typical caller: reader
  // with BT enabled), install the primary (1-bit) slot. When false (BT
  // cold), install the alt (2-bit) slot if non-zero, else fall through
  // to the primary. Pre-v41 sections only have the primary slot, so the
  // behavior is unchanged for old prebakes.
  bool tryInstallGlyphAtlas(uint32_t cpfontContentHash, bool preferLowBitDepth = false);

  // True after a successful tryInstallGlyphAtlas(); false otherwise.
  bool glyphAtlasInstalled() const { return glyphAtlasInstalled_; }

  // Free the in-RAM atlas (clears glyphAtlasSlots_ entries and
  // glyphAtlasBitmap_). Mirror of dropEmbeddedGlyphSubset() for the
  // BT-enable path -- callers can free atlas memory under pressure and
  // re-install it later via tryInstallGlyphAtlas(). Idempotent. No-op
  // when nothing is installed.
  void dropGlyphAtlas();

  // Look up a glyph by codepoint in the installed atlas for the given
  // style. Returns nullptr if no atlas is installed for this style or
  // the codepoint isn't covered. Caller renders via the entry's bitmap
  // offset (into glyphAtlasBitmapPtr()) and the entry's per-glyph
  // dimensions / advance / left / top metadata.
  const glyphatlas::GlyphEntry* lookupGlyphAtlasEntry(uint8_t styleId, uint32_t codepoint) const;

  // Pointer to the shared bitmap payload. nullptr if no atlas installed.
  const uint8_t* glyphAtlasBitmapPtr() const {
    return glyphAtlasBitmap_.empty() ? nullptr : glyphAtlasBitmap_.data();
  }
  uint8_t glyphAtlasBitDepth() const { return glyphAtlasBitDepth_; }

  // CrumBLE 4.4 step 5: per-style EpdFontData synthesized from the
  // atlas data at install time. Returns nullptr when no atlas is
  // installed for this style. Pointer lifetime: valid until the next
  // tryInstallGlyphAtlas / dropGlyphAtlas / Section destruction. The
  // reader passes these into GfxRenderer::setEmbeddedGlyphData (same
  // entry point the v39 embedded subset uses) so the existing render
  // path consumes atlas glyphs through the embedded-data slot without
  // any blit-path changes.
  const EpdFontData* glyphAtlasFontDataForStyle(uint8_t styleId) const;

  // Per-style metric accessors for the renderer.
  uint16_t glyphAtlasAscender(uint8_t styleId) const {
    return styleId < glyphAtlasSlots_.size() ? glyphAtlasSlots_[styleId].ascender : 0;
  }
  uint16_t glyphAtlasDescender(uint8_t styleId) const {
    return styleId < glyphAtlasSlots_.size() ? glyphAtlasSlots_[styleId].descender : 0;
  }
  uint16_t glyphAtlasLineHeight(uint8_t styleId) const {
    return styleId < glyphAtlasSlots_.size() ? glyphAtlasSlots_[styleId].lineHeight : 0;
  }
  // CrumBLE 4.3 diagnostic: surface fileVersion_ so callers can distinguish
  // "section is pre-v39 (no embedded subset trailer possible)" from "section
  // is v39 but the prebake CLI didn't emit a subset block". 0 if no section
  // has been loaded yet.
  uint8_t fileVersion() const { return fileVersion_; }
  // CrumBLE 4.3: read the embedded glyph subset block from the section file
  // and populate embeddedStyles_. Validates against the caller's
  // cpfontContentHash (which they get from SdCardFont::contentHash()) before
  // touching anything; mismatch leaves embeddedStyles_ untouched and
  // returns false so the renderer can fall back to the SD-font miss
  // handler. Returns true iff at least one style slot was populated.
  // Idempotent: calling twice with the same hash re-installs cleanly.
  bool tryInstallEmbeddedGlyphSubset(uint32_t cpfontContentHash);
  // True after a successful tryInstallEmbeddedGlyphSubset(); false on a
  // section that didn't carry a block or that failed hash validation.
  bool embeddedSubsetInstalled() const { return embeddedSubsetInstalled_; }
  // CrumBLE 4.3: free the embedded subset's per-style vectors. Caller is
  // EpubReaderActivity's BT-enable path -- the v2 subset (with kerning) is
  // ~10 KB and competes with NimBLE for heap on SD-font books. Drop here,
  // reload after the post-connect render. Idempotent. No-op when nothing
  // is installed.
  void dropEmbeddedGlyphSubset();
  // Returns an EpdFontData* for the requested style (REGULAR=0/BOLD=1/
  // ITALIC=2/BOLDITALIC=3) iff that style was in the embedded block,
  // else nullptr. The EpdFontFamily glyph router consults this before
  // falling through to the SD-font miss handler so prebaked sections
  // skip the SD-font miniData heap allocation entirely.
  const EpdFontData* embeddedFontDataForStyle(uint8_t styleId) const;
  // Path to the section file that was actually loaded (live cache or
  // prebake fallback). Needed by the embedded-glyph-subset install code so
  // it can re-open the file to read the block contents on demand without
  // disturbing the rest of Section's file state.
  const std::string& activeFilePathForGlyphSubset() const { return activeFilePath; }
  // v18.9.9.56: true iff the most recently loaded section file was the
  // read-only prebake artifact (sections-prebake/) rather than the live
  // sections/ slot. Used by the reader render gate to decide whether the
  // whole-DOM path is safe under BT -- prebake-served sections read all
  // their images from the .pxc cache (~2 KB blit each, no JPEG decoder),
  // while a fresh/cold-built section's images fall through to the ~53 KB
  // decoder path which OOM's under BT.
  bool wasLoadedFromPrebake() const {
    return !prebakeFilePath.empty() && activeFilePath == prebakeFilePath;
  }

  // CrumBLE 4.3 option 3: pre-allocate the ~18 KB page-heap reserve at
  // boot, while heap is least fragmented. Returns true on success. Call
  // once from setup() AFTER the long-lived global allocations have run
  // (so the reserve doesn't displace anything more important) but BEFORE
  // any chapter open. The reserve is held until loadPageFromSectionFile()
  // detects heap pressure and releases it for the deserialize allocator.
  static bool ensurePageHeapReserveAtBoot();
  // Diagnostic: current reserve state. true = held, false = released.
  static bool pageHeapReserveHeld();
  // CrumBLE 4.3 option 3: release the reserve unconditionally for callers
  // outside loadPageFromSectionFile() that need to hand its bytes to
  // another consumer. Primary user: BluetoothHIDManager::enable() pre-flight
  // -- the held reserve drops free heap below the 66 KB NimBLE threshold
  // and prevents BT from enabling. Caller is responsible for the post-BT
  // re-acquire (currently best-effort via tryReacquirePageHeapReserve).
  // No-op if the reserve was already released.
  static void releasePageHeapReserveForBtEnable();
  // CrumBLE 4.3 option 3: best-effort re-acquire of the reserve after a
  // BT-enable releaseFlow. Returns true if the reserve is held after the
  // call (either because it was already held or because the malloc succeeded).
  // Safe to call from anywhere; if the heap is too tight the reserve stays
  // released and the next loadPageFromSectionFile() will deserialize against
  // whatever's available.
  static bool tryReacquirePageHeapReserve();
};
