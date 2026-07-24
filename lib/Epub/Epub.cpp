#include "Epub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Print.h>
#include <ZipFile.h>
#include <expat.h>

#include <atomic>

namespace {
// CrumBLE 4.6: process-wide cover-tone curve, set from main.cpp after
// SETTINGS loads (and on Settings change). Atomic so the cover-render task
// can read it without holding a lock.
std::atomic<uint8_t> g_coverToneCurve{0};
}  // namespace

void Epub::setCoverToneCurve(uint8_t v) { g_coverToneCurve.store(v, std::memory_order_relaxed); }
uint8_t Epub::getCoverToneCurve() { return g_coverToneCurve.load(std::memory_order_relaxed); }

#include <cstring>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
constexpr int kDefaultThumbHeight = 180;

// CrumBLE: minimal expat-based parser used ONLY by extractSeriesFromOpf
// to read series metadata without touching the BookMetadataCache. We
// can't reuse ContentOpfParser for this because that parser writes to
// book.bin's intermediate files (tempItemStore, cache->createSpineEntry)
// during the manifest/spine pass — using it would corrupt or trigger a
// rebuild of the cached spine layout. This trimmed parser only handles
// metadata state transitions; manifest/spine/guide elements are
// ignored entirely (no state change, no character-data dispatch).
class SeriesOnlyOpfParser : public Print {
  enum State { START, IN_PACKAGE, IN_METADATA, IN_SERIES_NAME, IN_SERIES_INDEX, IN_DC_CREATOR };
  XML_Parser parser = nullptr;
  State state = START;
  size_t remainingSize;

  static void XMLCALL startElement(void* ud, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<SeriesOnlyOpfParser*>(ud);
    if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
      self->state = IN_PACKAGE;
      return;
    }
    if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
      self->state = IN_METADATA;
      return;
    }
    // CrumBLE 4.5.2: also capture dc:creator so AuthorAlpha sort can fill
    // its key cache without doing a full Epub::load + book.bin build on
    // never-opened books. Author appears in the metadata block alongside
    // series, so the existing </metadata> early-exit covers both.
    if (self->state == IN_METADATA &&
        (strcmp(name, "dc:creator") == 0 || strcmp(name, "creator") == 0)) {
      self->state = IN_DC_CREATOR;
      self->author.clear();
      return;
    }
    if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
      const char* attrName = nullptr;
      const char* attrContent = nullptr;
      const char* attrProperty = nullptr;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "name") == 0) attrName = atts[i + 1];
        else if (strcmp(atts[i], "content") == 0) attrContent = atts[i + 1];
        else if (strcmp(atts[i], "property") == 0) attrProperty = atts[i + 1];
      }
      if (attrName != nullptr && attrContent != nullptr) {
        if (strcmp(attrName, "calibre:series") == 0 || strcmp(attrName, "series") == 0) {
          self->seriesName = attrContent;
        } else if (strcmp(attrName, "calibre:series_index") == 0 || strcmp(attrName, "series_index") == 0) {
          self->seriesIndex = attrContent;
        }
      }
      if (attrProperty != nullptr) {
        if (strcmp(attrProperty, "belongs-to-collection") == 0) {
          self->state = IN_SERIES_NAME;
          self->seriesName.clear();
        } else if (strcmp(attrProperty, "group-position") == 0) {
          self->state = IN_SERIES_INDEX;
          self->seriesIndex.clear();
        }
      }
    }
  }

  static void XMLCALL endElement(void* ud, const XML_Char* name) {
    auto* self = static_cast<SeriesOnlyOpfParser*>(ud);
    if ((self->state == IN_SERIES_NAME || self->state == IN_SERIES_INDEX) &&
        (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
      self->state = IN_METADATA;
      return;
    }
    if (self->state == IN_DC_CREATOR &&
        (strcmp(name, "dc:creator") == 0 || strcmp(name, "creator") == 0)) {
      self->state = IN_METADATA;
      return;
    }
    if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
      // Series + author tags appear early in the metadata block; stop
      // parsing once metadata closes -- the rest of the OPF (manifest,
      // spine, guide) is irrelevant for the peek and would just burn
      // CPU. Returning XML_StopParser would be cleaner but expat's
      // status code propagates and the caller would log a false error.
      self->state = START;
    }
  }

  static void XMLCALL characterData(void* ud, const XML_Char* s, int len) {
    auto* self = static_cast<SeriesOnlyOpfParser*>(ud);
    if (self->state == IN_DC_CREATOR) {
      self->author.append(s, len);
    } else if (self->state == IN_SERIES_NAME) {
      self->seriesName.append(s, len);
    } else if (self->state == IN_SERIES_INDEX) {
      self->seriesIndex.append(s, len);
    }
  }

 public:
  std::string seriesName;
  std::string seriesIndex;
  std::string author;

  explicit SeriesOnlyOpfParser(size_t xmlSize) : remainingSize(xmlSize) {}
  ~SeriesOnlyOpfParser() override {
    if (parser) XML_ParserFree(parser);
  }
  bool setup() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) return false;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, characterData);
    return true;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser) return 0;
    auto remaining = size;
    auto* p = buffer;
    while (remaining > 0) {
      void* buf = XML_GetBuffer(parser, 1024);
      if (!buf) return 0;
      const auto chunk = remaining < 1024 ? remaining : 1024;
      memcpy(buf, p, chunk);
      if (XML_ParseBuffer(parser, static_cast<int>(chunk), remainingSize == chunk) == XML_STATUS_ERROR) {
        return 0;
      }
      p += chunk;
      remaining -= chunk;
      remainingSize -= chunk;
    }
    return size;
  }
};

bool nonEmptyFileExists(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("EBP", path, file)) {
    return false;
  }
  const bool nonEmpty = file.size() > 0;
  file.close();
  if (!nonEmpty) {
    Storage.remove(path.c_str());
  }
  return nonEmpty;
}

int32_t readLe32(const uint8_t* data) {
  return static_cast<int32_t>(static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24));
}

void normalizeThumbDimensions(int& width, int& height) {
  if (height <= 0) {
    height = kDefaultThumbHeight;
  }
  if (width <= 0) {
    width = static_cast<int>((static_cast<int64_t>(height) * 3 + 2) / 5);
  }
}

bool cachedBmpMatchesDimensions(const std::string& path, const int width, const int height,
                                const bool allowContainedDimensions = false) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("EBP", path, file)) {
    return false;
  }

  uint8_t header[26] = {};
  const bool hasHeader = file.size() >= sizeof(header) && file.read(header, sizeof(header)) == sizeof(header);
  file.close();
  const bool isBmp = hasHeader && header[0] == 'B' && header[1] == 'M';
  const int32_t bmpWidth = isBmp ? readLe32(header + 18) : 0;
  const int32_t bmpHeight = isBmp ? readLe32(header + 22) : 0;
  const int32_t absHeight = bmpHeight < 0 ? -bmpHeight : bmpHeight;
  const bool exactMatch = isBmp && bmpWidth == width && absHeight == height;
  const bool containedMatch = allowContainedDimensions && isBmp && bmpWidth > 0 && absHeight > 0 && bmpWidth <= width &&
                              absHeight <= height && (bmpWidth == width || absHeight == height);
  const bool matches = exactMatch || containedMatch;
  if (!matches) {
    LOG_DBG("EBP", "Removing stale thumbnail dimensions: %s (%dx%d expected %dx%d)", path.c_str(), bmpWidth, absHeight,
            width, height);
    Storage.remove(path.c_str());
  }
  return matches;
}

std::string getThumbBmpPathForDimensions(const std::string& cachePath, int width, int height) {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

std::string getAdaptiveThumbBmpPathForDimensions(const std::string& cachePath, int width, int height) {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + "_fit.bmp";
}

std::string legacyCachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  return cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(filepath));
}
}  // namespace

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = cachePathForFilePath(this->filepath, cacheDir);
  migrateLegacyCachePath(cacheDir);
}

std::string Epub::cachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  // Keep on-disk EPUB cache keys stable across standard library/toolchain changes.
  return cacheDir + "/epub_" + std::to_string(ZipFile::fnvHash64(filepath.c_str(), filepath.size()));
}

void Epub::migrateLegacyCachePath(const std::string& cacheDir) const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  const std::string legacyCachePath = legacyCachePathForFilePath(filepath, cacheDir);
  if (legacyCachePath == cachePath || !Storage.exists(legacyCachePath.c_str())) {
    return;
  }

  if (Storage.rename(legacyCachePath.c_str(), cachePath.c_str())) {
    LOG_INF("EBP", "Migrated legacy EPUB cache: %s -> %s", legacyCachePath.c_str(), cachePath.c_str());
  } else {
    LOG_ERR("EBP", "Failed to migrate legacy EPUB cache: %s -> %s", legacyCachePath.c_str(), cachePath.c_str());
  }
}

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize, bookMetadataCache.get());
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  // CrumBLE series fields (ported from aalu). Stored on the Epub
  // instance rather than `bookMetadata` because book.bin's binary
  // layout would need a version bump to add the fields, and we don't
  // gain much from persisting there — SeriesIndex.json caches across
  // sessions and is a more natural query target ("which books are in
  // series X?") than per-book lookup.
  lastSeriesName = opfParser.seriesName;
  lastSeriesIndex = opfParser.seriesIndex;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Check if it's an image file
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref) || FsHelpers::hasGifExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(coverPageBase + imageRef);
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  const auto tmpNcxPath = getCachePath() + "/toc.ncx";
  FsFile tempNcxFile;
  if (!Storage.openFileForWrite("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  readItemContentsToStream(tocNcxItem, tempNcxFile, 1024);
  // Explicitly close() file before reopening for reading
  tempNcxFile.close();
  if (!Storage.openFileForRead("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  const auto ncxSize = tempNcxFile.size();

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  const auto ncxBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!ncxBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc ncx parser");
    return false;
  }

  while (tempNcxFile.available()) {
    const auto readSize = tempNcxFile.read(ncxBuffer, 1024);
    if (readSize == 0) break;
    const auto processedSize = ncxParser.write(ncxBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc ncx data");
      free(ncxBuffer);
      return false;
    }
  }

  free(ncxBuffer);
  // Explicitly close() file before calling Storage.remove()
  tempNcxFile.close();
  Storage.remove(tmpNcxPath.c_str());

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  const auto tmpNavPath = getCachePath() + "/toc.nav";
  FsFile tempNavFile;
  if (!Storage.openFileForWrite("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  readItemContentsToStream(tocNavItem, tempNavFile, 1024);
  // Explicitly close() file before reopening for reading
  tempNavFile.close();
  if (!Storage.openFileForRead("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  const auto navSize = tempNavFile.size();

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  const auto navBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!navBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc nav parser");
    return false;
  }

  while (tempNavFile.available()) {
    const auto readSize = tempNavFile.read(navBuffer, 1024);
    const auto processedSize = navParser.write(navBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc nav data");
      free(navBuffer);
      return false;
    }
  }

  free(navBuffer);
  // Explicitly close() file before calling Storage.remove()
  tempNavFile.close();
  Storage.remove(tmpNavPath.c_str());

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return;
  }

  // No cache yet - parse CSS files
  bool parsedAllCss = true;
  size_t parsedCssFileCount = 0;
  size_t failedCssFileIndex = 0;
  std::string failedCssPath;
  for (size_t cssFileIndex = 0; cssFileIndex < cssFiles.size(); ++cssFileIndex) {
    const auto& cssPath = cssFiles[cssFileIndex];
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    FsFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    if (!cssParser->loadFromStream(tempCssFile)) {
      failedCssFileIndex = cssFileIndex + 1;
      failedCssPath = cssPath;
      LOG_ERR("EBP", "CSS parsing failed for file %zu/%zu after %zu parsed files: %s", failedCssFileIndex,
              cssFiles.size(), parsedCssFileCount, cssPath.c_str());
      parsedAllCss = false;
    } else {
      ++parsedCssFileCount;
    }
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    if (!parsedAllCss) {
      break;
    }
  }

  if (!parsedAllCss) {
    LOG_ERR("EBP",
            "Discarding %zu partial CSS rules after parse failure in %s; CSS cache will not be written for this book",
            cssParser->ruleCount(), failedCssPath.empty() ? "<unknown>" : failedCssPath.c_str());
    cssParser->clear();
    return;
  }

  // Save to cache for next time
  if (!cssParser->saveToCache()) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files", cssParser->ruleCount(), cssFiles.size());
  cssParser->clear();
}

bool Epub::extractSeriesFromOpf() {
  // Locate the OPF inside the EPUB ZIP. Same path findContentOpfFile
  // takes for the full parse — reuses the existing container.xml
  // reader and ZipFile membership.
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_DBG("EBP", "extractSeriesFromOpf: no OPF in %s", filepath.c_str());
    return false;
  }
  size_t contentOpfSize = 0;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    return false;
  }
  SeriesOnlyOpfParser parser(contentOpfSize);
  if (!parser.setup()) return false;
  if (!readItemContentsToStream(contentOpfFilePath, parser, 1024)) return false;
  // Save into the instance fields the caller will read via the
  // existing getSeriesName/getSeriesIndex accessors. CrumBLE 4.5.2:
  // also stash author so AuthorAlpha sort can backfill its key cache
  // without a separate OPF parse pass.
  lastSeriesName = parser.seriesName;
  lastSeriesIndex = parser.seriesIndex;
  lastAuthorPeek = parser.author;
  // Strip leading/trailing whitespace that EPUB 3 belongs-to-collection
  // text bodies often carry due to pretty-printed XML.
  auto trim = [](std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
      s.clear();
      return;
    }
    s = s.substr(a, b - a + 1);
  };
  trim(lastSeriesName);
  trim(lastSeriesIndex);
  trim(lastAuthorPeek);
  return true;
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
      if (!cssParser->hasCache() || !cssParser->loadFromCache()) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        if (!parseContentOpf(bookMetadataCache->coreMetadata)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        }
        parseCssFiles();
        // Invalidate section caches so they are rebuilt with the new CSS
        Storage.removeDir((cachePath + "/sections").c_str());
      }
    }
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  if (!skipLoadingCss) {
    // Parse CSS files after cache reload
    parseCssFiles();
    Storage.removeDir((cachePath + "/sections").c_str());
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

namespace {
// Best-effort recursive delete, used when removeDir() bails. SdFat's rmRf stops
// at the first entry it can't remove, so a single bad file leaves the whole
// directory behind. This walks the tree, deleting each file individually and
// continuing past per-entry failures, then removes the now-empty directories.
// Returns true only if the directory is finally gone. (Bounded depth: book
// caches are at most cachePath/sections/<file>, so recursion is shallow.)
//
// CrumBLE 4.5.5: yields delay(1) after every file remove. The prebake
// landed ~250 section files per book in sections-prebake/ on top of the
// existing pxc-cache + thumbnails + section files (live), so a fully-
// baked CJK book's cache dir is 500-1000 files. Without yields the
// recursive delete pegs the main task for 10+ s -- WiFi heartbeat
// stutters, render task starves, watchdog complains. A 1 ms yield per
// file lets the render task / WiFi / FT websocket stay responsive
// during the cleanup at the cost of ~500 ms extra total wall time.
// Worth it; the delete already wasn't on a hot path.
bool bestEffortRemoveDir(const std::string& dirPath) {
  auto dir = Storage.open(dirPath.c_str());
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    std::vector<std::string> subdirs;
    char name[128];
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      name[0] = '\0';
      entry.getName(name, sizeof(name));
      const bool isDir = entry.isDirectory();
      entry.close();
      if (name[0] == '\0') continue;
      (isDir ? subdirs : files).push_back(dirPath + "/" + name);
    }
    dir.close();
    for (const auto& sub : subdirs) bestEffortRemoveDir(sub);
    for (const auto& f : files) {
      Storage.remove(f.c_str());
      delay(1);  // see header comment -- yield so co-tasks aren't starved
    }
  } else if (dir) {
    dir.close();
  }
  Storage.rmdir(dirPath.c_str());
  return !Storage.exists(dirPath.c_str());
}
}  // namespace

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  // CrumBLE 4.5.5: skip the Storage.removeDir (SdFat rmRf) fast path and
  // go straight to bestEffortRemoveDir. rmRf is faster on small caches
  // but runs as one tight C++ loop with no yields -- on a fully-prebaked
  // CJK book (500+ files in sections-prebake/ + sections/ + pxc-cache/)
  // it pegs the web-server task for 10+ s and the FT page sits there
  // frozen. bestEffortRemoveDir below now yields delay(1) per file, so
  // the cleanup takes a bit longer wall-clock but the device stays
  // responsive (render task can still draw, WiFi heartbeat keeps the
  // connection alive, watchdog stays fed). The fast path also still
  // failed silently on some partial-corruption cases and we'd fall
  // through to bestEffortRemoveDir anyway, so flipping the default is
  // strictly a behaviour improvement.
  if (bestEffortRemoveDir(cachePath)) {
    LOG_DBG("EPB", "Cache cleared via yielding recursive delete");
    return true;
  }

  LOG_ERR("EPB", "Failed to clear cache (SD filesystem may be corrupt)");
  return false;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverJpg, 1024)) {
      LOG_ERR("EBP", "Failed to read cover JPG item: %s", coverImageHref.c_str());
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    coverBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverPng, 1024)) {
      LOG_ERR("EBP", "Failed to read cover PNG item: %s", coverImageHref.c_str());
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    coverBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[WIDTH]x[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return getThumbBmpPath(0, height); }
std::string Epub::getThumbBmpPath(int width, int height) const {
  normalizeThumbDimensions(width, height);
  const std::string newPath = getThumbBmpPathForDimensions(cachePath, width, height);
  if (Storage.exists(newPath.c_str())) {
    return newPath;
  }
  const std::string legacyPath = cachePath + "/thumb_" + std::to_string(height) + ".bmp";
  if (Storage.exists(legacyPath.c_str())) {
    return legacyPath;
  }
  return newPath;
}

std::string Epub::getAdaptiveThumbBmpPath(int width, int height) const {
  normalizeThumbDimensions(width, height);
  return getAdaptiveThumbBmpPathForDimensions(cachePath, width, height);
}

bool Epub::generateThumbBmp(int height) const { return generateThumbBmp(0, height); }

bool Epub::generateThumbBmp(int width, int height) const { return generateThumbBmpInternal(width, height, false); }

bool Epub::generateAdaptiveThumbBmp(int width, int height) const {
  return generateThumbBmpInternal(width, height, true);
}

bool Epub::generateThumbBmpInternal(int width, int height, const bool adaptiveContain) const {
  if (height <= 0) {
    LOG_DBG("EBP", "Using default thumb BMP height for requested dimensions: %dx%d", width, height);
  }
  normalizeThumbDimensions(width, height);
  const std::string thumbPath = adaptiveContain ? getAdaptiveThumbBmpPathForDimensions(cachePath, width, height)
                                                : getThumbBmpPathForDimensions(cachePath, width, height);

  // Already generated with matching dimensions, return true
  if (cachedBmpMatchesDimensions(thumbPath, width, height, adaptiveContain)) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  return convertCoverToThumbBmp(bookMetadataCache->coreMetadata.coverItemHref, thumbPath, width, height,
                                adaptiveContain);
}

bool Epub::generateThumbBmpNoIndex(int width, int height) {
  if (height <= 0) {
    height = kDefaultThumbHeight;
  }
  if (width <= 0) {
    width = static_cast<int>((static_cast<int64_t>(height) * 3 + 2) / 5);
  }
  const std::string thumbPath = getThumbBmpPathForDimensions(cachePath, width, height);

  // Already generated, return true.
  if (nonEmptyFileExists(thumbPath)) {
    return true;
  }

  // Fast path A: a full spine/TOC cache already exists (the book was opened
  // before) — reuse its already-parsed coverItemHref via a cheap cached load.
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (bookMetadataCache->load()) {
    return convertCoverToThumbBmp(bookMetadataCache->coreMetadata.coverItemHref, thumbPath, width, height);
  }

  // Fast path B: no cache yet. Parse ONLY content.opf to locate the cover
  // image, skipping the expensive spine resolution + TOC pass + book.bin
  // build that load(buildIfMissing=true) would do. Passing a null
  // BookMetadataCache to parseContentOpf makes ContentOpfParser skip the
  // spine pass entirely (it's guarded by `if (cache)`), leaving just the
  // OPF parse. This is what keeps scrolling a collection of never-opened
  // books (e.g. Recently Added) from freezing on the "Loading" popup: a
  // book with no extractable cover now returns false in OPF-parse time
  // instead of after a full index build.
  bookMetadataCache.reset();  // null -> ContentOpfParser uses no cache
  setupCacheDir();            // ensure the cache dir exists for the temp + thumb files
  BookMetadataCache::BookMetadata meta;
  if (!parseContentOpf(meta)) {
    LOG_DBG("EBP", "generateThumbBmpNoIndex: could not parse content.opf for %s", filepath.c_str());
    return false;
  }
  return convertCoverToThumbBmp(meta.coverItemHref, thumbPath, width, height);
}

bool Epub::convertCoverToThumbBmp(const std::string& coverImageHref, const std::string& thumbPath, int width,
                                  int height, bool adaptiveContain) const {
  // v18.9.9.291: check for pre-baked thumbnail from optimizer.js.
  // Zero on-device PNG decode when present -- skips the 32 KB DEFLATE
  // window entirely. Falls through to legacy PNG/JPG path on miss so
  // books not run through the v291+ optimizer still work.
  {
    char preBakedPath[80];
    snprintf(preBakedPath, sizeof(preBakedPath),
             "META-INF/crumble-covers/thumb_%dx%d.bmp", width, height);
    size_t preBakedSize = 0;
    uint8_t* preBakedBytes = readItemContentsToBytes(preBakedPath, &preBakedSize, false);
    if (preBakedBytes != nullptr && preBakedSize > 62) {  // >header
      FsFile thumbBmp;
      bool ok = false;
      if (Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
        const size_t n = thumbBmp.write(preBakedBytes, preBakedSize);
        thumbBmp.close();
        ok = (n == preBakedSize);
      }
      free(preBakedBytes);
      if (ok) {
        LOG_INF("EBP", "convertCoverToThumbBmp: used pre-baked thumb %dx%d (%zu bytes)",
                width, height, preBakedSize);
        return true;
      }
      LOG_ERR("EBP", "convertCoverToThumbBmp: pre-baked found but write failed, falling through");
    } else if (preBakedBytes != nullptr) {
      free(preBakedBytes);
    }
  }

  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverJpg, 1024)) {
      LOG_ERR("EBP", "Failed to read thumbnail JPG item: %s", coverImageHref.c_str());
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    int THUMB_TARGET_WIDTH = width;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(
        coverJpg, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT, adaptiveContain, getCoverToneCurve());
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    thumbBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      Storage.remove(thumbPath.c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverPng, 1024)) {
      LOG_ERR("EBP", "Failed to read thumbnail PNG item: %s", coverImageHref.c_str());
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    int THUMB_TARGET_WIDTH = width;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(
        coverPng, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT, adaptiveContain, getCoverToneCurve());
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    thumbBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      Storage.remove(thumbPath.c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
  }

  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

bool Epub::isItemStored(const std::string& itemHref) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  uint16_t method = 0xFFFF;
  if (!ZipFile(filepath).getCompressionMethod(path.c_str(), &method)) {
    return false;  // unknown -> treat as not-stored (keeps the safe drop-BLE path)
  }
  return method == 0;  // 0 == ZIP STORED (no DEFLATE window needed to read)
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getCumulativeSpineItemSize called but cache not loaded");
    return 0;
  }

  return bookMetadataCache->getSpineCumulativeSize(spineIndex);
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Extract filename (remove #anchor)
  std::string target = href;
  size_t hashPos = target.find('#');
  if (hashPos != std::string::npos) target = target.substr(0, hashPos);

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
