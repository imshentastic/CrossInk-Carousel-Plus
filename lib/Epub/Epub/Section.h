#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
  FsFile file;

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
        prebakeFilePath(epub->getCachePath() + "/sections-prebake/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool forceParagraphIndents,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, bool bionicReadingEnabled,
                       bool guideReadingEnabled);
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
};
