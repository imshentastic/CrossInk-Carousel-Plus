#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <MemoryBudget.h>
#include <Serialization.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../reader/BookReadingStats.h"
#include "../reader/BookStatsActivity.h"
#include "activities/home/BookshelfPickerActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ChoicePromptActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "../reader/PrebakeManifest.h"
#include "../reader/PrebakeManifestViewerActivity.h"
#include "AddBooksToCollectionActivity.h"
#include "BookActions.h"
#include "BookMetadataViewerActivity.h"
#include "BookmarkStore.h"
#include "BookmarksHomeActivity.h"
#include "CollectionPickerActivity.h"
#include "CoverThumbStatus.h"
#include "SeriesMiniPickerActivity.h"
#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "SeriesIndex.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RearrangeCollectionsActivity.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "SortPickerActivity.h"
#include "components/UITheme.h"
#include "CollectionsStore.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraFlowTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t CAROUSEL_CACHE_MAGIC = 0x43434152;  // "CCAR"
constexpr uint16_t CAROUSEL_CACHE_VERSION = 4;
constexpr char CAROUSEL_CACHE_PATH[] = "/.crosspoint/home_carousel_cache.bin";
constexpr char CAROUSEL_CACHE_TMP_PATH[] = "/.crosspoint/home_carousel_cache.tmp";

// Below this largest-contiguous-block size, shelf cover generation drops the
// Flow home's 48 KB fast-path snapshot buffers to free room for the cover
// extractor's DEFLATE inflate window (up to 32 KB) plus its read/output/decoder
// scratch. Sized with headroom over 32 KB so a compressed cover JPEG or the
// book's content.opf can be inflated without OOM.
constexpr uint32_t kCoverGenMinContiguousHeap = 40 * 1024;

enum class HomeMenuAction {
  BrowseFiles,
  ContinueReading,
  RecentBooks,
  OpdsBrowser,
  ReadingStats,
  Bookmarks,
  FileTransfer,
  Settings,
};

struct HomeMenuEntry {
  const char* label;
  UIIcon icon;
  HomeMenuAction action;
};

struct CarouselCacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t frameCount;
  uint32_t frameBufferSize;
  uint64_t keyHash;
  uint16_t screenWidth;
  uint16_t screenHeight;
  uint16_t centerCoverW;
  uint16_t centerCoverH;
  uint16_t sideCoverW;
  uint16_t sideCoverH;
};

uint64_t fnvHash64(const std::string& s) {
  uint64_t hash = 14695981039346656037ull;
  for (char c : s) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool hasAnyBookStats(const BookReadingStats& stats) {
  return stats.sessionCount > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 || stats.isCompleted;
}

bool hasAnyGlobalStats(const GlobalReadingStats& stats) {
  return stats.totalSessions > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 ||
         stats.completedBooks > 0;
}

void appendHashedFileStateToKey(std::string& key, const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("HOME", path, file)) {
    key += "missing";
    key += '\0';
    return;
  }

  uint64_t hash = 14695981039346656037ull;
  size_t totalBytes = 0;
  uint8_t buffer[64];
  while (true) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;
    totalBytes += static_cast<size_t>(bytesRead);
    for (int i = 0; i < bytesRead; ++i) {
      hash ^= buffer[i];
      hash *= 1099511628211ull;
    }
  }
  file.close();

  char digest[48];
  snprintf(digest, sizeof(digest), "%zu:%" PRIu64, totalBytes, static_cast<uint64_t>(hash));
  key += digest;
  key += '\0';
}

std::string getRecentBookCachePath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub::cachePathForFilePath(book.path, "/.crosspoint");
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return "/.crosspoint/txt_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  return "";
}

BookReadingStats loadRecentBookStats(const RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path)) {
    return BookReadingStats{};
  }

  const std::string cachePath = getRecentBookCachePath(book);
  return BookReadingStats::load(cachePath);
}

void updateRecentBookCoverPath(const RecentBook& book, const std::string& coverBmpPath) {
  if (!RECENT_BOOKS.updateBook(book.path, book.title, book.author, coverBmpPath)) {
    LOG_ERR("HOME", "failed to update recent book metadata: %s", book.path.c_str());
  }
}

bool hasThumbnailPlaceholder(const std::string& coverBmpPath) {
  return coverBmpPath.find("[WIDTH]") != std::string::npos || coverBmpPath.find("[HEIGHT]") != std::string::npos;
}

// A cover thumbnail counts as present only if it exists AND parses as a valid
// BMP with non-zero dimensions — the exact test the Lyra Carousel renderer
// applies before drawing it (LyraCarouselTheme::drawRecentBookCover). A file
// that exists but won't parse (e.g. a truncated/corrupt thumb left by an older
// build, or a partial write) is otherwise trusted by Storage.exists(), so
// generation is skipped and the carousel falls back to the placeholder forever
// — even though the same book's cover renders fine in every other theme, which
// use different-dimension thumbs. Deleting the bad file here forces a one-shot
// regeneration. Using the renderer's own criteria means we never reject a thumb
// the renderer would have accepted (no needless regen churn).
bool carouselThumbMissingOrInvalid(const std::string& thumbPath) {
  if (thumbPath.empty()) return true;
  FsFile file;
  if (!Storage.openFileForRead("HOME", thumbPath, file)) return true;  // genuinely missing
  Bitmap bitmap(file);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  file.close();
  if (!valid) {
    LOG_INF("HOME", "carousel: invalid cover thumb, deleting to regenerate: %s", thumbPath.c_str());
    Storage.remove(thumbPath.c_str());
  }
  return !valid;
}

std::string getReusableCoverPath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return Xtc(book.path, "/.crosspoint").getThumbBmpPath();
  }
  return book.coverBmpPath;
}

bool ensureReusableCoverPath(RecentBook& book) {
  // Already the reusable template ([WIDTH]x[HEIGHT] placeholder) — leave it.
  if (hasThumbnailPlaceholder(book.coverBmpPath)) {
    return false;
  }

  // Intentionally fall through when coverBmpPath is EMPTY. A book whose cover
  // generation transiently failed (e.g. cover inflate OOM'd under heap
  // pressure) had its stored path cleared to "" — and loadRecentCovers skips
  // books with an empty path, so it could never recover and stayed a
  // placeholder forever. Recomputing the deterministic template path here lets
  // the (self-healing) generation run again. For EPUB/XTC getReusableCoverPath
  // returns the template from the book path; for anything else it returns the
  // stored (empty) value, so the guard below still no-ops those.
  const std::string reusablePath = getReusableCoverPath(book);
  if (reusablePath.empty() || reusablePath == book.coverBmpPath) {
    return false;
  }

  book.coverBmpPath = reusablePath;
  updateRecentBookCoverPath(book, reusablePath);
  return true;
}

std::vector<HomeMenuEntry> buildHomeMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  std::vector<HomeMenuEntry> items = {
      {tr(STR_BROWSE_FILES), Folder, HomeMenuAction::BrowseFiles},
      {tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks},
  };

  if (hasOpdsServers) {
    items.push_back({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasReadingStats) {
    items.push_back({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }
  if (hasBookmarks) {
    items.push_back({tr(STR_BOOKMARKS), BookmarkIcon, HomeMenuAction::Bookmarks});
  }

  items.push_back({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  items.push_back({tr(STR_SETTINGS_TITLE), Settings, HomeMenuAction::Settings});
  return items;
}

std::vector<HomeMenuEntry> buildMinimalMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  std::vector<HomeMenuEntry> items = {
      {tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks},
  };

  if (hasOpdsServers) {
    items.push_back({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasBookmarks) {
    items.push_back({tr(STR_BOOKMARKS), BookmarkIcon, HomeMenuAction::Bookmarks});
  }
  if (hasReadingStats) {
    items.push_back({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }

  items.push_back({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  return items;
}

std::vector<HomeMenuEntry> buildSelectableHomeMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks,
                                                        bool includeContinueReading) {
  auto items = buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
  if (includeContinueReading) {
    items.insert(items.begin(), {tr(STR_CONTINUE_READING), Book, HomeMenuAction::ContinueReading});
  }
  return items;
}

HomeMenuAction homeActionForInitialMenuItem(HomeMenuItem item) {
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      return HomeMenuAction::BrowseFiles;
    case HomeMenuItem::RECENTS:
      return HomeMenuAction::RecentBooks;
    case HomeMenuItem::OPDS_BROWSER:
      return HomeMenuAction::OpdsBrowser;
    case HomeMenuItem::FILE_TRANSFER:
      return HomeMenuAction::FileTransfer;
    case HomeMenuItem::SETTINGS_MENU:
      return HomeMenuAction::Settings;
    case HomeMenuItem::NONE:
    default:
      return HomeMenuAction::ContinueReading;
  }
}

int findMenuActionIndex(const std::vector<HomeMenuEntry>& items, HomeMenuAction action) {
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    if (items[i].action == action) {
      return i;
    }
  }
  return -1;
}

bool isMinimalTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::MINIMAL;
}

bool isAnyFrontButtonPressed(const MappedInputManager& mappedInput) {
  return mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}

int minimalHomeNavCount(const bool hasCurrentBook) { return hasCurrentBook ? 4 : 3; }

int minimalHomeCoverWidth(int coverHeight) {
  (void)coverHeight;
  return MinimalMetrics::homeCoverImageWidth;
}

int minimalHomeCoverHeight(int coverHeight) {
  (void)coverHeight;
  return MinimalMetrics::homeCoverImageHeight;
}

std::string minimalHomeCoverPath(const RecentBook& book, int coverHeight) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint")
        .getAdaptiveThumbBmpPath(minimalHomeCoverWidth(coverHeight), minimalHomeCoverHeight(coverHeight));
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, minimalHomeCoverWidth(coverHeight),
                                    minimalHomeCoverHeight(coverHeight));
}

void appendCarouselCoverStateToKey(std::string& key, const RecentBook& book) {
  key += book.path;
  key += '\0';
  key += book.coverBmpPath;
  key += '\0';

  if (book.coverBmpPath.empty()) {
    key += "0:0";
    key += '\0';
    return;
  }

  const std::string centerPath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH);
  const std::string sidePath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
  key += Storage.exists(centerPath.c_str()) ? '1' : '0';
  key += ':';
  key += Storage.exists(sidePath.c_str()) ? '1' : '0';
  key += '\0';

  const std::string cachePath = getRecentBookCachePath(book);
  if (!cachePath.empty()) {
    appendHashedFileStateToKey(key, cachePath + "/progress.bin");
    if (FsHelpers::hasEpubExtension(book.path)) {
      appendHashedFileStateToKey(key, cachePath + "/stats.bin");
    }
  } else {
    key += "no-cache-path";
    key += '\0';
  }
}

void buildCarouselCacheKey(const std::vector<RecentBook>& recentBooks, std::string& key, uint64_t& keyHash) {
  key.clear();
  key.reserve(512);
  for (const auto& book : recentBooks) {
    appendCarouselCoverStateToKey(key, book);
  }
  appendHashedFileStateToKey(key, "/.crosspoint/global_stats.bin");
  keyHash = fnvHash64(key);
}

bool isCarouselCacheHeaderValid(const CarouselCacheHeader& header, uint64_t cacheKeyHash, int bookCount,
                                const GfxRenderer& renderer) {
  return header.magic == CAROUSEL_CACHE_MAGIC && header.version == CAROUSEL_CACHE_VERSION &&
         header.keyHash == cacheKeyHash && header.frameCount == bookCount &&
         header.frameBufferSize == renderer.getBufferSize() && header.screenWidth == renderer.getScreenWidth() &&
         header.screenHeight == renderer.getScreenHeight() && header.centerCoverW == LyraCarouselTheme::kCenterThumbW &&
         header.centerCoverH == LyraCarouselTheme::kCenterThumbH &&
         header.sideCoverW == LyraCarouselTheme::kSideCoverW && header.sideCoverH == LyraCarouselTheme::kSideCoverH;
}

bool readCarouselCacheHeader(FsFile& file, CarouselCacheHeader& header) {
  CarouselCacheHeader readHeader{};
  if (!serialization::tryReadPod(file, readHeader)) {
    return false;
  }
  header = readHeader;
  return true;
}

bool hasValidCarouselDiskCache(const std::vector<RecentBook>& recentBooks, const GfxRenderer& renderer) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount <= 0) return false;

  std::string cacheKey;
  uint64_t cacheKeyHash = 0;
  buildCarouselCacheKey(recentBooks, cacheKey, cacheKeyHash);

  FsFile cacheFile;
  if (!Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, cacheFile)) {
    return false;
  }

  CarouselCacheHeader header{};
  const bool readOk = readCarouselCacheHeader(cacheFile, header);
  cacheFile.close();
  return readOk && isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer);
}

int getHomeMenuSelectionOffset(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size());
}

// Small centered toast — mirrors the helper in FileBrowserActivity.cpp.
// Local to this TU because the file-browser version is also file-local.
void drawHomeToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
}
}  // namespace

// ---------------------------------------------------------------------------
// Static carousel frame cache — survives HomeActivity re-creation so that
// returning to home (e.g. after settings) doesn't re-read covers from SD.
// Freed explicitly in onSelectBook() before entering the reader.
// ---------------------------------------------------------------------------
namespace {
class CarouselCache {
 public:
  uint8_t* frames[HomeActivity::kCarouselFrameCount] = {};
  int frameBookIdx[HomeActivity::kCarouselFrameCount] = {-1};
  int frameCount = 0;
  int lastCenterIdx = -1;
  std::string key;
  uint64_t keyHash = 0;

  int findFrameSlot(int bookIdx) const {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frameBookIdx[i] == bookIdx && frames[i] != nullptr) return i;
    }
    return -1;
  }

  void invalidate() {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frames[i]) {
        free(frames[i]);
        frames[i] = nullptr;
      }
      frameBookIdx[i] = -1;
    }
    frameCount = 0;
    lastCenterIdx = -1;
    key.clear();
    keyHash = 0;
  }
};

CarouselCache gCarouselCache;
}  // namespace

static_assert(HomeActivity::kMaxCachedBooks >= LyraCarouselMetrics::values.homeRecentBooksCount,
              "kMaxCachedBooks must cover all carousel slots");

// CrumBLE #120: cursor-recall state lives in static storage so it
// outlives the per-transition HomeActivity instance (replaceActivity
// destroys + recreates the activity on every home <-> other-activity
// jump). See HomeActivity.h for the rationale.
bool HomeActivity::hasSavedCursor_ = false;
int HomeActivity::savedSelectorIndex_ = 0;
int HomeActivity::savedLastCarouselBookIndex_ = 0;
int HomeActivity::savedLastShelfBookIndex_ = 0;
int HomeActivity::savedLastMenuIndex_ = 0;
bool HomeActivity::savedShelfHeaderFocused_ = false;
std::unordered_map<std::string, HomeActivity::ShelfPos> HomeActivity::savedShelfPosByCollection_;

void HomeActivity::clearSavedCursor() {
  // CrumBLE #120: hard reset, not just the flag, so a stale snapshot
  // can't leak into a later restore if the flag ever gets flipped back
  // on independently.
  hasSavedCursor_ = false;
  savedSelectorIndex_ = 0;
  savedLastCarouselBookIndex_ = 0;
  savedLastShelfBookIndex_ = 0;
  savedLastMenuIndex_ = 0;
  savedShelfHeaderFocused_ = false;
  savedShelfPosByCollection_.clear();
}

int HomeActivity::getMenuItemCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count += recentBooks.size();
  } else if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count++;  // Continue Reading menu item
  }
  if (hasOpdsServers) {
    count++;
  }
  if (hasReadingStats) {
    count++;
  }
  if (hasBookmarks) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& storedBook : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    RecentBook book = storedBook;
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    ensureReusableCoverPath(book);
    recentBooks.push_back(book);
  }
}

void HomeActivity::loadAllBookStats() {
  const auto start = millis();
  const int count = std::min(static_cast<int>(recentBooks.size()), kMaxCachedBooks);
  for (int i = 0; i < count; ++i) {
    cachedBookStats[i] = loadRecentBookStats(recentBooks[i]);
    cachedBookProgress[i] = RecentBookProgress::loadPercent(recentBooks[i]);
  }
  bookStatsCached = true;
  LOG_DBG("HOME", "carousel: cached stats/progress for %d book(s) in %lums", count, millis() - start);
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Free the ~48KB cover-buffer snapshot before generating any covers. This
  // pass runs at end-of-render, AFTER storeCoverBuffer() has allocated that
  // snapshot — which leaves too little CONTIGUOUS heap for the ~32KB zlib
  // inflate window that extracting a cover image from the EPUB zip needs. (The
  // shelf/Collections loader succeeds only because it runs earlier, before the
  // snapshot exists — which is why covers show there but not on the carousel.)
  // Releasing it here lets recent-book covers regenerate; the next render
  // re-snapshots. Without this, a freshly-opened book stays a placeholder.
  freeCoverBuffer();

  // Also reclaim the carousel frame cache (~52 KB) before decoding covers.
  // Cover generation needs a large contiguous block — a ~32 KB zip-inflate
  // window plus the image decoder (PNG ~42 KB, JPEG ~17 KB). With the frame
  // cache resident, the single largest cover (often a PNG) OOMs every pass and
  // is stranded on a placeholder forever, while smaller covers succeed. The
  // repeated failure popups also drop the Flow home fast-path cache, which
  // makes navigation feel sluggish. The next render re-warms the frame from the
  // freshly generated BMP thumbnails (cheap — no re-decode). Mirrors the
  // free order used in onExit().
  gCarouselCache.invalidate();
  freeCarouselFrames();
  carouselFramesReady = false;

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const bool isMinimal = isMinimalTheme();
  const size_t recentBookCount = recentBooks.size();
  // Tracks which book indices had a thumbnail generated this pass.
  std::vector<char> bookUpdated(recentBookCount, false);
  const int progressIncrement = 90 / static_cast<int>(std::max<size_t>(1, recentBookCount));

  int progress = 0;
  for (size_t bookIdx = 0; bookIdx < recentBooks.size(); ++bookIdx) {
    RecentBook& book = recentBooks[bookIdx];
    if (!Storage.exists(book.path.c_str())) {
      progress++;
      continue;
    }
    // Books we've previously failed to thumbnail render the placeholder
    // path and never trigger the Loading popup. coverBmpPath would have
    // been cleared at the time of failure (recent.json persisted "")
    // but, defense in depth: also short-circuit here so a re-derivation
    // anywhere upstream can't reanimate the retry loop.
    //
    // CrumBLE 4.3-rc2 fix: previously this only checked the carousel
    // center-thumb dimensions (296x468). For non-carousel themes (flow,
    // minimal) the failed-gen marker is written at completely different
    // dimensions (e.g. 192x320 for the standard flow theme), so the
    // marker existed but the early-bail never matched -- every Home
    // render re-attempted the failing gen, dropping the fast-path cache
    // and re-indexing on every navigation. Check whichever dimension
    // the active theme would actually try to generate this pass.
    int markerCheckW = LyraCarouselTheme::kCenterThumbW;
    int markerCheckH = LyraCarouselTheme::kCenterThumbH;
    if (!isCarouselTheme) {
      if (isMinimal) {
        markerCheckW = minimalHomeCoverWidth(coverHeight);
        markerCheckH = minimalHomeCoverHeight(coverHeight);
      } else {
        // Standard flow: 3/5 aspect (matches the thumbW formula used in
        // the non-carousel gen path on line ~739).
        markerCheckW = static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
        markerCheckH = coverHeight;
      }
    }
    if (CoverThumbStatus::isMarkedFailed(book.path, markerCheckW, markerCheckH)) {
      progress++;
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      if (isCarouselTheme) {
        // For carousel: generate exact-size thumbnails for the center image rect and side slots.
        // Load the source image once even when both sizes are missing.
        const std::string centerPath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW,
                                                                  LyraCarouselTheme::kCenterThumbH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                LyraCarouselTheme::kSideCoverH);
        // Validate (not just exists): a corrupt/truncated thumb must be treated
        // as missing so it regenerates, else the carousel is stuck on a
        // placeholder while other themes (different thumb sizes) show the cover.
        const bool centerMissing = carouselThumbMissingOrInvalid(centerPath);
        const bool sideMissing = carouselThumbMissingOrInvalid(sidePath);

        if (centerMissing || sideMissing) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
            // Self-healing: generateThumbBmpNoIndex extracts the cover via an
            // OPF-only parse, regenerating even if the cache folder is missing
            // — instead of bailing on load(false) and clearing the cover path
            // (which left the carousel stuck on a placeholder forever).
            bool success = true;
            if (centerMissing)
              success = epub.generateThumbBmpNoIndex(LyraCarouselTheme::kCenterThumbW,
                                                     LyraCarouselTheme::kCenterThumbH) &&
                        success;
            if (sideMissing)
              success = epub.generateThumbBmpNoIndex(LyraCarouselTheme::kSideCoverW,
                                                     LyraCarouselTheme::kSideCoverH) &&
                        success;
            if (!success) {
              // Log heap at the point of failure so a serial capture can tell
              // OOM (low free/maxAlloc -> headroom problem) from a genuinely
              // undecodable cover (ample heap -> format issue).
              LOG_ERR("HOME", "carousel cover gen failed: %s (free=%u maxAlloc=%u)", book.path.c_str(),
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
              updateRecentBookCoverPath(book, "");
              book.coverBmpPath = "";
              if (centerMissing)
                CoverThumbStatus::markFailed(book.path, LyraCarouselTheme::kCenterThumbW,
                                             LyraCarouselTheme::kCenterThumbH);
              if (sideMissing)
                CoverThumbStatus::markFailed(book.path, LyraCarouselTheme::kSideCoverW,
                                             LyraCarouselTheme::kSideCoverH);
            } else {
              bookUpdated[bookIdx] = true;
              if (centerMissing)
                CoverThumbStatus::clearFailed(book.path, LyraCarouselTheme::kCenterThumbW,
                                              LyraCarouselTheme::kCenterThumbH);
              if (sideMissing)
                CoverThumbStatus::clearFailed(book.path, LyraCarouselTheme::kSideCoverW,
                                              LyraCarouselTheme::kSideCoverH);
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              if (!showingLoading) {
                showingLoading = true;
                popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
              }
              GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
              bool success = true;
              if (centerMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) && success;
              if (sideMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
              if (!success) {
                updateRecentBookCoverPath(book, "");
                book.coverBmpPath = "";
                if (centerMissing)
                  CoverThumbStatus::markFailed(book.path, LyraCarouselTheme::kCenterThumbW,
                                               LyraCarouselTheme::kCenterThumbH);
                if (sideMissing)
                  CoverThumbStatus::markFailed(book.path, LyraCarouselTheme::kSideCoverW,
                                               LyraCarouselTheme::kSideCoverH);
              } else {
                bookUpdated[bookIdx] = true;
                if (centerMissing)
                  CoverThumbStatus::clearFailed(book.path, LyraCarouselTheme::kCenterThumbW,
                                                LyraCarouselTheme::kCenterThumbH);
                if (sideMissing)
                  CoverThumbStatus::clearFailed(book.path, LyraCarouselTheme::kSideCoverW,
                                                LyraCarouselTheme::kSideCoverH);
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      } else {
        // Non-carousel: generate the active theme's thumbnail size.
        const bool useMinimalThumb =
            isMinimal && (FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasXtcExtension(book.path));
        const std::string coverPath = useMinimalThumb ? minimalHomeCoverPath(book, coverHeight)
                                                      : UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
            // W,H the gen attempt below will resolve to -- mirrored here so the
            // per-size CoverThumbStatus marker matches the dims of the actual
            // generateThumbBmp*() call (otherwise the marker is silently ignored).
            const int thumbW = useMinimalThumb
                                   ? minimalHomeCoverWidth(coverHeight)
                                   : static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
            const int thumbH = useMinimalThumb ? minimalHomeCoverHeight(coverHeight) : coverHeight;
            bool success;
            if (useMinimalThumb) {
              // Minimal uses an ADAPTIVE thumbnail (contain unusual ratios),
              // written to a distinct *_fit.bmp path and requiring the book's
              // metadata cache — so we load it first. Not OPF-only self-healing,
              // but minimal recent covers are a niche path.
              if (!epub.load(false, true)) {
                LOG_ERR("HOME", "failed to load EPUB cache for thumb generation: %s", book.path.c_str());
                updateRecentBookCoverPath(book, "");
                book.coverBmpPath = "";
                // Persist "give up" — repeated failed load() attempts cost
                // a Loading popup + a second of SD I/O per visit. If a
                // future build improves cache load, the user can clear
                // /.crosspoint/<hash>/thumb_failed.marker manually.
                CoverThumbStatus::markFailed(book.path, thumbW, thumbH);
                coverRendered = false;
                requestUpdate();
                progress++;
                continue;
              }
              success = epub.generateAdaptiveThumbBmp(minimalHomeCoverWidth(coverHeight),
                                                      minimalHomeCoverHeight(coverHeight));
            } else {
              // Flow / standard: self-healing OPF-only generation (see carousel
              // branch) — regenerates even if the cache folder is missing,
              // which is the fix for recent-book covers stuck on placeholders
              // while the shelf (already on this path) showed them fine.
              success = epub.generateThumbBmpNoIndex(0, coverHeight);
            }
            if (!success) {
              LOG_ERR("HOME", "recent cover gen failed: %s (free=%u maxAlloc=%u)", book.path.c_str(),
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
              updateRecentBookCoverPath(book, "");
              book.coverBmpPath = "";
              CoverThumbStatus::markFailed(book.path, thumbW, thumbH);
            } else {
              bookUpdated[bookIdx] = true;  // non-carousel path reuses same tracking
              CoverThumbStatus::clearFailed(book.path, thumbW, thumbH);
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              if (!showingLoading) {
                showingLoading = true;
                popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
              }
              GUI.fillPopupProgress(renderer, popupRect, 10 + progress * progressIncrement);
              // Mirror the W,H the gen attempt resolves to: Xtc::generateThumbBmp(h)
              // derives width as static_cast<uint16_t>(h * 0.6).
              const int thumbW = useMinimalThumb ? minimalHomeCoverWidth(coverHeight)
                                                 : static_cast<int>(static_cast<uint16_t>(coverHeight * 0.6));
              const int thumbH = useMinimalThumb ? minimalHomeCoverHeight(coverHeight) : coverHeight;
              const bool success =
                  useMinimalThumb ? xtc.generateThumbBmp(static_cast<uint16_t>(minimalHomeCoverWidth(coverHeight)),
                                                         static_cast<uint16_t>(minimalHomeCoverHeight(coverHeight)))
                                  : xtc.generateThumbBmp(coverHeight);
              if (!success) {
                updateRecentBookCoverPath(book, "");
                book.coverBmpPath = "";
                CoverThumbStatus::markFailed(book.path, thumbW, thumbH);
              } else {
                bookUpdated[bookIdx] = true;
                CoverThumbStatus::clearFailed(book.path, thumbW, thumbH);
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  // Re-render only the affected slots rather than rebuilding the entire cache.
  if (isCarouselTheme) {
    bool anyUpdated = false;
    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (static_cast<size_t>(i) >= bookUpdated.size() || !bookUpdated[i]) continue;
      anyUpdated = true;
      if (carouselFramesReady) {
        // Only re-render the slot holding this book; books outside the window
        // will be picked up by updateSlidingWindowCache on next navigation.
        const int slot = gCarouselCache.findFrameSlot(i);
        if (slot >= 0) renderCarouselFrame(i, slot);
      }
    }
    if (anyUpdated) {
      if (!carouselFramesReady) {
        // Cover assets changed before the carousel cache was initialised, so
        // any existing SD snapshot may still contain placeholder frames.
        // Force a rebuild from the fresh thumbs instead of reusing stale
        // `home_carousel_cache.bin` content keyed only by book order/layout.
        if (Storage.exists(CAROUSEL_CACHE_PATH)) {
          Storage.remove(CAROUSEL_CACHE_PATH);
        }
        if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
          Storage.remove(CAROUSEL_CACHE_TMP_PATH);
        }
        preRenderCarouselFrames();
      } else {
        // The live carousel frames are already updated above. Keep Home
        // responsive by invalidating any stale SD snapshot instead of
        // rewriting all 5 frames synchronously on this return-to-Home path.
        if (Storage.exists(CAROUSEL_CACHE_PATH)) {
          Storage.remove(CAROUSEL_CACHE_PATH);
        }
        if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
          Storage.remove(CAROUSEL_CACHE_TMP_PATH);
        }
      }
      requestUpdate();
    }
  }

  // CrumBLE #125: Flow path -- if any recent's BMP was just generated
  // (the carousel branch above is LYRA_CAROUSEL-only; Flow lands here
  // via the generic path at line ~738), re-bake the side-tile cache
  // so the perspective fast-path picks up the new covers instead of
  // staying on the slow drawPerspectiveBitmap fallback. Bounded to one
  // tile-bake pass per loadRecentCovers invocation.
  if (!isCarouselTheme &&
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
    bool anyFlowUpdated = false;
    for (size_t i = 0; i < bookUpdated.size(); ++i) {
      if (bookUpdated[i]) {
        anyFlowUpdated = true;
        break;
      }
    }
    if (anyFlowUpdated) {
      static_cast<const LyraFlowTheme&>(GUI).prerenderCarouselSideTiles(renderer, recentBooks);
      requestUpdate();
    }
  }

  // CrumBLE #125: Reading Stats covers. The carousel/flow loop above only
  // generates covers for the top homeRecentBooksCount books (Home's
  // displayed range). But the Stats screen's L/R nav cycles through
  // every book in RECENT_BOOKS (up to MAX_RECENT_BOOKS = 18 with
  // sessionCount > 0) at the same dimensions Home uses (220x320 on
  // Flow). Without this pass, books beyond the Home window render as
  // blank-cover entries in Stats. Generate the missing thumbs here so
  // Stats always has a cover -- bounded one-time cost per book (~100-
  // 500 ms each on first hit). Subsequent home entries: pure
  // Storage.exists() checks, near-instant. Failed books are tracked
  // via CoverThumbStatus so we don't retry forever on undecodable
  // covers.
  if (!isMinimal) {
    // Mirrors UITheme::getCoverThumbPath(coverBmpPath, coverHeight) and
    // Epub::generateThumbBmpNoIndex(0, coverHeight) -- both derive W from H
    // via (H*3+2)/5. For XTC we resolve the marker per-call below since
    // generateThumbBmp(h) derives W via static_cast<uint16_t>(h*0.6) and
    // can round to a different value at certain heights.
    const int statsThumbWEpub = static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
    const auto& allRecents = RECENT_BOOKS.getBooks();
    for (const RecentBook& sb : allRecents) {
      if (sb.coverBmpPath.empty()) continue;
      if (CoverThumbStatus::isMarkedFailed(sb.path, statsThumbWEpub, coverHeight)) continue;
      const std::string thumbPath = UITheme::getCoverThumbPath(sb.coverBmpPath, coverHeight);
      if (thumbPath.empty() || Storage.exists(thumbPath.c_str())) continue;
      if (!Storage.exists(sb.path.c_str())) continue;  // book deleted from SD
      // Memory guard before opening the EPUB/XTC -- generation is
      // heap-heavy (decoder + zlib window).
      if (!MemoryBudget::hasHeapForSeriesScan()) {
        LOG_DBG("HOME", "Stats cover gen deferred: low heap (free=%u)", ESP.getFreeHeap());
        break;
      }
      if (!showingLoading) {
        showingLoading = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      bool success = false;
      int attemptedW = statsThumbWEpub;
      if (FsHelpers::hasEpubExtension(sb.path)) {
        Epub epub(sb.path, "/.crosspoint");
        success = epub.generateThumbBmpNoIndex(0, coverHeight);
      } else if (FsHelpers::hasXtcExtension(sb.path)) {
        Xtc xtc(sb.path, "/.crosspoint");
        attemptedW = static_cast<int>(static_cast<uint16_t>(coverHeight * 0.6));
        if (xtc.load()) {
          success = xtc.generateThumbBmp(coverHeight);
        }
      }
      if (!success) {
        LOG_DBG("HOME", "Stats cover gen failed: %s (free=%u)", sb.path.c_str(), ESP.getFreeHeap());
        CoverThumbStatus::markFailed(sb.path, attemptedW, coverHeight);
      }
    }
  }
}

void HomeActivity::enrichActiveCollectionForSeries() {
  // Only meaningful on Flow theme (only theme with a shelf).
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) != CrossPointSettings::UI_THEME::LYRA_FLOW) {
    seriesEnrichmentNeededForActive = false;
    return;
  }
  // Global opt-in gate. When off, the OPF parse never runs — most
  // user libraries don't have Calibre / EPUB-3 series metadata so
  // the expensive first-time scan would yield no value. User can
  // enable in Settings → Series Detection.
  if (!SETTINGS.seriesDetectionEnabled) {
    seriesEnrichmentNeededForActive = false;
    return;
  }
  const Collection* active = CollectionsStore::getInstance().getActiveCollection();
  if (active == nullptr || !active->collapseSeries) {
    seriesEnrichmentNeededForActive = false;
    return;
  }
  const std::vector<std::string> paths =
      CollectionsStore::getInstance().resolveBookPaths(active->id);
  if (paths.empty()) return;

  // First pass: how many EPUBs need parsing? Avoids drawing a popup
  // when everything's already cached.
  std::vector<std::string> toCheck;
  toCheck.reserve(paths.size());
  for (const auto& p : paths) {
    if (!FsHelpers::hasEpubExtension(p)) continue;
    if (SeriesIndex::getInstance().hasBeenChecked(p)) continue;
    toCheck.push_back(p);
  }
  if (toCheck.empty()) {
    seriesEnrichmentNeededForActive = false;
    return;
  }

  // Memory guard BEFORE drawing the popup or scanning. Parsing OPF + growing
  // and persisting SeriesIndex needs headroom; on a large collection a low-heap
  // scan can OOM mid-pass, and since this re-runs on every home render until it
  // completes, that becomes a crash-loop the user can't escape (they can't
  // reach Settings to disable the feature). If headroom is too low, skip this
  // pass entirely — no popup, no scan, no crash — and retry on a later render
  // once heap frees up. Leaving seriesEnrichmentNeededForActive set means we'll
  // try again rather than silently giving up.
  if (!MemoryBudget::hasHeapForSeriesScan()) {
    LOG_DBG("HOME", "Series scan deferred: low heap (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return;
  }

  const Rect popupRect = GUI.drawPopup(renderer, "Detecting series...");
  // Popup drawn over the framebuffer; flag the frame so the end-of-render
  // snapshot is skipped and the follow-up render erases it (see
  // homeRenderPopupShown). Otherwise the "Detecting series..." popup can get
  // stuck over the carousel the same way the shelf Loading popup did.
  homeRenderPopupShown = true;
  const int total = static_cast<int>(toCheck.size());
  int processed = 0;
  for (const auto& p : toCheck) {
    // Per-book memory guard: heap can drop as the index grows over a long scan.
    // Stop this pass before an allocation fails rather than OOM-crashing; the
    // books processed so far are persisted (record() saves incrementally), so
    // the next pass resumes from here.
    if (!MemoryBudget::hasHeapForSeriesScan()) {
      LOG_DBG("HOME", "Series scan stopped mid-pass: low heap after %d books", processed);
      break;
    }
    // Mark this book checked BEFORE the risky parse. extractSeriesFromOpf
    // returns false gracefully for a missing/odd OPF, but a hard crash inside
    // the parser (e.g. malformed XML) would otherwise leave the book unrecorded
    // and re-trigger the same crash on every boot — an inescapable loop. By
    // recording it first, a crash mid-parse still leaves it "checked", so the
    // next boot skips it and the home screen recovers.
    SeriesIndex::getInstance().record(p, "", "");
    Epub epub(p, "/.crosspoint");
    // extractSeriesFromOpf doesn't touch book.bin — safe to call on
    // books with or without an existing cache. On success, overwrite the
    // placeholder record with the real series name/index.
    if (epub.extractSeriesFromOpf()) {
      SeriesIndex::getInstance().record(p, epub.getSeriesName(), epub.getSeriesIndex());
    }
    processed++;
    GUI.fillPopupProgress(renderer, popupRect, 5 + (processed * 90) / total);
  }
  seriesEnrichmentNeededForActive = false;
  // ShelfEntries derived from the new SeriesIndex state — bust the
  // path cache so the next resolveShelfEntries sees fresh data.
  invalidateShelfPathsCache();
  shelfSnapshotValid = false;
  lastRenderedCoverSelectorValid = false;
}

void HomeActivity::loadShelfCovers(int cellWidth, int cellHeight, int scrollOffset, int visibleCount) {
  // No-op for themes other than LYRA_FLOW (only theme that has a shelf).
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) != CrossPointSettings::UI_THEME::LYRA_FLOW) {
    shelfCoversLoaded = true;
    return;
  }
  // Pull the live book list via the per-frame cache so we don't re-sort
  // the LibraryIndex on every render (was the dominant cause of laggy
  // home-screen navigation with "All Books" active).
  const std::vector<std::string>& allPaths = cachedShelfPaths();
  if (allPaths.empty()) {
    shelfCoversLoaded = true;
    return;
  }
  if (cellWidth <= 0 || cellHeight <= 0 || visibleCount <= 0) {
    shelfCoversLoaded = true;
    return;
  }

  // Build the window slice. Clamping protects against scroll offsets
  // that drifted past the new active collection's length (e.g. user
  // cycled from All Books down to a small Favorites).
  const int total = static_cast<int>(allPaths.size());
  const int start = std::clamp(scrollOffset, 0, total);
  const int end = std::min(start + visibleCount, total);
  if (start >= end) {
    shelfCoversLoaded = true;
    return;
  }

  bool showingLoading = false;
  Rect popupRect;
  const int windowSize = end - start;
  const int progressIncrement = 90 / std::max(1, windowSize);
  int processed = 0;

  for (int i = start; i < end; ++i) {
    const auto& bookPath = allPaths[i];
    if (!Storage.exists(bookPath.c_str())) {
      processed++;
      continue;
    }
    // A book whose thumb generation already failed this session: render it
    // blank, never retry. Retrying every render is what produced the
    // flashing loop (failed gen -> no file -> "missing" again next render
    // -> popup + requestUpdate -> repeat).
    if (std::find(failedShelfCovers.begin(), failedShelfCovers.end(), bookPath) != failedShelfCovers.end()) {
      processed++;
      continue;
    }
    // Persistent across boots, unlike failedShelfCovers (cleared per
    // home visit to allow a single transient-failure retry). If gen has
    // been marked permanently failed for this book, render the
    // placeholder and don't even pay the existence-check below.
    if (CoverThumbStatus::isMarkedFailed(bookPath, cellWidth, cellHeight)) {
      processed++;
      continue;
    }
    // Build the dimension-specific resolved thumb path. If it already exists
    // on SD, this book is done — skip the expensive EPUB/XTC load.
    std::string templatePath;
    if (FsHelpers::hasEpubExtension(bookPath)) {
      templatePath = Epub(bookPath, "/.crosspoint").getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(bookPath)) {
      templatePath = Xtc(bookPath, "/.crosspoint").getThumbBmpPath();
    } else {
      processed++;
      continue;
    }
    const std::string resolved = UITheme::getCoverThumbPath(templatePath, cellWidth, cellHeight);
    if (!resolved.empty() && Storage.exists(resolved.c_str())) {
      processed++;
      continue;
    }

    // First-index safety cap: on the very first boot (fresh library index just
    // built), stop generating new covers past the cap so a large library can't
    // OOM mid first-time setup (the SD walk just ran; heap is fragmented).
    // Capped books are recorded like a failed cover (blank, no retry this
    // session) and generate normally on the next boot, when wasFreshFirstBoot()
    // is false and there's no walk competing for heap.
    if (LibraryIndex::getInstance().wasFreshFirstBoot() && firstIndexCoversGenerated >= kFirstIndexCoverCap) {
      failedShelfCovers.push_back(bookPath);
      processed++;
      continue;
    }

    // Reclaim the Flow home's fast-path snapshot buffers before extracting a
    // (possibly DEFLATE-compressed) cover image. The Flow home pins a 48 KB
    // full-framebuffer snapshot (coverBuffer) plus the carousel frame cache, so
    // the largest contiguous block here is only ~16-22 KB -- below the up-to-
    // 32 KB DEFLATE window the cover/`content.opf` extractor needs. Without this
    // every compressed cover failed ("[ZIP] Failed to init inflate reader") and
    // the book rendered a permanent blank cover for the session. Freeing them
    // restores ~65 KB+ contiguous so generation succeeds; they are rebuilt on
    // the follow-up repaint (the homeRenderPopupShown path at end-of-render
    // invalidates the snapshot and re-warms the carousel).
    if (ESP.getMaxAllocHeap() < kCoverGenMinContiguousHeap) {
      freeCoverBuffer();
      coverBufferStored = false;
      gCarouselCache.invalidate();
      freeCarouselFrames();
    }

    // Need to generate. Show a loading popup if this is the first book in
    // this pass that's missing — matches the loadRecentCovers UX.
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, popupRect, 10 + processed * progressIncrement);

    bool genSucceeded = false;
    if (FsHelpers::hasEpubExtension(bookPath)) {
      Epub epub(bookPath, "/.crosspoint");
      // generateThumbBmpNoIndex extracts the cover WITHOUT building the full
      // spine/TOC index (book.bin). That index build is what froze the UI on
      // the "Loading" popup while scrolling collections of never-opened books
      // (e.g. Recently Added): we'd index the whole EPUB only to find it has
      // no extractable cover. The no-index path parses just content.opf, so a
      // coverless book falls back to the placeholder in OPF-parse time. The
      // full index is built later, when the book is actually opened.
      genSucceeded = epub.generateThumbBmpNoIndex(cellWidth, cellHeight);
      if (!genSucceeded) {
        LOG_ERR("HOME", "shelf: failed to generate thumb for %s", bookPath.c_str());
      }
    } else if (FsHelpers::hasXtcExtension(bookPath)) {
      Xtc xtc(bookPath, "/.crosspoint");
      if (xtc.load()) {
        genSucceeded = xtc.generateThumbBmp(cellWidth, cellHeight);
        if (!genSucceeded) {
          LOG_ERR("HOME", "shelf: failed to generate xtc thumb for %s", bookPath.c_str());
        }
      }
    }
    // If the thumb still isn't on SD after our attempt, generation failed
    // (corrupt/unsupported cover image, load error, etc.). Record it so the
    // book is skipped on subsequent renders — otherwise it stays "missing"
    // forever and we'd re-show the popup + requestUpdate() every frame,
    // which is the flashing loop. The book just renders as a blank cover.
    const bool thumbNowExists = !resolved.empty() && Storage.exists(resolved.c_str());
    if (!thumbNowExists) {
      failedShelfCovers.push_back(bookPath);
      // Persist across boots so the next session's home doesn't repeat
      // the same Loading-popup flash for this book.
      CoverThumbStatus::markFailed(bookPath, cellWidth, cellHeight);
      LOG_ERR("HOME", "shelf: thumb generation failed for %s; rendering blank (won't retry)",
              bookPath.c_str());
    } else if (genSucceeded) {
      // Wipe the persistent marker on the rare cross-build "fixed cover"
      // case (decoder improved or user replaced the book file).
      CoverThumbStatus::clearFailed(bookPath, cellWidth, cellHeight);
    }
    // Count this generation attempt toward the first-index cap (only enforced
    // while wasFreshFirstBoot(); harmless to increment otherwise).
    firstIndexCoversGenerated++;
    processed++;
  }

  shelfCoversLoaded = true;
  // Only request a follow-up redraw if we actually produced new thumbs this
  // pass. If the only "work" was failed generations (now recorded in
  // failedShelfCovers and skipped next time), requesting another update
  // would just re-render with the same blanks — and on the very next render
  // those books are skipped, so showingLoading stays false and the loop
  // ends. Keeping the requestUpdate here is still correct for the success
  // case (covers that DID generate need one repaint to appear).
  if (showingLoading) {
    // The Loading popup was drawn over the framebuffer; flag the frame so the
    // end-of-render snapshot is skipped and the follow-up render erases it.
    homeRenderPopupShown = true;
    requestUpdate();
  }
}

const std::vector<ShelfEntry>& HomeActivity::cachedShelfEntries() {
  const std::string& activeId = CollectionsStore::getInstance().getActiveId();
  if (activeId.empty()) {
    shelfEntriesCache.clear();
    shelfPathsCache.clear();
    shelfPathsCacheKey.clear();
    return shelfEntriesCache;
  }
  if (activeId == shelfPathsCacheKey) {
    return shelfEntriesCache;
  }
  // Cache miss — resolve entries (does the path sort + series collapse
  // in one pass) and derive the path list as one firstPath per entry.
  auto& store = CollectionsStore::getInstance();
  shelfEntriesCache = store.resolveShelfEntries(activeId);
  shelfPathsCache.clear();
  shelfPathsCache.reserve(shelfEntriesCache.size());
  for (const auto& e : shelfEntriesCache) shelfPathsCache.push_back(e.firstPath);
  // CrumBLE 4.2.1: if resolveShelfEntries returned empty because its heap
  // pre-flight refused the build (heap-pressure-empty, not a legitimately
  // empty collection), DON'T commit the empty result as the cached value.
  // Without this, the shelf would render empty until the next action
  // invalidated shelfPathsCacheKey -- producing the "select sort,
  // collections show empty, click anything, then they appear" symptom.
  // Trigger one retry now via requestUpdate(); the next render typically
  // sees recovered heap (the previous render's transient allocations have
  // freed). Bound the retry count so an actually-stuck heap doesn't drive
  // an infinite render loop -- after kMaxRetries consecutive failures, we
  // commit the empty cache and the user can recover with a click.
  constexpr int kMaxHeapPressureRetries = 5;
  if (store.lastResolveHitHeapPressure() && shelfHeapRetryCount_ < kMaxHeapPressureRetries) {
    shelfHeapRetryCount_++;
    LOG_DBG("HOM", "cachedShelfEntries: heap-pressure empty, retry %d/%d", shelfHeapRetryCount_,
            kMaxHeapPressureRetries);
    shelfPathsCacheKey.clear();  // keep cache invalid so next render re-resolves
    requestUpdate();             // schedule that render
  } else {
    shelfHeapRetryCount_ = 0;  // either a real result or we've given up retrying
    shelfPathsCacheKey = activeId;
  }
  return shelfEntriesCache;
}

const std::vector<std::string>& HomeActivity::cachedShelfPaths() {
  // Ensures the entries cache is fresh (which also populates the
  // shelfPathsCache vector as a side effect). Existing call sites
  // that index paths can keep doing so — each path is the firstPath
  // of the corresponding ShelfEntry, so navigation stays 1:1 with
  // shelf cells.
  cachedShelfEntries();
  return shelfPathsCache;
}

std::string HomeActivity::getFocusedBookPath() {
  // Header focus is a separate row that isn't a "book" — long-press there
  // should NOT open an action menu (there's no book to act on).
  if (shelfHeaderFocused) {
    return {};
  }
  // Carousel range: selectorIndex < recentBooks.size().
  if (selectorIndex < recentBooks.size()) {
    return recentBooks[selectorIndex].path;
  }
  // Shelf range: only meaningful on the Flow theme. Indices sit between
  // the carousel and the menu icon bar. Resolve via cachedShelfPaths()
  // -- previous impl called CollectionsStore::resolveBookPaths each tick
  // which re-sorted the full collection over LibraryIndex every press,
  // showing as L/R/U/D lag on a 50-book All Books (#124).
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
    const std::string& activeId = CollectionsStore::getInstance().getActiveId();
    if (!activeId.empty()) {
      const std::vector<std::string>& paths = cachedShelfPaths();
      const int shelfStart = static_cast<int>(recentBooks.size());
      const int shelfCount = static_cast<int>(paths.size());
      if (static_cast<int>(selectorIndex) >= shelfStart &&
          static_cast<int>(selectorIndex) < shelfStart + shelfCount) {
        return paths[selectorIndex - shelfStart];
      }
    }
  }
  return {};  // empty => focus is somewhere that doesn't represent a book (e.g. menu row).
}

void HomeActivity::showHomeBookActionMenu(const std::string& bookPath) {
  // CrumBLE 4.2: route the home carousel long-press through the shared
  // BookActions::buildBookActionItems so the item order matches every
  // other long-press menu (file browser, recent-books list/grid). Order is
  // documented on BookActionMenuOptions: Add-to-collection, Remove-from-
  // recents, Mark-finished, Show-metadata, Delete-cache, Delete. The home
  // path uses the RemoveFromRecentBooks dispatch (action enum 7), distinct
  // from the recent-books-list activities' RemoveFromRecents (enum 16) --
  // same UX label, different handler.
  BookActions::BookActionMenuOptions opts;
  opts.addToCollection = true;
  opts.showMetadata = true;
  // Only emit Remove from Recent Books if the book is actually in the
  // recents list -- otherwise the option is meaningless (e.g. a
  // Favorites-only book that was never opened).
  const auto& recents = RECENT_BOOKS.getBooks();
  const bool inRecents =
      std::find_if(recents.begin(), recents.end(), [&](const RecentBook& r) { return r.path == bookPath; }) !=
      recents.end();
  if (inRecents) {
    opts.removeFromRecents = FileBrowserAction::RemoveFromRecentBooks;
  }
  std::vector<FileBrowserActionActivity::MenuItem> items = BookActions::buildBookActionItems(bookPath, opts);

  // CrumBLE 4.2: resolve title + author via BookActions (free in-memory
  // RecentBooks lookup, fallback to OPF parse, last-resort fallback to
  // filename). Passing author as subtitle moves the "Optimized" header
  // label to the second line so it can't collide with a long title.
  const BookActions::BookHeaderText header = BookActions::resolveBookHeaderText(bookPath);

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, header.title, std::move(items),
                                                  /*ignoreInitialConfirmRelease=*/true,
                                                  BookActions::optimizedHeaderLabel(bookPath), header.author),
      [this, bookPath](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          return;
        }
        const auto action = static_cast<FileBrowserAction>(std::get<FileBrowserActionResult>(result.data).action);
        switch (action) {
          case FileBrowserAction::Delete: {
            // Confirmation prompt mirrors FileBrowser. On confirm we wipe
            // the cache, drop the book from recents + every collection,
            // and finally remove the file itself. Failures are logged but
            // we still refresh the home view so stale entries don't
            // linger.
            const size_t ls = bookPath.find_last_of('/');
            const std::string entry = (ls != std::string::npos) ? bookPath.substr(ls + 1) : bookPath;
            const std::string heading = tr(STR_DELETE) + std::string("? ");
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry),
                [this, bookPath](const ActivityResult& confirm) {
                  if (confirm.isCancelled) {
                    return;
                  }
                  if (FsHelpers::hasEpubExtension(bookPath)) {
                    Epub(bookPath, "/.crosspoint").clearCache();
                    BookmarkStore::deleteForFilePath(bookPath, "epub");
                  } else if (FsHelpers::hasXtcExtension(bookPath)) {
                    Xtc(bookPath, "/.crosspoint").clearCache();
                    BookmarkStore::deleteForFilePath(bookPath, "xtc");
                  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
                    BookmarkStore::deleteForFilePath(bookPath, "txt");
                  }
                  RECENT_BOOKS.removeBook(bookPath);
                  CollectionsStore::getInstance().removeBookFromAllCollections(bookPath);
                  LibraryIndex::getInstance().forgetPath(bookPath);
                  SeriesIndex::getInstance().forgetPath(bookPath);
                  if (!Storage.remove(bookPath.c_str())) {
                    LOG_ERR("HOME", "Failed to delete file: %s", bookPath.c_str());
                  }
                  // Recents shrank — reload from the store so the
                  // carousel/shelf indices stay valid.
                  loadRecentBooks(UITheme::getInstance().getMetrics().homeRecentBooksCount);
                  // CrumBLE #124: re-prime the per-book stats/progress cache
                  // because the recentBooks vector just shifted -- otherwise
                  // bookStatsCached[i] would reference stats from the OLD
                  // book that used to occupy slot i.
                  loadAllBookStats();
                  // CrumBLE #125: same reason -- tile cache is keyed by
                  // book path so the existing entries are still valid,
                  // but the deleted/removed book's entry is now stale
                  // (its file may be gone). Rebuild to match the new
                  // recentBooks vector.
                  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) ==
                      CrossPointSettings::UI_THEME::LYRA_FLOW) {
                    static_cast<const LyraFlowTheme&>(GUI).prerenderCarouselSideTiles(renderer, recentBooks);
                  }
                  if (selectorIndex >= recentBooks.size() + 1) {
                    selectorIndex = recentBooks.empty() ? 0 : static_cast<int>(recentBooks.size()) - 1;
                  }
                  shelfCoversLoaded = false;
                  invalidateShelfPathsCache();
                  shelfSnapshotValid = false;
                  lastRenderedCoverSelectorValid = false;  // book gone from active collection too.
                  requestUpdate(true);
                });
            return;
          }
          case FileBrowserAction::DeleteCache: {
            bool ok = false;
            if (FsHelpers::hasEpubExtension(bookPath)) {
              ok = Epub(bookPath, "/.crosspoint").clearCache();
            } else if (FsHelpers::hasXtcExtension(bookPath)) {
              ok = Xtc(bookPath, "/.crosspoint").clearCache();
            }
            if (!ok) {
              LOG_ERR("HOME", "Failed to clear book cache for: %s", bookPath.c_str());
              drawHomeToast(renderer, tr(STR_CACHE_DELETE_FAILED));
              delay(1500);
            } else {
              drawHomeToast(renderer, tr(STR_BOOK_CACHE_DELETED));
              delay(800);
            }
            shelfCoversLoaded = false;  // thumbs in cache may have been wiped.
            requestUpdate();
            return;
          }
          case FileBrowserAction::ToggleCompleted: {
            // Simplified vs. FileBrowser: just flip the flag and update
            // GlobalReadingStats. We deliberately skip the
            // "auto-move-finished-to-/Read folder" dance because the home
            // screen doesn't have the surrounding redraw machinery for
            // the moved-file alert path. Users who want that should
            // mark from the file browser.
            Epub epub(bookPath, "/.crosspoint");
            epub.setupCacheDir();
            BookReadingStats stats = BookReadingStats::load(epub.getCachePath());
            const bool nowCompleted = !stats.isCompleted;
            stats.isCompleted = nowCompleted;
            GlobalReadingStats gs = GlobalReadingStats::load();
            if (nowCompleted) {
              gs.completedBooks++;
            } else if (gs.completedBooks > 0) {
              gs.completedBooks--;
            }
            stats.save(epub.getCachePath());
            gs.save();
            drawHomeToast(renderer, nowCompleted ? tr(STR_BOOK_FINISHED) : tr(STR_BOOK_UNFINISHED));
            delay(800);
            requestUpdate();
            return;
          }
          case FileBrowserAction::AddToCollection: {
            // Open the picker; it mutates CollectionsStore directly so
            // we just need to invalidate the shelf thumb cache and
            // refresh on return.
            const size_t ls = bookPath.find_last_of('/');
            const std::string title = (ls != std::string::npos) ? bookPath.substr(ls + 1) : bookPath;
            startActivityForResult(std::make_unique<CollectionPickerActivity>(renderer, mappedInput, bookPath, title),
                                   [this](const ActivityResult&) {
                                     shelfCoversLoaded = false;
                                     invalidateShelfPathsCache();
                                     shelfSnapshotValid = false;
                                     lastRenderedCoverSelectorValid = false;  // picker may have toggled membership of active.
                                     requestUpdate();
                                   });
            return;
          }
          case FileBrowserAction::RemoveFromRecentBooks: {
            if (RECENT_BOOKS.removeBook(bookPath)) {
              drawHomeToast(renderer, tr(STR_REMOVED_FROM_RECENT_BOOKS));
              delay(800);
              loadRecentBooks(UITheme::getInstance().getMetrics().homeRecentBooksCount);
              // CrumBLE #124: re-prime the per-book stats/progress cache so
              // the post-remove vector doesn't read stale stats out of slots
              // that used to hold the removed book / its neighbors.
              loadAllBookStats();
              // CrumBLE #125: rebake the Flow side-tile cache. Removed
              // book's entry would still render if cached, but a fresh
              // build keeps the table tidy.
              if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) ==
                  CrossPointSettings::UI_THEME::LYRA_FLOW) {
                static_cast<const LyraFlowTheme&>(GUI).prerenderCarouselSideTiles(renderer, recentBooks);
              }
              if (selectorIndex >= recentBooks.size() + 1) {
                selectorIndex = recentBooks.empty() ? 0 : static_cast<int>(recentBooks.size()) - 1;
              }
              // The removed book just disappeared from the recent list, but
              // the Flow carousel paints from `carouselFrames` (cached
              // pre-rasterized covers) and the Lyra shelf from
              // `shelfSnapshot` — neither of which knows the book set
              // changed. Without invalidation, the next paint replayed the
              // stale snapshot showing the removed cover until the user
              // moved the selector, which finally forced a re-layout. Flush
              // every relevant cache so the removal is visible immediately.
              carouselFramesReady = false;
              shelfCoversLoaded = false;
              invalidateShelfPathsCache();
              shelfSnapshotValid = false;
              lastRenderedCoverSelectorValid = false;
            }
            requestUpdate();
            return;
          }
          case FileBrowserAction::ShowMetadata: {
            startActivityForResult(
                std::make_unique<BookMetadataViewerActivity>(renderer, mappedInput, bookPath),
                [this](const ActivityResult&) { requestUpdate(); });
            return;
          }
          case FileBrowserAction::ViewOptimizedDetails: {
            // CrumBLE 4.2: user activated the Optimized header. Load the
            // prebake manifest sidecar and push the read-only viewer.
            PrebakeManifest pm;
            const std::string cachePath = Epub::cachePathForFilePath(bookPath, "/.crosspoint");
            if (tryLoadPrebakeManifest(cachePath, pm)) {
              BookActions::BookHeaderText header = BookActions::resolveBookHeaderText(bookPath);
              startActivityForResult(
                  std::make_unique<PrebakeManifestViewerActivity>(renderer, mappedInput, header.title,
                                                                  std::move(pm)),
                  [this](const ActivityResult&) { requestUpdate(); });
            }
            return;
          }
          case FileBrowserAction::PinFavorite:
          case FileBrowserAction::UnpinFavorite:
          case FileBrowserAction::RescanLibrary:
          case FileBrowserAction::SortBy:
          case FileBrowserAction::ToggleCollapseSeries:
          case FileBrowserAction::RenameCollection:
          case FileBrowserAction::DeleteCollection:
          case FileBrowserAction::CreateNewCollectionFromHeader:
          case FileBrowserAction::AddBooksToActiveCollection:
          case FileBrowserAction::MakeCollectionFromFolder:
          case FileBrowserAction::ReorderBooksInCollection:
            // Not exposed in the home book menu — sleep-image / shelf-
            // header / file-browser-folder-only actions.
            return;
        }
      });
}

void HomeActivity::openShelfEntry(const ShelfEntry& entry) {
  // Single-book cell — same as the pre-series behavior.
  if (entry.seriesName.empty() || entry.memberPaths.size() < 2) {
    if (!entry.firstPath.empty()) onSelectBook(entry.firstPath);
    return;
  }
  // Series cell: pick the most-recently-read member from RECENT_BOOKS
  // if any series book has been opened before. RECENT_BOOKS is ordered
  // most-recent first so the first match IS the most-recent read.
  const auto& recents = RECENT_BOOKS.getBooks();
  for (const auto& recent : recents) {
    for (const auto& member : entry.memberPaths) {
      if (member == recent.path) {
        onSelectBook(member);
        return;
      }
    }
  }
  // No prior read — fall back to the mini-picker so user can pick.
  openSeriesMiniPicker(entry);
}

void HomeActivity::openSeriesMiniPicker(const ShelfEntry& entry) {
  if (entry.memberPaths.size() < 2) return;
  // Capture by value so the picker stays valid even if the source
  // ShelfEntry goes out of scope during the modal transition.
  const std::vector<std::string> members = entry.memberPaths;
  const std::string name = entry.seriesName;

  auto onOpen = [this](const std::string& bookPath) { onSelectBook(bookPath); };
  auto onLongPress = [this](const std::string& bookPath) { showHomeBookActionMenu(bookPath); };
  auto onOptions = [this, members]() {
    // Series-level "Add to collection..." — opens the picker with the
    // series' first member as the focus (the picker toggles per-book;
    // for now we apply to the first member as a simple version. A
    // future refinement would toggle ALL members atomically.)
    if (members.empty()) return;
    const size_t ls = members[0].find_last_of('/');
    const std::string title = (ls != std::string::npos) ? members[0].substr(ls + 1) : members[0];
    startActivityForResult(std::make_unique<CollectionPickerActivity>(renderer, mappedInput, members[0], title),
                           [this](const ActivityResult&) {
                             shelfCoversLoaded = false;
                             invalidateShelfPathsCache();
                             shelfSnapshotValid = false;
                             lastRenderedCoverSelectorValid = false;
                             requestUpdate();
                           });
  };
  startActivityForResult(std::make_unique<SeriesMiniPickerActivity>(renderer, mappedInput, name, members,
                                                                    std::move(onOpen), std::move(onLongPress),
                                                                    std::move(onOptions)),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::showShelfHeaderActionMenu() {
  // Header context = the active collection's name tab. Builds a small
  // menu of collection-level operations. "Sort by..." is hidden for
  // Recently Added because that collection's order is intrinsic
  // (newest-first by definition).
  std::vector<FileBrowserActionActivity::MenuItem> items;
  const Collection* active = CollectionsStore::getInstance().getActiveCollection();
  const bool isRecentlyAdded =
      active != nullptr && active->id == CollectionsStore::RECENTLY_ADDED_ID;
  // Rename / Delete only apply to user collections. Virtuals are
  // auto-managed; Favorites is seeded and would reappear on next
  // boot if deleted (so we only allow rename for it, not delete).
  const bool isUserCollection = active != nullptr && !active->isVirtual;
  const bool isFavorites = active != nullptr && active->id == CollectionsStore::FAVORITES_ID;
  // Per-collection collapse toggle is only meaningful when the global
  // series-detection setting is on. Hiding it otherwise avoids the
  // confusion of "Series collapse: ON" not actually collapsing
  // anything because the scan never ran.
  if (active != nullptr && SETTINGS.seriesDetectionEnabled) {
    items.push_back({FileBrowserAction::ToggleCollapseSeries,
                     active->collapseSeries ? StrId::STR_COLLAPSE_SERIES_ON : StrId::STR_COLLAPSE_SERIES_OFF});
  }
  // Bulk add: only for user collections. Virtuals are auto-managed
  // so explicit add doesn't make sense there.
  if (isUserCollection) {
    items.push_back({FileBrowserAction::AddBooksToActiveCollection, StrId::STR_ADD_BOOKS_TO_COLLECTION});
  }
  if (isUserCollection) {
    items.push_back({FileBrowserAction::RenameCollection, StrId::STR_RENAME_COLLECTION});
  }
  // CrumBLE: Reorder books only makes sense for user collections with 2+
  // books. Virtuals are derived (their order is intrinsic to whatever
  // source the resolver pulls from). 1-book collections have nothing to
  // reorder; hiding the item there avoids the 1-row picker.
  if (isUserCollection && CollectionsStore::getInstance().countBooksInCollection(active->id) >= 2) {
    items.push_back({FileBrowserAction::ReorderBooksInCollection, StrId::STR_REORDER_BOOKS});
  }
  if (isUserCollection && !isFavorites) {
    items.push_back({FileBrowserAction::DeleteCollection, StrId::STR_DELETE_COLLECTION});
  }
  // CrumBLE: standard ordering for the always-shown collection-management
  // actions. Top-of-list = creating + arranging (most common actions);
  // bottom = library-scoped maintenance (Rescan).
  //   1. + New collection
  //   2. Sort by (hidden on Recently Added -- its sort is intrinsic)
  //   3-6. Show/Hide toggles for the virtual collections (right-justified
  //        value so the toggle state is scannable at a glance)
  //   7. Rescan library
  items.push_back({FileBrowserAction::CreateNewCollectionFromHeader, StrId::STR_HEADER_NEW_COLLECTION});
  if (active != nullptr && !isRecentlyAdded) {
    FileBrowserActionActivity::MenuItem sortItem;
    sortItem.action = FileBrowserAction::SortBy;
    sortItem.labelId = StrId::STR_SORT_BY;
    // CrumBLE: right-justified active-sort label so the user sees what's
    // currently selected at a glance. Confirm still opens the picker.
    // Blank string when the collection is on Manual (no real sort applied).
    if (active->sortMode != CollectionSort::Manual) {
      sortItem.rightValue = SortPickerActivity::labelFor(active->sortMode);
    }
    items.push_back(std::move(sortItem));
  }
  // Rearrange is only meaningful when there's more than one visible
  // collection. Hidden otherwise to avoid a one-item picker.
  if (CollectionsStore::getInstance().getCollections().size() > 1) {
    items.push_back({FileBrowserAction::RearrangeCollections, StrId::STR_REARRANGE});
  }
  auto showHideValue = [](bool on) -> std::string {
    return std::string(I18N.get(on ? StrId::STR_HIDE : StrId::STR_SHOW));
  };
  items.push_back({FileBrowserAction::ToggleShowAllBooks, StrId::STR_COL_ALL_BOOKS,
                   showHideValue(SETTINGS.showAllBooksCollection)});
  items.push_back({FileBrowserAction::ToggleShowRecentlyAdded, StrId::STR_COL_RECENTLY_ADDED,
                   showHideValue(SETTINGS.showRecentlyAddedCollection)});
  items.push_back({FileBrowserAction::ToggleShowNew, StrId::STR_COL_UNOPENED,
                   showHideValue(SETTINGS.showNewCollection)});
  items.push_back({FileBrowserAction::ToggleShowFinished, StrId::STR_COL_FINISHED,
                   showHideValue(SETTINGS.showFinishedCollection)});
  items.push_back({FileBrowserAction::RescanLibrary, StrId::STR_RESCAN_LIBRARY});

  const std::string title = (active != nullptr) ? active->name : std::string();

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, title, std::move(items),
                                                  /*ignoreInitialConfirmRelease=*/true),
      [this](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          return;
        }
        const auto action = static_cast<FileBrowserAction>(std::get<FileBrowserActionResult>(result.data).action);
        if (action == FileBrowserAction::RescanLibrary) {
          // Full rescan with the same indexing popup we use on first boot
          // — keeps the visual language consistent and signals clearly that
          // the device is doing work.
          const Rect popupRect = GUI.drawPopup(renderer, tr(STR_RESCAN_LIBRARY));
          LibraryIndex::getInstance().rescan(
              [&](int pct) { GUI.fillPopupProgress(renderer, popupRect, pct); });
          shelfCoversLoaded = false;
          invalidateShelfPathsCache();
          shelfSnapshotValid = false;
          lastRenderedCoverSelectorValid = false;
          drawHomeToast(renderer, tr(STR_LIBRARY_RESCANNED));
          delay(800);
          requestUpdate();
        } else if (action == FileBrowserAction::ToggleShowRecentlyAdded ||
                   action == FileBrowserAction::ToggleShowAllBooks ||
                   action == FileBrowserAction::ToggleShowFinished ||
                   action == FileBrowserAction::ToggleShowNew) {
          // Resolve the toggle target (id/name/settings byte pointer) in one
          // place so the on/off branches don't repeat the same 4-way fan-out.
          uint8_t* settingsByte = nullptr;
          const char* vid = nullptr;
          const char* vname = nullptr;
          switch (action) {
            case FileBrowserAction::ToggleShowRecentlyAdded:
              settingsByte = &SETTINGS.showRecentlyAddedCollection;
              vid = CollectionsStore::RECENTLY_ADDED_ID;
              vname = CollectionsStore::RECENTLY_ADDED_NAME;
              break;
            case FileBrowserAction::ToggleShowAllBooks:
              settingsByte = &SETTINGS.showAllBooksCollection;
              vid = CollectionsStore::ALL_BOOKS_ID;
              vname = CollectionsStore::ALL_BOOKS_NAME;
              break;
            case FileBrowserAction::ToggleShowFinished:
              settingsByte = &SETTINGS.showFinishedCollection;
              vid = CollectionsStore::FINISHED_ID;
              vname = CollectionsStore::FINISHED_NAME;
              break;
            case FileBrowserAction::ToggleShowNew:
              settingsByte = &SETTINGS.showNewCollection;
              vid = CollectionsStore::NEW_ID;
              vname = CollectionsStore::NEW_NAME;
              break;
            default:
              return;
          }
          const bool currentlyOn = *settingsByte != 0;
          if (currentlyOn) {
            // Turn OFF — just hide it; no scan needed.
            *settingsByte = 0;
            SETTINGS.saveToFile();
            CollectionsStore::getInstance().setVirtualCollectionVisible(vid, vname, false);
            invalidateShelfPathsCache();
            shelfSnapshotValid = false;
            lastRenderedCoverSelectorValid = false;
            shelfCoversLoaded = false;
            requestUpdate();
          } else {
            // Turn ON — confirm the (possibly first) library scan before walking SD.
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SCAN_LIBRARY_PROMPT), vname),
                [this, settingsByte, vid, vname](const ActivityResult& confirm) {
                  if (confirm.isCancelled) return;  // declined — stay hidden
                  *settingsByte = 1;
                  SETTINGS.saveToFile();
                  CollectionsStore::getInstance().setVirtualCollectionVisible(vid, vname, true);
                  // ensureWalked self-skips if a walk already ran this session
                  // (e.g. another virtual is already on), so this only costs
                  // the SD walk the first time.
                  const Rect popupRect = GUI.drawPopup(renderer, tr(STR_RESCAN_LIBRARY));
                  LibraryIndex::getInstance().ensureWalked(
                      [&](int pct) { GUI.fillPopupProgress(renderer, popupRect, pct); });
                  // Finished / New also need the per-book BookReadingStats
                  // pass -- invalidate so resolveBookPaths rebuilds against
                  // the freshly-walked library.
                  CollectionsStore::getInstance().invalidateScannedVirtuals();
                  invalidateShelfPathsCache();
                  shelfSnapshotValid = false;
                  lastRenderedCoverSelectorValid = false;
                  shelfCoversLoaded = false;
                  requestUpdate();
                });
          }
        } else if (action == FileBrowserAction::ToggleCollapseSeries) {
          const Collection* active = CollectionsStore::getInstance().getActiveCollection();
          if (active == nullptr) return;
          const bool newValue = !active->collapseSeries;
          CollectionsStore::getInstance().setCollapseSeries(active->id, newValue);
          // ShelfEntries shape changed (collapsed vs flat) — refresh.
          invalidateShelfPathsCache();
          shelfSnapshotValid = false;
          lastRenderedCoverSelectorValid = false;
          shelfCoversLoaded = false;
          shelfScrollOffset = 0;  // index space shifted; jump to top.
          drawHomeToast(renderer, newValue ? tr(STR_COLLAPSE_SERIES_ENABLED) : tr(STR_COLLAPSE_SERIES_DISABLED));
          delay(800);
          requestUpdate();
        } else if (action == FileBrowserAction::SortBy) {
          // Open the sort picker for the active collection. The picker
          // returns a SortPickerResult; we persist via setSortMode and
          // bust the per-frame path cache so the new order takes
          // effect on the next render.
          const Collection* active = CollectionsStore::getInstance().getActiveCollection();
          if (active == nullptr) return;
          const std::string activeId = active->id;
          const std::string activeName = active->name;
          const CollectionSort current = active->sortMode;
          const bool allowManual = !active->isVirtual;
          startActivityForResult(
              std::make_unique<SortPickerActivity>(renderer, mappedInput, activeName, current, allowManual),
              [this, activeId](const ActivityResult& pickRes) {
                if (pickRes.isCancelled) return;
                const auto& sr = std::get<SortPickerResult>(pickRes.data);
                CollectionsStore::getInstance().setSortMode(activeId,
                                                             static_cast<CollectionSort>(sr.sortMode));
                invalidateShelfPathsCache();
                shelfSnapshotValid = false;
                shelfCoversLoaded = false;  // thumbs themselves are unchanged, but the visible window shifts.
                shelfScrollOffset = 0;       // jump back to the top of the freshly-sorted list.
                requestUpdate();
              });
        } else if (action == FileBrowserAction::RearrangeCollections) {
          // Snapshot the current collection list (in present order) and hand
          // it to the rearrange UI. The user assigns Mark 1..N via Confirm;
          // on completion the new order is persisted and the first
          // collection becomes the active one (per spec).
          std::vector<RearrangeCollectionsActivity::Item> snapshot;
          for (const auto& c : CollectionsStore::getInstance().getCollections()) {
            snapshot.push_back({c.id, c.name});
          }
          if (snapshot.size() < 2) {
            requestUpdate();
            return;
          }
          startActivityForResult(
              std::make_unique<RearrangeCollectionsActivity>(renderer, mappedInput, std::move(snapshot)),
              [this](const ActivityResult& res) {
                if (res.isCancelled) return;
                const auto& rr = std::get<RearrangeCollectionsResult>(res.data);
                if (rr.orderedIds.empty()) return;
                CollectionsStore::getInstance().setDisplayOrder(rr.orderedIds);
                // Per spec: returning to Home should land on the first
                // collection in the new order.
                CollectionsStore::getInstance().setActiveId(rr.orderedIds.front());
                invalidateShelfPathsCache();
                shelfSnapshotValid = false;
                lastRenderedCoverSelectorValid = false;
                shelfCoversLoaded = false;
                shelfScrollOffset = 0;
                requestUpdate();
              });
        } else if (action == FileBrowserAction::ToggleTwoRowShelf) {
          // No-op: the Rows row is wired with inlineToggle (handled inside
          // the menu activity), so this action value never reaches the
          // result handler. Kept for completeness against the enum.
          return;
        } else if (action == FileBrowserAction::RenameCollection) {
          const Collection* active = CollectionsStore::getInstance().getActiveCollection();
          if (active == nullptr || active->isVirtual) return;
          const std::string activeId = active->id;
          const std::string currentName = active->name;
          startActivityForResult(
              std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME_COLLECTION_PROMPT),
                                                      currentName, /*maxLength=*/40, InputType::Text),
              [this, activeId](const ActivityResult& res) {
                if (res.isCancelled) return;
                const auto& kr = std::get<KeyboardResult>(res.data);
                // Trim whitespace (same logic the create-new flow uses).
                std::string trimmed = kr.text;
                const auto l = trimmed.find_first_not_of(" \t");
                const auto r = trimmed.find_last_not_of(" \t");
                if (l == std::string::npos) {
                  requestUpdate();
                  return;
                }
                trimmed = trimmed.substr(l, r - l + 1);
                if (trimmed.empty()) {
                  requestUpdate();
                  return;
                }
                if (CollectionsStore::getInstance().renameCollection(activeId, trimmed)) {
                  drawHomeToast(renderer, tr(STR_COLLECTION_RENAMED));
                  delay(800);
                  // The active id is unchanged but the name rendered
                  // in the tab is different. The shelf skip-fast-path
                  // would otherwise reuse the prior frame's tab text
                  // (same activeId, same scroll, same focus) and the
                  // user would only see the new name after cycling
                  // off and back. Force a repaint by invalidating the
                  // snapshot.
                  shelfSnapshotValid = false;
                }
                requestUpdate();
              });
        } else if (action == FileBrowserAction::ReorderBooksInCollection) {
          // CrumBLE: open RearrangeCollectionsActivity (generic over Items)
          // over the active collection's books. User taps Confirm in the
          // desired order; on finish we replace bookPaths in-place AND
          // force sortMode to Manual via reorderBooksInCollection.
          const Collection* active = CollectionsStore::getInstance().getActiveCollection();
          if (active == nullptr || active->isVirtual) return;
          const std::string activeId = active->id;
          // Use the resolved view (honors series collapse / live virtuals if
          // somehow virtual slips through), not the raw stored bookPaths.
          const auto paths = CollectionsStore::getInstance().resolveBookPaths(activeId);
          if (paths.size() < 2) {
            requestUpdate();
            return;
          }
          std::vector<RearrangeCollectionsActivity::Item> snapshot;
          snapshot.reserve(paths.size());
          for (const auto& p : paths) {
            // Display label: filename without extension. Title-from-metadata
            // would be nicer but would require a per-book SD read each open;
            // the basename fallback is what the carousel/grid already uses
            // when title isn't cached, so the labels here match what the
            // user sees elsewhere for the same books.
            std::string display = p;
            const size_t lastSlash = display.find_last_of('/');
            if (lastSlash != std::string::npos) display = display.substr(lastSlash + 1);
            const size_t lastDot = display.find_last_of('.');
            if (lastDot != std::string::npos && lastDot > 0) display = display.substr(0, lastDot);
            snapshot.push_back({p, display});
          }
          startActivityForResult(
              std::make_unique<RearrangeCollectionsActivity>(renderer, mappedInput, std::move(snapshot)),
              [this, activeId](const ActivityResult& res) {
                if (res.isCancelled) return;
                const auto& rr = std::get<RearrangeCollectionsResult>(res.data);
                if (rr.orderedIds.empty()) return;
                if (CollectionsStore::getInstance().reorderBooksInCollection(activeId, rr.orderedIds)) {
                  drawHomeToast(renderer, tr(STR_BOOKS_REORDERED));
                  delay(800);
                  invalidateShelfPathsCache();
                  shelfSnapshotValid = false;
                  shelfCoversLoaded = false;
                }
                requestUpdate();
              });
        } else if (action == FileBrowserAction::DeleteCollection) {
          const Collection* active = CollectionsStore::getInstance().getActiveCollection();
          if (active == nullptr || active->isVirtual || active->id == CollectionsStore::FAVORITES_ID) return;
          const std::string activeId = active->id;
          const std::string heading = tr(STR_DELETE_COLLECTION_PROMPT);
          startActivityForResult(
              std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, active->name),
              [this, activeId](const ActivityResult& confirm) {
                if (confirm.isCancelled) return;
                if (!CollectionsStore::getInstance().deleteCollection(activeId)) {
                  requestUpdate();
                  return;
                }
                // The active collection just changed (deleteCollection
                // resets it to Favorites). Bust all caches that key on
                // active id so the next render shows Favorites cleanly.
                shelfScrollOffset = 0;
                lastShelfBookIndex = 0;
                shelfCoversLoaded = false;
                invalidateShelfPathsCache();
                shelfSnapshotValid = false;
                lastRenderedCoverSelectorValid = false;
                seriesEnrichmentNeededForActive = true;
                drawHomeToast(renderer, tr(STR_COLLECTION_DELETED));
                delay(800);
                requestUpdate();
              });
        } else if (action == FileBrowserAction::CreateNewCollectionFromHeader) {
          // Open keyboard for a name. On submit, create the collection
          // AND switch the active id to it so the user immediately
          // sees their new collection on the shelf.
          startActivityForResult(
              std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NEW_COLLECTION_PROMPT),
                                                      /*initialText=*/"", /*maxLength=*/40, InputType::Text),
              [this](const ActivityResult& res) {
                if (res.isCancelled) return;
                const auto& kr = std::get<KeyboardResult>(res.data);
                std::string trimmed = kr.text;
                const auto l = trimmed.find_first_not_of(" \t");
                const auto r = trimmed.find_last_not_of(" \t");
                if (l == std::string::npos) {
                  requestUpdate();
                  return;
                }
                trimmed = trimmed.substr(l, r - l + 1);
                if (trimmed.empty()) {
                  requestUpdate();
                  return;
                }
                const std::string newId = CollectionsStore::getInstance().createCollection(trimmed);
                if (!newId.empty()) {
                  // Jump to the brand-new collection so the user can
                  // immediately add books / verify creation. Reset
                  // shelf state because the activeId changed (cycle
                  // semantics).
                  CollectionsStore::getInstance().setActiveId(newId);
                  shelfScrollOffset = 0;
                  lastShelfBookIndex = 0;
                  shelfCoversLoaded = false;
                  invalidateShelfPathsCache();
                  shelfSnapshotValid = false;
                  lastRenderedCoverSelectorValid = false;
                  seriesEnrichmentNeededForActive = true;
                  drawHomeToast(renderer, tr(STR_COLLECTION_CREATED));
                  delay(800);
                }
                requestUpdate();
              });
        } else if (action == FileBrowserAction::AddBooksToActiveCollection) {
          const Collection* active = CollectionsStore::getInstance().getActiveCollection();
          if (active == nullptr || active->isVirtual) return;
          const std::string activeId = active->id;
          const std::string activeName = active->name;
          startActivityForResult(
              std::make_unique<AddBooksToCollectionActivity>(renderer, mappedInput, activeId, activeName),
              [this](const ActivityResult&) {
                // Member list of the active collection just changed —
                // re-evaluate shelf paths and force a repaint.
                invalidateShelfPathsCache();
                shelfSnapshotValid = false;
                shelfCoversLoaded = false;
                requestUpdate();
              });
        }
      });
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // CrumBLE 4.2: rehydrate the in-RAM collection / series stores if a
  // previous activity (reader, File Transfer) called releaseMemory() to
  // free up contiguous heap. Without this, the collections strip on
  // Home renders empty until the next full boot. Both begin() calls are
  // cheap when the store is already populated (idempotent JSON load)
  // and free when it isn't (~milliseconds reading the small JSON files).
  if (CollectionsStore::getInstance().getCollections().empty()) {
    CollectionsStore::getInstance().begin();
  }
  SeriesIndex::getInstance().begin();

  // ActivityManager::loop() releases the render lock *before* calling onEnter
  // (so each activity decides whether it needs it). HomeActivity::onEnter
  // rebuilds recentBooks and — on the Lyra Carousel theme — pre-renders
  // carousel frames, which writes the framebuffer, mutates the shared global
  // gCarouselCache, and runs the JPEG cover decoder. The render task touches
  // all of that under the lock, so without holding it here a concurrently
  // notified render() races us. That race corrupted the heap and tripped
  // `xTaskPriorityDisinherit` (mutex released by a non-owner) during carousel
  // cover decode. Hold the lock across the whole setup to serialize with
  // render(). Safe from deadlock: onEnter is always called with the lock
  // released, and nothing below re-takes it or blocks on the render task.
  RenderLock lock;

  hasOpdsServers = OPDS_STORE.hasServers();
  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  // Check if any books have bookmarks (directory scan only, no file parsing)
  hasBookmarks = BookmarkStore::hasAnyBookmarks();

  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  minimalMenuOpen = false;
  minimalSuppressInitialFrontRelease = isMinimalTheme();
  minimalMenuIndex = 0;
  minimalHomeNavIndex = -1;
  carouselFramesReady = false;
  carouselWarmupPending = isCarouselTheme;
  // Clear ghosting from the previous screen (e.g. a dense reader page) with one
  // full refresh on the first present of this Home visit; fast refreshes after.
  pendingFullRefresh = true;
  // Force a re-check of shelf thumbnails on every onEnter so books that
  // were just toggled into a collection (e.g. via the file browser long-
  // press) get their cover generated on the next return to Home.
  shelfCoversLoaded = false;
  // Give covers that failed last session one fresh retry per home visit
  // (the failure may have been transient — low heap, etc.).
  failedShelfCovers.clear();
  shelfHeaderFocused = false;
  lastShelfBookIndex = 0;  // every onEnter starts the row at book 0.
  shelfPosByCollection.clear();  // per-collection shelf positions reset each home visit.
  lastMenuIndex = 0;       // and the menu at icon 0.
  seriesEnrichmentNeededForActive = true;
  // Drop any stale cached path list — the active collection's
  // membership may have changed while we were elsewhere.
  invalidateShelfPathsCache();
  shelfSnapshotValid = false;
  lastRenderedCoverSelectorValid = false;

  // CrumBLE: heal recent.json if a foreign firmware (e.g.
  // rhythmerc/crosspoint-reader) ran between boots and wiped or
  // replaced it. Per-book stats.bin sidecars in /.crosspoint/<hash>/
  // survive on the SD card, so we can backfill recents from them.
  // Gated to once per boot, and only when recents is genuinely short --
  // a healthy user with > kRecentsHealThreshold entries already sees
  // their full carousel and never triggers this path.
  {
    static bool recentsHealAttempted = false;
    constexpr int kRecentsHealThreshold = 6;
    if (!recentsHealAttempted) {
      recentsHealAttempted = true;
      if (RECENT_BOOKS.getCount() < kRecentsHealThreshold) {
        // Don't kick a fresh SD walk from here -- if LibraryIndex isn't
        // populated yet (e.g. first boot, no virtual collection visited),
        // skip and try again next boot once the user has touched the
        // virtual-collection flow that walks it. healFromStats handles
        // the empty case as a no-op.
        const int added = RECENT_BOOKS.healFromStats(nullptr);
        if (added > 0) {
          LOG_INF("HOME", "Recents auto-healed: +%d entries from stats.bin sidecars", added);
        }
      }
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  // CrumBLE #120: always restore "other row" memory from the saved
  // cursor when we have one, regardless of which branch below sets
  // selectorIndex. lastCarouselBookIndex / lastShelfBookIndex /
  // lastMenuIndex / shelfPosByCollection / shelfHeaderFocused control
  // where each ROW lands when the user navigates back into it — they
  // should track the user's last position even when the activity
  // explicitly drops the cursor on a specific icon (initialMenuItem)
  // or on the just-read book (openEpubPath).
  if (hasSavedCursor_) {
    shelfPosByCollection = savedShelfPosByCollection_;
    lastCarouselBookIndex = std::clamp(
        savedLastCarouselBookIndex_, 0,
        std::max(0, static_cast<int>(recentBooks.size()) - 1));
    lastShelfBookIndex = std::max(0, savedLastShelfBookIndex_);
    lastMenuIndex = std::max(0, savedLastMenuIndex_);
    shelfHeaderFocused = savedShelfHeaderFocused_;
  }

  globalStats = GlobalReadingStats::load();
  // CrumBLE #124: pre-load stats/progress for ALL themes that show recent
  // books in any focusable row, not just Carousel. Flow's carousel pulls
  // from the same `recentBooks` vector but used to skip this pre-load —
  // meaning every L/R press on the Flow carousel did 2 SD reads inside
  // updateHighlightedBookContext (loadRecentBookStats + RecentBookProgress::
  // loadPercent), perceived as input latency. Doing it once at onEnter is
  // a few-ms one-time cost; per-press goes from 2 SD reads to a cache hit.
  // No-op on themes whose recentBooks is empty (e.g. minimal theme).
  loadAllBookStats();
  // CrumBLE #125: pre-bake the 4 perspective side-cover tiles for the
  // Flow carousel. ~24 KB tile cache; one-time SD load + perspective
  // walk per book per side AT entry, so the per-press carousel L/R
  // path becomes a pure 1bpp blit. No-op on non-Flow themes.
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
    static_cast<const LyraFlowTheme&>(GUI).prerenderCarouselSideTiles(renderer, recentBooks);
  }
  updateHighlightedBookContext();

  // CrumBLE #120: pick selectorIndex by priority:
  //   (1) initialMenuItem — caller (e.g. ActivityManager::goHome from a
  //       known menu activity like Bookshelf / Settings / FileBrowser)
  //       explicitly wants us on a specific icon. Highest priority.
  //   (2) hasSavedCursor_ — returning from an activity that didn't pass
  //       an initialMenuItem (popped overlays etc.). ReaderActivity::
  //       onEnter clears the saved cursor explicitly so reader -> Home
  //       falls through to (3) and the just-read book gets highlighted.
  //   (3) APP_STATE.openEpubPath — just-exited reader (cleared cursor
  //       above) or cold boot; land on the open book in the carousel.
  //   (4) Default selectorIndex = 0 from the row above.
  //
  // Before this restructure, the openEpubPath branch ran first and
  // always-wins-when-set was the bug — openEpubPath persists across
  // home visits once a book has been read, so every return from
  // Bookshelf landed on the prior-read book instead of the Bookshelf
  // icon. See clearSavedCursor() for the matching reader-side hook.
  if (initialMenuItem != HomeMenuItem::NONE) {
    const bool includeContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();
    const auto menuItems =
        buildSelectableHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks, includeContinueReading);
    const int menuIndex = findMenuActionIndex(menuItems, homeActionForInitialMenuItem(initialMenuItem));
    if (menuIndex >= 0) {
      // CrumBLE #120: on Flow theme the menu icons sit AFTER both the
      // carousel AND the shelf (selectorIndex = bookCount + shelfCount
      // + menuIdx). getHomeMenuSelectionOffset only contributes the
      // bookCount portion — we have to add shelfBookCount here too or
      // the cursor lands on a shelf book at index `menuIdx`.
      int shelfBookCount = 0;
      if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
        if (CollectionsStore::getInstance().getActiveCollection() != nullptr) {
          shelfBookCount = static_cast<int>(cachedShelfPaths().size());
        }
      }
      selectorIndex = getHomeMenuSelectionOffset(recentBooks) + shelfBookCount + menuIndex;
      updateHighlightedBookContext();
    }
  } else if (hasSavedCursor_) {
    // selectorIndex bounds are theme-dependent; use the saved value and
    // let the standard render-prep clamp it on the first render.
    selectorIndex = std::max(0, savedSelectorIndex_);
    updateHighlightedBookContext();
  } else if (!APP_STATE.openEpubPath.empty()) {
    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (recentBooks[i].path == APP_STATE.openEpubPath) {
        selectorIndex = i;
        lastCarouselBookIndex = i;
        updateHighlightedBookContext();
        break;
      }
    }
  }

  if (isCarouselTheme && hasValidCarouselDiskCache(recentBooks, renderer)) {
    preRenderCarouselFrames(false);
  }

  requestUpdate();
}

int HomeActivity::getHighlightedBookIndex() const {
  if (recentBooks.empty()) {
    return -1;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int highlightedBookIdx = (selectorIndex < bookCount) ? selectorIndex : lastCarouselBookIndex;
  return std::clamp(highlightedBookIdx, 0, bookCount - 1);
}

std::string HomeActivity::getCurrentBookPath() const {
  const int idx = getHighlightedBookIndex();
  return idx >= 0 ? recentBooks[idx].path : std::string{};
}

void HomeActivity::updateHighlightedBookContext() {
  const auto start = millis();
  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;

  const int idx = getHighlightedBookIndex();
  const bool useCachedStats = idx >= 0 && bookStatsCached && idx < kMaxCachedBooks;
  if (idx >= 0) {
    if (useCachedStats) {
      currentBookStats = cachedBookStats[idx];
      currentBookProgressPercent = cachedBookProgress[idx];
    } else {
      currentBookStats = loadRecentBookStats(recentBooks[idx]);
      currentBookProgressPercent = RecentBookProgress::loadPercent(recentBooks[idx]);
    }
  }

  hasReadingStats = hasAnyBookStats(currentBookStats) || hasAnyGlobalStats(globalStats);
  LOG_DBG("HOME", "updateHighlightedBookContext idx=%d cached=%s took %lums", idx, useCachedStats ? "yes" : "no",
          millis() - start);
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Snapshot cursor position so the next home visit lands the user
  // where they left off (whether they're going to settings, the file
  // browser, the bookshelf grid, etc.). The reader's just-read-book
  // promotion (see onEnter's openEpubPath block) wins over this snapshot
  // when applicable.
  savedSelectorIndex_ = selectorIndex;
  savedLastCarouselBookIndex_ = lastCarouselBookIndex;
  savedLastShelfBookIndex_ = lastShelfBookIndex;
  savedLastMenuIndex_ = lastMenuIndex;
  savedShelfHeaderFocused_ = shelfHeaderFocused;
  savedShelfPosByCollection_ = shelfPosByCollection;
  hasSavedCursor_ = true;

  freeCoverBuffer();
  gCarouselCache.invalidate();
  freeCarouselFrames();
  carouselWarmupPending = false;

  // CrumBLE #125: drop the pre-baked Flow carousel side-cover tiles
  // (~24 KB) so the reader's heap envelope isn't squeezed by carousel
  // state we're done with. Rebuilt on next home onEnter.
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
    static_cast<const LyraFlowTheme&>(GUI).clearCarouselSideTiles();
  }

  // CrumBLE #131: pre-shrink the in-RAM cover-bitmap cache only IF
  // heap is currently under pressure. Was renderer.clearImageCache()
  // which dumped every cached cover unconditionally -- that made
  // Home → Bookshelf transitions slow (~300 ms cold) because the grid
  // had to re-read all 9 thumb BMPs from SD when the home carousel had
  // just had them in RAM. Reconcile shrinks the cache only when free
  // heap demands it (e.g. NimBLE has eaten ~58 KB), so the common
  // non-BLE transitions keep the cache warm. The BLE concern the
  // original clear was added for is handled by the natural eviction-
  // on-insert path: when reader-side allocations trigger
  // lookupCachedBitmap, reconcile runs and evicts if heap is tight.
  renderer.reconcileImageCacheBudgetExt();
}

bool HomeActivity::storeCoverBuffer() {
  freeCoverBuffer();
  const bool isFlow =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW;
  if (isFlow) {
    // CrumBLE Flow: the shelf-skip fast-path restores the WHOLE home
    // (carousel + shelf + header + menu) on the next render, so snapshot the
    // full framebuffer — not just the cover tile (1.3's optimization, used in
    // the non-Flow carousel branch below).
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    if (!frameBuffer) return false;
    const size_t bufferSize = renderer.getBufferSize();
    coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
    if (!coverBuffer) {
      LOG_ERR("HOME", "OOM: cover buffer (full, %u bytes)", (unsigned)bufferSize);
      return false;
    }
    coverBufferSize = bufferSize;
    memcpy(coverBuffer, frameBuffer, bufferSize);
    return true;
  }
  // Non-Flow (CrossInk 1.3 carousel): snapshot just the cover tile. render()
  // must have set the cover rect; without it we'd clone the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) return false;
  // Distinguish a full-framebuffer snapshot (Flow) from a cover-tile snapshot
  // (non-Flow) by the stored size.
  if (coverBufferSize == renderer.getBufferSize()) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    if (!frameBuffer) return false;
    memcpy(frameBuffer, coverBuffer, coverBufferSize);
    return true;
  }
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::freeCarouselFrames() {
  // Instance pointers are aliases into the static cache — do not free here.
  for (int i = 0; i < kCarouselFrameCount; ++i) carouselFrames[i] = nullptr;
  carouselFramesReady = false;
}

bool HomeActivity::allocateCarouselFrameSlots(int targetFrameCount) {
  const size_t bufferSize = renderer.getBufferSize();
  int frameCount = 0;
  for (int attemptFrameCount = targetFrameCount; attemptFrameCount >= 1; --attemptFrameCount) {
    bool allocFailed = false;
    for (int i = 0; i < attemptFrameCount; ++i) {
      gCarouselCache.frames[i] = static_cast<uint8_t*>(malloc(bufferSize));
      if (!gCarouselCache.frames[i]) {
        LOG_ERR("HOME", "preRenderCarouselFrames: malloc failed for frame %d while allocating %d frame(s)", i,
                attemptFrameCount);
        allocFailed = true;
        break;
      }
      gCarouselCache.frameBookIdx[i] = -1;
    }

    if (!allocFailed) {
      frameCount = attemptFrameCount;
      break;
    }

    for (int i = 0; i < attemptFrameCount; ++i) {
      if (gCarouselCache.frames[i]) {
        free(gCarouselCache.frames[i]);
        gCarouselCache.frames[i] = nullptr;
      }
      gCarouselCache.frameBookIdx[i] = -1;
    }
  }

  if (frameCount == 0) {
    gCarouselCache.invalidate();
    return false;
  }

  gCarouselCache.frameCount = frameCount;
  LOG_INF("HOME", "carousel: frame cache capacity %d/%d", frameCount, targetFrameCount);
  return true;
}

void HomeActivity::renderCarouselFrameToCurrentBuffer(int bookIdx, BookReadingStats* outStats,
                                                      float* outProgressPercent, bool* outUsedCachedStats) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int bookCount = static_cast<int>(recentBooks.size());
  bool dummy1 = false, dummy2 = false, dummy3 = false;
  BookReadingStats frameStats;
  const BookReadingStats* frameStatsPtr = nullptr;
  float frameProgressPercent = -1.0f;
  bool usedCachedStats = false;

  if (bookIdx >= 0 && bookIdx < bookCount) {
    if (bookStatsCached && bookIdx < kMaxCachedBooks) {
      usedCachedStats = true;
      frameStats = cachedBookStats[bookIdx];
      frameProgressPercent = cachedBookProgress[bookIdx];
    } else {
      frameStats = loadRecentBookStats(recentBooks[bookIdx]);
      frameProgressPercent = RecentBookProgress::loadPercent(recentBooks[bookIdx]);
    }
    if (hasAnyBookStats(frameStats)) frameStatsPtr = &frameStats;
  }

  LyraCarouselTheme::setPreRenderIndex(bookIdx);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
  GUI.drawRecentBookCover(
      renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks, bookCount, dummy1,
      dummy2, dummy3, []() { return true; }, frameStatsPtr, frameProgressPercent);

  const bool frameHasReadingStats = hasAnyBookStats(frameStats) || hasAnyGlobalStats(globalStats);
  const auto menuItems = buildHomeMenuItems(hasOpdsServers, frameHasReadingStats, hasBookmarks);
  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), -1, [&menuItems](int index) { return std::string(menuItems[index].label); },
      [&menuItems](int index) { return menuItems[index].icon; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (outStats) *outStats = frameStats;
  if (outProgressPercent) *outProgressPercent = frameProgressPercent;
  if (outUsedCachedStats) *outUsedCachedStats = usedCachedStats;
}

bool HomeActivity::buildCarouselCacheFile(const std::string& cacheKey, uint64_t cacheKeyHash, int bookCount,
                                          bool showProgressPopup) {
  (void)cacheKey;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || bookCount <= 0) return false;

  Storage.mkdir("/.crosspoint");
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }

  FsFile file;
  if (!Storage.openFileForWrite("HOME", CAROUSEL_CACHE_TMP_PATH, file)) {
    return false;
  }

  const CarouselCacheHeader header = {
      CAROUSEL_CACHE_MAGIC,
      CAROUSEL_CACHE_VERSION,
      static_cast<uint16_t>(bookCount),
      static_cast<uint32_t>(renderer.getBufferSize()),
      cacheKeyHash,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbW),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbH),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverW),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverH),
  };
  if (!serialization::tryWritePod(file, header)) {
    file.close();
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache header");
    return false;
  }

  const auto start = millis();
  Rect popupRect{};
  uint8_t* progressFrameBuffer = nullptr;
  const size_t bufferSize = renderer.getBufferSize();
  if (showProgressPopup) {
    progressFrameBuffer = static_cast<uint8_t*>(malloc(bufferSize));
    if (!progressFrameBuffer) {
      LOG_ERR("HOME", "carousel: failed to allocate progress overlay buffer");
      showProgressPopup = false;
      // Heap is too tight for the animated progress bar (it needs a full-frame
      // backup to repaint between frames). Still show a static "Loading…" so
      // the warmup doesn't look like a hang. The build below renders frames to
      // SD without calling displayBuffer(), so this popup stays on the panel
      // until warmup finishes and the next render paints the carousel.
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      renderer.displayBuffer();
    } else {
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
      memcpy(progressFrameBuffer, frameBuffer, bufferSize);
    }
  }
  bool writeFailed = false;
  for (int i = 0; i < bookCount; ++i) {
    const int cachedSlot = gCarouselCache.findFrameSlot(i);
    if (cachedSlot >= 0 && carouselFrames[cachedSlot]) {
      memcpy(frameBuffer, carouselFrames[cachedSlot], renderer.getBufferSize());
    } else {
      renderCarouselFrameToCurrentBuffer(i, nullptr, nullptr, nullptr);
    }
    if (file.write(frameBuffer, renderer.getBufferSize()) != renderer.getBufferSize()) {
      writeFailed = true;
      break;
    }
    if (showProgressPopup) {
      memcpy(frameBuffer, progressFrameBuffer, bufferSize);
      GUI.fillPopupProgress(renderer, popupRect, ((i + 1) * 100) / bookCount);
    }
  }

  const bool syncOk = file.sync();
  file.close();

  if (writeFailed || !syncOk) {
    free(progressFrameBuffer);
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache snapshot");
    return false;
  }

  if (Storage.exists(CAROUSEL_CACHE_PATH)) {
    Storage.remove(CAROUSEL_CACHE_PATH);
  }
  if (!Storage.rename(CAROUSEL_CACHE_TMP_PATH, CAROUSEL_CACHE_PATH)) {
    free(progressFrameBuffer);
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to promote SD cache snapshot");
    return false;
  }

  free(progressFrameBuffer);
  LOG_DBG("HOME", "carousel: built SD cache for %d book(s) in %lums", bookCount, millis() - start);
  return true;
}

bool HomeActivity::loadCarouselFrameFromDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= kCarouselFrameCount || !gCarouselCache.frames[slotIdx] || bookIdx < 0 ||
      bookIdx >= bookCount) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, file)) {
    return false;
  }

  CarouselCacheHeader header{};
  if (!readCarouselCacheHeader(file, header) ||
      !isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer)) {
    file.close();
    return false;
  }

  const size_t frameOffset = sizeof(CarouselCacheHeader) + static_cast<size_t>(bookIdx) * renderer.getBufferSize();
  if (!file.seek(frameOffset)) {
    file.close();
    return false;
  }
  const size_t expectedBytes = renderer.getBufferSize();
  size_t totalBytesRead = 0;
  while (totalBytesRead < expectedBytes) {
    const int bytesRead = file.read(gCarouselCache.frames[slotIdx] + totalBytesRead, expectedBytes - totalBytesRead);
    if (bytesRead <= 0) {
      break;
    }
    totalBytesRead += static_cast<size_t>(bytesRead);
  }
  file.close();
  if (totalBytesRead != expectedBytes) {
    LOG_ERR("HOME", "carousel: short read for slot %d (%zu/%zu bytes)", slotIdx, totalBytesRead, expectedBytes);
    return false;
  }

  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
  return true;
}

int HomeActivity::chooseCarouselEvictionSlot(int centerIdx, int bookCount, std::optional<int> protectedBookIdx) const {
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (gCarouselCache.frames[i] && gCarouselCache.frameBookIdx[i] < 0) {
      return i;
    }
  }

  int evictSlot = -1;
  int maxDist = -1;
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (!gCarouselCache.frames[i]) continue;
    const int cachedBookIdx = gCarouselCache.frameBookIdx[i];
    if (protectedBookIdx.has_value() && cachedBookIdx == protectedBookIdx.value()) continue;
    const int diff = std::abs(cachedBookIdx - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }
  return evictSlot;
}

bool HomeActivity::preRenderCarouselFrames(bool showProgressPopup) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) return false;
  bool showedProgressPopup = false;

  // Build cache key from book paths plus thumb-asset availability so we don't
  // reuse a stale snapshot built before carousel-sized thumbs existed.
  std::string newKey;
  uint64_t newKeyHash = 0;
  buildCarouselCacheKey(recentBooks, newKey, newKeyHash);

  // Cache hit: same books in same order — reuse without any SD reads
  if (newKey == gCarouselCache.key && gCarouselCache.frameCount > 0) {
    for (int i = 0; i < gCarouselCache.frameCount; ++i) carouselFrames[i] = gCarouselCache.frames[i];
    carouselFramesReady = true;
    coverRendered = false;
    coverBufferStored = false;
    return false;
  }

  // Cache miss: free old cache and re-render
  if (!renderer.getFrameBuffer()) return false;
  freeCoverBuffer();  // reclaim 48KB before allocating frames
  gCarouselCache.invalidate();

  const int targetFrameCount = std::min(bookCount, kCarouselFrameCount);
  bool diskCacheValid = false;
  FsFile cacheFile;
  if (Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, cacheFile)) {
    CarouselCacheHeader header{};
    const bool readOk = readCarouselCacheHeader(cacheFile, header);
    cacheFile.close();
    diskCacheValid = readOk && isCarouselCacheHeaderValid(header, newKeyHash, bookCount, renderer);
  }

  if (!allocateCarouselFrameSlots(targetFrameCount)) {
    return showedProgressPopup;
  }

  // Keep only the current frame in RAM; adjacent frames come from the SD
  // snapshot on demand instead of occupying another framebuffer-sized slot.
  const int selectedBookIdx = (selectorIndex < bookCount) ? selectorIndex : lastCarouselBookIndex;
  const int initialBookIdx = (selectedBookIdx >= 0 && selectedBookIdx < bookCount) ? selectedBookIdx : 0;
  auto loadOrRender = [&](int bookIdx, int slot) {
    if (!diskCacheValid || !loadCarouselFrameFromDisk(newKeyHash, bookCount, bookIdx, slot)) {
      renderCarouselFrame(bookIdx, slot);
    }
  };
  loadOrRender(initialBookIdx, 0);
  gCarouselCache.lastCenterIdx = initialBookIdx;

  if (gCarouselCache.frameCount >= 2 && bookCount >= 2) {
    const int nextIdx = (initialBookIdx + 1) % bookCount;
    loadOrRender(nextIdx, 1);
  }

  if (gCarouselCache.frameCount >= 3 && bookCount >= 3) {
    const int prevIdx = (initialBookIdx + bookCount - 1) % bookCount;
    loadOrRender(prevIdx, 2);
  }

  const bool hasFullFrameCache = gCarouselCache.frameCount >= targetFrameCount;
  gCarouselCache.key = newKey;
  gCarouselCache.keyHash = diskCacheValid ? newKeyHash : 0;
  carouselFramesReady = true;
  coverRendered = false;
  coverBufferStored = false;

  // Persist the freshly-rendered carousel snapshot back to SD after Home is
  // already visible so later reader->Home returns and carousel navigation can
  // bootstrap from disk instead of live-rendering covers again.
  if (!diskCacheValid && gCarouselCache.frameCount > 0) {
    if (hasFullFrameCache) {
      const bool cacheBuilt = buildCarouselCacheFile(newKey, newKeyHash, bookCount, showProgressPopup);
      if (cacheBuilt) {
        gCarouselCache.keyHash = newKeyHash;
        showedProgressPopup = true;
      }
    } else {
      LOG_INF("HOME", "carousel: skipping SD cache build in degraded frame cache mode");
    }
  }
  return showedProgressPopup;
}

void HomeActivity::loop() {
  if (isMinimalTheme()) {
    const int pressedFrontButton = mappedInput.getPressedFrontButton();
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    if (minimalSuppressInitialFrontRelease) {
      if (releasedFrontButton >= 0) {
        minimalSuppressInitialFrontRelease = false;
        return;
      }
      if (!isAnyFrontButtonPressed(mappedInput)) {
        minimalSuppressInitialFrontRelease = false;
      }
    }

    if (minimalMenuOpen) {
      const auto menuItems = buildMinimalMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
      const int menuCount = static_cast<int>(menuItems.size());
      if (menuCount <= 0) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }

      if (minimalMenuIndex >= menuCount) {
        minimalMenuIndex = menuCount - 1;
      }

      buttonNavigator.onPreviousPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onNextPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        switch (menuItems[minimalMenuIndex].action) {
          case HomeMenuAction::BrowseFiles:
            onFileBrowserOpen();
            break;
          case HomeMenuAction::RecentBooks:
            onRecentsOpen();
            break;
          case HomeMenuAction::OpdsBrowser:
            onOpdsBrowserOpen();
            break;
          case HomeMenuAction::ReadingStats:
            onReadingStatsOpen();
            break;
          case HomeMenuAction::Bookmarks:
            onBookmarksOpen();
            break;
          case HomeMenuAction::FileTransfer:
            onFileTransferOpen();
            break;
          case HomeMenuAction::ContinueReading:
          case HomeMenuAction::Settings:
            break;
        }
      }
      return;
    }

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0 ? homeNavCount - 1
                                                    : ButtonNavigator::previousIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0 ? 0 : ButtonNavigator::nextIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }

    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1:
          onFileBrowserOpen();
          break;
        case 2:
          onSettingsOpen();
          break;
        case 3:
          onContinueReading();
          break;
      }
    };

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      minimalHomeNavIndex = 0;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_CONFIRM) {
      minimalHomeNavIndex = 1;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_LEFT) {
      minimalHomeNavIndex = 2;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        minimalHomeNavIndex = 3;
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (minimalHomeNavIndex >= 0) {
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    return;
  }

  const auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  const bool isLyraCarousel = themeType == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const bool isLyraFlow = themeType == CrossPointSettings::UI_THEME::LYRA_FLOW;
  const int previousHighlightedBookIdx = getHighlightedBookIndex();

  if (isLyraCarousel || isLyraFlow) {
    // Carousel + Flow share the same navigation grammar now that Flow also
    // renders its menu as a horizontal icon bar:
    //   - L/R iterates within the current row
    //   - U/D toggles between rows (carousel ↕ shelf header ↕ shelf books
    //     ↕ icon bar). The shelf rows only exist in Flow (phase 1/2);
    //     Carousel still does the two-row carousel/menu toggle.
    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuItemCount =
        static_cast<int>(buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks).size());

    // The shelf header is its own focus row (between carousel and books)
    // that appears whenever the user has at least one collection. The
    // header is what the user lands on when they press Down from the
    // carousel — from there L/R cycles the active collection and Down
    // enters its books. Carousel theme never shows the shelf, so it
    // ignores all of this and stays in the two-row carousel/menu model.
    const auto& collections = CollectionsStore::getInstance().getCollections();
    const bool shelfHeaderExists = isLyraFlow && !collections.empty();
    if (!isLyraFlow) {
      shelfHeaderFocused = false;  // safety: defensive reset off-Flow
    }
    // For virtual collections (Recently Added / All Books) the path list
    // comes from LibraryIndex, not from Collection::bookPaths. The
    // per-frame cache below means the heavy work (sort + copy) only
    // runs when the active collection actually changes.
    const Collection* activeCollection =
        isLyraFlow ? CollectionsStore::getInstance().getActiveCollection() : nullptr;
    const int shelfCount = (isLyraFlow && activeCollection != nullptr) ? static_cast<int>(cachedShelfPaths().size())
                                                                       : 0;
    const int shelfStart = bookCount;
    const int shelfEnd = shelfStart + shelfCount;
    const int menuStart = shelfEnd;
    const int menuEnd = menuStart + menuItemCount;

    const bool inHeaderRow = shelfHeaderFocused;
    const bool inCarouselRow = !inHeaderRow && selectorIndex < bookCount;
    const bool inShelfRow =
        !inHeaderRow && (shelfCount > 0) && (selectorIndex >= shelfStart) && (selectorIndex < shelfEnd);
    const bool inMenuRow = !inHeaderRow && selectorIndex >= menuStart && selectorIndex < menuEnd;

    // CrumBLE: per-collection two-row layout flag. When ON, the shelf is a
    // paged grid of 6 cols x 2 rows (12 covers/page) instead of a single
    // 4-cell row. Affects how L/R/U/D move within the shelf row -- with
    // 2-row on, U/D jump between rows of the current page, and L/R can
    // page-flip at the row edges.
    const Collection* activeColForNav = CollectionsStore::getInstance().getActiveCollection();
    const bool twoRowShelfActive = activeColForNav != nullptr && activeColForNav->twoRowShelf;
    const int navCellsPerRow =
        twoRowShelfActive ? LyraFlowTheme::shelfLayoutFor(2).cellsPerRow : LyraFlowTheme::shelfLayoutFor(1).cellsPerRow;
    const int navRowsPerPage = twoRowShelfActive ? 2 : 1;
    const int navPerPage = navCellsPerRow * navRowsPerPage;

    // Cycles the active collection. direction is +1 (next) or -1 (prev).
    // Wraps. Resets shelf-side render state so the new collection's
    // thumbs regenerate on next render. Persists the new activeId via
    // CollectionsStore::setActiveId which writes to SD. If the new
    // active is a virtual collection (Recently Added / All Books) and
    // the LibraryIndex hasn't been built this session, pre-warms it
    // here with a visible progress popup so the user sees feedback
    // instead of an unexplained pause.
    auto cycleActiveCollection = [this, &collections](int direction) {
      if (collections.size() <= 1) return;
      // Copy (not ref): getActiveId() returns a reference into the store that
      // setActiveId() below mutates — we need the leaving-collection id intact
      // to key its saved position.
      const std::string currentActive = CollectionsStore::getInstance().getActiveId();
      int idx = 0;
      for (size_t i = 0; i < collections.size(); ++i) {
        if (collections[i].id == currentActive) {
          idx = static_cast<int>(i);
          break;
        }
      }
      // Remember where we were in the collection we're leaving so a later
      // switch back restores the same scroll window + focused book.
      shelfPosByCollection[currentActive] = ShelfPos{shelfScrollOffset, lastShelfBookIndex};

      const int n = static_cast<int>(collections.size());
      idx = (idx + direction + n) % n;
      const std::string newActive = collections[idx].id;
      CollectionsStore::getInstance().setActiveId(newActive);
      // Restore the entering collection's saved position (default top-of-list
      // the first time it's visited this session). Both values get re-clamped
      // against the live collection size during render and on Down-into-books,
      // so a shrunk collection can't strand the cursor.
      const auto savedPos = shelfPosByCollection.find(newActive);
      if (savedPos != shelfPosByCollection.end()) {
        shelfScrollOffset = savedPos->second.scrollOffset;
        lastShelfBookIndex = savedPos->second.bookIndex;
      } else {
        shelfScrollOffset = 0;
        lastShelfBookIndex = 0;
      }
      shelfCoversLoaded = false;  // new collection probably has missing thumbs.
      seriesEnrichmentNeededForActive = true;  // new collection may have un-checked books.
      // The cache key check inside cachedShelfPaths() will detect the
      // new activeId automatically — no explicit invalidate needed.

      const Collection* newActiveCollection = CollectionsStore::getInstance().getActiveCollection();
      if (newActiveCollection != nullptr && newActiveCollection->isVirtual) {
        // Pre-warm the SD walk if it hasn't happened yet. ensureWalked
        // self-skips after the first walk this session so this is free
        // on subsequent cycles between virtuals.
        const Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        LibraryIndex::getInstance().ensureWalked(
            [&](int pct) { GUI.fillPopupProgress(renderer, popupRect, pct); });
      }
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (inHeaderRow) {
        cycleActiveCollection(+1);
      } else if (inCarouselRow && bookCount > 0) {
        selectorIndex = (selectorIndex + 1) % bookCount;
        lastCarouselBookIndex = selectorIndex;
      } else if (inShelfRow && shelfCount > 0) {
        const int shelfIdx = selectorIndex - shelfStart;
        if (twoRowShelfActive) {
          // 2-row R: within-row column step. At col=cellsPerRow-1, hop to
          // the same row of the next page (so the user's scan stays on
          // the row they were on). Wrap to first page if we ran off the
          // end of the collection.
          const int page = shelfIdx / navPerPage;
          const int visIdx = shelfIdx % navPerPage;
          const int row = visIdx / navCellsPerRow;
          const int col = visIdx % navCellsPerRow;
          int newCol = col + 1;
          int newPage = page;
          if (newCol >= navCellsPerRow) { newCol = 0; newPage++; }
          int newIdx = newPage * navPerPage + row * navCellsPerRow + newCol;
          if (newIdx >= shelfCount) {
            // Past the end -- wrap to same row, first page. Clamp to last
            // existing book in case the first-page row isn't fully
            // populated either (very small collection).
            newIdx = std::min(shelfCount - 1, row * navCellsPerRow);
          }
          selectorIndex = shelfStart + newIdx;
        } else {
          selectorIndex = shelfStart + (shelfIdx + 1) % shelfCount;
        }
      } else if (inMenuRow && menuItemCount > 0) {
        const int menuIdx = selectorIndex - menuStart;
        selectorIndex = menuStart + (menuIdx + 1) % menuItemCount;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (inHeaderRow) {
        cycleActiveCollection(-1);
      } else if (inCarouselRow && bookCount > 0) {
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
        lastCarouselBookIndex = selectorIndex;
      } else if (inShelfRow && shelfCount > 0) {
        const int shelfIdx = selectorIndex - shelfStart;
        if (twoRowShelfActive) {
          // 2-row L: within-row column step. At col=0, hop to the same row
          // of the previous page. Wrap to last page if we'd go negative.
          const int page = shelfIdx / navPerPage;
          const int visIdx = shelfIdx % navPerPage;
          const int row = visIdx / navCellsPerRow;
          const int col = visIdx % navCellsPerRow;
          int newCol = col - 1;
          int newPage = page;
          if (newCol < 0) {
            newCol = navCellsPerRow - 1;
            newPage--;
          }
          if (newPage < 0) {
            // Wrap to the LAST page on the same row. The last page may
            // not have a book at this col -- clamp to the last existing
            // book of that row.
            const int lastPage = (shelfCount - 1) / navPerPage;
            int newIdx = lastPage * navPerPage + row * navCellsPerRow + newCol;
            // If the row on the last page is partially populated and
            // newCol exceeds its actual book count, clamp.
            if (newIdx >= shelfCount) newIdx = shelfCount - 1;
            selectorIndex = shelfStart + newIdx;
          } else {
            const int newIdx = newPage * navPerPage + row * navCellsPerRow + newCol;
            selectorIndex = shelfStart + std::min(newIdx, shelfCount - 1);
          }
        } else {
          selectorIndex = shelfStart + (shelfIdx + shelfCount - 1) % shelfCount;
        }
      } else if (inMenuRow && menuItemCount > 0) {
        const int menuIdx = selectorIndex - menuStart;
        selectorIndex = menuStart + (menuIdx + menuItemCount - 1) % menuItemCount;
      }
      requestUpdate();
    }
    // Helper: clamp the remembered shelf-row index against the
    // current collection's size so a removed book / shrunk collection
    // doesn't strand the cursor past the end. Returns the resolved
    // selectorIndex (already shifted to shelfStart).
    auto enterShelfRowAtLastPos = [&]() {
      const int safeIdx = (shelfCount > 0) ? std::clamp(lastShelfBookIndex, 0, shelfCount - 1) : 0;
      return shelfStart + safeIdx;
    };
    // Same idea for the bottom menu row. Menu item count can vary
    // (e.g. depending on whether OPDS / bookmarks / etc. are present),
    // so clamp against the current count rather than the index space
    // at the time of last save.
    auto enterMenuRowAtLastPos = [&]() {
      const int safeIdx = (menuItemCount > 0) ? std::clamp(lastMenuIndex, 0, menuItemCount - 1) : 0;
      return menuStart + safeIdx;
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;
        if (shelfHeaderExists) {
          shelfHeaderFocused = true;
        } else if (shelfCount > 0) {
          selectorIndex = enterShelfRowAtLastPos();
        } else {
          selectorIndex = enterMenuRowAtLastPos();
        }
      } else if (inHeaderRow) {
        shelfHeaderFocused = false;
        // Enter books if any; otherwise skip the empty row straight to
        // the menu so Down still does something useful.
        selectorIndex = (shelfCount > 0) ? enterShelfRowAtLastPos() : enterMenuRowAtLastPos();
      } else if (inShelfRow) {
        // 2-row mode: D from TOP row -> bottom row same column on the same
        // page (if a book exists there). D from BOTTOM row -> exit to menu.
        // 1-row mode: any D -> menu.
        const int shelfIdx = static_cast<int>(selectorIndex) - shelfStart;
        if (twoRowShelfActive) {
          const int visIdx = shelfIdx % navPerPage;
          const int row = visIdx / navCellsPerRow;
          if (row == 0) {
            const int target = shelfIdx + navCellsPerRow;
            if (target < shelfCount) {
              selectorIndex = shelfStart + target;
              requestUpdate();
              return;
            }
            // No book in the bottom-row slot of this page -- fall through
            // to the exit-to-menu behavior below.
          }
        }
        // Save where we were in the books row so a future return
        // (Up from menu, Down from header) lands on the same book.
        lastShelfBookIndex = shelfIdx;
        selectorIndex = enterMenuRowAtLastPos();
      } else /* inMenuRow */ {
        // Save the menu position before wrapping back to the carousel
        // so a later Down→ here returns to the same icon.
        lastMenuIndex = static_cast<int>(selectorIndex) - menuStart;
        selectorIndex = lastCarouselBookIndex;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (inCarouselRow) {
        // Wrap to the bottom of the screen (menu row) for symmetry.
        lastCarouselBookIndex = selectorIndex;
        selectorIndex = enterMenuRowAtLastPos();
      } else if (inHeaderRow) {
        shelfHeaderFocused = false;
        selectorIndex = lastCarouselBookIndex;
      } else if (inShelfRow) {
        // 2-row mode: U from BOTTOM row -> top row same column. U from
        // TOP row -> header (existing behaviour). 1-row mode: any U ->
        // header.
        const int shelfIdx = static_cast<int>(selectorIndex) - shelfStart;
        if (twoRowShelfActive) {
          const int visIdx = shelfIdx % navPerPage;
          const int row = visIdx / navCellsPerRow;
          if (row == 1) {
            const int target = shelfIdx - navCellsPerRow;
            if (target >= 0) {
              selectorIndex = shelfStart + target;
              requestUpdate();
              return;
            }
            // Shouldn't reach here -- row=1 implies shelfIdx >=
            // cellsPerRow within the page -- but fall through safely.
          }
        }
        // Save where we were before bouncing up to the header.
        lastShelfBookIndex = shelfIdx;
        if (shelfHeaderExists) {
          shelfHeaderFocused = true;
        } else {
          selectorIndex = lastCarouselBookIndex;
        }
      } else /* inMenuRow */ {
        // Save menu position on the way out.
        lastMenuIndex = static_cast<int>(selectorIndex) - menuStart;
        if (shelfCount > 0) {
          selectorIndex = enterShelfRowAtLastPos();
        } else if (shelfHeaderExists) {
          shelfHeaderFocused = true;
        } else {
          selectorIndex = lastCarouselBookIndex;
        }
      }
      requestUpdate();
    }
  } else {
    const int menuCount = getMenuItemCount();
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  if (getHighlightedBookIndex() != previousHighlightedBookIdx) {
    // CrumBLE: updateHighlightedBookContext() reads per-book stats/progress from
    // the SD card on a cache miss. loop() runs on the main task without the
    // render lock, and the render task may be loading covers from the same
    // (non-thread-safe) SdFat + shared SPI bus at the same moment. Concurrent SD
    // access corrupts the SPI transaction/mutex state and panics in
    // xTaskPriorityDisinherit -- seen when bouncing to Home under a fragmented
    // heap (e.g. after a failed BLE chapter load). Hold the render lock so the
    // two never touch SD at once. Cheap: the highlight only changes on
    // navigation, and the common case is a cache hit with no SD I/O.
    RenderLock lock;
    updateHighlightedBookContext();
  }

  // CrumBLE Collections — keep the shelf's selected spine visible. Recompute
  // from the live collection size each iteration; cheap and avoids stale
  // offsets if the user added/removed books from another activity.
  if (isLyraFlow) {
    const Collection* activeCollection = CollectionsStore::getInstance().getActiveCollection();
    if (activeCollection != nullptr) {
      // Virtual collections have empty stored bookPaths — use the per-
      // frame cache so the scroll math doesn't pay for a fresh resolve.
      const int shelfCount = static_cast<int>(cachedShelfPaths().size());
      const int shelfStart = static_cast<int>(recentBooks.size());
      // Mirror the visible-cell math used inside drawBookshelfStrip so the
      // scroll window matches the renderer's view exactly. Pull dimensions
      // from the theme so this stays in lockstep with the strip's own
      // layout (1-row 4x1 vs 2-row 6x2).
      const int navRowCount = activeCollection->twoRowShelf ? 2 : 1;
      const LyraFlowTheme::ShelfLayout navLayout = LyraFlowTheme::shelfLayoutFor(navRowCount);
      const int visibleSpines = navLayout.cellsPerRow * navLayout.rowCount;
      if (selectorIndex >= shelfStart && selectorIndex < shelfStart + shelfCount) {
        const int focused = selectorIndex - shelfStart;
        if (activeCollection->twoRowShelf) {
          // Page-aligned scroll: each "page" is visibleSpines (=12) books.
          // L/R that crosses a page boundary causes the window to jump by
          // a full page so the user sees a clean group, not a sliding row.
          shelfScrollOffset = (focused / visibleSpines) * visibleSpines;
        } else {
          if (focused < shelfScrollOffset) shelfScrollOffset = focused;
          if (focused >= shelfScrollOffset + visibleSpines) shelfScrollOffset = focused - visibleSpines + 1;
        }
      }
      if (shelfScrollOffset > std::max(0, shelfCount - visibleSpines)) {
        shelfScrollOffset = std::max(0, shelfCount - visibleSpines);
      }
      if (shelfScrollOffset < 0) shelfScrollOffset = 0;
    } else {
      shelfScrollOffset = 0;
    }
  }

  // Long-press Confirm:
  //   • on a focused book (carousel or shelf single-book row) → file-action menu
  //   • on a focused SERIES cell on the shelf → series mini-picker
  //   • on the shelf header (collection tab) → header action menu
  //     (Sort, Rescan library, Collapse series toggle)
  // Threshold matches FileBrowser's GO_HOME_MS so the muscle memory
  // carries over.
  if (!longPressConfirmHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= 1000) {
    if (shelfHeaderFocused) {
      longPressConfirmHandled = true;
      showShelfHeaderActionMenu();
      return;
    }
    // Series-cell long-press → mini-picker (per-book action menu is
    // available via long-press INSIDE the mini-picker).
    if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
      const Collection* active = CollectionsStore::getInstance().getActiveCollection();
      if (active != nullptr) {
        const std::vector<ShelfEntry>& entries = cachedShelfEntries();
        const int shelfStart = static_cast<int>(recentBooks.size());
        const int idx = static_cast<int>(selectorIndex) - shelfStart;
        if (idx >= 0 && idx < static_cast<int>(entries.size()) && entries[idx].memberPaths.size() >= 2) {
          longPressConfirmHandled = true;
          openSeriesMiniPicker(entries[idx]);
          return;
        }
      }
    }
    // CrumBLE #81: long-press on the icon-bar's Bookshelf entry brings up
    // a collection picker. Tap = open the active collection's grid;
    // long-press = pick a different collection, then open that grid.
    // shelfBookCount is computed inline (matches the short-press path's
    // shelfBookCount derivation a few hundred lines below).
    int shelfBookCountForLongPress = 0;
    if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW) {
      const Collection* activeCol = CollectionsStore::getInstance().getActiveCollection();
      if (activeCol != nullptr) {
        shelfBookCountForLongPress = static_cast<int>(cachedShelfEntries().size());
      }
    }
    const auto& menuItemsForLongPress = buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
    const int menuSelectedIdxForLongPress =
        selectorIndex - getHomeMenuSelectionOffset(recentBooks) - shelfBookCountForLongPress;
    if (menuSelectedIdxForLongPress >= 0 &&
        menuSelectedIdxForLongPress < static_cast<int>(menuItemsForLongPress.size()) &&
        menuItemsForLongPress[menuSelectedIdxForLongPress].action == HomeMenuAction::RecentBooks) {
      longPressConfirmHandled = true;
      showBookshelfCollectionPicker();
      return;
    }
    const std::string focusedPath = getFocusedBookPath();
    if (!focusedPath.empty()) {
      longPressConfirmHandled = true;
      showHomeBookActionMenu(focusedPath);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressConfirmHandled) {
      // Swallow the release that ended the long-press so the short-press
      // open-book / menu-activation handlers below don't also fire.
      longPressConfirmHandled = false;
      return;
    }
    // Short-press on the collection title (shelf header focus) opens
    // the Bookshelf grid for the active collection -- same target as
    // a tap of the Bookshelf icon below. Previously this dove the
    // selector into the shelf row; that behaviour was redundant with
    // the Down key, and surveying every book in the active collection
    // is exactly what the Bookshelf grid was added for.
    if (shelfHeaderFocused) {
      activityManager.goToBookshelf();
      return;
    }
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!metrics.homeContinueReadingInMenu && selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }

    // CrumBLE Collections — Flow theme's bookshelf row. Selection indices
    // sit between the carousel and the menu icon bar; open the matching
    // book path directly.
    const auto activeThemeForConfirm = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
    int shelfBookCount = 0;
    if (activeThemeForConfirm == CrossPointSettings::UI_THEME::LYRA_FLOW) {
      const Collection* activeCollection = CollectionsStore::getInstance().getActiveCollection();
      if (activeCollection != nullptr) {
        // Per-frame cache — covers both user collections (stored) and
        // virtuals (LibraryIndex-derived) uniformly. Indices map 1:1
        // to ShelfEntries (a series group counts as one entry).
        const std::vector<ShelfEntry>& entries = cachedShelfEntries();
        const int shelfStart = static_cast<int>(recentBooks.size());
        shelfBookCount = static_cast<int>(entries.size());
        if (selectorIndex >= shelfStart && selectorIndex < shelfStart + shelfBookCount) {
          openShelfEntry(entries[selectorIndex - shelfStart]);
          return;
        }
      }
    }

    auto menuItems = buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
    if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
      menuItems.insert(menuItems.begin(), {tr(STR_CONTINUE_READING), Book, HomeMenuAction::ContinueReading});
    }
    const int menuSelectedIndex = selectorIndex - getHomeMenuSelectionOffset(recentBooks) - shelfBookCount;
    if (menuSelectedIndex < 0 || menuSelectedIndex >= static_cast<int>(menuItems.size())) {
      return;
    }

    switch (menuItems[menuSelectedIndex].action) {
      case HomeMenuAction::BrowseFiles:
        onFileBrowserOpen();
        break;
      case HomeMenuAction::ContinueReading:
        onContinueReading();
        break;
      case HomeMenuAction::RecentBooks:
        onRecentsOpen();
        break;
      case HomeMenuAction::OpdsBrowser:
        onOpdsBrowserOpen();
        break;
      case HomeMenuAction::ReadingStats:
        onReadingStatsOpen();
        break;
      case HomeMenuAction::Bookmarks:
        onBookmarksOpen();
        break;
      case HomeMenuAction::FileTransfer:
        onFileTransferOpen();
        break;
      case HomeMenuAction::Settings:
        onSettingsOpen();
        break;
    }
  }
}

void HomeActivity::updateFocusedBookMeta(const std::string& path) {
  if (path == focusedMetaPath) return;  // focused book unchanged — reuse cache
  focusedMetaPath = path;
  focusedMetaTitle.clear();
  focusedMetaAuthor.clear();
  const size_t slash = path.find_last_of('/');
  const std::string fname = (slash != std::string::npos) ? path.substr(slash + 1) : path;
  // Read the cached metadata only (buildIfMissing=false): cheap, and leaves the
  // title blank for un-indexed books so the caller falls back to the filename.
  // Strip trailing ";"/whitespace from author -- some EPUBs leave a
  // separator without a value, which without this trim renders as a
  // dangling ";" on the carousel/shelf. Local helper inline to avoid
  // pulling RecentBooksGridActivity's namespace.
  const auto trimAuthor = [](std::string s) -> std::string {
    while (!s.empty()) {
      const char c = s.back();
      if (c == ' ' || c == '\t' || c == ';' || c == ',' || c == '\r' || c == '\n') s.pop_back();
      else break;
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ';' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i > 0) s.erase(0, i);
    return s;
  };
  if (FsHelpers::hasEpubExtension(fname)) {
    Epub epub(path, "/.crosspoint");
    epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
    focusedMetaTitle = epub.getTitle();
    focusedMetaAuthor = trimAuthor(epub.getAuthor());
  } else if (FsHelpers::hasXtcExtension(fname)) {
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      focusedMetaTitle = xtc.getTitle();
      focusedMetaAuthor = trimAuthor(xtc.getAuthor());
    }
  }
  // .txt / .md have no embedded metadata — leave title empty (filename fallback).
}

void HomeActivity::presentHomeBuffer() {
  if (pendingFullRefresh) {
    pendingFullRefresh = false;
    // One full clear on entry wipes ghosting bled through from the previous
    // screen (the reader page, a low-memory alert, etc.).
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (isMinimalTheme()) {
    renderer.clearScreen();

    if (minimalMenuOpen) {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      const auto menuItems = buildMinimalMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
      GUI.drawButtonMenu(
          renderer, Rect{0, metrics.homeTopPadding, pageWidth, pageHeight - metrics.homeTopPadding},
          static_cast<int>(menuItems.size()), minimalMenuIndex,
          [&menuItems](int index) { return std::string(menuItems[index].label); },
          [&menuItems](int index) { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      presentHomeBuffer();
      return;
    }

    bool bufferRestored = coverBufferStored && restoreCoverBuffer();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

    GUI.drawRecentBookCover(
        renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks, selectorIndex,
        coverRendered, coverBufferStored, bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this),
        hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent);

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }
    MinimalTheme::setHomeButtonHintSelection(minimalHomeNavIndex);
    GUI.drawButtonHints(renderer, "Menu", "Browse", "Settings", recentBooks.empty() ? "" : "Read");

    presentHomeBuffer();

    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
      return;
    }

    if (!recentsLoaded && !recentsLoading) {
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
    return;
  }

  // Fast path: pre-rendered frames ready — memcpy + border overlay
  if (carouselFramesReady) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int centerIdx = inCarouselRow ? selectorIndex : lastCarouselBookIndex;
    int slotIdx = gCarouselCache.findFrameSlot(centerIdx);

    if (frameBuffer && slotIdx < 0 && gCarouselCache.keyHash != 0 && bookCount > 0) {
      const int evictSlot = chooseCarouselEvictionSlot(centerIdx, bookCount);
      if (evictSlot >= 0 && loadCarouselFrameFromDisk(gCarouselCache.keyHash, bookCount, centerIdx, evictSlot)) {
        slotIdx = evictSlot;
      }
    }

    if (frameBuffer && slotIdx >= 0 && carouselFrames[slotIdx]) {
      memcpy(frameBuffer, carouselFrames[slotIdx], renderer.getBufferSize());
      LyraCarouselTheme::setPreRenderIndex(centerIdx);

      GUI.drawCarouselBorder(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                             recentBooks, centerIdx, inCarouselRow);
      if (!inCarouselRow) {
        const auto menuItems = buildHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks);
        if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) ==
            CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
          static_cast<const LyraCarouselTheme&>(GUI).drawButtonMenuSelectionOverlay(
              renderer, static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
              [&menuItems](int index) { return std::string(menuItems[index].label); },
              [&menuItems](int index) { return menuItems[index].icon; });
        }
      }

      presentHomeBuffer();
      // E-ink refresh complete — pre-render the missing adjacent frame while idle.
      updateSlidingWindowCache(centerIdx, bookCount);
      // Mirror the slow-path trigger: generate missing thumbnails on the second
      // render so the E-ink is already showing something before the SD work starts.
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      } else if (!recentsLoaded && !recentsLoading) {
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
      }
      return;
    }
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  // Reset per-render: set true if any progress popup gets drawn over the
  // framebuffer below (see homeRenderPopupShown doc). Drives the snapshot
  // skip + clean-repaint at end of render so popups don't get stuck.
  homeRenderPopupShown = false;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Flow theme decodes (selectorIndex - bookCount) as a "carousel center hint"
  // when no carousel slot is selected. Without this encoding the carousel
  // would drift forward by one as the user iterates through menu items.
  // Pinning the encoded hint to lastCarouselBookIndex keeps the carousel
  // visually stationary while the menu cursor moves.
  const int bookCountForRender = static_cast<int>(recentBooks.size());
  const auto activeThemeForRender = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  // Hold the carousel still whenever focus is NOT on a carousel book. That
  // includes focus on a menu item (selectorIndex >= bookCount) AND focus
  // on the shelf header — in the header case selectorIndex still sits in
  // carousel range from the cursor's last carousel position, but the
  // carousel itself shouldn't appear to be the active row.
  const bool flowCarouselHold = activeThemeForRender == CrossPointSettings::UI_THEME::LYRA_FLOW &&
                                (selectorIndex >= bookCountForRender || shelfHeaderFocused);
  const int coverSelectorIndex =
      flowCarouselHold ? (bookCountForRender +
                          (lastCarouselBookIndex >= 0 && lastCarouselBookIndex < bookCountForRender
                               ? lastCarouselBookIndex
                               : 0))
                       : selectorIndex;

  // On Flow theme we defer the framebuffer snapshot until AFTER the shelf
  // is painted (see end of render). The theme's built-in storer would
  // snapshot a pre-cover, pre-shelf state — too early to be useful for
  // the shelf skip-fast-path. Passing a no-op lets the theme keep its
  // flag bookkeeping while we own the snapshot timing. Non-Flow themes
  // continue to use the in-theme snapshot since they don't have a shelf.
  const bool isLyraFlowForRender = activeThemeForRender == CrossPointSettings::UI_THEME::LYRA_FLOW;
  auto storer =
      isLyraFlowForRender ? std::function<bool()>([] { return true; })
                          : std::function<bool()>(std::bind(&HomeActivity::storeCoverBuffer, this));

  // Carousel cover-load skip fast-path. When the buffer restore brought
  // back the previous frame's carousel pixels AND the actual center book
  // matches the one that was painted into the buffer, the theme can skip
  // its 5 BMP loads. Saves ~80% of drawRecentBookCover's cost on every
  // "L/R within shelf/menu" type input — those don't change the carousel
  // but currently force it to repaint anyway.
  //
  // CrumBLE #125: previously this compared the ENCODED coverSelectorIndex
  // — which flips between `selectorIndex` and `(bookCount + lastCarousel
  // BookIndex)` when focus enters/leaves the carousel (flowCarouselHold).
  // That made the Down-from-carousel transition a guaranteed cache miss
  // even though the center book didn't actually change, forcing a full
  // 5-cover repaint just to remove the selection border. Decode here so
  // the comparison sees the real center idx; the theme now reconciles
  // the selection border separately within its skip path.
  if (isLyraFlowForRender) {
    int effectiveCenterIdx = 0;
    if (flowCarouselHold) {
      effectiveCenterIdx =
          (lastCarouselBookIndex >= 0 && lastCarouselBookIndex < bookCountForRender) ? lastCarouselBookIndex : 0;
    } else if (selectorIndex >= 0 && selectorIndex < bookCountForRender) {
      effectiveCenterIdx = selectorIndex;
    }
    const bool canSkipCovers =
        bufferRestored && lastRenderedCoverSelectorValid && effectiveCenterIdx == lastRenderedCoverSelectorIdx;
    // skipCarouselCoverLoads is declared `mutable` precisely to allow
    // a single-flight assignment through the const theme reference.
    static_cast<const LyraFlowTheme&>(GUI).skipCarouselCoverLoads = canSkipCovers;
  }
  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, coverSelectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          storer,
                          hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent);
  // Remember what we just painted so the next render can short-circuit.
  // CrumBLE #125: store the DECODED center idx (not the encoded
  // coverSelectorIndex) so the next render's comparison correctly hits
  // across the flowCarouselHold transition. Mirrors the decode block
  // that computes `effectiveCenterIdx` for canSkipCovers above.
  if (isLyraFlowForRender) {
    int effectiveCenterIdx = 0;
    if (flowCarouselHold) {
      effectiveCenterIdx =
          (lastCarouselBookIndex >= 0 && lastCarouselBookIndex < bookCountForRender) ? lastCarouselBookIndex : 0;
    } else if (selectorIndex >= 0 && selectorIndex < bookCountForRender) {
      effectiveCenterIdx = selectorIndex;
    }
    lastRenderedCoverSelectorIdx = effectiveCenterIdx;
    lastRenderedCoverSelectorValid = true;
  }

  auto menuItems = buildSelectableHomeMenuItems(hasOpdsServers, hasReadingStats, hasBookmarks,
                                                metrics.homeContinueReadingInMenu && !recentBooks.empty());

  const int menuStartY = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int menuEndY = pageHeight - metrics.buttonHintsHeight;
  const int menuHeight = std::max(0, menuEndY - menuStartY);

  // NOTE: do NOT manually insert "Continue Reading" here -- buildSelectableHomeMenuItems
  // already inserts it at items.begin() when the includeContinueReading flag is true.
  // Inserting it again duplicated the entry in any theme with
  // homeContinueReadingInMenu = true (RoundedRaff), shifting every other action by
  // one slot (File Transfer fired Settings, last item became a silent no-op).

  // CrumBLE Flow bookshelf — render strip between cover footer and icon bar,
  // and offset the menu's selected-index calculation so the icon-bar
  // selection-highlight tracks the right item when the cursor moves past
  // the shelf.
  const auto activeThemeForShelf = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  int shelfBookCount = 0;
  int shelfSelectedSpine = -1;
  if (activeThemeForShelf == CrossPointSettings::UI_THEME::LYRA_FLOW) {
    const Collection* activeCollection = CollectionsStore::getInstance().getActiveCollection();
    if (activeCollection != nullptr) {
      // Per-frame cache — counts are derived from the same resolved
      // vector that the render uses below, so this is free.
      shelfBookCount = static_cast<int>(cachedShelfPaths().size());
      const int shelfStart = static_cast<int>(recentBooks.size());
      // Only mark a book as focused when the cursor is actually on the
      // books row. When `shelfHeaderFocused` is true, selectorIndex
      // still sits in shelf range (we don't move it on Up-from-books)
      // but the focus is conceptually on the header — leaving
      // shelfSelectedSpine at -1 hides both the focus ring and the
      // book-title overlay so the UI matches the user's mental model.
      if (!shelfHeaderFocused && selectorIndex >= shelfStart && selectorIndex < shelfStart + shelfBookCount) {
        shelfSelectedSpine = selectorIndex - shelfStart;
      }
    }
    // Strip sits in the dead space between the carousel footer and the icon
    // bar. Center it vertically in that empty band so neither edge crowds.
    // Empirical anchors on Flow @ 480x800 portrait:
    //   carousel footer ends ~y=460 (cover + reading-progress bar)
    //   icon-bar label top  ~y=689
    //   midpoint            ~y=575
    // CrumBLE: pull the cell/strip dimensions from the theme so we don't
    // duplicate the layout constants. Two-row layout was removed from the
    // UI; the layout-for-rowCount infrastructure stays in place for the
    // future but is hard-pinned to 1-row here.
    constexpr int shelfRowCount = 1;
    const LyraFlowTheme::ShelfLayout shelfLayout = LyraFlowTheme::shelfLayoutFor(shelfRowCount);
    const int kShelfCellWidth = shelfLayout.cellWidth;
    const int kShelfCellHeight = shelfLayout.cellHeight;
    const int kShelfStripHeight = shelfLayout.stripHeight;
    // sidePad/cellGap stay constant across layouts; visibleCells counts the
    // FULL page (rows*cols) so scrolling math advances by a whole page.
    constexpr int kShelfSidePad = 16;
    (void)kShelfSidePad;  // referenced indirectly via the strip rect / theme
    const int shelfVisibleCells = shelfLayout.cellsPerRow * shelfLayout.rowCount;

    // Series enrichment — runs once per cycle into a new active
    // collection, only when collapseSeries is on for that collection.
    // Shows its own "Detecting series..." progress popup; no-op if
    // SeriesIndex already has every book covered.
    if (seriesEnrichmentNeededForActive) {
      enrichActiveCollectionForSeries();
    }

    // Lazy thumb generation: only build BMPs for the cells currently in
    // view. Collections like "All Books" can hold hundreds of entries;
    // eager generation would freeze the UI for minutes the first time
    // the user switched to it. Storage.exists short-circuits visited
    // cells, so subsequent renders are near-free once a window's thumbs
    // are on SD.
    loadShelfCovers(kShelfCellWidth, kShelfCellHeight, shelfScrollOffset, shelfVisibleCells);

    // Fast-path skip: if the framebuffer was restored from the last
    // render AND every shelf-affecting state value is unchanged, the
    // shelf pixels are already on screen — no need to repeat the
    // 4-cells-of-SD-BMP load that drawBookshelfStrip does. This is the
    // single biggest contributor to home-screen lag for navigation that
    // doesn't touch the shelf row.
    const std::string& currentShelfActiveId = CollectionsStore::getInstance().getActiveId();
    const bool shelfStateMatchesSnapshot =
        bufferRestored && shelfSnapshotValid && currentShelfActiveId == shelfSnapshotActiveId &&
        shelfScrollOffset == shelfSnapshotScrollOffset && shelfSelectedSpine == shelfSnapshotFocusedSpine &&
        shelfHeaderFocused == shelfSnapshotHeaderFocused;

    // CrumBLE #125: focus-only diff -- same shelf state except the
    // focused-cell index changed. This is the dominant case for shelf
    // L/R navigation within a single page. We can take the partial-
    // repaint fast path (erase prev ring + redraw shadow it overlapped,
    // draw new ring, refresh title text strip) instead of the full
    // 4-cell repaint.
    const bool shelfFocusOnlyDiff = !shelfStateMatchesSnapshot && bufferRestored && shelfSnapshotValid &&
                                    currentShelfActiveId == shelfSnapshotActiveId &&
                                    shelfScrollOffset == shelfSnapshotScrollOffset &&
                                    shelfHeaderFocused == shelfSnapshotHeaderFocused &&
                                    shelfSelectedSpine != shelfSnapshotFocusedSpine;

    // Position the strip slightly below the geometric midpoint of the empty
    // band between cover tile bottom (~y=401) and icon-bar label top (~y=686).
    // Re-tuned in iter 5 to make room for the focused-book title under the
    // row: previous value (240) clipped the title's bottom ~1/3 against
    // the icon bar's label area.
    // CrumBLE: dropped from 260 to 242 to push the shelf strip ~18 px lower
    // (one text line worth). The carousel now stacks title -> author above
    // the center cover (LyraFlowTheme), so the cover + footer block sits
    // visually lower; without dropping the shelf, the collection-name tab
    // crowded the author caption above it.
    constexpr int kEmptySpaceMidpointFromBottom = 242;
    const int shelfStripY = pageHeight - kEmptySpaceMidpointFromBottom - (kShelfStripHeight / 2);
    const Rect shelfRect{0, shelfStripY, pageWidth, kShelfStripHeight};

    const Collection* activeCollection2 = CollectionsStore::getInstance().getActiveCollection();
    const char* collectionName = (activeCollection2 != nullptr) ? activeCollection2->name.c_str() : "";
    // Header focus + cycle-hint flags forwarded to the theme so it can draw
    // the "◀ Collection ▶" affordance only when both apply. Otherwise the
    // arrows would be misleading (single-collection case can't cycle).
    const bool hasMultipleCollections = CollectionsStore::getInstance().getCollections().size() > 1;
    // Compute the focused book title (filename minus extension) only when
    // a shelf book is focused — same trick the carousel uses to caption
    // a cover without having to load the EPUB metadata up front.
    // thread_local buffer avoids reallocating per render while still
    // keeping the c_str pointer stable until the next call.
    static thread_local std::string focusedTitleBuf;
    const char* focusedTitle = nullptr;
    // Author shown on a second line under the title -- only when we resolved the
    // title from metadata (filename-fallback and series cells have no author).
    const char* focusedAuthor = nullptr;
    if (shelfSelectedSpine >= 0 && activeCollection2 != nullptr) {
      const std::vector<ShelfEntry>& entries = cachedShelfEntries();
      if (shelfSelectedSpine < static_cast<int>(entries.size())) {
        const ShelfEntry& e = entries[shelfSelectedSpine];
        if (!e.seriesName.empty() && e.memberPaths.size() >= 2) {
          // Series cell: show "Series Name (N)" instead of filename.
          focusedTitleBuf = e.seriesName;
          focusedTitleBuf += " (";
          focusedTitleBuf += std::to_string(e.memberPaths.size());
          focusedTitleBuf += ")";
        } else {
          // Single book: prefer the EPUB/XTC metadata title; fall back to the
          // filename (minus extension) only when no metadata is available.
          const std::string& bp = e.firstPath;
          updateFocusedBookMeta(bp);
          if (!focusedMetaTitle.empty()) {
            focusedTitleBuf = focusedMetaTitle;
            if (!focusedMetaAuthor.empty()) focusedAuthor = focusedMetaAuthor.c_str();
          } else {
            const size_t slash = bp.find_last_of('/');
            const std::string fname = (slash != std::string::npos) ? bp.substr(slash + 1) : bp;
            const size_t dot = fname.find_last_of('.');
            focusedTitleBuf = (dot != std::string::npos && dot > 0) ? fname.substr(0, dot) : fname;
          }
        }
        focusedTitle = focusedTitleBuf.c_str();
      }
    }
    // Build the parallel per-cell series-member-count vector so the
    // theme knows which cells deserve the spine glyph. One int per
    // ShelfEntry; 1 = single book, ≥2 = series group.
    // CrumBLE #125: also needed on the focus-only path so the partial
    // repaint knows whether to restore the dark series spine that the
    // erased ring stroke overlapped with on the previously focused cell.
    std::vector<int> seriesMemberCounts;
    if (activeCollection2 != nullptr && (!shelfStateMatchesSnapshot || shelfFocusOnlyDiff)) {
      const std::vector<ShelfEntry>& entries = cachedShelfEntries();
      seriesMemberCounts.reserve(entries.size());
      for (const auto& e : entries) seriesMemberCounts.push_back(static_cast<int>(e.memberPaths.size()));
    }

    // Resolve a concrete (dimension-substituted) cover thumbnail path for
    // each book. getThumbBmpPath() returns a template like
    //   /.crosspoint/<book>/thumb_[WIDTH]x[HEIGHT].bmp
    // which is not a real file — UITheme::getCoverThumbPath fills in the
    // placeholders. Books whose thumb wasn't generated (or whose generation
    // failed in loadShelfCovers) get an empty string so the renderer draws
    // the placeholder card instead of trying to open a non-existent file.
    //
    // CrumBLE #124: only resolve cover paths for the VISIBLE window.
    // drawBookshelfStrip only reads indices [scrollOffset, scrollOffset +
    // actualDrawn) — a 4-book window for the 1-row layout — but this loop
    // used to walk every entry in the collection. With "All Books" active
    // (100+ entries), that was 100 Epub/Xtc constructions + 100
    // Storage.exists() SD-stat calls per render, on a hot path that fires
    // on every shelf-row L/R press. Now it scales with the on-screen cell
    // count instead of the collection size. The full-sized vector with
    // empty slots outside the window keeps the theme's `coverPaths[spineIdx]`
    // indexing valid (empty string => placeholder card, but those cells
    // are off-screen anyway so the theme never draws them).
    // CrumBLE #125: on the focus-only fast path we don't need cover
    // paths -- the cells (including their cover bitmaps) are already on
    // screen from the framebuffer restore. Skip the resolution loop
    // entirely. Same for the full-snapshot-match case.
    std::vector<std::string> shelfCoverPaths;
    // CrumBLE: per-cell titles for the theme's placeholder fallback.
    // Filled with the series name for series cells, otherwise the
    // filename (no metadata lookup -- the focused-cell branch above
    // is the only place we pay for metadata resolution). Empty entries
    // outside the visible window leave the theme on its CoverIcon
    // fallback for those cells (which never render anyway since the
    // theme loops only over the visible page).
    std::vector<std::string> shelfCellTitles;
    if (activeCollection2 != nullptr && !shelfStateMatchesSnapshot && !shelfFocusOnlyDiff) {
      const std::vector<std::string>& renderPaths = cachedShelfPaths();
      const std::vector<ShelfEntry>& entries = cachedShelfEntries();
      const int total = static_cast<int>(renderPaths.size());
      shelfCoverPaths.assign(total, std::string{});
      shelfCellTitles.assign(total, std::string{});
      const int winStart = std::clamp(shelfScrollOffset, 0, total);
      const int winEnd = std::min(winStart + shelfVisibleCells, total);
      for (int i = winStart; i < winEnd; ++i) {
        const std::string& path = renderPaths[i];
        std::string templatePath;
        if (FsHelpers::hasEpubExtension(path)) {
          templatePath = Epub(path, "/.crosspoint").getThumbBmpPath();
        } else if (FsHelpers::hasXtcExtension(path)) {
          templatePath = Xtc(path, "/.crosspoint").getThumbBmpPath();
        }
        if (!templatePath.empty()) {
          std::string resolved = UITheme::getCoverThumbPath(templatePath, kShelfCellWidth, kShelfCellHeight);
          if (!resolved.empty() && Storage.exists(resolved.c_str())) {
            shelfCoverPaths[i] = std::move(resolved);
          }
        }
        // Title fallback: series name for series cells, else filename.
        if (i < static_cast<int>(entries.size())) {
          const ShelfEntry& e = entries[i];
          if (!e.seriesName.empty() && e.memberPaths.size() >= 2) {
            shelfCellTitles[i] = e.seriesName;
          } else {
            const size_t slash = path.find_last_of('/');
            std::string fname = (slash != std::string::npos) ? path.substr(slash + 1) : path;
            const size_t dot = fname.find_last_of('.');
            if (dot != std::string::npos && dot > 0) fname = fname.substr(0, dot);
            shelfCellTitles[i] = std::move(fname);
          }
        }
      }
    }
    if (shelfFocusOnlyDiff) {
      // CrumBLE #125: focus moved within the same page. Patch the two
      // affected cells' ring + title band; skip the full cell-by-cell
      // repaint loop in drawBookshelfStrip.
      const int bookCountForFocus =
          (activeCollection2 != nullptr) ? static_cast<int>(cachedShelfEntries().size()) : 0;
      static_cast<const LyraFlowTheme&>(GUI).drawBookshelfStripFocusUpdate(
          renderer, shelfRect, shelfSnapshotFocusedSpine, shelfSelectedSpine, shelfScrollOffset, bookCountForFocus,
          focusedTitle, &seriesMemberCounts, shelfRowCount);
      // Only the focused-spine slot of the snapshot needs to advance;
      // everything else (activeId, scroll, header focus) is unchanged
      // by the focus diff.
      shelfSnapshotFocusedSpine = shelfSelectedSpine;
    } else if (!shelfStateMatchesSnapshot) {
      static_cast<const LyraFlowTheme&>(GUI).drawBookshelfStrip(
          renderer, shelfRect, collectionName, shelfCoverPaths, shelfSelectedSpine, shelfScrollOffset,
          shelfHeaderFocused, hasMultipleCollections, focusedTitle, &seriesMemberCounts, focusedAuthor,
          shelfRowCount, &shelfCellTitles);
      // Remember the state of the shelf we just painted so the next
      // render can short-circuit if nothing about it has changed.
      shelfSnapshotActiveId = currentShelfActiveId;
      shelfSnapshotScrollOffset = shelfScrollOffset;
      shelfSnapshotFocusedSpine = shelfSelectedSpine;
      shelfSnapshotHeaderFocused = shelfHeaderFocused;
      shelfSnapshotValid = true;
    }

    // CrumBLE Flow: route the focused book's author into the icon bar's
    // label slot (consumed by drawButtonMenu below). Same physical spot
    // the selected icon's name normally occupies, so the user always sees
    // ONE contextual label adjacent to the icon row -- author when a
    // book is hovered, icon name when an icon is hovered. The old author
    // position under the shelf cells was being wiped by drawButtonMenu's
    // pre-render clear, so this re-routes it to a slot the clear leaves
    // alone (and re-paints with our text).
    auto& flowTheme = const_cast<LyraFlowTheme&>(static_cast<const LyraFlowTheme&>(GUI));
    flowTheme.focusedBookAuthorForLabel.clear();
    if (!shelfHeaderFocused && shelfSelectedSpine >= 0 && focusedAuthor != nullptr && *focusedAuthor != '\0') {
      flowTheme.focusedBookAuthorForLabel = focusedAuthor;
    }
  }

  // While the shelf header is focused, force "no menu selection" so the
  // icon bar doesn't show a misleading highlight from the carousel/menu's
  // shared selectorIndex value.
  const int menuSelectedIndex = shelfHeaderFocused
                                    ? -1
                                    : selectorIndex - getHomeMenuSelectionOffset(recentBooks) - shelfBookCount;
  GUI.drawButtonMenu(
      renderer, Rect{0, menuStartY, pageWidth, menuHeight}, static_cast<int>(menuItems.size()), menuSelectedIndex,
      [&menuItems](int index) { return std::string(menuItems[index].label); },
      [&menuItems](int index) { return menuItems[index].icon; });

  const auto activeTheme = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  // LYRA_CAROUSEL and LYRA_FLOW share a row-and-column grammar: L/R iterates
  // within the active row, U/D toggles rows — label as Left/Right.
  // Everything else (non-carousel themes) hints "Up/Down" since their L/R is
  // a thin wrapper over the menu's vertical buttonNavigator.
  MappedInputManager::Labels labels;
  if (activeTheme == CrossPointSettings::UI_THEME::LYRA_CAROUSEL ||
      activeTheme == CrossPointSettings::UI_THEME::LYRA_FLOW) {
    labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  } else {
    labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Flow theme: snapshot the framebuffer AFTER the whole home screen is
  // composed (header + carousel + shelf + menu + hints). Next render's
  // restoreCoverBuffer() brings everything back, letting the shelf
  // fast-path skip its expensive BMP reads when nothing about it has
  // changed. We deliberately store before displayBuffer() so the panel
  // I/O isn't blocked waiting for the memcpy, and `coverBufferStored`
  // flag drives the restore on the next render.
  if (isLyraFlowForRender) {
    if (homeRenderPopupShown) {
      // A progress popup (shelf cover load / series detection) was drawn over
      // the framebuffer this frame. Snapshotting it would bake the popup into
      // the cached buffer, and the carousel/shelf fast-paths (which don't
      // repaint the carousel area the popup sits over) would keep restoring
      // it — leaving the popup stuck on screen until the user navigated to
      // the carousel. Instead, drop all cached render state so the follow-up
      // requestUpdate() does a full clean repaint that erases the popup.
      coverBufferStored = false;
      shelfSnapshotValid = false;
      lastRenderedCoverSelectorValid = false;
      coverRendered = false;
      requestUpdate();
    } else if (storeCoverBuffer()) {
      coverBufferStored = true;
    }
  }

  presentHomeBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
    return;
  }

  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }

  if (carouselWarmupPending && !carouselFramesReady) {
    // Resolve any missing cover thumbs first, then warm the carousel snapshot.
    // Cover generation needs more contiguous heap than the frame cache path.
    carouselWarmupPending = false;
    const bool showedWarmupProgress = preRenderCarouselFrames(true);
    if (carouselFramesReady || showedWarmupProgress) {
      requestUpdate();
    }
  }
}

void HomeActivity::renderCarouselFrame(int bookIdx, int slotIdx) {
  const auto start = millis();
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || !gCarouselCache.frames[slotIdx]) return;
  BookReadingStats frameStats;
  float frameProgressPercent = -1.0f;
  bool usedCachedStats = false;
  renderCarouselFrameToCurrentBuffer(bookIdx, &frameStats, &frameProgressPercent, &usedCachedStats);

  memcpy(gCarouselCache.frames[slotIdx], frameBuffer, renderer.getBufferSize());
  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
  LOG_DBG("HOME", "carousel: renderCarouselFrame book=%d slot=%d cached=%s took %lums", bookIdx, slotIdx,
          usedCachedStats ? "yes" : "no", millis() - start);
}

void HomeActivity::updateSlidingWindowCache(int centerIdx, int bookCount) {
  (void)centerIdx;
  (void)bookCount;
  // The current carousel cache keeps one frame in RAM; other frames are paged
  // from the SD snapshot cache on demand in render().
}

void HomeActivity::onSelectBook(const std::string& path) {
  gCarouselCache.invalidate();
  freeCarouselFrames();
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }
  activityManager.goToReader(path);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onContinueReading() {
  if (!recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
  }
}

void HomeActivity::onRecentsOpen() {
  // CrumBLE #81: the icon-bar entry (now labelled "Bookshelf") opens the
  // grid over the currently-active collection. Long-press of the same
  // entry brings up a collection picker (see the long-press branch in
  // the loop's Confirm handler).
  activityManager.goToBookshelf();
}

void HomeActivity::showBookshelfCollectionPicker() {
  // CrumBLE #81: snapshot the visible collections (id+name) so the
  // BookshelfPickerActivity has stable strings to render and we can
  // map the picked index back to a collection id after the activity
  // exits. The picker visually matches Reader Options (full-screen
  // list with header + button hints) rather than the popup-style
  // ChoicePromptActivity.
  const auto& collections = CollectionsStore::getInstance().getCollections();
  if (collections.size() <= 1) {
    // Only one collection visible -- the picker would be a one-item list.
    // Just open the grid directly.
    activityManager.goToBookshelf();
    return;
  }
  std::vector<std::string> labels;
  std::vector<std::string> ids;
  labels.reserve(collections.size());
  ids.reserve(collections.size());
  const std::string activeId = CollectionsStore::getInstance().getActiveId();
  int currentIndex = -1;
  for (size_t i = 0; i < collections.size(); ++i) {
    labels.push_back(collections[i].name);
    ids.push_back(collections[i].id);
    if (collections[i].id == activeId) {
      currentIndex = static_cast<int>(i);
    }
  }
  startActivityForResult(
      std::make_unique<BookshelfPickerActivity>(renderer, mappedInput, std::move(labels), currentIndex),
      [this, ids = std::move(ids)](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();
          return;
        }
        const auto* cr = std::get_if<ChoicePromptResult>(&res.data);
        if (cr == nullptr || cr->choice < 0 || cr->choice >= static_cast<int>(ids.size())) {
          requestUpdate();
          return;
        }
        // Set picked collection as active so the carousel header reflects
        // the user's choice when they return to Home later, AND so the
        // Bookshelf grid we're about to open inherits the same id.
        CollectionsStore::getInstance().setActiveId(ids[cr->choice]);
        activityManager.goToBookshelf(ids[cr->choice]);
      });
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onReadingStatsOpen() {
  const int highlightedBookIdx = getHighlightedBookIndex();
  const std::string bookTitle =
      highlightedBookIdx >= 0 ? recentBooks[highlightedBookIdx].title : std::string(tr(STR_READING_STATS));
  const std::string bookPath = highlightedBookIdx >= 0 ? recentBooks[highlightedBookIdx].path : std::string();
  const std::string coverBmpPath =
      highlightedBookIdx >= 0 ? recentBooks[highlightedBookIdx].coverBmpPath : std::string();
  // CrumBLE inherits chintanvajariya's richer BookStatsActivity (recent-books
  // navigation + cover image). The richer constructor needs path/cover and a
  // backToHome flag; launched from home, so back returns here.
  startActivityForResult(
      std::make_unique<BookStatsActivity>(renderer, mappedInput, bookPath, bookTitle, coverBmpPath, currentBookStats,
                                          globalStats, /*backToHome=*/true),
      [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::onBookmarksOpen() {
  startActivityForResult(std::make_unique<BookmarksHomeActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}
