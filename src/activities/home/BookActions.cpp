#include "BookActions.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <cstdio>

#include "BookmarkStore.h"
#include "CollectionsStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "LibraryIndex.h"
#include "RecentBooksStore.h"
#include "SeriesIndex.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir("/Read");
  std::string dstPath = "/Read/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = "/Read/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

}  // namespace

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const BookActionMenuOptions& options) {
  // Item order is documented on BookActionMenuOptions in BookActions.h --
  // benign -> destructive, top to bottom. Don't reorder casually: the
  // file-browser, home carousel, recent-books list/grid, and bookmarks-home
  // long-press menus all share this ordering so users get the same row
  // layout everywhere.
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isBookFile = isEpub || FsHelpers::hasXtcExtension(fullPath) ||
                          FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath);

  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(6);

  // 1. Add to / remove from collection
  if (options.addToCollection && isBookFile) {
    items.push_back({FileBrowserAction::AddToCollection, StrId::STR_ADD_TO_COLLECTION});
  }
  // 2. Remove from Recent Books (caller picks which dispatch enum)
  if (options.removeFromRecents) {
    const FileBrowserAction action = *options.removeFromRecents;
    // Same display label for both -- behavior differs in which handler
    // fires, but the user-facing prompt reads identically.
    const StrId label = (action == FileBrowserAction::RemoveFromRecentBooks) ? StrId::STR_REMOVE_FROM_RECENT_BOOKS
                                                                             : StrId::STR_REMOVE_FROM_RECENTS_ACTION;
    items.push_back({action, label});
  }
  // 3. Mark as finished / unfinished
  if (isEpub) {
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isEpubCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  // 4. Show metadata (debug inspector for book files)
  if (options.showMetadata && isBookFile) {
    items.push_back({FileBrowserAction::ShowMetadata, StrId::STR_SHOW_METADATA});
  }
  // 5. Delete book cache
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  // 5b. CrumBLE 4.4 (ported from CrossInk v1.3.3): delete just this book's
  // reading stats (stats.bin). Shown only for EPUBs since CrumBLE's stats
  // pipeline records per-EPUB. Placed adjacent to Delete Cache so the two
  // destructive-but-bounded actions cluster together above plain Delete.
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
  }
  // 6. Delete file -- always last as the most destructive.
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  return items;
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

std::string optimizedHeaderLabel(const std::string& fullPath) {
  // Only EPUB carries a prebake. Skip the SD lookup for non-EPUB paths.
  if (!FsHelpers::hasEpubExtension(fullPath)) return "";
  // CrumBLE 4.4: three-tier badge (matches the FT page wording exactly).
  //   prebake-v2.marker only                  -> "✓IMG"
  //   + prebake-chap.marker (or sections-prebake/) -> "✓IMG+CHAP"
  //   + prebake-cpfont.marker                 -> "✓IMG+CHAP+CP.FONT"
  // Each Storage.exists is a fast inode lookup; the four lookups still
  // fit comfortably within the long-press path's budget.
  const std::string cacheDir = Epub::cachePathForFilePath(fullPath, "/.crosspoint");
  if (!Storage.exists((cacheDir + "/prebake-v2.marker").c_str())) return "";
  const bool hasChap = Storage.exists((cacheDir + "/prebake-chap.marker").c_str()) ||
                       Storage.exists((cacheDir + "/sections-prebake").c_str());
  const bool hasCpFont = Storage.exists((cacheDir + "/prebake-cpfont.marker").c_str());
  // No leading checkmark on device-side labels -- the long-press header
  // renders a lightning glyph (drawBolt) next to the label, which carries
  // the "this is the optimization indicator" signal. The FT page uses
  // an emoji ⚡ in the text instead since it renders cleanly in browsers.
  if (hasChap && hasCpFont) return "IMG+CHAP+CP.FONT";
  if (hasChap) return "IMG+CHAP";
  return "IMG";
}

BookHeaderText resolveBookHeaderText(const std::string& fullPath) {
  BookHeaderText out;

  // Filename is the last-resort title -- always populated even when
  // metadata lookups fail or the path isn't a book file at all.
  const size_t lastSlash = fullPath.find_last_of('/');
  out.title = (lastSlash != std::string::npos) ? fullPath.substr(lastSlash + 1) : fullPath;

  // Step 1: RecentBooksStore is in-RAM (we read it at boot from
  // .crosspoint/recent-books.bin). If the user has opened this book
  // recently the cached title/author beat anything we'd parse from disk.
  // BUT -- if the recents row is missing the author (older entries
  // sometimes were saved with empty author when OPF parse failed at
  // open time), we fall through to the OPF parse below so the long-press
  // menu can still show the author line. Title from recents wins because
  // it's almost always populated (filename-derived if nothing else) and
  // we want a stable display name.
  bool recentsHadAuthor = false;
  for (const auto& rb : RECENT_BOOKS.getBooks()) {
    if (rb.path == fullPath) {
      if (!rb.title.empty()) out.title = rb.title;
      if (!rb.author.empty()) {
        out.author = rb.author;
        recentsHadAuthor = true;
      }
      break;
    }
  }
  if (recentsHadAuthor) return out;

  // Step 2: not in recents (or recents lacked author) -- if it's an EPUB
  // or XTC we can open it and parse OPF metadata for title/author. ~50-
  // 100 ms cold; acceptable on long-press (runs once per menu open, not
  // on a render loop). For anything else (PDF, TXT, MD, sleep images,
  // etc.) the filename is the best we have and we leave author empty so
  // the header collapses to the original single-line layout.
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub epub(fullPath, "/.crosspoint");
    const std::string& t = epub.getTitle();
    const std::string& a = epub.getAuthor();
    if (!t.empty()) out.title = t;
    if (!a.empty()) out.author = a;
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc xtc(fullPath, "/.crosspoint");
    const std::string t = xtc.getTitle();
    const std::string a = xtc.getAuthor();
    if (!t.empty()) out.title = t;
    if (!a.empty()) out.author = a;
  }

  return out;
}

void clearFileMetadata(const std::string& fullPath) {
  // CrumBLE 4.4: clear EVERY index/store that holds a reference to this book
  // path, not just bookmarks + cache. Previously only HomeActivity's delete
  // path did the full sweep inline -- the bookshelf / file browser deletes
  // called this helper which left stale entries in Collections, LibraryIndex,
  // and SeriesIndex. Symptom: deleting a book from inside the bookshelf left
  // a coverless placeholder that re-appeared in Collections and couldn't be
  // opened (file gone but RECENT_BOOKS/index entries lingered). Doing the
  // full cleanup in one place means all four delete sites stay in sync.
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  }
  CollectionsStore::getInstance().removeBookFromAllCollections(fullPath);
  LibraryIndex::getInstance().forgetPath(fullPath);
  SeriesIndex::getInstance().forgetPath(fullPath);
  LOG_DBG("BookActions", "Cleared metadata for: %s", fullPath.c_str());
}

bool clearBookCache(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    return Epub(fullPath, "/.crosspoint").clearCache();
  }
  if (FsHelpers::hasXtcExtension(fullPath)) {
    return Xtc(fullPath, "/.crosspoint").clearCache();
  }
  return false;
}

bool isEpubCompleted(const std::string& fullPath) {
  const Epub epub(fullPath, "/.crosspoint");
  return BookReadingStats::load(epub.getCachePath()).isCompleted;
}

bool toggleEpubCompleted(const std::string& fullPath, const std::string& displayName, bool& completed) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }

  Epub epub(fullPath, "/.crosspoint");
  epub.setupCacheDir();

  BookReadingStats stats = BookReadingStats::load(epub.getCachePath());
  completed = !stats.isCompleted;
  stats.isCompleted = completed;

  GlobalReadingStats globalStats = GlobalReadingStats::load();
  if (completed) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(epub.getCachePath());
  globalStats.save();
  // CrumBLE: completion flip changes Finished / New collection membership.
  // Drop the cache so the next Home enter rescans the affected book.
  CollectionsStore::getInstance().invalidateScannedVirtuals();

  if (completed && SETTINGS.moveFinishedToReadFolder && fullPath.rfind("/Read/", 0) != 0) {
    const std::string oldCachePath = epub.getCachePath();
    const std::string dstPath = buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!Storage.rename(fullPath.c_str(), dstPath.c_str())) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
               tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      return true;
    }

    const std::string newCachePath = Epub::cachePathForFilePath(dstPath, "/.crosspoint");
    if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
      if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
        LOG_ERR("BookActions", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(),
                newCachePath.c_str());
      }
    }

    RECENT_BOOKS.updatePath(fullPath, dstPath, oldCachePath, newCachePath);
    if (APP_STATE.openEpubPath == fullPath) {
      APP_STATE.openEpubPath = dstPath;
      APP_STATE.saveToFile();
    }
  }

  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
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

}  // namespace BookActions
