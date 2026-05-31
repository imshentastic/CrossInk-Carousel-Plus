#include "RecentBooksGridActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "BookActions.h"
#include "BookshelfPickerActivity.h"
#include "CollectionPickerActivity.h"
#include "CollectionsStore.h"
#include "CoverThumbStatus.h"
#include "CrossPointSettings.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

namespace {
constexpr int kCoverCornerRadius = 2;
// CrumBLE #133: gridColumns_ is gone -- column count is now an
// instance field gridColumns_ on the activity (driven by
// SETTINGS.bookshelfLayout). Helpers that need it take it as an arg.
constexpr unsigned long kLongPressMs = 1000;
// CrumBLE #113: bookshelf-local header. Halfway between the shared Lyra
// header (84 px tall, very generous padding) and the too-tight 28 px we
// tried first. Battery + collection name sit in the upper half of the
// strip with the divider line floating ~36 px below the title text --
// matching the original-style gap the user wanted to keep.
// CrumBLE #133 follow-up: pulled up from 8 -> 4 px so the "Bookshelf"
// title text + divider sit closer to the top edge, freeing 4 px below
// for the grid / page-dots band.
constexpr int kHeaderTopPadding = 4;
constexpr int kHeaderHeight = 52;
constexpr int kHeaderToStripGap = 14;
constexpr int kLyraGridContentTop = kHeaderTopPadding + kHeaderHeight + kHeaderToStripGap;
constexpr int kLyraGridSpacing = LyraMetrics::values.verticalSpacing;

void drawGridHeader(const GfxRenderer& renderer, const int pageWidth, const char* titleText) {
  const Rect rect{0, kHeaderTopPadding, pageWidth, kHeaderHeight};
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  // CrumBLE #133 follow-up: battery Y anchored to LyraMetrics::topPadding
  // (the value LyraTheme::drawHeader uses on Home) instead of the
  // Bookshelf's own kHeaderTopPadding -- so the battery sits in the
  // same vertical slot whether the user is on Home or in Bookshelf.
  // Title text + divider still use rect.y / kHeaderTopPadding (which
  // moved up a smidge in this round), but they're at a different x
  // from the right-aligned battery so they don't visually collide.
  const int batteryY = LyraMetrics::values.topPadding + 2;
  GUI.drawBatteryRight(renderer,
                       Rect{batteryX, batteryY, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                       showBatteryPercentage);

  const int titleMaxWidth = rect.width - LyraMetrics::values.contentSidePadding * 3;
  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, titleText, titleMaxWidth, EpdFontFamily::BOLD);
  // Title baseline lands a few px below the battery icon -- gives the
  // collection name room while keeping it nestled in the header rather
  // than floating in dead space above the divider.
  const int titleY = rect.y + LyraMetrics::values.batteryHeight + 6;
  renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding, titleY,
                    title.c_str(), true, EpdFontFamily::BOLD);
  // Divider line at the bottom of the header strip, drawn at the
  // standard 2-px weight (back from the 1-px we briefly tried; matches
  // the visual heft of the rest of the Lyra UI).
  renderer.drawLine(rect.x, rect.y + rect.height - 2, rect.x + rect.width - 1, rect.y + rect.height - 2, 2, true);
}

// Some EPUBs leave an empty author with a stray trailing ";" (a
// separator without a value after it) or just whitespace-around-semis.
// Strip them so callers can treat the result as a real empty author
// and skip the "no author" rendering. Idempotent on well-formed input.
std::string normalizeAuthorMeta(std::string s) {
  while (!s.empty()) {
    const char c = s.back();
    if (c == ' ' || c == '\t' || c == ';' || c == ',' || c == '\r' || c == '\n') {
      s.pop_back();
    } else {
      break;
    }
  }
  // Also trim leading whitespace / separators so " ; Author" -> "Author".
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ';' || s[i] == '\r' || s[i] == '\n')) ++i;
  if (i > 0) s.erase(0, i);
  return s;
}

// Strip the directory + extension from an EPUB path so we can use the
// bare filename as a fallback when the book has no metadata title.
// Matches the same logic LyraFlowTheme uses on the carousel center
// fallback so the Bookshelf grid and the carousel agree on what to
// show for a title-less book.
std::string filenameWithoutExtension(const std::string& path) {
  std::string name = path;
  const size_t lastSlash = name.find_last_of('/');
  if (lastSlash != std::string::npos) name = name.substr(lastSlash + 1);
  const size_t lastDot = name.find_last_of('.');
  if (lastDot != std::string::npos && lastDot > 0) name = name.substr(0, lastDot);
  return name;
}

int moveHorizontalInGrid(const int currentIndex, const int totalItems, const bool moveRight) {
  if (totalItems <= 0) return 0;
  return moveRight ? ButtonNavigator::nextIndex(currentIndex, totalItems)
                   : ButtonNavigator::previousIndex(currentIndex, totalItems);
}

int moveVerticalInGrid(const int currentIndex, const int totalItems, const int columns, const int itemsPerPage,
                       const bool moveDown) {
  if (totalItems <= 0 || columns <= 0) return 0;

  const int safeItemsPerPage = std::max(columns, itemsPerPage);
  // Contract: safeItemsPerPage should describe whole grid rows. Partial rows
  // are allowed only on the final page after totalItems is applied below.
  if (safeItemsPerPage % columns != 0) {
    LOG_ERR("RBGA", "moveVerticalInGrid requires whole rows (itemsPerPage=%d columns=%d)", safeItemsPerPage, columns);
    return currentIndex;
  }
  const int totalPages = (totalItems + safeItemsPerPage - 1) / safeItemsPerPage;
  const int currentPage = currentIndex / safeItemsPerPage;
  const int indexInPage = currentIndex % safeItemsPerPage;
  const int currentRow = indexInPage / columns;
  const int currentColumn = indexInPage % columns;
  const int rowsPerPage = safeItemsPerPage / columns;

  if (moveDown) {
    if (currentRow < rowsPerPage - 1) {
      const int nextRowCandidate = currentIndex + columns;
      if (nextRowCandidate < totalItems && (nextRowCandidate / safeItemsPerPage) == currentPage) {
        return nextRowCandidate;
      }
    }

    const int nextPage = (currentPage + 1) % totalPages;
    const int nextPageStart = nextPage * safeItemsPerPage;
    const int nextPageCount = std::min(safeItemsPerPage, totalItems - nextPageStart);
    if (nextPageCount <= 0) return currentIndex;

    if (currentColumn < nextPageCount) {
      return nextPageStart + currentColumn;
    }
    return nextPageStart + nextPageCount - 1;
  }

  if (currentRow > 0) {
    return currentIndex - columns;
  }

  const int previousPage = (currentPage - 1 + totalPages) % totalPages;
  const int previousPageStart = previousPage * safeItemsPerPage;
  const int previousPageCount = std::min(safeItemsPerPage, totalItems - previousPageStart);
  if (previousPageCount <= 0) return currentIndex;

  int previousPageCandidate = previousPageStart + ((previousPageCount - 1) / columns) * columns + currentColumn;
  while (previousPageCandidate >= previousPageStart + previousPageCount) {
    previousPageCandidate -= columns;
  }
  return std::max(previousPageStart, previousPageCandidate);
}

void updateRecentBookCoverPath(const RecentBook& book, const std::string& coverBmpPath) {
  if (!RECENT_BOOKS.updateBook(book.path, book.title, book.author, coverBmpPath)) {
    LOG_ERR("RBGA", "failed to update recent book metadata: %s", book.path.c_str());
  }
}

bool hasThumbnailPlaceholder(const std::string& coverBmpPath) {
  return coverBmpPath.find("[WIDTH]") != std::string::npos || coverBmpPath.find("[HEIGHT]") != std::string::npos;
}

bool needsCoverThumbGeneration(const RecentBook& book, const std::string& thumbPath, int targetCoverWidth,
                               int targetCoverHeight) {
  if (thumbPath.empty() || !Storage.exists(thumbPath.c_str())) {
    return true;
  }
  if (!FsHelpers::hasXtcExtension(book.path)) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBGA", thumbPath, file)) {
    return true;
  }
  Bitmap bitmap(file);
  const bool hasExpectedSize = bitmap.parseHeaders() == BmpReaderError::Ok &&
                               bitmap.getWidth() == targetCoverWidth && bitmap.getHeight() == targetCoverHeight;
  file.close();
  return !hasExpectedSize;
}

void calculateCoverFillCrop(const Bitmap& bitmap, int targetCoverWidth, int targetCoverHeight, float& cropX,
                            float& cropY) {
  cropX = 0.0f;
  cropY = 0.0f;
  const float srcW = static_cast<float>(bitmap.getWidth());
  const float srcH = static_cast<float>(bitmap.getHeight());
  if (srcW <= 0.0f || srcH <= 0.0f) return;

  const float srcRatio = srcW / srcH;
  const float targetRatio = static_cast<float>(targetCoverWidth) / static_cast<float>(targetCoverHeight);
  if (srcRatio > targetRatio) {
    cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
  } else if (srcRatio < targetRatio) {
    cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
  }
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

void ensureReusableCoverPath(RecentBook& book, int coverWidth, int coverHeight) {
  // Books whose thumb-gen has previously failed should stay on the
  // placeholder path. Without this gate, the re-derivation below would
  // refill coverBmpPath with the template, which trips needsCoverThumbGeneration
  // -> Loading popup -> repeat failure on every shelf revisit.
  if (CoverThumbStatus::isMarkedFailed(book.path, coverWidth, coverHeight)) {
    if (!book.coverBmpPath.empty()) {
      book.coverBmpPath = "";
      updateRecentBookCoverPath(book, "");
    }
    return;
  }
  if (book.coverBmpPath.empty() || hasThumbnailPlaceholder(book.coverBmpPath)) {
    return;
  }

  const std::string reusablePath = getReusableCoverPath(book);
  if (reusablePath.empty() || reusablePath == book.coverBmpPath) {
    return;
  }

  book.coverBmpPath = reusablePath;
  updateRecentBookCoverPath(book, reusablePath);
}
}  // namespace

void RecentBooksGridActivity::loadRecentBooks() {
  recentBooks.clear();
  if (isCollectionMode()) {
    // Collection mode (#81): pull paths from CollectionsStore and synthesise
    // RecentBook entries. Pre-populate coverBmpPath with the canonical
    // thumb-template path (/.crosspoint/<hash>/thumb_[WIDTH]x[HEIGHT].bmp)
    // -- without that, needsCoverThumbGeneration() can't find existing
    // thumbs across visits because it looks them up via book.coverBmpPath,
    // so the Loading popup fires + regenerates every single time the user
    // re-enters the grid.
    const auto paths = CollectionsStore::getInstance().resolveBookPaths(collectionId_);
    recentBooks.reserve(paths.size());
    for (const auto& path : paths) {
      // CrumBLE #131: dropped the Storage.exists() pre-check (was the
      // dominant cost on Bookshelf entry for large collections -- ~10
      // ms per book * N books). Trust resolveBookPaths to return live
      // entries; LibraryIndex and stored collections are kept current
      // by the rescan/healing paths. If a book was deleted from SD
      // since the last index update, its cover load falls to the
      // placeholder (no crash, no UI corruption -- the user just sees
      // an outlined cell with the filename, same as a never-thumbed
      // book). Worth the trade for near-instant grid entry.
      RecentBook book;
      book.path = path;
      // Template path; getCoverThumbPath fills in [WIDTH]/[HEIGHT] at the
      // grid's chosen cell size (coverWidth_ x coverHeight_). Skip when a
      // previous generation has been marked permanently-failed so we don't
      // re-derive a path that will only trigger a doomed retry below
      // (loading popup + same failure every revisit).
      if (!CoverThumbStatus::isMarkedFailed(path, coverWidth_, coverHeight_)) {
        if (FsHelpers::hasEpubExtension(path)) {
          book.coverBmpPath = Epub(path, "/.crosspoint").getThumbBmpPath();
        } else if (FsHelpers::hasXtcExtension(path)) {
          book.coverBmpPath = Xtc(path, "/.crosspoint").getThumbBmpPath();
        }
      }
      // Filename-without-extension title fallback. The page-cover load
      // pass may overwrite this with real metadata if it parses the book
      // for a thumbnail; that's fine.
      const size_t slash = path.find_last_of('/');
      const std::string fname = (slash != std::string::npos) ? path.substr(slash + 1) : path;
      const size_t dot = fname.find_last_of('.');
      book.title = (dot != std::string::npos && dot > 0) ? fname.substr(0, dot) : fname;
      recentBooks.push_back(BookState{book});
    }
    return;
  }
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), static_cast<size_t>(maxGridBooks_)));

  for (const auto& book : books) {
    if (recentBooks.size() >= maxGridBooks_) break;
    // CrumBLE #131: dropped Storage.exists() filter. RecentBooksStore
    // self-prunes via pruneMissing() on add/update + the healFromStats
    // path, so stale entries are rare. Same trade as the collection
    // mode loop above: a deleted book that slipped through shows as a
    // placeholder cell, not a crash.
    recentBooks.push_back(BookState{book});
  }
}

void RecentBooksGridActivity::ensureProgressLoaded(const int index) {
  if (index < 0 || index >= static_cast<int>(recentBooks.size())) return;
  if (recentBooks[index].progressLoaded) {
    return;
  }

  recentBooks[index].progress = RecentBookProgress::loadPercent(recentBooks[index].book);
  recentBooks[index].progressLoaded = true;
}

void RecentBooksGridActivity::ensureFocusedMetaLoaded(const std::string& path) {
  if (path == focusedMetaPath_) return;  // unchanged focus — reuse fields
  focusedMetaPath_ = path;
  // CrumBLE #125: cache-backed. loadBookMetaToCache does the SD I/O
  // only on cache miss; subsequent presses to a previously-visited
  // book hit RAM only. Pre-warmed on entry by prewarmVisiblePage so
  // the first L/R press is also fast.
  const auto& entry = loadBookMetaToCache(path);
  focusedMetaTitle_ = entry.title;
  focusedMetaAuthor_ = entry.author;
  focusedMetaStats_ = entry.stats;
  focusedMetaStatsLoaded_ = entry.statsLoaded;
}

const RecentBooksGridActivity::BookMetaCacheEntry& RecentBooksGridActivity::loadBookMetaToCache(
    const std::string& path) {
  auto it = bookMetaCache_.find(path);
  if (it != bookMetaCache_.end()) return it->second;
  BookMetaCacheEntry e;
  std::string statsCachePath;
  if (FsHelpers::hasEpubExtension(path)) {
    Epub epub(path, "/.crosspoint");
    epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
    e.title = epub.getTitle();
    e.author = normalizeAuthorMeta(epub.getAuthor());
    statsCachePath = epub.getCachePath();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      e.title = xtc.getTitle();
      e.author = normalizeAuthorMeta(xtc.getAuthor());
    }
    statsCachePath = "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(path));
  }
  // .txt / .md have no embedded metadata; stats load is EPUB/XTC-only.
  if (!statsCachePath.empty()) {
    e.stats = BookReadingStats::load(statsCachePath);
    e.statsLoaded = true;
  }
  return bookMetaCache_.emplace(path, std::move(e)).first->second;
}

void RecentBooksGridActivity::prewarmVisiblePage(int focusedIndex) {
  const int totalBooks = static_cast<int>(recentBooks.size());
  if (totalBooks <= 0) return;
  const int currentPage = focusedIndex / booksPerPage_;
  const int pageStart = currentPage * booksPerPage_;
  const int pageEnd = std::min(pageStart + booksPerPage_, totalBooks);
  for (int i = pageStart; i < pageEnd; ++i) {
    ensureProgressLoaded(i);
    if (!recentBooks[i].book.path.empty()) {
      loadBookMetaToCache(recentBooks[i].book.path);
    }
  }
}

void RecentBooksGridActivity::loadPageCovers(int pageStart) {
  const int pageEnd = std::min(pageStart + booksPerPage_, static_cast<int>(recentBooks.size()));

  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; ++i) {
    RecentBook& book = recentBooks[i].book;
    ensureReusableCoverPath(book, coverWidth_, coverHeight_);
    // Books with a "thumb generation failed" marker render the placeholder
    // (coverBmpPath stays empty) and never trigger the Loading popup.
    if (CoverThumbStatus::isMarkedFailed(book.path, coverWidth_, coverHeight_)) continue;
    if (book.coverBmpPath.empty()) {
      needsGeneration = true;
      break;
    }
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverWidth_, coverHeight_);
    if (needsCoverThumbGeneration(book, thumbPath, coverWidth_, coverHeight_)) {
      needsGeneration = true;
      break;
    }
  }
  if (!needsGeneration) {
    loadedPageStart = pageStart;
    return;
  }

  bool showingLoading = false;
  Rect popupRect;
  const int totalToProcess = pageEnd - pageStart;
  int processedCount = 0;

  for (int i = pageStart; i < pageEnd; ++i) {
    RecentBook& book = recentBooks[i].book;
    // Already-known-failed books: render placeholder, no Loading popup.
    if (CoverThumbStatus::isMarkedFailed(book.path, coverWidth_, coverHeight_)) {
      processedCount++;
      continue;
    }
    const std::string coverPath =
        book.coverBmpPath.empty() ? "" : UITheme::getCoverThumbPath(book.coverBmpPath, coverWidth_, coverHeight_);
    if (needsCoverThumbGeneration(book, coverPath, coverWidth_, coverHeight_)) {
      if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        // Try generateThumbBmpNoIndex first (cheap content.opf-only
        // parse), then fall back to the heavy epub.load+generateThumbBmp
        // pair for books without a pre-built BookMetadataCache.
        if (!showingLoading) {
          showingLoading = true;
          popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        }
        GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
        bool genSucceeded = epub.generateThumbBmpNoIndex(coverWidth_, coverHeight_);
        if (!genSucceeded) {
          LOG_ERR("RBGA", "NoIndex gen failed for %s (%dx%d, free=%u maxAlloc=%u); trying heavy path",
                  book.path.c_str(), coverWidth_, coverHeight_, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          if (epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
            genSucceeded = epub.generateThumbBmp(coverWidth_, coverHeight_);
            if (!genSucceeded) {
              LOG_ERR("RBGA", "Heavy gen also failed for %s (free=%u maxAlloc=%u)", book.path.c_str(),
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            }
          } else {
            LOG_ERR("RBGA", "epub.load failed for %s", book.path.c_str());
          }
        }
        if (genSucceeded) {
          const std::string reusablePath = epub.getThumbBmpPath();
          book.coverBmpPath = reusablePath;
          updateRecentBookCoverPath(book, reusablePath);
          CoverThumbStatus::clearFailed(book.path, coverWidth_, coverHeight_);
        } else {
          // CrumBLE #133 follow-up: re-enabled markFailed after BOTH
          // gen paths fail (NoIndex AND heavy). Earlier this branch
          // was skipped to avoid trapping books on placeholder after
          // a transient heap-OOM failure; the new failure marker
          // suffix (_v2) plus the heap-pressure-relief done above
          // loadPageCovers (snapshot + image cache freed) means
          // failures here are now almost always permanent (no cover
          // image in the EPUB / unsupported cover format), so
          // marking them prevents the Loading popup from re-firing
          // for the same books on every Bookshelf entry.
          book.coverBmpPath = "";
          updateRecentBookCoverPath(book, "");
          CoverThumbStatus::markFailed(book.path, coverWidth_, coverHeight_);
        }
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        Xtc xtc(book.path, "/.crosspoint");
        // XTC has no NoIndex variant -- it's a simpler format with no
        // spine/TOC indexing, so xtc.load() is already cheap. Kept as
        // the only branch that pays a load() cost.
        if (xtc.load()) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
          if (xtc.generateThumbBmp(coverWidth_, coverHeight_)) {
            const std::string reusablePath = xtc.getThumbBmpPath();
            book.coverBmpPath = reusablePath;
            updateRecentBookCoverPath(book, reusablePath);
            CoverThumbStatus::clearFailed(book.path, coverWidth_, coverHeight_);
          } else {
            book.coverBmpPath = "";
            updateRecentBookCoverPath(book, "");
            CoverThumbStatus::markFailed(book.path, coverWidth_, coverHeight_);
          }
        }
      }
    }
    processedCount++;
  }

  loadedPageStart = pageStart;
  if (showingLoading) {
    requestUpdate();
  }
}

void RecentBooksGridActivity::applyLayoutFromSettings() {
  // CrumBLE #133: maps the persisted enum (3x3/4x4/2x2) to grid dims
  // + cell sizes. maxGridBooks_ stays at 2 pages worth for L/R paging.
  // Cell sizes deliberately reuse the dimensions already cached by
  // other activities so the user never pays a re-decode cost when
  // entering Bookshelf with a warm cache (Flow shelf for 4x4, carousel
  // center + Stats main cover for 2x2). 3x3 keeps the legacy 123x180
  // size from before the Flow-shelf unification -- the user prefers
  // those proportions at 9-cell density (#133 follow-up).
  switch (SETTINGS.bookshelfLayout) {
    case CrossPointSettings::BOOKSHELF_LAYOUT_4X4:
      gridColumns_ = 4;
      gridRows_ = 4;
      coverWidth_ = 100;
      coverHeight_ = 150;
      break;
    case CrossPointSettings::BOOKSHELF_LAYOUT_2X2:
      gridColumns_ = 2;
      gridRows_ = 2;
      // Carousel center cover (LyraFlowTheme::centerCoverHeight=320).
      // 220 width keeps the 2x2 from overflowing the 250 px wide
      // half-pane with room for the selection ring + gutters.
      coverWidth_ = 220;
      coverHeight_ = 320;
      break;
    case CrossPointSettings::BOOKSHELF_LAYOUT_3X3:
    default:
      gridColumns_ = 3;
      gridRows_ = 3;
      // 130x190 cells -- bigger than the legacy 123x180 to fill the
      // 9-cell page more generously. With 480 px panel width and
      // gridSpacing=24 (3x3 override below), total grid =
      // 3*130 + 2*24 = 438, leaving 21 px each side. Cache reuse
      // with other activities isn't a priority here (the user
      // prefers the proportions over the cache hit at this density).
      coverWidth_ = 130;
      coverHeight_ = 190;
      break;
  }
  booksPerPage_ = gridColumns_ * gridRows_;
  maxGridBooks_ = booksPerPage_ * 2;
}

void RecentBooksGridActivity::onEnter() {
  Activity::onEnter();
  // CrumBLE #133: re-read SETTINGS.bookshelfLayout every entry. The
  // user toggles the Layout row from BookshelfPickerActivity, then
  // returns here -- we want the new column/cell sizes to take effect
  // without a reboot. The picker writes the setting; we read it.
  applyLayoutFromSettings();
  loadRecentBooks();
  // CrumBLE #133 follow-up: NO on-entry marker clear. Each gen failure
  // in loadPageCovers writes a marker, and loadRecentBooks honours it
  // (book.coverBmpPath stays empty -> placeholder, no retry). This is
  // the #94 contract restored. Old-build stale markers were rendered
  // moot by bumping CoverThumbStatus's marker suffix to _v2 (see
  // src/CoverThumbStatus.cpp), so on the user's existing SD card any
  // legacy markers are ignored and the books get one fresh attempt.
  selectorIndex = 0;
  loadedPageStart = NO_PAGE_LOADED;
  // CrumBLE #125: snapshot starts invalid -- first render does a full
  // paint and captures the framebuffer for subsequent focus-only
  // diffs.
  freeGridSnapshot();
  bookMetaCache_.clear();
  // CrumBLE #125: pre-warm runs at the END of render() (after
  // displayBuffer), NOT here. Calling it pre-render blocks the first
  // paint by ~200-300 ms; doing it post-display means the user sees
  // the grid immediately, then the cache fills before they can press
  // L/R faster than the prewarm completes. First press to an
  // un-cached book MAY pay a one-time ~30 ms SD cost (acceptable).
  requestUpdate();
}

void RecentBooksGridActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  freeGridSnapshot();
  bookMetaCache_.clear();
  // CrumBLE #131: was renderer.clearImageCache() -- now reconcile so
  // covers stay warm for return-to-home (HomeActivity adopted the same
  // approach for symmetric reasons). The reconcile only shrinks the
  // cache when actual heap pressure exists.
  renderer.reconcileImageCacheBudgetExt();
}

bool RecentBooksGridActivity::storeGridSnapshot() {
  // CrumBLE #125 BUGFIX: only free the BUFFER -- preserve the
  // gridSnapshotValid_/Page_/FocusedIndex_/CollectionId_ metadata so
  // the caller's subsequent assignments survive. Previously
  // freeGridSnapshot() reset all four fields, which on the focus-only
  // fast path meant gridSnapshotPage_ and gridSnapshotCollectionId_
  // were reset to -1 / "" after every press -- the NEXT press's fast-
  // path check then saw "" != collectionId_ and fell through to full
  // repaint. Result: every other L/R press was a full repaint instead
  // of fast path.
  if (gridSnapshot_) {
    free(gridSnapshot_);
    gridSnapshot_ = nullptr;
  }
  gridSnapshotSize_ = 0;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;
  const size_t bufferSize = renderer.getBufferSize();
  gridSnapshot_ = static_cast<uint8_t*>(malloc(bufferSize));
  if (!gridSnapshot_) {
    LOG_ERR("RBGA", "OOM: grid snapshot (%u bytes)", static_cast<unsigned>(bufferSize));
    return false;
  }
  gridSnapshotSize_ = bufferSize;
  memcpy(gridSnapshot_, frameBuffer, bufferSize);
  return true;
}

bool RecentBooksGridActivity::restoreGridSnapshot() {
  if (!gridSnapshot_ || gridSnapshotSize_ == 0) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;
  if (gridSnapshotSize_ != renderer.getBufferSize()) return false;
  memcpy(frameBuffer, gridSnapshot_, gridSnapshotSize_);
  return true;
}

void RecentBooksGridActivity::freeGridSnapshot() {
  if (gridSnapshot_) {
    free(gridSnapshot_);
    gridSnapshot_ = nullptr;
  }
  gridSnapshotSize_ = 0;
  gridSnapshotValid_ = false;
  gridSnapshotPage_ = -1;
  gridSnapshotFocusedIndex_ = -1;
  gridSnapshotCollectionId_.clear();
}

void RecentBooksGridActivity::paintGridFocusUpdate(int prevFocusedIndex, int newFocusedIndex) {
  // Mirrors render()'s layout math exactly. Any tweak there must be
  // reflected here or the focus rings will land on the wrong pixels.
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // CrumBLE #133 follow-up: must match render() -- 2x2 pulls grid up;
  // title-at-top moves grid below the strip; bar flush in 2x2.
  const bool is2x2 = (SETTINGS.bookshelfLayout == CrossPointSettings::BOOKSHELF_LAYOUT_2X2);
  const bool is3x3 = (SETTINGS.bookshelfLayout == CrossPointSettings::BOOKSHELF_LAYOUT_3X3);
  const bool titleAtTop =
      (SETTINGS.bookshelfTitlePlacement == CrossPointSettings::BOOKSHELF_TITLE_PLACEMENT_TOP);
  const int titleTopLift = is2x2 ? 9 : (is3x3 ? 0 : 8);
  // CrumBLE #133 follow-up: 2x2 bottom mode pulls up 6 px (72 -> 66)
  // to (a) match the user's "smidge up" request and (b) recover the
  // 2 px the fatter progress bar (kProgressBarHeight = 7) added to
  // the row stride -- without the pull-up, row 1's bar bottom would
  // crowd the page-dot indicator.
  const int contentTop = titleAtTop ? (kHeaderTopPadding + kHeaderHeight + 6 + 40 + 18 - titleTopLift)
                                    : (is2x2 ? 66 : kLyraGridContentTop);
  constexpr int titleStripHeight = 40;
  constexpr int kBottomMargin = 14;
  const int titleStripY = titleAtTop ? (kHeaderTopPadding + kHeaderHeight + 6)
                                     : (pageHeight - kBottomMargin - titleStripHeight);
  // CrumBLE #133 follow-up: mirror render()'s 2x2 selection-ring +
  // progress-bar geometry so partial repaints land on the same pixels.
  const int selectionPadding = is2x2 ? 3 : 4;
  const int selectionOutlineGap = is2x2 ? 1 : 2;
  const int selectionOuterInset = selectionPadding + selectionOutlineGap;
  const int gridSpacing = is3x3 ? 24 : kLyraGridSpacing;
  const int kProgressBarHeight = is2x2 ? 7 : 5;
  const int kProgressTopGap = 4;
  const int rowSpacing = is2x2 ? 4 : (is3x3 ? (titleAtTop ? 14 : 18) : 5);
  const int totalGridWidth = gridColumns_ * coverWidth_ + (gridColumns_ - 1) * gridSpacing;
  const int startXOffset = (pageWidth - totalGridWidth) / 2;
  const int rowStride = coverHeight_ + kProgressTopGap + kProgressBarHeight + rowSpacing;

  const int totalBooks = static_cast<int>(recentBooks.size());
  const int currentPage = (totalBooks > 0) ? (newFocusedIndex / booksPerPage_) : 0;
  const int pageStart = currentPage * booksPerPage_;
  const int pageCount = std::min(booksPerPage_, totalBooks - pageStart);
  const int rowsThisPage = (pageCount + gridColumns_ - 1) / gridColumns_;
  const int lastRowCount = pageCount - (rowsThisPage - 1) * gridColumns_;

  auto cellOrigin = [&](int bookIdx, int& outX, int& outY) -> bool {
    if (bookIdx < pageStart || bookIdx >= pageStart + pageCount) return false;
    const int i = bookIdx - pageStart;
    const int col = i % gridColumns_;
    const int row = i / gridColumns_;
    const bool isPartialLastRow = (row == rowsThisPage - 1) && (lastRowCount < gridColumns_);
    const int rowCellCount = isPartialLastRow ? lastRowCount : gridColumns_;
    const int rowWidth = rowCellCount * coverWidth_ + (rowCellCount - 1) * gridSpacing;
    const int rowStartX = startXOffset + (totalGridWidth - rowWidth) / 2;
    outX = rowStartX + col * (coverWidth_ + gridSpacing);
    // CrumBLE: grid starts directly at contentTop (title strip moved
    // to the bottom of screen). Mirrors render()'s y math.
    outY = contentTop + row * rowStride;
    return true;
  };

  auto paintRing = [&](int bx, int by, bool inkBlack) {
    // Two concentric rounded rects (3 px inner + 1 px outer with a 2 px
    // gap), identical to the render() path. Painting with state=false
    // strokes WHITE -- erases cleanly since both rings sit on the
    // page-white substrate (no L-shadow on Bookshelf grid cells).
    renderer.drawRoundedRect(bx - selectionPadding, by - selectionPadding, coverWidth_ + selectionPadding * 2,
                             coverHeight_ + selectionPadding * 2, 3, kCoverCornerRadius + selectionPadding, inkBlack);
    renderer.drawRoundedRect(bx - selectionOuterInset, by - selectionOuterInset,
                             coverWidth_ + selectionOuterInset * 2, coverHeight_ + selectionOuterInset * 2, 1,
                             kCoverCornerRadius + selectionOuterInset, inkBlack);
  };

  // Erase the prev cell's ring (paint white).
  int px = 0, py = 0;
  if (cellOrigin(prevFocusedIndex, px, py)) {
    paintRing(px, py, false);
  }
  // Draw the new cell's ring (paint black).
  int nx = 0, ny = 0;
  if (cellOrigin(newFocusedIndex, nx, ny)) {
    paintRing(nx, ny, true);
  }

  // Refresh the focused-book title strip (line 1: title, line 2: read
  // time / author / remaining time). The strip lives at titleStripY
  // (bottom of screen) with titleStripHeight tall, spanning the full
  // page width. Clear first so a shorter title doesn't leave leftover
  // characters from the prev book showing through.
  renderer.fillRect(0, titleStripY, pageWidth, titleStripHeight, false);
  if (newFocusedIndex < 0 || newFocusedIndex >= totalBooks) return;
  const auto& selectedBook = recentBooks[newFocusedIndex].book;
  ensureFocusedMetaLoaded(selectedBook.path);
  const int selTitleLh = renderer.getLineHeight(UI_10_FONT_ID);
  const int selSubLh = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int kSelLineGap = 2;
  std::string selTitleStr = focusedMetaTitle_;
  if (selTitleStr.empty()) selTitleStr = selectedBook.title;
  if (selTitleStr.empty()) selTitleStr = filenameWithoutExtension(selectedBook.path);
  std::string selAuthorStr = focusedMetaAuthor_;
  if (selAuthorStr.empty()) selAuthorStr = selectedBook.author;
  const int contentH = selTitleLh + kSelLineGap + selSubLh;
  const int titleY = titleStripY + (titleStripHeight - contentH) / 2;
  // CrumBLE #133 follow-up: focused book title rendered in BOLD so it
  // visually leads the title strip; the author + times line (next row)
  // stays regular weight so the title is the dominant element.
  const std::string truncSelTitle =
      renderer.truncatedText(UI_10_FONT_ID, selTitleStr.c_str(), totalGridWidth, EpdFontFamily::BOLD);
  const int selTitleW = renderer.getTextWidth(UI_10_FONT_ID, truncSelTitle.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, startXOffset + (totalGridWidth - selTitleW) / 2, titleY, truncSelTitle.c_str(),
                    true, EpdFontFamily::BOLD);
  const int subY = titleY + selTitleLh + kSelLineGap;
  const bool hasStats = focusedMetaStatsLoaded_ && focusedMetaStats_.sessionCount > 0;
  const float clampedProgress = std::clamp(recentBooks[newFocusedIndex].progress, 0.0f, 100.0f);
  const bool hasProgress = recentBooks[newFocusedIndex].progressLoaded &&
                            RecentBookProgress::hasPercent(recentBooks[newFocusedIndex].progress);
  char readBuf[24] = "";
  if (hasStats) {
    const uint32_t seconds = focusedMetaStats_.totalReadingSeconds;
    if (seconds < 60) snprintf(readBuf, sizeof(readBuf), "<1m");
    else if (seconds < 3600) snprintf(readBuf, sizeof(readBuf), "%um", static_cast<unsigned>(seconds / 60));
    else
      snprintf(readBuf, sizeof(readBuf), "%uh %um", static_cast<unsigned>(seconds / 3600),
               static_cast<unsigned>((seconds % 3600) / 60));
  }
  char remainBuf[24] = "";
  if (hasStats && hasProgress && clampedProgress >= 1.0f) {
    const uint32_t remaining = static_cast<uint32_t>(
        static_cast<float>(focusedMetaStats_.totalReadingSeconds) * (100.0f - clampedProgress) / clampedProgress);
    if (remaining < 60) snprintf(remainBuf, sizeof(remainBuf), "<1m left");
    else if (remaining < 3600) snprintf(remainBuf, sizeof(remainBuf), "%um left", static_cast<unsigned>(remaining / 60));
    else snprintf(remainBuf, sizeof(remainBuf), "%uh left", static_cast<unsigned>((remaining + 1800) / 3600));
  }
  if (readBuf[0]) {
    renderer.drawText(SMALL_FONT_ID, startXOffset, subY, readBuf, true, EpdFontFamily::REGULAR);
  }
  if (remainBuf[0]) {
    const int rw = renderer.getTextWidth(SMALL_FONT_ID, remainBuf, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, startXOffset + totalGridWidth - rw, subY, remainBuf, true,
                      EpdFontFamily::REGULAR);
  }
  if (!selAuthorStr.empty()) {
    const int readW = readBuf[0] ? renderer.getTextWidth(SMALL_FONT_ID, readBuf, EpdFontFamily::REGULAR) : 0;
    const int remainW = remainBuf[0] ? renderer.getTextWidth(SMALL_FONT_ID, remainBuf, EpdFontFamily::REGULAR) : 0;
    constexpr int kAuthorSidePad = 8;
    const int authorBudget = std::max(0, totalGridWidth - 2 * std::max(readW, remainW) - 2 * kAuthorSidePad);
    const std::string truncAuthor =
        renderer.truncatedText(SMALL_FONT_ID, selAuthorStr.c_str(), authorBudget, EpdFontFamily::REGULAR);
    const int aw = renderer.getTextWidth(SMALL_FONT_ID, truncAuthor.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, startXOffset + (totalGridWidth - aw) / 2, subY, truncAuthor.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
}

void RecentBooksGridActivity::loop() {
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // CrumBLE #130: long-press Back inside the grid opens the bookshelf
  // picker (so the user can switch collections without exiting to
  // home first). Latch on backLongPressFired so the eventual release
  // doesn't also fire the short-press Back -> goHome handler.
  if (backLongPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      backLongPressFired = false;
      // Suppress the release so the navigator doesn't fire its short
      // press handler (goHome) after we just opened the picker.
      mappedInput.suppressNextBackRelease();
    }
    return;
  }
  if (isCollectionMode() && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= kLongPressMs) {
    backLongPressFired = true;
    showBookshelfCollectionPicker();
    return;
  }

  if (!recentBooks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()) &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= kLongPressMs) {
    longPressFired = true;
    showBookActionMenu(selectorIndex, true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBGA", "Selected recent book: %s", recentBooks[selectorIndex].book.path.c_str());
      onSelectBook(recentBooks[selectorIndex].book.path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int listSize = static_cast<int>(recentBooks.size());
  enum class NavDirection { Right, Left, Down, Up };
  auto handleNav = [this, listSize](NavDirection direction) {
    switch (direction) {
      case NavDirection::Right:
        selectorIndex = moveHorizontalInGrid(selectorIndex, listSize, true);
        break;
      case NavDirection::Left:
        selectorIndex = moveHorizontalInGrid(selectorIndex, listSize, false);
        break;
      case NavDirection::Down:
        selectorIndex = moveVerticalInGrid(selectorIndex, listSize, gridColumns_, booksPerPage_, true);
        break;
      case NavDirection::Up:
        selectorIndex = moveVerticalInGrid(selectorIndex, listSize, gridColumns_, booksPerPage_, false);
        break;
    }
    ensureProgressLoaded(selectorIndex);
    requestUpdate();
  };

  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [&] { handleNav(NavDirection::Right); });
  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [&] { handleNav(NavDirection::Left); });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [&] { handleNav(NavDirection::Down); });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [&] { handleNav(NavDirection::Up); });

  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [&] { handleNav(NavDirection::Right); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [&] { handleNav(NavDirection::Left); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [&] { handleNav(NavDirection::Down); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [&] { handleNav(NavDirection::Up); });
}

void RecentBooksGridActivity::showBookshelfCollectionPicker() {
  // CrumBLE #130: mirrors HomeActivity::showBookshelfCollectionPicker.
  // Snapshot the visible collections, launch BookshelfPickerActivity,
  // and on result: set the picked collection active and switch the
  // grid to it in-place (no exit -> re-enter -- the user already gets
  // a fresh render via loadRecentBooks + requestUpdate).
  const auto& collections = CollectionsStore::getInstance().getCollections();
  if (collections.size() <= 1) return;  // nothing to pick from
  std::vector<std::string> labels;
  std::vector<std::string> ids;
  labels.reserve(collections.size());
  ids.reserve(collections.size());
  const std::string activeId = CollectionsStore::getInstance().getActiveId();
  int currentIndex = -1;
  for (size_t i = 0; i < collections.size(); ++i) {
    labels.push_back(collections[i].name);
    ids.push_back(collections[i].id);
    if (collections[i].id == activeId) currentIndex = static_cast<int>(i);
  }
  // Suppress the impending Back release (the user is still holding Back
  // when we get here -- they pressed long enough to trip the threshold)
  // so the picker's wasReleased(Back) handler doesn't immediately fire
  // cancel as soon as the user lets go.
  mappedInput.suppressNextBackRelease();
  startActivityForResult(
      std::make_unique<BookshelfPickerActivity>(renderer, mappedInput, std::move(labels), currentIndex),
      [this, ids = std::move(ids)](const ActivityResult& res) {
        // CrumBLE #133: re-apply layout-from-settings on every picker
        // dismissal. If the user cycled the Layout row inside the
        // picker (with or without changing collection), the new cell
        // size/columns need to take effect; refusing means the user
        // sees the old layout until they exit & re-enter the grid.
        const int prevColumns = gridColumns_;
        const int prevRows = gridRows_;
        const int prevCoverW = coverWidth_;
        const int prevCoverH = coverHeight_;
        applyLayoutFromSettings();
        const bool layoutChanged =
            prevColumns != gridColumns_ || prevRows != gridRows_ || prevCoverW != coverWidth_ ||
            prevCoverH != coverHeight_;
        if (layoutChanged) {
          // Page-stride / cell-size change invalidates the snapshot
          // (geometry mismatch) and the cover-load cache (different
          // thumb dimensions). loadedPageStart reset forces a re-pass
          // through loadPageCovers with the new cell size.
          invalidateGridSnapshot();
          loadedPageStart = NO_PAGE_LOADED;
          // selectorIndex was valid for the old booksPerPage; clamp
          // to the new totalBooks bound. Don't try to track the
          // logical "same book" -- the user's eye picks up a new
          // grid as a fresh view.
          const int totalBooks = static_cast<int>(recentBooks.size());
          if (totalBooks > 0 && selectorIndex >= totalBooks) selectorIndex = totalBooks - 1;
        }
        if (res.isCancelled) {
          requestUpdate();
          return;
        }
        const auto* cr = std::get_if<ChoicePromptResult>(&res.data);
        if (cr == nullptr || cr->choice < 0 || cr->choice >= static_cast<int>(ids.size())) {
          requestUpdate();
          return;
        }
        // Pick: set active and reload the grid in-place.
        const std::string& pickedId = ids[cr->choice];
        if (pickedId == collectionId_) {
          // Same collection -- no-op, just repaint.
          requestUpdate();
          return;
        }
        CollectionsStore::getInstance().setActiveId(pickedId);
        collectionId_ = pickedId;
        selectorIndex = 0;
        loadedPageStart = NO_PAGE_LOADED;
        focusedMetaPath_.clear();
        focusedMetaStatsLoaded_ = false;
        loadRecentBooks();
        // CrumBLE #125: snapshot captures the previous collection's
        // cells; must invalidate so the new collection paints fresh.
        // Metadata cache stays valid across collection switches (keyed
        // by book path); pre-warm the new visible page.
        invalidateGridSnapshot();
        prewarmVisiblePage(selectorIndex);
        requestUpdate(true);
      });
}

void RecentBooksGridActivity::reloadAfterBookAction() {
  loadRecentBooks();
  if (recentBooks.empty()) {
    selectorIndex = 0;
  } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
    selectorIndex = static_cast<int>(recentBooks.size()) - 1;
  }
  loadedPageStart = NO_PAGE_LOADED;
  // CrumBLE #125: book list mutated -- the snapshot captures stale
  // cell layout (removed/added book shifts indices) so the next render
  // must do a full repaint. Metadata cache stays valid (keyed by path
  // so surviving books reuse entries) but pre-warm the new page to
  // pick up any added books.
  invalidateGridSnapshot();
  prewarmVisiblePage(selectorIndex);
  requestUpdate(true);
}

void RecentBooksGridActivity::promptDeleteBook(const RecentBook& book) {
  const std::string path = book.path;
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBGA", "Delete cancelled");
      return;
    }

    LOG_DBG("RBGA", "Attempting to delete: %s", path.c_str());
    BookActions::clearFileMetadata(path);
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("RBGA", "Failed to delete file: %s", path.c_str());
      return;
    }

    RECENT_BOOKS.removeByPath(path);
    reloadAfterBookAction();
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book.title),
                         std::move(handler));
}

void RecentBooksGridActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBGA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBGA", "Removed from recents: %s", path.c_str());
      reloadAfterBookAction();
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksGridActivity::showBookActionMenu(const int bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(recentBooks.size())) return;

  const RecentBook book = recentBooks[bookIndex].book;
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/true);
  // CrumBLE #123: expose the "Add to / Remove from collection..." picker
  // from inside the Bookshelf so users can curate without bouncing back
  // to home + carousel. Matches the carousel center book's long-press
  // menu (HomeActivity::showHomeBookActionMenu).
  const bool isBookFile = FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasXtcExtension(book.path) ||
                          FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path);
  if (isBookFile) {
    items.push_back({FileBrowserAction::AddToCollection, StrId::STR_ADD_TO_COLLECTION});
  }

  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title,
                                                                     std::move(items), ignoreInitialConfirmRelease),
                         [this, book](const ActivityResult& result) {
                           if (result.isCancelled) {
                             return;
                           }

                           const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
                           if (!actionResult) {
                             LOG_ERR("RBGA", "Book action result missing");
                             return;
                           }

                           switch (static_cast<FileBrowserAction>(actionResult->action)) {
                             case FileBrowserAction::Delete:
                               promptDeleteBook(book);
                               return;
                             case FileBrowserAction::DeleteCache:
                               if (!BookActions::clearBookCache(book.path)) {
                                 LOG_ERR("RBGA", "Failed to clear book cache for: %s", book.path.c_str());
                                 BookActions::drawToast(renderer, tr(STR_CACHE_DELETE_FAILED));
                                 delay(1500);
                               } else {
                                 BookActions::drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                                 delay(1000);
                               }
                               reloadAfterBookAction();
                               return;
                             case FileBrowserAction::ToggleCompleted: {
                               bool completed = false;
                               if (BookActions::toggleEpubCompleted(book.path, book.title, completed)) {
                                 BookActions::drawToast(
                                     renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
                                 delay(1000);
                               }
                               reloadAfterBookAction();
                               return;
                             }
                             case FileBrowserAction::RemoveFromRecents:
                               promptRemoveBook(book.path, book.title);
                               return;
                             case FileBrowserAction::AddToCollection: {
                               // CrumBLE #123: same picker the carousel uses.
                               // CollectionsStore is mutated directly inside the
                               // picker; on return we just need to invalidate the
                               // grid's shelf state so any membership-driven
                               // visibility changes show on the next render.
                               const std::string title = book.title;
                               startActivityForResult(
                                   std::make_unique<CollectionPickerActivity>(renderer, mappedInput, book.path, title),
                                   [this](const ActivityResult&) {
                                     reloadAfterBookAction();
                                   });
                               return;
                             }
                             case FileBrowserAction::PinFavorite:
                             case FileBrowserAction::UnpinFavorite:
                             default:
                               return;
                           }
                         });
}

void RecentBooksGridActivity::render(RenderLock&&) {
  // CrumBLE #125: focus-only fast-path. When the snapshot matches the
  // current activity state (same collection, same page) AND only the
  // focused cell within the page changed, restore the framebuffer and
  // patch just the ring + title strip instead of doing the full
  // clearScreen + 9-cell repaint. Same idea as HomeActivity's Flow
  // shelf snapshot/restore pattern. Bookshelf grid cells don't have
  // the L-shadow that the home shelf had (dropped in #113), so the
  // ring erase is a simple drawRoundedRect(state=false) over white.
  const int fastPathTotalBooks = static_cast<int>(recentBooks.size());
  const int fastPathCurrentPage = (fastPathTotalBooks > 0) ? (selectorIndex / booksPerPage_) : 0;
  if (gridSnapshotValid_ && gridSnapshotCollectionId_ == collectionId_ &&
      gridSnapshotPage_ == fastPathCurrentPage && gridSnapshotFocusedIndex_ != selectorIndex &&
      !recentBooks.empty()) {
    if (restoreGridSnapshot()) {
      const int prevIdx = gridSnapshotFocusedIndex_;
      paintGridFocusUpdate(prevIdx, selectorIndex);
      renderer.displayBuffer();
      // Re-snapshot so the next diff sees the new focus state.
      if (storeGridSnapshot()) {
        gridSnapshotFocusedIndex_ = selectorIndex;
        gridSnapshotValid_ = true;
      } else {
        invalidateGridSnapshot();
      }
      return;
    }
    // Restore failed (alloc miss); fall through to full repaint.
    invalidateGridSnapshot();
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // CrumBLE #81: header title is the collection name in collection mode,
  // otherwise the legacy "Recent Books" label. Looking up the collection
  // each frame is cheap (in-memory map find) so we don't cache it.
  const char* headerTitle = tr(STR_MENU_RECENT_BOOKS);
  if (isCollectionMode()) {
    const Collection* c = CollectionsStore::getInstance().findCollection(collectionId_);
    if (c != nullptr) headerTitle = c->name.c_str();
  }
  drawGridHeader(renderer, pageWidth, headerTitle);
  // CrumBLE #133 follow-up: 2x2 uses the largest cover height (320) and
  // only 2 rows. The grid still sits a touch higher than 3x3/4x4 to
  // give the bottom progress bar room above the page dots, but not so
  // high that it crowds the header divider (the previous 66 looked
  // tight). Geometry:
  //   header divider sits at kHeaderTopPadding + kHeaderHeight - 2 = 58
  //   selection ring outer pad = 6 px above cover top
  //   contentTop = 72 -> 8 px gap from divider to selection ring top
  const bool is2x2 = (SETTINGS.bookshelfLayout == CrossPointSettings::BOOKSHELF_LAYOUT_2X2);
  // CrumBLE #133 follow-up: Title Placement. TOP puts the label strip
  // just under the header (consumes the gap between header divider and
  // grid); BOTTOM (default) keeps the strip at the screen bottom.
  const bool titleAtTop =
      (SETTINGS.bookshelfTitlePlacement == CrossPointSettings::BOOKSHELF_TITLE_PLACEMENT_TOP);
  // With the strip at TOP the 2x2 tightening doesn't apply -- the grid
  // starts BELOW the strip. The base +18 of breathing room between the
  // strip-text bottom and the selection ring leaves more vertical
  // headroom than 4x4 / 2x2 need (the user found their grid pushed
  // too far down). Each layout pulls the grid up by approximately
  // its own inter-row gap (rowSpacing + progressTopGap + bar) so the
  // amount lifted matches the gap that already exists between any
  // two rows -- visually consistent.
  // 3x3 keeps the +18 base (rooms its big 130x190 cells comfortably).
  // 4x4 lift trimmed 14 -> 8 px: the previous lift pulled the top-row
  // selection ring close enough to the title-strip author/times line
  // (~7 px gap) that the ring felt visually crowded; the smaller lift
  // restores a ~13 px gap.
  // 2x2 inter-row gap = 4+0+5 = 9 px lift.
  const bool is3x3_for_lift = (SETTINGS.bookshelfLayout == CrossPointSettings::BOOKSHELF_LAYOUT_3X3);
  const int titleTopLift = is2x2 ? 9 : (is3x3_for_lift ? 0 : 8);
  // CrumBLE #133 follow-up: 2x2 bottom mode pulls up 6 px (72 -> 66)
  // to (a) match the user's "smidge up" request and (b) recover the
  // 2 px the fatter progress bar (kProgressBarHeight = 7) added to
  // the row stride -- without the pull-up, row 1's bar bottom would
  // crowd the page-dot indicator.
  const int contentTop = titleAtTop ? (kHeaderTopPadding + kHeaderHeight + 6 + 40 + 18 - titleTopLift)
                                    : (is2x2 ? 66 : kLyraGridContentTop);
  // CrumBLE #113: selected-book label strip. Line 1 is the title in
  // UI_10 REGULAR (centered). Line 2 splits into three:
  //   [read time left]  [author centered]  [remaining time right]
  // Same vocabulary as the LyraFlow carousel center's footer, just
  // moved up here because the bookshelf cells don't have room for it
  // each. Author + times come from the focused-book metadata + stats
  // caches via ensureFocusedMetaLoaded.
  constexpr int titleStripHeight = 40;
  // CrumBLE: title strip moved from above the grid to the bottom of
  // the screen. Grid now starts directly below the header divider,
  // giving cells more vertical room. kBottomMargin bumped from 6 to
  // 14 so the title strip text isn't right against the screen edge --
  // 4x4 layouts in particular need the page-dot indicator AND title
  // strip to comfortably stack between the last row and the bottom.
  constexpr int kBottomMargin = 14;
  constexpr int titleGridGap = 0;  // no top title strip -- grid right under header
  // CrumBLE #133 follow-up: 2x2 selection ring is one notch smaller
  // (padding 3 / gap 1 -> outer extent 4 px) instead of the default
  // (padding 4 / gap 2 -> outer extent 6 px). At 220x320 covers the
  // wider ring was visually crowding the inter-row gap -- cutting
  // into the per-cell progress bar on both rows. The ring keeps its
  // double-stroke look (inner thick + outer thin); only the absolute
  // distance from cover edge shrinks.
  const int selectionPadding = is2x2 ? 3 : 4;
  const int selectionOutlineGap = is2x2 ? 1 : 2;
  const int selectionOuterInset = selectionPadding + selectionOutlineGap;
  // CrumBLE #133 follow-up: 3x3 uses a larger gridSpacing (24) for an
  // airier 9-cell page since the bigger 130x190 covers can carry the
  // extra breathing room. 4x4 keeps the default 16 (its 16 cells need
  // every column-pixel to fit at 100x150). 2x2 keeps default too.
  const bool is3x3 = (SETTINGS.bookshelfLayout == CrossPointSettings::BOOKSHELF_LAYOUT_3X3);
  const int gridSpacing = is3x3 ? 24 : kLyraGridSpacing;
  // Per-cell footer below each cover: a thin progress bar sized exactly
  // to the cover width, LightGray-dither track + solid-black fill (same
  // styling as the LyraFlow carousel center cover). Doubled the gap
  // between the cover bottom and the bar so the bar reads as part of
  // the book rather than glued to it.
  // CrumBLE #133 follow-up: 2x2's progress bar is fatter (7 vs 5 px)
  // so it visually balances against the 220x320 cover. The +2 was
  // sized as "about half the empty gap between cover bottom and bar
  // top" per user request -- gap is 4 px, so +2 makes the bar half
  // as thick as the gap. Row stride absorbs the +2 px; 2x2 bottom
  // mode's contentTop pulls up to compensate (see below).
  const int kProgressBarHeight = is2x2 ? 7 : 5;
  // CrumBLE #133 follow-up: 2x2 used to flush the progress bar against
  // the cover (gap=0); user requested a slight breathing space matching
  // 3x3 / 4x4 (4 px). The tightened selection ring (outerInset=4 vs 6)
  // gives back the row stride budget. 3x3 in BOTTOM placement gets
  // extra vertical space between rows (18 vs 14 in top placement);
  // top placement is geometrically tight (last row's progress bar sits
  // 1 px above the title strip already) so it keeps the smaller value.
  const int kProgressTopGap = 4;
  const int rowSpacing = is2x2 ? 4 : (is3x3 ? (titleAtTop ? 14 : 18) : 5);
  const int totalGridWidth = gridColumns_ * coverWidth_ + (gridColumns_ - 1) * gridSpacing;
  const int startXOffset = (pageWidth - totalGridWidth) / 2;
  // CrumBLE #133 follow-up: Title Placement-aware Y. TOP sits just
  // under the header divider (header takes 8..60, strip starts at 66
  // = 6 px below); BOTTOM keeps the historical anchor near the screen
  // bottom (above the page dots).
  const int titleStripY = titleAtTop ? (kHeaderTopPadding + kHeaderHeight + 6)
                                     : (pageHeight - kBottomMargin - titleStripHeight);

  const int totalBooks = static_cast<int>(recentBooks.size());
  const int totalPages = (totalBooks + booksPerPage_ - 1) / booksPerPage_;
  const int currentPage = (totalBooks > 0) ? (selectorIndex / booksPerPage_) : 0;
  const int pageStart = currentPage * booksPerPage_;
  const int pageCount = std::min(booksPerPage_, totalBooks - pageStart);

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    // CrumBLE #133 follow-up: peek row support for 3x3 layout. Title
    // strip drawing relocated to AFTER the cells loop so the optional
    // peek covers can extend into its area without stomping on text.
    // Lambda captures the locals computed above (contentTop, etc.) so
    // the layout math doesn't need to be plumbed.
    auto drawTitleStrip = [&]() {
    // CrumBLE #113: selected-book label strip.
    //   Line 1: title (UI_10 REGULAR, centered)
    //   Line 2: [read time left] [author centered] [remaining time right]
    if (selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size())) {
      const auto& selectedBook = recentBooks[selectorIndex].book;
      ensureFocusedMetaLoaded(selectedBook.path);
      const int selTitleLh = renderer.getLineHeight(UI_10_FONT_ID);
      const int selSubLh = renderer.getLineHeight(SMALL_FONT_ID);
      constexpr int kSelLineGap = 2;

      // Prefer metadata-cache title; then RecentBooksStore title (which
      // also gets populated from real metadata when the reader opens
      // the book); then filename-sans-extension fallback.
      std::string selTitleStr = focusedMetaTitle_;
      if (selTitleStr.empty()) selTitleStr = selectedBook.title;
      if (selTitleStr.empty()) selTitleStr = filenameWithoutExtension(selectedBook.path);

      std::string selAuthorStr = focusedMetaAuthor_;
      if (selAuthorStr.empty()) selAuthorStr = selectedBook.author;

      const int contentH = selTitleLh + kSelLineGap + selSubLh;
      // CrumBLE: titleY anchored to titleStripY (bottom of screen)
      // instead of contentTop (top under header). Same vertical
      // centering math, just in the new strip rect.
      const int titleY = titleStripY + (titleStripHeight - contentH) / 2;
      // CrumBLE #133 follow-up: bold title (matches paintGridFocusUpdate).
      const std::string truncSelTitle =
          renderer.truncatedText(UI_10_FONT_ID, selTitleStr.c_str(), totalGridWidth, EpdFontFamily::BOLD);
      const int selTitleW = renderer.getTextWidth(UI_10_FONT_ID, truncSelTitle.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, startXOffset + (totalGridWidth - selTitleW) / 2, titleY,
                        truncSelTitle.c_str(), true, EpdFontFamily::BOLD);

      // Line 2: read time (left edge of grid), author (centered),
      // remaining time (right edge). Same time-string vocabulary as
      // LyraFlowTheme: "<1m" / "Nm" / "Hh Mm" for elapsed, "<1m left"
      // / "Nm left" / "Nh left" for remaining (rounded to nearest hour
      // above 1h). Times only show when the book has reading stats;
      // remaining additionally requires >= 1% progress so we don't
      // show wildly inflated estimates on freshly-opened books.
      const int subY = titleY + selTitleLh + kSelLineGap;
      const bool hasStats = focusedMetaStatsLoaded_ && focusedMetaStats_.sessionCount > 0;
      const float clampedProgress = std::clamp(recentBooks[selectorIndex].progress, 0.0f, 100.0f);
      const bool hasProgress =
          recentBooks[selectorIndex].progressLoaded && RecentBookProgress::hasPercent(recentBooks[selectorIndex].progress);

      char readBuf[24] = "";
      if (hasStats) {
        const uint32_t seconds = focusedMetaStats_.totalReadingSeconds;
        if (seconds < 60) {
          snprintf(readBuf, sizeof(readBuf), "<1m");
        } else if (seconds < 3600) {
          snprintf(readBuf, sizeof(readBuf), "%um", static_cast<unsigned>(seconds / 60));
        } else {
          snprintf(readBuf, sizeof(readBuf), "%uh %um", static_cast<unsigned>(seconds / 3600),
                   static_cast<unsigned>((seconds % 3600) / 60));
        }
      }
      char remainBuf[24] = "";
      if (hasStats && hasProgress && clampedProgress >= 1.0f) {
        const uint32_t remaining = static_cast<uint32_t>(static_cast<float>(focusedMetaStats_.totalReadingSeconds) *
                                                        (100.0f - clampedProgress) / clampedProgress);
        if (remaining < 60) {
          snprintf(remainBuf, sizeof(remainBuf), "<1m left");
        } else if (remaining < 3600) {
          snprintf(remainBuf, sizeof(remainBuf), "%um left", static_cast<unsigned>(remaining / 60));
        } else {
          const unsigned hours = static_cast<unsigned>((remaining + 1800) / 3600);
          snprintf(remainBuf, sizeof(remainBuf), "%uh left", hours);
        }
      }

      if (readBuf[0]) {
        renderer.drawText(SMALL_FONT_ID, startXOffset, subY, readBuf, true, EpdFontFamily::REGULAR);
      }
      if (remainBuf[0]) {
        const int rw = renderer.getTextWidth(SMALL_FONT_ID, remainBuf, EpdFontFamily::REGULAR);
        renderer.drawText(SMALL_FONT_ID, startXOffset + totalGridWidth - rw, subY, remainBuf, true,
                          EpdFontFamily::REGULAR);
      }
      if (!selAuthorStr.empty()) {
        // Carve out the space the times consume so a long author name
        // doesn't visually collide with either side.
        const int readW = readBuf[0] ? renderer.getTextWidth(SMALL_FONT_ID, readBuf, EpdFontFamily::REGULAR) : 0;
        const int remainW = remainBuf[0] ? renderer.getTextWidth(SMALL_FONT_ID, remainBuf, EpdFontFamily::REGULAR) : 0;
        constexpr int kAuthorSidePad = 8;
        const int authorBudget =
            std::max(0, totalGridWidth - 2 * std::max(readW, remainW) - 2 * kAuthorSidePad);
        const std::string truncAuthor =
            renderer.truncatedText(SMALL_FONT_ID, selAuthorStr.c_str(), authorBudget, EpdFontFamily::REGULAR);
        const int aw = renderer.getTextWidth(SMALL_FONT_ID, truncAuthor.c_str(), EpdFontFamily::REGULAR);
        renderer.drawText(SMALL_FONT_ID, startXOffset + (totalGridWidth - aw) / 2, subY, truncAuthor.c_str(), true,
                          EpdFontFamily::REGULAR);
      }
    }
    };  // drawTitleStrip lambda

    const int rowStride = coverHeight_ + kProgressTopGap + kProgressBarHeight + rowSpacing;
    // CrumBLE #122: when the LAST row has fewer than gridColumns_ books,
    // center those books horizontally instead of left-justifying them.
    // (Full-width rows always have gridColumns_ cells, so they keep the
    // standard left-anchored layout.)
    const int rowsThisPage = (pageCount + gridColumns_ - 1) / gridColumns_;
    const int lastRowCount = pageCount - (rowsThisPage - 1) * gridColumns_;
    for (int i = 0; i < pageCount; ++i) {
      const int bookIdx = pageStart + i;
      // CrumBLE #113: load progress for every visible cell, not just the
      // focused one -- otherwise the progress bar only appears after the
      // user scrolls to a book at least once. ensureProgressLoaded is
      // idempotent + cheap (one stats.bin read per book per session).
      ensureProgressLoaded(bookIdx);
      const int col = i % gridColumns_;
      const int row = i / gridColumns_;
      const bool isPartialLastRow = (row == rowsThisPage - 1) && (lastRowCount < gridColumns_);
      const int rowCellCount = isPartialLastRow ? lastRowCount : gridColumns_;
      const int rowWidth = rowCellCount * coverWidth_ + (rowCellCount - 1) * gridSpacing;
      const int rowStartX = startXOffset + (totalGridWidth - rowWidth) / 2;
      const int x = rowStartX + col * (coverWidth_ + gridSpacing);
      // CrumBLE: grid rows now start directly at contentTop (under the
      // header divider). Was contentTop + titleStripHeight + gap when
      // the title strip lived above the grid.
      const int y = contentTop + row * rowStride;

      const int bx = x;
      const int by = y;
      const int bw = coverWidth_;
      const int bh = coverHeight_;

      // CrumBLE #113: dropped the L-shape drop shadow that used to sit
      // under and to the right of each cover. With the new per-cell
      // progress bar occupying the bottom-edge real estate, the shadow
      // overlapped the bar and the cell read as visually noisy.
      // Removing it also brings the Bookshelf grid visually closer to
      // the Flow carousel's clean cover + footer-bar styling.

      bool drawn = false;
      const std::string thumbPath =
          recentBooks[bookIdx].book.coverBmpPath.empty()
              ? ""
              : UITheme::getCoverThumbPath(recentBooks[bookIdx].book.coverBmpPath, coverWidth_, coverHeight_);
      if (!thumbPath.empty()) {
        // CrumBLE #131: try the in-RAM cover cache first (~1-2 ms blit
        // on hit, vs ~30-50 ms SD-load+decode on miss). The fallback
        // below catches the case where the cache budget rejected the
        // entry (low heap / many cached covers competing) but the file
        // DOES exist on SD -- without the fallback those books render
        // as placeholders even though we have a real thumb to show.
        GfxRenderer::CachedBitmap* handle = renderer.lookupCachedBitmap(thumbPath);
        int srcW = 0, srcH = 0;
        if (renderer.getCachedBitmapDimensions(handle, &srcW, &srcH) && srcW > 0 && srcH > 0) {
          const float srcRatio = static_cast<float>(srcW) / static_cast<float>(srcH);
          const float targetRatio = static_cast<float>(coverWidth_) / static_cast<float>(coverHeight_);
          float cropX = 0.0f;
          float cropY = 0.0f;
          if (srcRatio > targetRatio) {
            cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
          } else if (srcRatio < targetRatio) {
            cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
          }
          renderer.fillRoundedRect(bx, by, bw, bh, kCoverCornerRadius, Color::White);
          renderer.drawCachedBitmap<true>(handle, bx, by, bw, bh, cropX, cropY, kCoverCornerRadius);
          drawn = true;
        } else {
          // Cache-rejected (or file-not-cached, file-not-exists) path:
          // try a direct SD stream. For files that don't exist this
          // openFileForRead fails cheaply (same cost as the Storage.exists
          // check we dropped); for files that DO exist this is the
          // safety net that keeps the cover visible when the in-RAM
          // cache budget couldn't hold it.
          FsFile file;
          if (Storage.openFileForRead("RBGA", thumbPath, file)) {
            Bitmap bmp(file);
            if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
              float cropX = 0.0f;
              float cropY = 0.0f;
              calculateCoverFillCrop(bmp, coverWidth_, coverHeight_, cropX, cropY);
              renderer.fillRoundedRect(bx, by, bw, bh, kCoverCornerRadius, Color::White);
              renderer.drawBitmap(bmp, bx, by, bw, bh, cropX, cropY);
              renderer.maskRoundedRectOutsideCorners(bx, by, bw, bh, kCoverCornerRadius, Color::White);
              drawn = true;
            }
            file.close();
          }
        }
      }
      if (!drawn) {
        // CrumBLE: placeholder shows the centered book title (wrapped)
        // inside an outlined rounded cell, mirroring the carousel
        // fallback look. More informative than the previous generic
        // BookIcon and consistent with the new unified placeholder
        // style across Bookshelf / Collections shelf / carousel.
        renderer.fillRoundedRect(bx, by, bw, bh, kCoverCornerRadius, Color::White);
        renderer.drawRoundedRect(bx, by, bw, bh, 1, kCoverCornerRadius, true);
        const std::string& bookTitle = recentBooks[bookIdx].book.title.empty()
                                           ? filenameWithoutExtension(recentBooks[bookIdx].book.path)
                                           : recentBooks[bookIdx].book.title;
        constexpr int kPlaceholderPadX = 6;
        // CrumBLE #133 follow-up: cap is 5 lines at 3x3 and below
        // (per-user) so the title block reads as a "label" rather than
        // a wall of wrapped text. Larger covers (4x4/2x2) keep the
        // larger cap since they have more vertical room and very
        // long titles do need it.
        const int kPlaceholderMaxLines = is3x3 ? 5 : 12;
        const auto titleLines = renderer.wrappedText(SMALL_FONT_ID, bookTitle.c_str(), bw - 2 * kPlaceholderPadX,
                                                     kPlaceholderMaxLines, EpdFontFamily::BOLD);
        const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
        const int blockH = static_cast<int>(titleLines.size()) * lineH;
        int textY = by + (bh - blockH) / 2;
        for (const auto& line : titleLines) {
          const int lineW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
          renderer.drawText(SMALL_FONT_ID, bx + (bw - lineW) / 2, textY, line.c_str(), true, EpdFontFamily::BOLD);
          textY += lineH;
        }
      }
      if (bookIdx == static_cast<int>(selectorIndex)) {
        renderer.drawRoundedRect(bx - selectionPadding, by - selectionPadding, bw + selectionPadding * 2,
                                 bh + selectionPadding * 2, 3, kCoverCornerRadius + selectionPadding, true);
        renderer.drawRoundedRect(bx - selectionOuterInset, by - selectionOuterInset, bw + selectionOuterInset * 2,
                                 bh + selectionOuterInset * 2, 1, kCoverCornerRadius + selectionOuterInset, true);
      }

      // CrumBLE #113: per-cell progress bar, styled like the Flow carousel
      // center cover's footer (LightGray-dithered track with solid-black
      // fill). Width matches the cover; ~2 px gap below the cover so the
      // bar reads as part of the book, not as separate UI.
      const int barY = by + bh + kProgressTopGap;
      renderer.fillRectDither(bx, barY, bw, kProgressBarHeight, Color::LightGray);
      if (recentBooks[bookIdx].progressLoaded &&
          RecentBookProgress::hasPercent(recentBooks[bookIdx].progress)) {
        const float pct = std::clamp(recentBooks[bookIdx].progress, 0.0f, 100.0f);
        const int fillW = static_cast<int>(std::round(bw * pct / 100.0f));
        if (fillW > 0) {
          renderer.fillRect(bx, barY, fillW, kProgressBarHeight, true);
        }
      }
    }

    // Title strip text comes AFTER cells. The lambda was hoisted out
    // of the original pre-cells block so future render passes (e.g. a
    // bottom-peek that draws below row 2 in 3x3) won't stomp on the
    // text; for now the order is just: cells -> title strip -> dots.
    drawTitleStrip();

    if (totalPages > 1) {
      // CrumBLE #133 follow-up: smaller dots in 2x2 -- the bottom row's
      // progress bar sits close enough to the dots band that 8 px dots
      // overlap the bar; 6 px restores the visual gap without losing
      // legibility (the dots are anchored relative to titleStripY so
      // shrinking them widens the gap between progress bar bottom and
      // dot top).
      const int dotSize = is2x2 ? 6 : 8;
      constexpr int dotSpacing = 6;
      const int totalDotWidth = totalPages * dotSize + (totalPages - 1) * dotSpacing;
      const int dotsStartX = (pageWidth - totalDotWidth) / 2;
      // CrumBLE #133 follow-up: dots sit just above the title strip
      // when it's at the BOTTOM, and at the screen bottom when the
      // strip is at the TOP -- always paired with the strip so they
      // read as one "status row" at the screen edge.
      constexpr int kDotsTitleGap = 6;
      const int dotY = titleAtTop ? (pageHeight - kBottomMargin - dotSize)
                                  : (titleStripY - kDotsTitleGap - dotSize);
      for (int p = 0; p < totalPages; p++) {
        const int dx = dotsStartX + p * (dotSize + dotSpacing);
        if (p == currentPage) {
          renderer.fillRect(dx, dotY, dotSize, dotSize, true);
        } else {
          renderer.drawRect(dx, dotY, dotSize, dotSize, true);
        }
      }
    }
  }

  // The four physical hint slots are already occupied; Up/Down still navigate
  // the grid but are not rendered in this compact hint bar.
  // CrumBLE: button hints intentionally omitted on Bookshelf. The grid
  // is its own visual surface (chrome at top, title strip at bottom,
  // grid filling the rest) and the L/R/U/D semantics are obvious from
  // the focus ring. Frees ~40 px of vertical real estate at the bottom
  // for the title strip to live in.

  renderer.displayBuffer();

  // CrumBLE #133 follow-up: free heap-heavy state BEFORE loadPageCovers.
  // Sequential cover gens (16 books in 4x4, 4 at 220x320 in 2x2) need
  // every contiguous KB they can get for the JPG/PNG decoder + scaled
  // BMP write buffer. The 48 KB framebuffer snapshot was being held
  // during gen and starving the decode allocations -- that was the
  // dominant reason placeholders stayed stuck across visits even with
  // the NoIndex + heavy-fallback gen pair. HomeActivity.loadShelfCovers
  // does the same thing (frees coverBuffer + carousel caches before
  // gen at line 1084-1089). Image cache stays put: any covers already
  // there (e.g. top-4 from the Flow shelf) are still useful for the
  // visible cells; lookupCachedBitmap's LRU evicts as gen+render
  // pressure dictates.
  freeGridSnapshot();
  invalidateGridSnapshot();

  bool genMayHaveRun = false;
  if (!recentBooks.empty() && loadedPageStart != pageStart) {
    // CrumBLE #133 follow-up: also dump the in-RAM image cache before
    // gen. The cache may hold up to 64 KB of cached cover bitmaps from
    // the Flow shelf / carousel; gen needs that contiguous heap more
    // than the cells need a warm cache hit. lookupCachedBitmap repopulates
    // lazily on the next render after gen.
    renderer.clearImageCache();
#ifdef ESP_PLATFORM
    LOG_ERR("RBGA", "pre-loadPageCovers: free=%u maxAlloc=%u page=%d books=%d", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), pageStart, pageCount);
#endif
    loadPageCovers(pageStart);
#ifdef ESP_PLATFORM
    LOG_ERR("RBGA", "post-loadPageCovers: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif
    genMayHaveRun = true;
  }

  // Re-capture the snapshot AFTER gen so the focus-only fast path on
  // subsequent L/R presses has a clean framebuffer to restore. Skip
  // when gen ran -- if its popup was shown, requestUpdate fires and
  // the next full render will capture a clean snapshot anyway; if no
  // popup was shown the framebuffer is already clean but we'd just be
  // racing the next render. Either way no harm in deferring.
  if (!genMayHaveRun) {
    const bool snapshotOk = storeGridSnapshot();
    if (snapshotOk) {
      gridSnapshotPage_ = (totalBooks > 0) ? (selectorIndex / booksPerPage_) : 0;
      gridSnapshotFocusedIndex_ = selectorIndex;
      gridSnapshotCollectionId_ = collectionId_;
      gridSnapshotValid_ = true;
    } else {
      invalidateGridSnapshot();
    }
  }
  // CrumBLE #125: pre-warm metadata + progress for every book on the
  // newly-painted page. The focus-only fast path returns early above
  // this code, so this runs only on full repaints (entry, page change,
  // collection switch, book mutation). Cache lookups are idempotent;
  // already-cached books cost a hashmap hit. Newly visible books pay
  // their first-visit SD cost here -- AFTER the user sees the page --
  // so subsequent L/R presses within the page are RAM-only.
  prewarmVisiblePage(selectorIndex);
}
