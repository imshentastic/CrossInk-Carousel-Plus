#pragma once
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "./FileBrowserActionActivity.h"
#include "./FileBrowserActivity.h"
#include "CollectionsStore.h"  // ShelfEntry
#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "util/DirtyRegion.h"
#include "activities/reader/GlobalReadingStats.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

// CrumBLE 4.6: called from Settings' Regenerate All Covers action so the
// next Home entry drops its carousel frame cache + Flow snapshot buffer
// and re-reads thumbs from SD (which we just nuked). Without this, Home
// returns from Settings via the fast snapshot path and shows the old
// pre-tone bitmaps even though Bookshelf has updated.
void invalidateHomeCoverCachesGlobal();

class HomeActivity final : public Activity {
 public:
  // v18.9.9.450: bumped 1->3 (current + next + prev). Enables the pre-
  // existing neighbor-slot code at preRenderCarouselFrames() so L/R nav
  // hits RAM instead of an SD read per press. Cost: ~144 KB total held
  // during a Home visit (~48 KB per slot * 3 on X4).
  //
  // History:
  //   v206 reverted 2->1 because the 2-slot version's freed slots caused
  //   fragmentation that carried through Home->Reader, breaking BT/FT/dict
  //   maxAlloc gates on X3. Since v206 the heap-management story has
  //   tightened significantly (v383 book-open heap-restart, v432/v433
  //   framebuffer release during FT, v441+ stats bitfield trims, etc.)
  //   so the fragmentation carry-through is less load-bearing than it was.
  //   HomeActivity::onExit already frees these slots (see the loop at
  //   ~2898) so the Reader/FT paths get the full heap back on transition.
  //
  //   If field reports show renewed X3 fragmentation post-Home, either
  //   condition the count on device (X4=3, X3=2) or revert to 2.
  static constexpr int kCarouselFrameCount = 3;
  // Must be >= max(homeRecentBooksCount) across themes — asserted in .cpp.
  // Bumped from 3 to 5 (CrumBLE #124) so Flow's 5-slot carousel can also hit
  // the BookReadingStats / progress cache. Cost is ~32 extra bytes total
  // (BookReadingStats ~12 B + float 4 B per slot); the win is eliminating
  // ~2 SD reads on every carousel L/R press for the 2 books that previously
  // fell outside the cap.
  static constexpr int kMaxCachedBooks = 5;

  // CrumBLE #120: forget any saved cursor position so the next Home visit
  // falls through to APP_STATE.openEpubPath (just-read book promotion) or
  // the default cold-boot landing. Called by ReaderActivity::onEnter so
  // that reader -> Home highlights the book the user just exited, even
  // when the user navigated through Home before opening the reader (which
  // would otherwise have left a stale saved cursor pointing at some icon
  // or shelf book).
  static void clearSavedCursor();

 private:
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int lastCarouselBookIndex = 0;  // remembered position when leaving carousel row
  // Mirror of lastCarouselBookIndex but for the shelf. Saved when the
  // user leaves the books row (Up to header, Down to menu) and
  // restored when they come back, so navigating off and back doesn't
  // dump the cursor at book 0. Reset to 0 on onEnter and when the
  // active collection changes (the index would otherwise point into
  // the wrong collection's content).
  int lastShelfBookIndex = 0;
  // Mirror again for the bottom icon-bar menu. Saved when leaving
  // the menu row in any direction; restored when re-entering. Reset
  // on onEnter — the menu's content (number of items) can also change
  // based on settings, so the index is clamped against menuItemCount
  // each time it's used.
  int lastMenuIndex = 0;
  // CrumBLE Collections — leftmost visible spine index on the bookshelf
  // strip; adjusted when the focused spine would otherwise scroll out of view.
  int shelfScrollOffset = 0;
  // True when the cursor is on the collection-name header (the "Favorites"
  // tab) rather than on a book in the shelf row. Header is a separate
  // focus state above the books — L/R cycles the active collection,
  // Down/Confirm enters the books, Up returns to the carousel. Reset on
  // onEnter so navigation always starts on the carousel.
  bool shelfHeaderFocused = false;
  // Cache of the active collection's resolved book paths. Without this,
  // every frame called resolveBookPaths() up to 5 times — for "All Books"
  // with hundreds of entries that meant ~250 KB of string-vector copies
  // and sorts per frame, causing visible home-screen lag. Refresh only
  // when the active collection id changes, when membership mutates
  // (picker, rescan, file delete), or on onEnter. Cache key holds the
  // active id at the time of refresh; an empty key means cold.
  std::string shelfPathsCacheKey;
  std::vector<std::string> shelfPathsCache;
  // Series-collapsed view of the active collection — derived from the
  // path list + SeriesIndex. cachedShelfEntries() is the primary cache
  // and shelfPathsCache is recomputed alongside it (one firstPath per
  // entry). Navigation indexes ShelfEntries 1:1 with cells on the
  // shelf row.
  std::vector<ShelfEntry> shelfEntriesCache;
  // CrumBLE 4.2.1: consecutive count of cachedShelfEntries() calls that hit
  // a heap-pressure-empty result from resolveShelfEntries(). Bounded retry
  // (cap is kMaxHeapPressureRetries in the .cpp) so a permanently low heap
  // doesn't drive an infinite render loop. Reset to 0 the moment a real
  // result lands or the retry cap is reached.
  int shelfHeapRetryCount_ = 0;
  // Snapshot of the shelf-relevant state at the moment the cached
  // framebuffer (coverBuffer) was last filled. When this matches the
  // live state AND we successfully restored the framebuffer, the
  // shelf's pixels are already on-screen and we can skip the expensive
  // BMP loads for each visible cell — this is the dominant cost of a
  // home re-render. Reset whenever the shelf could possibly look
  // different (active collection, scroll, focused index/header).
  bool shelfSnapshotValid = false;
  std::string shelfSnapshotActiveId;
  int shelfSnapshotScrollOffset = -1;
  int shelfSnapshotFocusedSpine = -2;  // -1 means "no focus", so use a different sentinel.
  bool shelfSnapshotHeaderFocused = false;
  // True when we have NOT yet run the series-enrichment pass for the
  // currently active collection. Set on onEnter and on cycle; cleared
  // by enrichActiveCollectionForSeries(). Keeps the "Detecting
  // series..." popup from re-firing on every render of the same shelf.
  bool seriesEnrichmentNeededForActive = true;
  // The carousel center hint (coverSelectorIndex) painted into the
  // cached framebuffer. When this matches the value we'd compute for
  // the current render AND the buffer was restored, the carousel's 5
  // covers are still correct on screen — set
  // LyraFlowTheme::skipCarouselCoverLoads so the theme bypasses its
  // SD reads. Sentinel = -1 means "no prior snapshot exists".
  int lastRenderedCoverSelectorIdx = -1;
  bool lastRenderedCoverSelectorValid = false;
  // Set once shelf cover BMPs have been resolved + generated-if-missing for
  // the current home session. Reset on onEnter so a freshly added book picks
  // up its thumbnail on the next return-to-Home.
  bool shelfCoversLoaded = false;
  // Book paths whose shelf thumbnail generation failed this session (corrupt
  // or unsupported cover image, etc.). loadShelfCovers runs on every shelf
  // render and skips books whose thumb already exists on SD — but a book
  // that *fails* generation never produces a file, so without this guard it
  // would be retried every single render, re-showing the Loading popup and
  // calling requestUpdate() each time → an endless flashing loop. We record
  // the failure once and thereafter render the book as a blank cover instead
  // of retrying. Cleared on onEnter so a transient failure gets one retry
  // per home visit. std::vector (not set) — these lists are tiny.
  std::vector<std::string> failedShelfCovers;
  // First-index cover safety cap. On the very first boot (fresh library index
  // just built), generating covers for a large library — on top of the SD walk
  // that just ran and a fragmented heap — has OOM-crashed devices. We cap how
  // many covers we generate during that one boot; capped books render blank and
  // get their cover on the next boot (index cached, no walk, no cap). Counter
  // is session-scoped and only enforced while LibraryIndex::wasFreshFirstBoot().
  static constexpr int kFirstIndexCoverCap = 24;
  int firstIndexCoversGenerated = 0;
  // Per-collection shelf position (scroll offset + focused book index),
  // keyed by collection id. When the user cycles the active collection on
  // the shelf header and later switches back, we restore where they were —
  // both the visible scroll window and the index they land on when pressing
  // Down into the books — so the two stay consistent (previously every
  // switch reset to book 0 while the restored framebuffer still showed the
  // old scroll position). Cleared on onEnter so each home visit starts fresh.
  struct ShelfPos {
    int scrollOffset = 0;
    int bookIndex = 0;
  };
  std::unordered_map<std::string, ShelfPos> shelfPosByCollection;
  // CrumBLE: cursor recall across home visits. onExit snapshots where
  // the user was (selectorIndex + per-row mirrors + shelf scroll); the
  // next onEnter restores it instead of leaving the cursor at 0. Reset
  // implicitly only by destructing the HomeActivity (e.g. cold boot).
  // Skipped on returns from the reader (APP_STATE.openEpubPath set) so
  // the "just-read book is highlighted" affordance still wins for that
  // specific path. The map deep-copies (small; one entry per visited
  // collection).
  // CrumBLE #120: STATIC so the saved cursor survives the activity
  // recreation that happens on every home <-> other-activity transition
  // (ActivityManager::replaceActivity destroys the current activity).
  // Instance-field versions of these used to reset to default before the
  // restoring onEnter could read them, so cursor recall only "worked"
  // for transitions whose return path passed goHome() an initialMenuItem
  // for the specific icon. The static promotion makes full carousel /
  // shelf / menu position recall work across every entry path.
  static bool hasSavedCursor_;
  static int savedSelectorIndex_;
  static int savedLastCarouselBookIndex_;
  static int savedLastShelfBookIndex_;
  static int savedLastMenuIndex_;
  static bool savedShelfHeaderFocused_;
  static std::unordered_map<std::string, ShelfPos> savedShelfPosByCollection_;
  // Set true during a Flow render whenever a progress popup (shelf cover
  // loading or "Detecting series...") was drawn over the framebuffer before
  // the end-of-render snapshot. The popup sits over the carousel, which the
  // carousel/shelf fast-paths don't repaint — so if we snapshotted it, every
  // subsequent restore would keep the stale popup on screen until the user
  // navigated to the carousel (forcing a repaint). When set, we skip the
  // snapshot and drop the cached render state so the follow-up render does a
  // full clean repaint that erases the popup. Reset at the start of each render.
  bool homeRenderPopupShown = false;
  // Suppresses the short-press Confirm handler when the user just held Confirm
  // long enough to trigger the book action menu — otherwise the matching
  // release would immediately open the book they were just acting on. Same
  // pattern as FileBrowserActivity::longPressConfirmHandled.
  bool longPressConfirmHandled = false;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  // v18.9.9.345: fires the ONCE-per-session cleanup that follows the
  // first successful Home render -- shrink imageCache budget to the
  // new low-tier (16 KB) and free the font decompressor's hot-group
  // buffer (1-3 KB). Both are wins with essentially no UX cost after
  // the first paint has settled.
  void postFirstRenderCleanup_();
  // CrumBLE: set on every onEnter so the first time Home presents after a
  // transition it does one full (HALF) refresh to clear ghosting bled through
  // from the previous screen (e.g. a dense reader page). Consumed by
  // presentHomeBuffer(); subsequent in-place updates stay fast.
  bool pendingFullRefresh = false;
  // v18.9.9.340: promote presentHomeBuffer's HALF_REFRESH_DEEP to a full
  // FULL_REFRESH when this bit is set. Purpose: returns from full-screen
  // activities (Reading Heatmap, Book Stats, Reading Stats) leave text-
  // heavy pixels on the panel that HALF (which is a single-pass 0xD7
  // waveform on X4) can't fully overwrite -- symptom: user reports
  // "heatmap still visible over carousel" after Back. FULL uses the
  // multi-flash GC waveform which reliably scrubs. Consumed once per
  // present, then reset. Sourced from pendingHomeFullRefresh's consume
  // block in onEnter.
  bool pendingFullRefreshHard_ = false;

  // v18.9.9.343: nav-coalesce (v314) removed. Users reported "sometimes
  // weird behavior" and the added complexity (deferred render, drain
  // tick in loop()) didn't win them enough. Reverted to direct
  // requestUpdate() on every arrow press.
  // (Removed: homeFastLutBaselineReady_ field. The displayBufferFastLut
  // experiment using lut_grayscale_revert ran but failed on hardware --
  // dark pixels accumulated over-drive, getting darker each nav. The SDK
  // primitive is kept for a future re-enable once a proper partial-mode
  // LUT is sourced from Good Display.)
  // CrumBLE 4.5.5+: render-profile timing. Stamped at the top of render()
  // and read at the bottom of presentHomeBuffer() to log a per-nav
  // (prep, panel-refresh, total) breakdown. Drives the "where can we
  // actually shave home nav latency" investigation. Free at runtime
  // (a single millis() read + a LOG_INF line per render).
  unsigned long renderProfileStartMs_ = 0;
  // 4.5.5+: per-frame dirty-region tracking for partial-refresh routing.
  // Render paths that update only a small slice of the screen (e.g. the
  // shelfFocusOnlyDiff fast-path that patches just the focus ring + title
  // strip) call markDirty(rect) on this. presentHomeBuffer() inspects it:
  // non-empty + sub-screen -> displayBufferRegion(bbox); empty or full-
  // screen -> regular displayBuffer (full refresh). Cleared after each
  // present so each new render starts with a fresh dirty slate.
  //
  // Today L1 (displayBufferRegion) forwards to full displayBuffer, so this
  // wiring is plumbing-only -- user sees no behavior change. When L1 v2
  // (SSD1677 windowed-update sequence) lands, the speed-up appears for
  // every render path that opted in here, with no further Home changes.
  DirtyRegion homeDirtyRegion;
  bool hasReadingStats = false;
  bool hasBookmarks = false;
  bool hasOpdsServers = false;
  // CrumBLE: cache the focused shelf book's metadata title/author keyed by path,
  // so we only read the book's metadata when the focused book changes -- not on
  // every render. Empty title means "no metadata; fall back to filename".
  std::string focusedMetaPath;
  std::string focusedMetaTitle;
  std::string focusedMetaAuthor;
  bool minimalMenuOpen = false;
  bool minimalSuppressInitialFrontRelease = false;
  int minimalMenuIndex = 0;
  int minimalHomeNavIndex = -1;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  // CrumBLE 4.5.5+: Flow theme's shelf-only snapshot rect. Used by
  // storeCoverBuffer + restoreCoverBuffer (Flow path) so we only snapshot
  // the shelf strip (~14-24 KB) instead of the full framebuffer (48 KB).
  // The freed heap lets the side-tile prerender cache all 5 carousel
  // books. Set in render() before storeCoverBuffer is called.
  int shelfSnapshotRectX = 0;
  int shelfSnapshotRectY = 0;
  int shelfSnapshotRectW = 0;
  int shelfSnapshotRectH = 0;
  float currentBookProgressPercent = -1.0f;
  BookReadingStats currentBookStats;
  GlobalReadingStats globalStats;

  // Per-book stats and progress cached at onEnter() to avoid SD reads during navigation.
  std::array<BookReadingStats, kMaxCachedBooks> cachedBookStats{};
  std::array<float, kMaxCachedBooks> cachedBookProgress{};
  bool bookStatsCached = false;

  uint8_t* carouselFrames[kCarouselFrameCount] = {};
  bool carouselFramesReady = false;
  bool carouselWarmupPending = false;

  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onContinueReading();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onReadingStatsOpen();
  void onBookmarksOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  bool preRenderCarouselFrames(bool showProgressPopup = false);

 public:
  // v18.9.9.205: set by Activity::exitToHomeWithPopup so the next Home
  // carousel warmup runs silently behind the already-visible "Going
  // home..." popup instead of stacking a "Loading" popup on top of it.
  // Consumed (cleared) by the first warmup after arrival.
  static void noteGoingHomePopupShown();

 private:
  static bool sArrivedWithGoingHomePopup;
  // v18.9.9.208: instance-scoped companion to the static flag. Captured
  // in onEnter, cleared at the first presentHomeBuffer — suppresses the
  // synchronous load popups (cover gen, series scan) that draw DURING the
  // first render pass, before Home has ever painted, which stacked
  // "Loading" on top of the reader's "Going home..." popup.
  bool suppressLoadPopups_ = false;
  void freeCarouselFrames();
  bool allocateCarouselFrameSlots(int targetFrameCount);
  bool buildCarouselCacheFile(const std::string& cacheKey, uint64_t cacheKeyHash, int bookCount,
                              bool showProgressPopup = false);
  bool loadCarouselFrameFromDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx);
  int chooseCarouselEvictionSlot(int centerIdx, int bookCount,
                                 std::optional<int> protectedBookIdx = std::nullopt) const;
  void renderCarouselFrameToCurrentBuffer(int bookIdx, BookReadingStats* outStats, float* outProgressPercent,
                                          bool* outUsedCachedStats);
  void renderCarouselFrame(int bookIdx, int slotIdx);
  // Present the composed Home framebuffer. Does one HALF (full) refresh the
  // first time after onEnter (clears transition ghosting), then FAST refreshes.
  void presentHomeBuffer();
  void updateSlidingWindowCache(int centerIdx, int bookCount);
  int getHighlightedBookIndex() const;
  void updateHighlightedBookContext();
  void loadRecentBooks(int maxBooks);
  void loadAllBookStats();
  void loadRecentCovers(int coverHeight);
  // CrumBLE Collections — generate BMP thumbnails at the bookshelf's exact
  // cell dimensions for the books that are currently visible on the
  // shelf. Lazy by design: an active collection like "All Books" can
  // have hundreds of entries and eager generation would freeze the UI
  // for minutes. Only the [scrollOffset, scrollOffset+visibleCount)
  // window pays the cost; the next batch generates when the user
  // scrolls into uncovered territory.
  void loadShelfCovers(int cellWidth, int cellHeight, int scrollOffset, int visibleCount);
  // CrumBLE Series — runs the OPF series-only parse for every EPUB in
  // the active collection that hasn't been checked yet (SeriesIndex
  // doesn't know about it). Shows a loading popup since this can take
  // ~50-200 ms per book. Skipped entirely when the active collection
  // has collapseSeries = false. Records empty entries for books that
  // turn out not to be in a series, so we don't re-parse them.
  void enrichActiveCollectionForSeries();
  // Returns the active collection's resolved book-path list, lazily
  // recomputing only when the active id changes since last access (or
  // when invalidateShelfPathsCache() was called). Critical to home
  // smoothness when "All Books" is active — without this cache, every
  // frame paid 5x resolveBookPaths copies. The returned vector is
  // derived from cachedShelfEntries() — one firstPath per entry.
  const std::vector<std::string>& cachedShelfPaths();
  // Same caching contract as above but returns the rich ShelfEntries
  // (series-collapsed). Navigation indexes these 1:1 with shelf cells.
  // For non-series cells, entries[i].memberPaths == {entries[i].firstPath}.
  const std::vector<ShelfEntry>& cachedShelfEntries();
  // Clear the cache so the next cachedShelfPaths() call re-resolves.
  // Call after any operation that may have changed the active
  // collection's contents (picker toggle, file delete, library rescan).
  void invalidateShelfPathsCache() { shelfPathsCacheKey.clear(); }

  // Long-press Confirm helpers (used by both the carousel and the Flow
  // bookshelf). getFocusedBookPath returns the file path of the book under
  // the cursor when the user is on either a carousel slot or a shelf slot.
  // Empty string => not on a book (e.g. cursor is on the menu icon bar).
  // CrumBLE #124: non-const because the impl now reuses cachedShelfPaths()
  // (which lazily populates the shelf-entries cache). Without that, every
  // loop tick was recomputing the full collection's path list via
  // resolveBookPaths -- O(N log N) over LibraryIndex for a 50-book
  // virtual collection, perceived as L/R/U/D lag on large libraries.
  std::string getFocusedBookPath();
  // Opens the FileBrowserActionActivity picker for `bookPath` with a menu
  // tailored to the home screen (delete, delete cache, mark finished /
  // unfinished, add/remove favorites, remove from recent books). Result
  // handler performs the action, refreshes recents, and requests a redraw.
  void showHomeBookActionMenu(const std::string& bookPath);
  // Action menu invoked by long-pressing the shelf header (collection
  // name tab). Currently exposes a single "Rescan library" action that
  // re-walks the SD card and refreshes the LibraryIndex. Carved off as a
  // separate menu (vs. the per-book one) so the items always match the
  // collection-level context.
  // focusOn: when set to a valid FileBrowserAction, opens the menu with
  // the initial selection on that row (falls back to index 0 if the
  // action is not present in the built item list). Used to keep the
  // user on the row they just toggled instead of resetting focus to
  // the first item every time the menu re-opens after Show/Hide.
  void showShelfHeaderActionMenu(FileBrowserAction focusOn = FileBrowserAction::None);
  // CrumBLE #81: long-press handler for the icon-bar's Bookshelf entry.
  // Opens a ChoicePromptActivity listing the visible collections; on
  // confirm, sets the picked collection as active and opens the
  // Bookshelf grid over it.
  void showBookshelfCollectionPicker();
  // CrumBLE series — Confirm on a shelf cell. For single-book cells
  // this is just onSelectBook(firstPath). For series cells, opens the
  // most-recently-read member if any is in RECENT_BOOKS; otherwise
  // opens the SeriesMiniPicker.
  void openShelfEntry(const ShelfEntry& entry);
  // v18.9.9.230: launch AddBooksToCollectionActivity for the current
  // active collection. Extracted from the inline call site in the
  // long-press header menu so the empty-collection placeholder cell's
  // Confirm handler can route to the same flow.
  void launchAddBooksToActiveCollection();
  // Sentinel firstPath used for the synthetic "empty user collection"
  // ShelfEntry that cachedShelfEntries() injects when the active
  // non-virtual collection has no members. The strip renders it as a
  // placeholder cell whose title is STR_EMPTY_COLLECTION_ADD; Confirm
  // routes to launchAddBooksToActiveCollection().
  static constexpr const char* kEmptyCollectionCtaPath = "__EMPTY_CTA__";
  // Always opens the SeriesMiniPicker for a series cell (used by the
  // long-press path).
  void openSeriesMiniPicker(const ShelfEntry& entry);
  // CrumBLE: refresh focusedMeta{Title,Author} from the book's metadata cache
  // for `path` (cheap: reads the cached metadata, no full EPUB parse). Title is
  // left empty when no metadata is available so the caller falls back to the
  // filename. No-op when `path` already matches the cached one.
  void updateFocusedBookMeta(const std::string& path);

  // v18.9.9.267: one-time first-boot suggestion for the sleep-image
  // bake. Walks /.sleep/ (or /sleep/) counting *.png / *.bmp files that
  // don't have a paired .slp cache companion, and if that count > 0
  // pushes a ChoicePromptActivity ("Bake now / Later / Never ask
  // again"). "Never" writes a tiny sidecar file so the check skips on
  // future boots. Called once per HomeActivity session, gated on a
  // static flag so navigating in/out of Home doesn't re-prompt. Cheap:
  // ~10 ms filesystem walk with typical <30 sleep files.
  void maybeShowSleepBakePrompt();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  std::string getCurrentBookPath() const override;
};
