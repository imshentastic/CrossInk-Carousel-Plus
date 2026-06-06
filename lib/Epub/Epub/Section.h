#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../EpdFont/EpdFontData.h"  // EpdFontData / EpdUnicodeInterval / EpdGlyph
#include "Epub.h"

class Page;
class GfxRenderer;

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

  bool writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                              bool bionicReadingEnabled, bool guideReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  // CrumBLE: shared implementation of loadSectionFile that works for either
  // the live filePath or the prebakeFilePath. Returns true on a clean load
  // (magic + version + 12 fingerprint fields all matching the args). On
  // mismatch / parse failure, returns false WITHOUT calling clearCache --
  // the caller (loadSectionFile) decides whether the live cache should be
  // cleared, so we never accidentally delete the prebake fallback.
  bool tryLoadFromPath(const std::string& path, int fontId, float lineCompression, bool extraParagraphSpacing,
                       bool forceParagraphIndents, uint8_t paragraphAlignment, uint16_t viewportWidth,
                       uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                       bool bionicReadingEnabled, bool guideReadingEnabled);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin"),
        prebakeFilePath(epub->getCachePath() + "/sections-prebake/" + std::to_string(spineIndex) + ".bin"),
        activeFilePath(filePath) {}
  ~Section() = default;
  // CrumBLE: when prebakeFallbackEnabled is false, only the live sections/
  // file is consulted (matches stock 3.7.3 behaviour). When true, the live
  // file is tried first, then sections-prebake/ as a read-only fallback.
  // Default-false preserves call-site compatibility for any caller that
  // hasn't been updated to thread the SETTINGS toggle through.
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                       bool guideReadingEnabled, bool prebakeFallbackEnabled = false);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                         bool guideReadingEnabled, const std::function<void()>& popupFn = nullptr,
                         bool* imagesWereSuppressed = nullptr, bool* layoutAbortedForLowMemory = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

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
};
