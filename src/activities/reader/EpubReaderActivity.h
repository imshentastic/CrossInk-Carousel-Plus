#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
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

// v18.9.9.58: which render path this book open is on. Hoisted to file scope
// so the anonymous-namespace sidecar helpers in EpubReaderActivity.cpp (and
// ReaderOptionsActivity.cpp's compat toggle) can reference it without
// crossing a class private boundary.
enum class ReaderPath : uint8_t { PreparedLayout = 0, CustomSettings = 1 };

class EpubReaderActivity final : public Activity {
 public:
  using ReaderPath = ::ReaderPath;
 private:
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  // 4.5.5: tracks whether the LAST rendered page had any saved-highlight
  // underlines drawn into the framebuffer. e-ink FAST_REFRESH doesn't fully
  // erase dark pixels that become light -- a solid horizontal bar (the
  // underline) leaves a faint ghost on the next page at the same Y. When
  // this flag is set going into the next render, we promote that render to
  // HALF_REFRESH which uses a different waveform that DOES cleanly erase
  // the ghost. Cost is a slightly slower page turn on the page AFTER a
  // highlighted one, but the visual artifact goes away.
  bool prevPageHadHighlights = false;
  // (Removed: ghostClearOnNextRender_ field. Highlight rendering switched
  // from solid underline to faux-bold overprint, which doesn't leave a
  // continuous-line ghost, so the post-highlight FULL_REFRESH path is
  // no longer needed. See renderSavedHighlightsOverlay.)
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  // v18.9.9.454: defer status-bar title on the FIRST render after a section
  // build completes. Title glyphs on non-Latin books (Chinese title, Bitter
  // primary + LXGW SD fallback) trigger a per-glyph SD read that delays the
  // first-page paint by 100-500 ms. Skipping the title on the first render
  // pushes those reads off the critical path — user sees the page appear,
  // then the title fills in on the next page turn. Reset false on each new
  // section build; set true after one post-build render has completed.
  // mutable so renderStatusBar() const can flip it — the flag is
  // presentation-cache state, not observable book state.
  mutable bool postBuildFirstRenderShown_ = false;

  // v18.9.9.455: session flag — user has actively declined prebake for this
  // book (either from the prompt this session, or a persisted decline
  // sidecar from a prior session with matching settings). When true, the
  // section loader skips sections-prebake/N.bin fallback entirely rather
  // than opening + fingerprint-checking + rejecting each. Field cost of
  // NOT having this: 30-60 ms of wasted SD I/O per section on declined
  // books. Set at book-open (from sidecar) and on decline (from prompt).
  bool prebakeDeclinedForThisBook_ = false;
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
  // v18.9.9.471: page-turn count since the last progress.bin save. Debounces
  // the per-render save site so we hit the SD 3× less often — reduces FAT
  // stress that manifested as the "Could not save progress" popup. Reset
  // whenever a save succeeds (render, onExit, onBeforeDeepSleep, chapter
  // jump, or KOR sync). onExit + onBeforeDeepSleep always save, so a
  // debounced page never loses more than 2 turns even on hard power-off.
  int pagesSinceProgressSave_ = 0;
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
  // v18.9.6: second-tier fallback. When a chapter still aborts after the
  // BLE-drop retry (or BLE wasn't involved), retry ONCE more with all
  // memory-frugal guards forced on: images suppressed, embedded style off,
  // bionic+guide reading off, tables collapsed to paragraphs. If that
  // succeeds, simpleRenderingActive_ stays true for the rest of the book
  // so subsequent chapters skip straight to the simple path (no crash-
  // then-retry). Reset on book close (in onEnter). Phase 2 will persist
  // this to a sidecar so a re-open skips the first-chapter crash too.
  bool layoutSimpleRetryAttempted = false;
  bool simpleRenderingActive_ = false;
  // v18.9.9.58: which render path this book open is on.
  //   0 = PreparedLayout: prebake manifest present AND user's SETTINGS match
  //       the prebake fingerprint (or the user just answered "Restore prepared
  //       layout" at the mismatch prompt). Sections load from
  //       sections-prebake/ when the live fingerprint drifts; images blit
  //       from .pxc cache under BT.
  //   1 = CustomSettings: no prebake manifest, or the user answered "Keep my
  //       current settings" at the mismatch prompt. Sections build cold on
  //       demand; images use the JPEG-decoder path (heap-hungry, subject to
  //       compat escalation under BT).
  // Compat is now scoped per-path via compat_prepared.flag / compat_custom.flag
  // sidecars. A book whose PreparedLayout works fine can still legitimately
  // need compat under CustomSettings (or vice versa). Enum declared at file
  // scope above the class body so private-namespace helpers can reference it.
  ReaderPath readerActivePath_ = ReaderPath::CustomSettings;
  // v18.9.9.6 Level 2: parallel to simpleRenderingActive_ but narrower --
  // only tables get collapsed to paragraphs, images/embedded style/bionic/
  // guide stay on. Seeded at book open from tables_suppressed.flag sidecar,
  // set true by the Level 2 escalation branch when Level 1 defrag didn't
  // fit but full Simple Rendering feels heavy-handed for a book whose
  // only failing page-load culprit is oversized PageTableFragments.
  bool tableSuppressionActive_ = false;
  // v18.9.9.9: MaxAlloc captured immediately after renderContents returns,
  // BEFORE the cache-drop path frees ~10 KB of page DOM. Used by the
  // post-render heap-floor check to decide escalation against the ACTUAL
  // drained heap rather than the post-cache-drop misleadingly-high value.
  // Reset to 0 after each check consumes it.
  uint32_t postRenderDrainedMaxAlloc_ = 0;
  // v18.9.9.9 Level 4: one-shot. Set true when the post-render floor check
  // determines this book can't fit even Simple Rendering under BT and drops
  // BT to reclaim NimBLE's ~58 KB share. Prevents a Level 4 loop if the
  // book still fails post-BT-drop for some other reason.
  bool layoutDroppedBtForBook_ = false;
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
  // v18.9.9.145: field REMOVED. The pre-BT reserve strategy starved
  // NimBLE (NimBLE needs the full ~73 KB budget for enable + connect;
  // any reserve took heap from NimBLE's connect step and caused link-up
  // timeout). Kept as a dead unique_ptr for ABI stability during the
  // build; the alloc/release code paths in EpubReaderActivity.cpp are
  // no-ops now.
  std::unique_ptr<uint8_t[]> btConnectedReadReserve_;
  static constexpr size_t kBtConnectedReadReserveSize = 0;
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

  // v18.9.9.298: total visible-text character count for the open book,
  // read from META-INF/crumble-stats.json (written by the optimizer).
  // 0 = manifest absent -> Stable Page Numbers falls back to the
  // inflated-byte-size approximation from getBookSize(). Populated once
  // at book open, held until close.
  uint32_t bookVisibleCharCount_ = 0;

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
  // v18.9.9.41 (task #26): atomic so the "already showing?" check-and-set
  // in checkAndFirePrebakePromptIfNeeded races safely between the loop()
  // and render() call sites (main task vs render task). Prior plain-bool
  // version could see both callers pass the initial guard, both log a
  // "Prebake fingerprint mismatch", and both call startActivityForResult,
  // which triggered "pendingActivity while pushActivity is not expected"
  // in the activity manager.
  std::atomic<bool> prebakePromptShowing_{false};

  // v18.9.9.311: fontId peeked from sections-prebake/0.bin at fingerprint-
  // check time. Kept across the prompt callback so the accept path can
  // trigger the auto-match-installed-font rescue when the manifest's
  // restored settings still don't produce a matching fontId. Zero means
  // "not peeked yet / unknown" (0 is also the "not found" sentinel of
  // SdCardFontManager::computeFontId so it's safely non-matching).
  int32_t sectionFontIdFromPeek_ = 0;

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
  // v18.9.9.5: session-scoped Level 1 defrag budget. False on fresh open;
  // set to true when we fire a silent-restart-with-EnableBt in response to
  // a page-load-refuse. Seeded true at book open when isDefragRetryContinuation()
  // returns true (this boot IS the Level 1 continuation), so a second
  // failure escalates content instead of hopping through another restart.
  bool layoutDefragRetryAttempted_ = false;
  // v18.9.9.168: per-spine chapter-boundary defrag tracker. -1 means no
  // chapter-boundary defrag has fired yet. Set to the spine we last
  // silent-restarted for; a cache-miss on a DIFFERENT spine is allowed
  // another defrag attempt. Prevents infinite loops on a genuinely
  // unbuildable spine while still letting the user traverse N chapters
  // per session (the session-wide layoutDefragRetryAttempted_ was too
  // aggressive -- v163's revert to v48 skip-deinit means requestDisableLater
  // fallback can't free NimBLE's ~58 KB, so subsequent boundaries crashed
  // at framebuffer realloc).
  int16_t layoutDefragRetryChapterSpine_ = -1;
  // v18.9.9.174: latched by renderContents to whatever page.hasImages()
  // returned. Read in onExit to signal HomeActivity that the transition
  // needs pendingFullRefresh so the cover doesn't ghost through the shelf.
  bool lastRenderedPageHadImages_ = false;
  // v18.9.9.36 (v20 Phase C2): incremental Section build driven from
  // loop() rather than blocking inside render(). Kickoff still happens
  // in render() when loadSectionFile misses, but the buildSomeMore
  // loop, popup animation, and success/failure dispatch move to loop()
  // so the render lock isn't held for the multi-second parse -- input
  // polling, sleep timer, BT drain all tick during indexing. User can
  // hit Back / prev / next mid-index to cancel (section.reset() on the
  // navigation path fires abandonBuild via the destructor). Cleared on
  // book open, on build complete, on build failure resume, on cancel.
  bool sectionBuildInProgress_ = false;
  int sectionBuildSpine_ = -1;
  int sectionBuildPopupMinWidth_ = 0;
  unsigned long sectionBuildPopupLastMs_ = 0;
  uint8_t sectionBuildPopupDotPhase_ = 0;
  // Set true by loop() when buildSomeMore returns false; render() sees
  // the flag on the next tick and dispatches the failure branches
  // (defrag restart, BLE-drop retry, Simple Rendering escalation) using
  // the snapshotted outcome flags. Consumed on entry to that block.
  bool sectionBuildJustFailed_ = false;
  bool sectionBuildLayoutAbortedForLowMemory_ = false;
  bool sectionBuildImagesWereSuppressed_ = false;
  bool sectionBuildBleWasDroppedForFail_ = false;
  // v18.9.9.2: post-BT-link MEM instrumentation window. Set to the target
  // millis() when btWasLinked_ transitions to true. While millis() < this
  // value, logPostBtStep() fires MEM snapshots at named steps of loop()
  // and render() so we can pinpoint the throwing alloc between BT connect
  // and the terminate that follows ~86ms later.
  unsigned long postBtDiagUntilMs_ = 0UL;
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
  // v18.9.9.249: parallel to pendingLookupDefinitionWord_ but carries the
  // byte offset within the target entry we want the definition to open
  // on. Non-zero only when the post-boot dispatch consumed a value from
  // silentRestartToReaderWithDefinitionAtChunk (chunk-transition refuse
  // path). Forwarded to the word-select activity via
  // setPendingDefinitionChunk; 0 means start-of-entry as usual.
  uint32_t pendingLookupDefinitionChunkStart_ = 0;
  // CrumBLE 4.5.6: CrossPoint-style pause/resume BT recovery for atlas +
  // page load failures. When atlas install or page load refuses under
  // post-BT heap pressure, requestDisableLater to free NimBLE's ~15 KB,
  // retry on next render tick, then requestEnableLater so bonded remote
  // auto-reconnects. Deferred drains run in main loop (safe from render
  // lock). Cleared on success OR on second failure with BT already down.
  bool atlasRetryPendingBtDrop_ = false;
  bool pageLoadRetryPendingBtDrop_ = false;
  bool pendingBleQuickConnect_ = false;
  bool pendingBleQuickConnectNoImages_ = false;
  // CrumBLE 4.5.7 v18.1: true when this QC came from a silent-restart's
  // ReaderPostBootAction::EnableBt dispatch (defrag-then-enable path). The
  // pre-flight uses this to skip its heap check ONLY for that specific case,
  // not for QCs that happen to fire after ANY silent restart (e.g. a cover-
  // heap-guard restart-to-home whose reader entry then hits the BT-connect
  // menu). Without this distinction, the pre-flight was skipping heap checks
  // it should have run, letting BT enable proceed at ~24 KB maxAlloc and
  // crashing after connect.
  bool pendingBleQuickConnectFromBootDispatch_ = false;
  // v18.9.9.164: true after drawer-close painted the "Connecting Bluetooth..."
  // popup (:4287). Loop Step 3 checks this to downgrade its own paint from
  // HALF_REFRESH to NO_REFRESH, avoiding the visible double-flicker.
  bool bleConnectingPopupPainted_ = false;
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
  // v18.9.9.202: writes <cache>/chapter_title.txt (current ToC entry) so
  // Home's Dashboard theme can show the chapter without loading the EPUB.
  void writeChapterTitleSidecar();

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

  // CrumBLE 4.5.5: persistent highlight rendering. Walks the current page's
  // PageLine/TextBlock elements counting word index, and draws a dotted
  // underline for words within any saved highlight's [startWord, endWord]
  // range. Same walk pattern as DictionaryWordSelectActivity::extractWords
  // (1:1 with TextBlock::getWords() entries; em-dashes that split a word
  // into multiple WordInfo entries cause a worst-case one-word visual
  // miscount in rare cases -- acceptable tradeoff vs the cost of mirroring
  // the full split logic at every render). Cheap: no allocations, just
  // pixel writes for each highlighted word. Heap-gated: skips at maxAlloc
  // < 4 KB so it never contributes to the reader's heap pressure floor.
  // 4.5.5: const removed -- the overlay now sets prevPageHadHighlights so
  // the next render can promote to HALF_REFRESH and clear the e-ink ghost.
  void renderSavedHighlightsOverlay(const Page& page, int marginLeft, int marginTop);
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
  // v18.9.9.418: narrow the gate. Snapshot is used as the background behind
  // transparent sleep PNGs; modes that fill the screen with their own content
  // (DARK/LIGHT/BLANK/READING_STATS/MINIMAL_SLEEP/QUICK_RESUME) never sample
  // it. Skipping the snapshot for those users kills a visible requestUpdate-
  // AndWait re-render (~500 ms of "the book image reloads first, then Home"
  // that they explicitly noticed on reader exit) with no functional change.
  bool canSnapshotForSleepOverlay() const override {
    switch (SETTINGS.sleepScreen) {
      case CrossPointSettings::COVER:
      case CrossPointSettings::COVER_CUSTOM:
      case CrossPointSettings::OVERLAY:
        return true;
      default:
        return false;
    }
  }
  std::string getCurrentBookPath() const override { return epub ? epub->getPath() : std::string{}; }
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
