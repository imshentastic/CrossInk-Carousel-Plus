#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <optional>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "PxcManifest.h"  // shared with BookSettingsDrawerActivity
#include "PrebakeManifest.h"  // section-0 fingerprint for switch-back prompt
#include "activities/Activity.h"
#include "activities/settings/SettingsActivity.h"  // for SettingInfo (drawer cache)

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  unsigned long sessionStartMs = 0UL;
  // Wall-clock anchor for the current "session segment" — reset by
  // commitReadingSession every time it banks elapsed time so we don't
  // double-count across deep-sleep / shutdown commits.
  unsigned long sessionSegmentStartMs = 0UL;
  // Cumulative session ms already banked into stats this opening. Used
  // only to gate sessionCount (the +1 happens once per session ≥ 60s,
  // even if multiple commits add up to >60s).
  unsigned long totalSessionMsThisOpen = 0UL;
  bool sessionCountedThisOpen = false;
  // Wall-clock anchor for the periodic incremental save. Reading
  // sessions that crash before onExit (e.g. brown-out, hard hang) used
  // to lose ALL elapsed time. Now we flush every kIncrementalSaveMs
  // milliseconds during loop() so worst-case loss is bounded.
  unsigned long lastIncrementalSaveMs = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool longPressMenuHandled = false;
  bool longPowerButtonHandled = false;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  int pageLoadRetryCount = 0;
  // CrumBLE: if a chapter layout aborts under heap pressure and BLE is
  // currently consuming its ~58 KB share, retry the layout once with BLE
  // disabled. Flag gates the retry so we don't loop forever if the
  // chapter genuinely can't be parsed.
  bool layoutBleRetryAttempted = false;
  // CrumBLE: when we proactively drop BLE around a heavy re-layout (drawer
  // settings change, or the reactive chapter-abort retry), set this so the
  // next successful section build re-enables BLE via requestEnableLater().
  // Without this, the user is stuck without their remote until they go to
  // Reader Menu -> Bluetooth to re-enable manually — checkAutoReconnect()
  // refuses to do anything while _enabled is false.
  bool bleAutoReEnableAfterReindex = false;
  // CrumBLE: after a low-memory rebuild dropped BLE (bleAutoReEnableAfterReindex
  // path), we must NOT bring BLE back while we're still sitting on a page that
  // needs the JPEG/PNG decoder -- re-enabling there just starves the decode and
  // drops BLE again (a connect/disconnect thrash at every image-heavy chapter
  // boundary). Instead we latch the re-enable here and only fire it from
  // renderContents() once a page renders cleanly AND has no images, so the
  // bonded remote reconnects only after we're past the un-decodable page(s).
  bool bleReEnableHeldForImagePage = false;
  // CrumBLE: a chapter layout aborted under BLE pressure; we requested a BLE
  // disable and must retry the build only AFTER it's actually off. Set here and
  // drained in loop() once !isEnabled(), instead of requestUpdate()'ing inline --
  // the render task would otherwise re-attempt the build before the deferred
  // disable drained, burning the one-shot retry while NimBLE still held ~58 KB.
  bool pendingLayoutRetryAfterBleOff = false;
  // CrumBLE: set once per book open when a page can't render with a BLE remote
  // connected (image decode or glyphs starved by NimBLE's ~58 KB). We drop
  // Bluetooth so the full heap renders the page (images AND text), and show the
  // explanatory alert only once. Image-heavy books are simply unreadable with a
  // remote attached on this chip; the user reads with device buttons.
  bool btDisabledForMemoryThisBook = false;
  // Post-connect grace tracking for the auto-drop above. NimBLE's connect
  // handshake briefly spikes heap pressure; a single render in that window can
  // starve even on books that read fine with BLE. We ignore starvation until
  // kBtConnectGraceMs after the remote came up, so the auto-drop only fires on
  // books that stay unrenderable past the transient.
  unsigned long btEnabledAtMs = 0UL;
  bool btWasEnabled = false;
  // Length of the post-connect grace window during which a starved render is
  // attributed to NimBLE's transient connect-spike rather than a genuinely
  // unrenderable book. Shared by render() (auto-drop gate) and loop() (the #48
  // post-grace re-render).
  static constexpr unsigned long kBtConnectGraceMs = 4000;
  // #48: set when a render was suppressed because it starved inside the connect
  // grace window (half-drawn glyphs). loop() fires exactly one re-render once the
  // grace window expires (or BLE drops) -- never a tight in-grace retry loop,
  // which previously tripped the auto-drop on books that stay connected.
  bool pendingGraceReRender = false;
  // BT No Images Quick Connect: latched true once the bonded remote actually
  // links while image suppression is armed. Lets loop() tell a genuine link drop
  // (controller powered off / out of range -- stack stays enabled, isConnected
  // goes false) apart from the brief pre-link handshake window, so we restore
  // images on the drop but not during the initial connect.
  bool btNoImgLinkSeen = false;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;

  // CrumBLE phase 3/7: held start anchor for cross-page / cross-chapter
  // highlights. Set when the user back-outs of WordSelect in HighlightRange
  // mode with the start anchor placed -- the menu then surfaces
  // FINISH_HIGHLIGHT / CANCEL_HIGHLIGHT until the user resolves. Cleared
  // on save, cancel, or book close (we don't persist; if the user closes
  // the book mid-highlight the partial state is intentionally dropped --
  // word indices wouldn't necessarily survive a re-pagination anyway).
  struct PendingHighlightStart {
    uint16_t spineIndex;
    float progress;        // start page's progress within section
    uint16_t pageCount;    // start page count (for de-dup at save)
    uint16_t wordIndex;    // word position within the start page
    std::string startWordText;     // raw text -- becomes "<start>... <end>" preview
    std::string chapterTitle;      // captured at hold time
  };
  std::optional<PendingHighlightStart> pendingHighlightStart_;

  // CrumBLE: when the user jumps to a page via View Bookmarks, the next
  // short-press Back should land them back in the bookmark list instead
  // of exiting to Home. Set in the VIEW_BOOKMARKS result handler on a
  // successful pick; consumed (and cleared) in the Back handler. Page
  // turns and other navigation don't clear it -- the user can read
  // around the bookmark, then Back to return to the list.
  bool returnToBookmarkListOnBack_ = false;

  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  bool pendingTiltPageTurnFeedback = false;
  bool tiltPageTurnFeedbackEnabled = false;
  unsigned long tiltPageTurnFeedbackShowTime = 0UL;
  // True if the previous render drew a feedback toast. Used to force a clean
  // half-refresh on the toast's dismiss frame only, so the toast box is fully
  // erased instead of ghosting ("failing to disappear") on a fast refresh.
  bool toastShownLastRender = false;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool lastAtOrPastCompletionTrigger = false;

  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // CrumBLE: reader-category settings list cached at book-open, when heap
  // is healthy and BLE typically hasn't connected yet. Lifetime is the
  // reader's. Passed into BookSettingsDrawerActivity by const-pointer so
  // the drawer never has to rebuild getSettingsList() under BLE pressure
  // (which crashed-OOMd it). ~3 KB across reader lifetime. Empty if the
  // build itself was gated out (BLE on AND heap already fragmented at
  // reader entry); the drawer falls back to its own gated build.
  std::vector<SettingInfo> readerSettingsCache_;

  // CrumBLE: parsed manifest from META-INF/crumble-pxc.json, if present in
  // the EPUB. Optional (most books won't have one). Used by the BLE-link
  // edge detector below to decide whether to prompt the user to switch to
  // the prepared layout when they connect a remote.
  std::optional<PxcManifest> pxcManifest_;

  // CrumBLE: parsed prebake manifest -- the 12-field fingerprint baked into
  // section 0's header by the off-device prebake CLI. Optional (only books
  // the user ran through /optimizer with Pre-bake on have this). On book
  // open, compared against current SETTINGS; on mismatch the reader prompts
  // the user to switch back to the prebake'd layout so the cached sections
  // can actually load instead of being rebuilt from HTML on every chapter.
  std::optional<PrebakeManifest> prebakeManifest_;
  // CrumBLE 4.2.1: rate-limit the "why didn't the prompt fire?" diagnostic
  // log to one per book entry so repeated render/loop ticks don't spam.
  // Reset to false in onEnter; the first reason the check declines to
  // prompt logs at INF/DBG and flips this true.
  bool prebakePromptDiagLogged_ = false;
  // Snapshot of the DIRECT SETTINGS values when the user opened this book
  // (or last accepted a settings change that invalidates the prebake cache).
  // We hold the raw SETTINGS values rather than the derived fingerprint
  // values because the prompt's "Cancel" path needs to copy these back into
  // SETTINGS to ACTUALLY undo the user's just-made change -- previous
  // designs left SETTINGS at the new value and just declined to revert to
  // the prebake's value, which wasn't what the user wanted.
  // Initialised on the first tick after book open; cancelling the prompt
  // restores from this snapshot, confirming updates the snapshot to the
  // current SETTINGS (taking the user's choice as the new baseline).
  struct PrebakeSettingsSnapshot {
    uint8_t orientation = 0;
    uint8_t screenMargin = 0;
    uint8_t imageRendering = 0;
    uint8_t fontFamily = 0;
    uint8_t fontSize = 0;
    uint8_t sdFontSizeRange = 0;
    char sdFontFamilyName[64] = "";
    uint8_t lineSpacing = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t extraParagraphSpacing = 0;
    uint8_t forceParagraphIndents = 0;
    uint8_t hyphenationEnabled = 0;
    uint8_t embeddedStyle = 0;
    uint8_t bionicReadingEnabled = 0;
    uint8_t guideReadingEnabled = 0;
    bool initialised = false;
  } prebakeLastSnapshot_;
  bool prebakePromptShowing_ = false;  // suppress re-fire while dialog is open

  // CrumBLE: evaluates the prebake-cache mismatch state and fires the
  // settings-change prompt if needed. Returns true when a prompt has been
  // pushed (caller should bail from the surrounding render/tick to avoid
  // running any chapter-parse / heap-heavy work behind the user's back
  // before they've decided whether to keep their change or revert it).
  // Idempotent across multiple call sites in the same tick (the
  // prebakePromptShowing_ guard short-circuits subsequent invocations).
  bool checkAndFirePrebakePromptIfNeeded();

  // CrumBLE Phase 1 fast-open: non-critical onEnter work (font buffer
  // pre-grow, reader-settings cache build, .pxc manifest parse) is
  // deferred to the first loop() tick AFTER the first render. Net
  // saving: ~30-50 ms off tap-to-first-pixel. The deferred steps are
  // BLE-protective (prevent heap-fragmentation during reading) but BLE
  // can't pair faster than the first-render window, so deferring is
  // safe. firstRenderCompleted_ flips at the bottom of render(); loop()
  // checks both flags and calls runDeferredOnEnter() exactly once.
  bool deferredOnEnterPending_ = false;
  bool firstRenderCompleted_ = false;
  void runDeferredOnEnter();

 public:
  // CrumBLE Phase 1 fast-open: pre-grow the reader's glyph decompression
  // buffer to its high-water mark, then drop the prewarm's page-slot
  // buffers. Must be called before any BT-enable from inside the reader
  // -- without a pre-grown buffer, NimBLE's ~58 KB heap claim fragments
  // the heap and the first text-page glyph group can't allocate the
  // ~6 KB decompression buffer, starving glyphs and dropping the BT
  // link within a page or two. Used to run at book-open unconditionally
  // (~20 ms penalty for every book open); now only fires when the user
  // actually enables BT, inside the "Connecting Bluetooth..." popup
  // window where it's invisible. Static so callers from the drawer,
  // reader menu, and other reader-context activities can all reach it
  // without an instance pointer.
  static void prewarmReaderTextBuffer(GfxRenderer& renderer);
  // CrumBLE 4.2: passive heap recovery for the WordInfo-vector pre-flight
  // checks in LOOKUP / ADD_HIGHLIGHT / FINISH_HIGHLIGHT. Drops the
  // FontCacheManager hot-group buffer + per-style SD-font miniData
  // (heaviest reusable allocations) and the parsed Section DOM (frees
  // ~10-20 KB of EPUB blocks), then yields to FreeRTOS so the heap
  // consolidator gets a tick. The next render rebuilds the section
  // against a compacted heap. Callers still raise the "memory tight,
  // try again" alert -- the recovery is a best-effort cleanup that
  // makes the user's retry far more likely to succeed than the previous
  // "close + reopen the book" friction. Non-static because we touch
  // `section` and `renderer`.
  void tryRecoverLowHeapForLookup();
  // CrumBLE 4.2: pre-load the current page's DOM into cachedRenderPage_
  // immediately after btMgr.enable() returns, before the async
  // connect / HID-subscribe burst fragments the post-NimBLE heap.
  //
  // Timeline at BT-connect: enable() returns sync (~13 ms), then NimBLE
  // host runs the connect handshake (~2.7 s) and subscribes to 6 HID
  // characteristics (~1.5 s). During the connect phase, contiguous free
  // heap sits at ~30 KB -- enough to allocate a 25 KB Page DOM. By the
  // time the subscriptions finish, maxAllocHeap has dropped to ~6 KB
  // and any cache-miss render fails. The "Connecting" popup suppresses
  // renderContents during this whole window, so the natural cache
  // refill we'd otherwise get on next render never fires -- the
  // post-connect re-render hits a cold cache against the worst possible
  // heap. This helper does the page-DOM load explicitly during the
  // friendly window, without drawing, so the post-connect render is a
  // cache hit and needs zero allocation. No-op if the cache is already
  // valid for the current (section, spine, page).
  void warmPageCacheForBtTransition();
  // Edge tracker: set true when a remote actually links (NimBLE handshake
  // completes), false when it drops. Manifest mismatch check fires on the
  // 0 -> 1 transition + stability window. Distinct from btWasEnabled --
  // that one tracks the stack, this one tracks whether a remote is paired
  // up RIGHT NOW.
  bool btWasLinked_ = false;
  // Earliest ms timestamp at which we may fire the manifest mismatch prompt
  // after observing a fresh link. NimBLE's connect handshake briefly toggles
  // the linked state and we'd rather not race that; gating on a few seconds
  // of continuous linked-ness avoids ghost prompts during the connect spike.
  unsigned long btManifestPromptEarliestMs_ = 0UL;
  // Session-scoped: once the user has answered the manifest prompt (either
  // applied the prepared layout or kept their current settings), don't ask
  // again for this book open. The previous "per-link" semantics re-fired the
  // prompt whenever the controller briefly dropped link and re-established,
  // which was confusing -- if the user said "keep mine", they meant it.
  // Cleared on book entry (onEnter) so a fresh book gets a fresh decision.
  bool btManifestPromptAnsweredThisSession_ = false;
  // CrumBLE: drawer BT-Quick-Connect deferred-execution state. When the
  // drawer's QC row is tapped, the drawer just sets MenuResult flags and
  // finishes -- the reader runs the .pxc mismatch prompt FIRST (so the user
  // chooses whether to keep their settings or use the prepared layout
  // BEFORE any re-layout), THEN drains a section rebuild + the actual
  // bt.enable() + connectToDevice(). Doing the connect inline from the
  // drawer's lambda used to race the NimBLE handshake against a
  // heap-heavy section rebuild and brick the link.
  // CrumBLE 4.4 post-bisect: post-silent-restart restore for the inline
  // definition overlay. Set by the OpenDefinition post-boot dispatch from
  // the word carried in silentRebootDefinitionWord; consumed by the LOOKUP
  // case to thread the word into the freshly-built DictionaryWordSelectActivity
  // via setPendingDefinitionWord. The activity then snaps the cursor to
  // that word and auto-opens the popup, putting the user back exactly
  // where they were before the heap-defrag restart.
  std::string pendingLookupDefinitionWord_;
  // CrumBLE 4.4 post-bisect: parallel to pendingLookupDefinitionWord_ but
  // signals the post-boot dispatch came from the dismiss-time silent-restart
  // path (OpenLookupAtWord). The launchWordSelect lambda threads this flag
  // into the activity so it navigates the cursor to the word WITHOUT
  // auto-opening the definition popup -- user resumes on the same word
  // they were just reading, free to dismiss or pick a different word.
  bool pendingLookupCursorOnly_ = false;
  bool pendingBleQuickConnect_ = false;
  bool pendingBleQuickConnectNoImages_ = false;
  // True when the drawer reported settingsChanged alongside the QC request.
  // The reader's result handler defers the section.reset() until the user
  // answers the manifest prompt (if any), so the prompt fires BEFORE the
  // re-layout, not after.
  bool pendingBleQuickConnectSettingsChanged_ = false;
  // Tracks the prompt's lifecycle within a single QC attempt. -1 = not yet
  // shown; 0 = shown, user picked "Use mine" (drop section if needed, then
  // connect); 1 = shown, user picked "Use prepared" (apply manifest, drop
  // section, then connect). On Back, we clear pendingBleQuickConnect_
  // entirely so this never advances.
  int pendingBleQuickConnectPromptStage_ = -1;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Banks elapsed time from `sessionSegmentStartMs` into stats.bin +
  // GlobalReadingStats and resets the anchor so subsequent calls don't
  // double-count. Idempotent: a 0-ms segment is a no-op. Called from
  // onExit, onBeforeDeepSleep, and the incremental save tick.
  void commitReadingSession();

  // CrumBLE 4.2: page DOM cache. The render path used to drop and
  // re-allocate the Page (~25-40 KB of vector<string> for words +
  // PageLine elements) every render tick because
  // Section::loadPageFromSectionFile returns a fresh unique_ptr<Page>
  // and renderContents consumed it. Under BT-connect heap pressure
  // (NimBLE takes 58 KB) the next render's TextBlock::deserialize would
  // bad_alloc inside vector<string>::resize and terminate the device.
  //
  // Caching the page here lets BT connect against an already-allocated
  // page DOM: the render before the connect populates the cache, BT
  // eats its 58 KB, the post-connect re-render reuses the cached page
  // (no allocation) and only the FontCacheManager prewarm has to fit
  // into whatever heap is left. Invalidation is driven by pointer
  // identity comparison against `section.get()` plus the (spine, page)
  // index pair: page turns, chapter advances, settings-driven section
  // rebuilds, and Section::reset all cause natural cache misses without
  // explicit invalidate() calls scattered across the file.
  std::unique_ptr<Page> cachedRenderPage_;
  void* cachedRenderSection_ = nullptr;  // raw Section* identity for cache validity
  int cachedRenderSpine_ = -1;
  int cachedRenderPageIndex_ = -1;

  void renderContents(const Page& page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void reindexCurrentSection();
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void executeLongPressMenuAction();
  void pageTurn(bool isForwardTurn);
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);
  void showTiltPageTurnFeedback(bool enabled);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  // Banks the current reading session into stats before the device
  // powers off. Without this, time read since the last commit was
  // lost — onExit only fires on explicit activity transitions, and
  // hardware deep-sleep skips that path. Idempotent with onExit.
  void onBeforeDeepSleep() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  std::string getCurrentBookPath() const override { return epub ? epub->getPath() : std::string{}; }
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
