#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../Activity.h"
#include "../reader/BookReadingStats.h"  // focused-book stats cache for the label strip
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksGridActivity final : public Activity {
 public:
  // CrumBLE #133: layout is now driven by SETTINGS.bookshelfLayout
  // (3x3/4x4/2x2). The fields below are non-static so they can be
  // re-derived on every onEnter -- the user toggles the Layout row
  // from BookshelfPickerActivity, and on return we need the new
  // dimensions to take effect.
  //
  // Cover-size strategy (deliberate -- maximises cache reuse with
  // other activities that already render the same book):
  //   3x3 -> 123x180 (legacy bookshelf cells, kept by user preference)
  //   4x4 -> 100x150 (matches Flow shelf cells: warm cache on entry)
  //   2x2 -> 220x320 (matches carousel center cover + Stats main cover)
  // Defaults below describe the 4x4 layout (the new default) so any
  // code that runs before applyLayoutFromSettings() (e.g.
  // construction) sees a sensible value rather than 0.
  int coverWidth_ = 100;
  int coverHeight_ = 150;
  int gridColumns_ = 4;
  int gridRows_ = 4;
  int booksPerPage_ = 16;
  int maxGridBooks_ = 32;
  // Reads SETTINGS.bookshelfLayout and populates the fields above.
  // Idempotent -- safe to call from onEnter every time.
  void applyLayoutFromSettings();

  // CrumBLE #81: dual-mode constructor. Default form preserves the legacy
  // RECENT_BOOKS view (max maxGridBooks_ books, ordered by last-opened).
  // The collectionId overload sources books from CollectionsStore --
  // typically the currently-active collection -- so the icon-bar entry
  // becomes a "Bookshelf" grid over the user's collections.
  explicit RecentBooksGridActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooksGrid", renderer, mappedInput) {}
  RecentBooksGridActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string collectionId)
      : Activity("RecentBooksGrid", renderer, mappedInput), collectionId_(std::move(collectionId)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct BookState {
    RecentBook book;
    float progress = -1.0f;
    bool progressLoaded = false;
  };
  static constexpr int NO_PAGE_LOADED = -1;

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool longPressFired = false;
  // CrumBLE #130: same kLongPressMs guard as the Confirm long-press, but
  // for the Back button -- holding past the threshold opens the
  // bookshelf collection picker so the user can jump to another
  // collection without exiting to home first. Latched so the eventual
  // Back release doesn't also fire the short-press goHome handler.
  bool backLongPressFired = false;
  void showBookshelfCollectionPicker();
  std::vector<BookState> recentBooks;
  int loadedPageStart = NO_PAGE_LOADED;
  // CrumBLE: when non-empty, loadRecentBooks() pulls from the given
  // CollectionsStore collection instead of RECENT_BOOKS. Title gracefully
  // falls back to filename for books without cached metadata. Empty
  // string = legacy RECENT_BOOKS mode.
  std::string collectionId_;

  // CrumBLE #113: lazy metadata + stats cache for the SELECTED book's
  // title / author / read-time / remaining-time. Collection-mode
  // bootstrap (#81) only fills book.title with the filename because
  // parsing every book's metadata at load time would cost ~5 ms x 18
  // books. Instead, the top selected-book label strip calls
  // ensureFocusedMetaLoaded(path) before rendering -- mirrors the
  // pattern HomeActivity uses for the Flow shelf focused-book.
  std::string focusedMetaPath_;
  std::string focusedMetaTitle_;
  std::string focusedMetaAuthor_;
  BookReadingStats focusedMetaStats_;
  bool focusedMetaStatsLoaded_ = false;
  void ensureFocusedMetaLoaded(const std::string& path);

  // CrumBLE #125: multi-slot per-book metadata cache. Was a single
  // focused-book cache; the focus-only fast path re-introduced the
  // first-visit-per-book SD cost on every L/R press to a new book.
  // With this cache, prewarmVisiblePage() can populate every visible
  // book's metadata at entry, so subsequent L/R presses hit RAM only.
  // Cleared on onExit and whenever the book list mutates.
  struct BookMetaCacheEntry {
    std::string title;
    std::string author;
    BookReadingStats stats;
    bool statsLoaded = false;
  };
  std::unordered_map<std::string, BookMetaCacheEntry> bookMetaCache_;
  // Loads + caches book metadata (title/author/stats) from SD. Cache
  // hit returns instantly. Used by both ensureFocusedMetaLoaded (which
  // copies into the focused* fields) and prewarmVisiblePage (which
  // only populates the cache).
  const BookMetaCacheEntry& loadBookMetaToCache(const std::string& path);
  // Pre-loads metadata + progress for every book on the page that
  // contains `focusedIndex`. Called at onEnter so the initial render
  // and subsequent L/R presses within the page are RAM-only (zero SD
  // per press). Bounded: booksPerPage_ books per page (4-16 depending
  // on layout), ~20-30 ms each on cold cache. Hidden behind any
  // Loading popup loadPageCovers shows.
  void prewarmVisiblePage(int focusedIndex);

  bool isCollectionMode() const { return !collectionId_.empty(); }
  void loadRecentBooks();
  void loadPageCovers(int pageStart);
  void ensureProgressLoaded(int index);
  void reloadAfterBookAction();
  void promptDeleteBook(const RecentBook& book);
  void promptRemoveBook(const std::string& path, const std::string& title);
  void showBookActionMenu(int bookIndex, bool ignoreInitialConfirmRelease = false);

  // CrumBLE #125: framebuffer-snapshot + focus-only fast path. Mirrors
  // HomeActivity's storeCoverBuffer/restoreCoverBuffer pattern. Every
  // L/R/U/D within the same page used to fire a full clearScreen + 9-
  // cell repaint -- now the snapshot is restored and only the focus
  // ring + title strip are repainted. Restored to a full repaint when
  // the page changes, the collection switches, or any other state
  // shift invalidates the snapshot.
  uint8_t* gridSnapshot_ = nullptr;
  size_t gridSnapshotSize_ = 0;
  bool gridSnapshotValid_ = false;
  int gridSnapshotPage_ = -1;
  int gridSnapshotFocusedIndex_ = -1;
  std::string gridSnapshotCollectionId_;
  // Helpers to take/restore/release the snapshot. Implemented inline
  // when straightforward; named alongside the storeCoverBuffer pattern
  // for cross-activity familiarity.
  bool storeGridSnapshot();
  bool restoreGridSnapshot();
  void freeGridSnapshot();
  void invalidateGridSnapshot() { gridSnapshotValid_ = false; }
  // Partial repaint: erase the previously focused cell's selection
  // ring, draw the new one, and refresh the focused-book title strip.
  // Caller must have already restored the framebuffer snapshot and
  // confirmed that the only state change is selectorIndex within the
  // same page (gridSnapshotPage_ == currentPage).
  void paintGridFocusUpdate(int prevFocusedIndex, int newFocusedIndex);
};
