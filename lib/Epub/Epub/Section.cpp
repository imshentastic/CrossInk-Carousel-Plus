#include "Section.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v38: added imagesSuppressed + buildMaxAlloc to the header so a chapter cached
// with images dropped under low heap can be rebuilt with images once memory
// recovers. The bump also invalidates any v37 cache that was silently cached
// imageless (it will rebuild fresh on next open).
constexpr uint8_t SECTION_FILE_VERSION = 38;
// How much the largest free block must have grown since a degraded build before
// we bother rebuilding it for images (avoids rebuild churn on tiny variations).
constexpr uint32_t SECTION_DEGRADED_REBUILD_MARGIN = 12 * 1024;
constexpr uint32_t HEADER_SIZE = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(bool) /*imagesSuppressed*/ + sizeof(uint32_t) /*buildMaxAlloc*/ +
                                 sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed (pos=%lu, free=%u, maxAlloc=%u)", pageCount, static_cast<unsigned long>(position),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  pageCount++;
  return position;
}

bool Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const bool embeddedStyle,
                                     const uint8_t imageRendering, const bool bionicReadingEnabled,
                                     const bool guideReadingEnabled) {
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
                                   sizeof(uint32_t) /*buildMaxAlloc*/ + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
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
         serialization::tryWritePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                              const bool bionicReadingEnabled, const bool guideReadingEnabled,
                              const bool prebakeFallbackEnabled) {
  // Try the live cache (sections/) first. This is the path the device
  // writes to when it rebuilds a chapter under current settings, so it's
  // always going to fingerprint-match if the cache is still fresh.
  if (tryLoadFromPath(filePath, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
                      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle,
                      imageRendering, bionicReadingEnabled, guideReadingEnabled)) {
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
                      imageRendering, bionicReadingEnabled, guideReadingEnabled)) {
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
                              const bool extraParagraphSpacing, const bool forceParagraphIndents,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool bionicReadingEnabled,
                              const bool guideReadingEnabled) {
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
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u (%s)", version, path.c_str());
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
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

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || forceParagraphIndents != fileForceParagraphIndents ||
        paragraphAlignment != fileParagraphAlignment || viewportWidth != fileViewportWidth ||
        viewportHeight != fileViewportHeight || hyphenationEnabled != fileHyphenationEnabled ||
        embeddedStyle != fileEmbeddedStyle || imageRendering != fileImageRendering ||
        bionicReadingEnabled != fileBionicReadingEnabled || guideReadingEnabled != fileGuideReadingEnabled) {
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

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const bool bionicReadingEnabled, const bool guideReadingEnabled,
                                const std::function<void()>& popupFn, bool* imagesWereSuppressed,
                                bool* layoutAbortedForLowMemory) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
  pageCount = 0;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  // CrumBLE: snapshot the largest free block we're building with. If images end
  // up suppressed, this is stored in the cache header so a later load can tell
  // whether the heap has recovered enough (e.g. BLE disconnected) to be worth
  // rebuilding the chapter with images.
  const uint32_t buildStartMaxAlloc = ESP.getMaxAllocHeap();
  LOG_DBG("SCT", "Create section start: spine=%d viewport=%ux%u image=%u bionic=%u guide=%u free=%u maxAlloc=%u",
          spineIndex, viewportWidth, viewportHeight, imageRendering, bionicReadingEnabled, guideReadingEnabled,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
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
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes, free=%u, maxAlloc=%u)", tmpHtmlPath.c_str(), fileSize,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    return false;
  }
  if (!writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                              bionicReadingEnabled, guideReadingEnabled)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, static_cast<unsigned>(cssParser->ruleCount()), cssHeapBefore.freeHeap,
              cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, bionicReadingEnabled, guideReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  LOG_DBG("SCT", "Parser start: spine=%d free=%u maxAlloc=%u", spineIndex, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  success = visitor.parseAndBuildPages();
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  const bool builtImagesSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (imagesWereSuppressed) *imagesWereSuppressed = builtImagesSuppressed;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = visitor.wasLowMemoryAbortTriggered();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
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
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (const auto& [anchor, page] : anchors) {
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(lut.size()))) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (const auto& entry : lut) {
    if (!serialization::tryWritePod(file, entry.paragraphIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    if (!serialization::tryWritePod(file, entry.listItemIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  // Patch header with final imagesSuppressed, buildMaxAlloc, pageCount, lutOffset,
  // anchorMapOffset, paragraphLutOffset, and liLutOffset (all written as
  // placeholders by writeSectionFileHeader, in this exact order).
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount) - sizeof(uint32_t) - sizeof(bool)) ||
      !serialization::tryWritePod(file, builtImagesSuppressed) ||
      !serialization::tryWritePod(file, buildStartMaxAlloc) || !serialization::tryWritePod(file, pageCount) ||
      !serialization::tryWritePod(file, lutOffset) || !serialization::tryWritePod(file, anchorMapOffset) ||
      !serialization::tryWritePod(file, paragraphLutOffset) ||
      !serialization::tryWritePod(file, liLutFileOffset) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(tmpSectionPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // CrumBLE: a freshly built section lives at filePath; point activeFilePath
  // at it so subsequent reads (loadPageFromSectionFile etc.) target the new
  // live cache rather than whichever path was used by a stale prior load.
  activeFilePath = filePath;
  if (cssParser) {
    cssParser->clear();
  }
  LOG_DBG("SCT", "Create section done: spine=%d pages=%u free=%u maxAlloc=%u", spineIndex, pageCount, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", activeFilePath, file)) {
    return nullptr;
  }

  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
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

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", activeFilePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
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
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
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
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
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
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) {
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
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
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
