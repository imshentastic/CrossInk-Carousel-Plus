#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Stable cache path based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // CrumBLE: series captured from the most recent parseContentOpf
  // pass. Not persisted to book.bin (cache version compatibility),
  // so getSeriesName/Index returns empty if load() reused the on-disk
  // cache. SeriesIndex caches across sessions instead.
  std::string lastSeriesName;
  std::string lastSeriesIndex;
  // CrumBLE 4.5.2: author captured by the cheap extractSeriesFromOpf
  // peek (which also reads dc:creator since it's in the same metadata
  // block). Lets the AuthorAlpha sort backfill keys for books whose
  // full book.bin cache hasn't been built yet (never opened).
  std::string lastAuthorPeek;

  void migrateLegacyCachePath(const std::string& cacheDir) const;
  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void parseCssFiles() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  static std::string cachePathForFilePath(const std::string& filepath, const std::string& cacheDir);
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // CrumBLE series detection (ported from aalu, MIT-licensed by Dave
  // Allie 2025; original repo: dawsonfi/aalu). Both values are
  // populated by parseContentOpf — empty when the OPF didn't declare
  // series, OR when load() short-circuited from the on-disk cache
  // (book.bin doesn't persist series fields; SeriesIndex handles
  // cross-session caching instead).
  const std::string& getSeriesName() const { return lastSeriesName; }
  const std::string& getSeriesIndex() const { return lastSeriesIndex; }
  // Lightweight OPF parse that captures ONLY series metadata into the
  // lastSeriesName/lastSeriesIndex fields. Doesn't touch book.bin so
  // it's safe to run on already-cached books without invalidating the
  // spine/TOC cache. Used by the lazy series-enrichment pass — the
  // caller reads getSeriesName()/getSeriesIndex() afterwards and
  // persists into SeriesIndex.  Returns true if the OPF was readable
  // (regardless of whether series info was found).
  bool extractSeriesFromOpf();
  // CrumBLE 4.5.2: author captured during the same extractSeriesFromOpf
  // peek (dc:creator lives in the same metadata block). Returns the
  // raw author string (not the sort key) -- caller normalises via
  // LibraryIndex::lastNameLowerForKey.
  const std::string& getLastAuthorPeek() const { return lastAuthorPeek; }
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  // Deprecated compatibility wrapper; forwards to getThumbBmpPath(0, height).
  [[deprecated("use getThumbBmpPath(int width, int height)")]]
  std::string getThumbBmpPath(int height) const;
  // Returns the thumbnail cache path. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  std::string getThumbBmpPath(int width, int height) const;
  // Returns a Minimal-style adaptive thumbnail path. Normal cover ratios fill
  // the requested box; unusual ratios are contained inside the box.
  std::string getAdaptiveThumbBmpPath(int width, int height) const;
  // Deprecated compatibility wrapper; forwards to generateThumbBmp(0, height).
  [[deprecated("use generateThumbBmp(int width, int height)")]]
  bool generateThumbBmp(int height) const;
  // Writes a thumbnail BMP to cache. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  // Returns false on missing cache/cover, unsupported image format, or conversion failure.
  bool generateThumbBmp(int width, int height) const;
  // Like generateThumbBmp, but does NOT build the full spine/TOC index
  // (book.bin) when the book hasn't been opened yet. Instead it parses only
  // content.opf to locate the cover image. Used by home/shelf cover loading
  // where scrolling past many never-opened books (e.g. the Recently Added
  // collection) would otherwise freeze the UI on full EPUB indexing — most
  // of which is wasted when the book turns out to have no extractable cover.
  // Returns false (in OPF-parse time) when there is no usable cover so the
  // caller can render a placeholder cheaply. Non-const because it (re)sets
  // the metadata cache and may run the OPF parse.
  bool generateThumbBmpNoIndex(int width, int height);
  // Writes a thumbnail that can either crop-to-fill or contain unusual cover
  // ratios, depending on the source image dimensions.
  bool generateAdaptiveThumbBmp(int width, int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  // CrumBLE: true if the item is STORED (uncompressed) in the EPUB zip. A STORED
  // chapter needs no 32 KB DEFLATE window to cold-load, so the reader can build
  // it in place while BLE is connected instead of dropping/re-enabling BLE
  // (which re-fragments the heap). Returns false if the method can't be read.
  bool isItemStored(const std::string& itemHref) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

  // CrumBLE 4.6: cover tone curve, applied to grayscale cover scanlines
  // before dither. Settable from setup() once SETTINGS is loaded, and again
  // whenever the user changes the setting. Static -- no per-Epub overhead.
  // 0=Off, 1=Mild, 2=Strong (see lib/ToneCurve).
  static void setCoverToneCurve(uint8_t v);
  static uint8_t getCoverToneCurve();

 private:
  bool generateThumbBmpInternal(int width, int height, bool adaptiveContain) const;
  // Shared cover-image -> 1-bit BMP conversion used by both the cached
  // (generateThumbBmpInternal) and no-index (generateThumbBmpNoIndex)
  // thumbnail paths. coverImageHref must already be resolved against the
  // EPUB's content base path. adaptiveContain selects crop-to-fill vs
  // contain-unusual-ratios behaviour. Returns false for empty/unsupported
  // covers.
  bool convertCoverToThumbBmp(const std::string& coverImageHref, const std::string& thumbPath, int width,
                              int height, bool adaptiveContain = false) const;
};
