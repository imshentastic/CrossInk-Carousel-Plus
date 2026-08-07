#pragma once

#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();              // home screen
// v18.9.5.8: same as silentRestart() but does NOT overlay a "Loading" popup
// on the framebuffer. Use when the framebuffer already carries an
// appropriate transition popup (e.g. reader's "Going Home") -- overlaying
// two popups produces a visible pile-up after the boot restore.
void silentRestartPreservingFrame();
void silentRestartToReader();      // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToFileTransfer();// goes straight back to File Transfer activity

// 4.5.5+: cover-gen heap-recovery one-shot. HomeActivity::loadShelfCovers
// calls hasAttemptedCoverHeapRestart() to check if this boot already burned
// the recovery; if false AND heap is too fragmented to gen the missing
// thumbs, marks the flag + silentRestart()s. Boot lands back on Home with
// ~85 KB free heap, covers gen cleanly, no boot loop possible (max 1
// restart per boot regardless of how many covers are missing).
bool hasAttemptedCoverHeapRestart();
void markCoverHeapRestartAttempted();

// v18.9.9.260: sleep-image bake defrag restart. Settings action calls
// this to reboot into Home; setup() runs SleepActivity::bakeAllSleepImages
// on the fresh ~93 KB heap (well above PNGdec's ~60 KB floor) before
// activity dispatch, then continues to Home. Naturally clears on cold
// boot so a crash mid-bake doesn't loop.
void silentRestartToBakeSleepImages();
bool hasPendingBakeSleepImages();
void clearPendingBakeSleepImages();

// v4.7.5: cross-restart validity marker for the library's SD walk. The walk
// (LibraryIndex::ensureWalked -> rescan) costs ~1.5 s on a 43-book card and is
// gated by a plain RAM bool, so every silent restart re-walked a card that had
// not changed since the walk moments earlier.
//
// Only LibraryIndex may call these: it arms on a completed rescan() and clears
// on markStale() / forgetPath() / releaseMemory(). setup() honours the marker
// only for a clean silent restart with a successfully loaded on-disk index --
// cold boot, power-cycle and crash restarts always re-walk. Anything that can
// add or remove books behind the index's back (the HTTP server's upload,
// delete, rename and WebDAV endpoints) suppresses arming for the rest of the
// boot via LibraryIndex::suppressCrossBootWalkReuse().
void markLibraryWalkValidForNextBoot();
void invalidateLibraryWalkForNextBoot();
bool isLibraryWalkStillValidFromLastBoot();

// CrumBLE 4.5.5+: when loadShelfCovers triggers a silentRestart while the
// user is on the shelf header (typical: they just pressed L/R to cycle
// the collection, missing thumbs in the new collection's window kicked the
// heap-guard), mark this flag so the post-restart HomeActivity::onEnter
// lands focus on the shelf header instead of resetting to the carousel.
// One-shot: consume() returns the value and clears it. Naturally zero on
// any cold boot (RTC NOINIT is uninitialized on power-up).
void markPendingHomeFocusOnShelfHeader();
bool consumePendingHomeFocusOnShelfHeader();

// Deferred file/dir delete via silent restart. FileBrowserActivity sets the
// pending path when the user confirms Delete but heap is too fragmented to
// safely run BookActions::clearFileMetadata + Storage.remove (typical after
// closing a heap-heavy book like a broken CJK title). Boot's setup() picks
// the path up after Storage.begin() / index loads, runs the delete with a
// fresh ~85 KB heap, then proceeds to Home with the file gone. User sees a
// brief "Loading..." popup and lands at Home -- no FT detour, no looking-
// like-a-hijack redirect. One-shot: cleared after execution (or on any cold
// boot, since RTC NOINIT is uninitialized on power-up).
bool hasPendingDelete();
void setPendingDelete(const char* path, bool isDir);
// Same RTC NOINIT slot, different action code -- the boot-time executor
// dispatches to Epub::clearCache() instead of Storage.remove/removeDir.
// Used by FileBrowserActivity's "Delete book cache" handler when on-device
// heap is too low to safely walk the prebake-cache subdir inline.
void setPendingClearBookCache(const char* path);
void clearPendingDelete();
const char* getPendingDeletePath();
bool getPendingDeleteIsDir();
// Returns the raw action code (PendingDeleteAction enum value in main.cpp:
// 0 = file delete, 1 = dir delete, 2 = clear book cache). Boot dispatch
// uses this to route to the right BookActions helper.
uint8_t getPendingDeleteAction();

// CrumBLE: optional mode hint persisted across silentRestartToFileTransfer
// so the FT activity skips the mode-picker on auto-recover.
// Valid values: 0 = no hint, 1 = JOIN_NETWORK, 2 = CREATE_HOTSPOT.
void setSilentRebootFtModeHint(uint32_t mode);
uint32_t consumeSilentRebootFtModeHint();

// CrumBLE 4.5.4: panic-recovery flag for FT WS uploads. Called by the WS
// upload handler on START accept (true) and on DONE / abort / disconnect /
// FT exit (false). If a panic-reboot happens while true, setup() detects
// it on the next boot and silent-restart-to-FT so the browser's WS retry
// + RESUME protocol can naturally continue the interrupted upload. The
// false-call also resets the consecutive-fail counter, so a clean run
// restores full auto-resume budget for the next session.
void setFtUploadInProgress(bool active);

// v18.9.9.394: getter matching setFtUploadInProgress. The FT idle-restart
// watchdog reads this to skip the periodic freshener while a WS upload
// is running (a mid-upload restart would drop the connection unnecessarily,
// even though the browser could technically auto-resume).
bool isFtUploadInProgress();

// CrumBLE 4.4 task #48: quick-restart on natural pauses. The pre-boot
// action runs once the activity stack lands back on the reader, giving
// the operation a fresh post-defrag heap to work with.
//
//   ReaderPostBootAction::EnableBt
//     -- ESP.restart, route to reader, then trigger BT enable via the
//        reader's Quick Connect path (which drops settings cache +
//        page-heap reserve before enabling NimBLE). Replaces the
//        "Refusing to enable Bluetooth: free heap below threshold"
//        dead-end on the second-and-later BT enable per boot.
//   ReaderPostBootAction::OpenLookup
//     -- ESP.restart, route to reader, then push the Lookup activity.
//        Replaces the "low memory alert" Lookup currently produces
//        when MaxAlloc is under ~32 KB.
//   ReaderPostBootAction::OpenHighlight (task #62)
//     -- ESP.restart, route to reader, then re-enter the AddHighlight
//        menu action. Replaces the "low memory" alert Highlight
//        previously produced when MaxAlloc dipped below ~32 KB --
//        Highlight reuses Lookup's WordInfo allocation path so the
//        same heap budget applies; same recovery pattern fits.
//   ReaderPostBootAction::ResumeAtSpine (task #63)
//     -- ESP.restart, route to reader, then resume at a target spine
//        (carried in a separate RTC var since the action enum has no
//        payload). Used by the chapter-transition pre-flight when
//        MaxAlloc is too low to safely run section createSectionFile /
//        loadSectionFile (peaks ~13-20 KB). Post-boot the section
//        rebuild happens on a fresh ~115 KB heap; first paint uses HALF
//        refresh since the panel pre-restart held the previous chapter's
//        page and we want a clean transition to the new chapter.
enum class ReaderPostBootAction : uint8_t {
  None = 0,
  EnableBt = 1,
  OpenLookup = 2,
  OpenHighlight = 3,
  ResumeAtSpine = 4,
  // CrumBLE 4.4 post-bisect: jump straight to the definition of the word
  // the user just tapped. Word is carried in silentRebootDefinitionWord
  // (RTC slot); consume via consumePendingDefinitionWord().
  OpenDefinition = 5,
  // CrumBLE 4.4 post-bisect: route post-boot dispatch into Reading Stats.
  // Used when the user selects Reading Stats while BT is connected and
  // heap is too fragmented to safely run NimBLE teardown -- silent-restart
  // first, then open Stats against a fresh ~115 KB heap (BT cold).
  OpenReadingStats = 6,
  // CrumBLE 4.4 post-bisect: like OpenDefinition (word in silentRebootDefinitionWord),
  // but post-boot does NOT auto-open the definition popup -- just navigates
  // the cursor to the word and stops. Used by the dismiss-time silent-restart
  // path so the user resumes on the same word they just looked up without
  // the popup reappearing.
  OpenLookupAtWord = 7,
  // CrumBLE 4.4: KOReader Sync TLS handshake needs ~55 KB contiguous heap,
  // and mid-reading heap can be way under that (typical: 16-19 KB after
  // section + BT). When the user opens Sync from the menu under tight heap,
  // silent-restart first, then re-trigger the SYNC menu action on a fresh
  // ~115 KB heap (BT cold) -- mirrors the OpenReadingStats pre-flight.
  OpenKoSync = 8,
  // v18.9.9.25: reopen the Book Settings drawer with a specific group already
  // expanded. Group id lives in silentRebootDrawerExpandGroup (RTC slot).
  // Used when BookSettingsDrawerActivity::ensureSettingsSrcBuilt refuses on
  // tight heap; a silent-restart lands us on a fresh ~90 KB heap and the
  // drawer's next expand succeeds naturally -- boot reopens it pre-expanded
  // so the user doesn't have to tap twice.
  OpenBookSettingsDrawer = 9,
  // v18.9.9.49: reopen Reader Options after a silent-restart. Used when
  // the user taps Reader Options while BT is connected -- v18.9.9.48
  // made bt.disable() skip NimBLE deinit, so the ~58 KB stays held and
  // ROA's settings-list build refuses on tight heap. Silent-restart
  // gives ROA a fresh ~90 KB heap with NimBLE cold, and post-boot
  // dispatch replays MenuAction::READER_OPTIONS.
  OpenReaderOptions = 10,
  // v18.9.9.270: reopen the Looked Up Words screen after a silent-
  // restart. Fires when LOOKED_UP_WORDS pre-flight sees maxAlloc below
  // the ~32 KB floor -- previously showed a dead-end "low memory"
  // alert. Post-boot dispatch fires the LOOKED_UP_WORDS menu action
  // on the fresh ~90 KB heap so the user lands exactly where they were
  // trying to go, not on Lookup (a superset activity but not what they
  // asked for).
  OpenLookedUpWords = 11,
};

// Restart + queue OpenDefinition with a word string. Allocation-free.
void silentRestartToReaderWithDefinition(const char* word);

// v18.9.9.249: OpenDefinition + a byte offset within the target entry.
// Used by the chunked reader: when Up/Down at a chunk boundary can't
// safely wrap the next chunk on the current heap, we arm this variant
// so the post-boot definition opens directly on the target chunk
// rather than back at chunk 0. `chunkStart` is a raw byte offset within
// the .dict entry (clamped to entry.totalSize on the read side).
// Passing 0 is equivalent to silentRestartToReaderWithDefinition.
void silentRestartToReaderWithDefinitionAtChunk(const char* word, uint32_t chunkStart);

// v18.9.9.249: read-and-clear the queued chunk-start offset. Returns 0
// if no chunk was queued (i.e. post-boot should open the definition at
// chunk 0 as usual); otherwise the byte offset to start on. Consumer
// is EpubReaderActivity's post-boot OpenDefinition dispatch.
uint32_t consumePendingDefinitionChunkStart();

// Restart + queue OpenLookupAtWord with a word string. Allocation-free. Same
// word-carrying path as silentRestartToReaderWithDefinition, but post-boot
// skips auto-opening the popup -- only moves cursor to the word.
void silentRestartToReaderWithCursorWord(const char* word);

// Read-and-clear the queued word string. Returns nullptr if no
// definition is queued (or the queued word was empty).
const char* consumePendingDefinitionWord();

// Variant for ResumeAtSpine: caller specifies the target spine the post-boot
// resume should land on. The reader resumes that spine, page 0. Path-scoped
// via APP_STATE.openEpubPath -- no separate book hash needed because if the
// user opens a different book post-restart the override is just ignored
// (currentSpineIndex stays at whatever progress.bin had).
void silentRestartToReaderResumingAtSpine(int targetSpine);

// v18.9.9.25: silent-restart + queue OpenBookSettingsDrawer with the target
// group id. Allocation-free -- called from BookSettingsDrawerActivity's tight-
// heap refuse path. Boot 2's reader consumes the action and reopens the
// drawer with `groupId` pre-expanded so the user sees the settings they were
// trying to reach without having to tap Expand a second time.
void silentRestartToReaderOpeningDrawerAt(uint8_t groupId);

// Read-and-clear the queued drawer group id. Returns -1 if no group was
// queued. EpubReaderActivity calls this when it dispatches
// ReaderPostBootAction::OpenBookSettingsDrawer.
int consumePendingDrawerExpandGroup();

// Read-and-clear the queued resume spine. EpubReaderActivity calls this in
// onEnter after the progress.bin load; if a value was queued it overrides
// currentSpineIndex and forces nextPageNumber = 0. Returns -1 if no resume
// was queued.
int consumePendingResumeSpine();

// v18.9.9.58: stash the reader's active render path so the next silent-restart
// can pick it up. Reader calls this once its readerActivePath_ is decided --
// on cold-boot the RTC slot is uninitialized garbage; only trust the value if
// the paired magic matches, which is what stash/consume guarantees.
void stashReaderActivePathForNextBoot(uint8_t path);
// Read-and-clear the stashed reader active path. Returns std::nullopt (via
// out-param sentinel) if the magic doesn't match (cold boot or never
// stashed). Consumer is EpubReaderActivity::onEnter -- if a value comes
// back, use it; otherwise re-determine from prebake+SETTINGS match.
bool consumePendingReaderActivePath(uint8_t& out);

// v18.9.9.59: arm the "Compatibility Mode required" toast for the next boot.
// Called only from the Layer 2 write-sidecar site when the user had just
// manually disabled compat this session.
void armCompatReenabledToast();
// Read-and-clear the compat-toast flag. Returns true iff the flag was armed
// pre-restart. Consumer is EpubReaderActivity::onEnter -- when true, it
// briefly draws a popup explaining why compat came back on.
bool consumePendingCompatReenabledToast();

// v18.9.9.438: chapter-heap-refuse toast. Armed by the v437 escalation
// gate when a chapter jump fails specifically because prebake+heap-refuse
// (loadPageFromSectionFile refuses at maxAlloc < 12 KB after atlas
// install). The heap-refuse case is NOT a book-content problem so we
// don't want to write compat_custom.flag; we silent-restart-to-reader
// without the resumeSpine override to fall back to progress.bin's last
// working spine, and this toast tells the user why they're not at the
// chapter they clicked. Consumer is EpubReaderActivity::onEnter.
void armChapterHeapRefuseToast(int failedSpine);
int consumePendingChapterHeapRefuseToast();

// Restart + queue the action. Allocation-free (just RTC writes + ESP.restart)
// so it can be called from low-heap contexts. Caller may draw a contextual
// popup before calling -- the e-ink panel retains the last frame across the
// reboot, so the user sees that popup until the reader paints.
void silentRestartToReaderWithAction(ReaderPostBootAction action);
// v18.9.9.5: Level 1 defrag variant of silentRestartToReaderWithAction.
// Sets the RTC defrag-retry marker before restarting so boot 2's reader
// knows this book has already spent its one-shot defrag budget for
// this open. Used by the page-load-refuse path to reset heap
// fragmentation while preserving full render (tables + images), before
// falling back to Level 2 (tables suppressed) or Level 3 (Simple Rendering)
// content escalations.
void silentRestartToReaderWithDefragRetry(ReaderPostBootAction action);
// v18.9.9.32: same as above but also carries a target spine so the reader
// resumes at that chapter after boot instead of whatever progress.bin
// last committed. Used at the section-build-fail defrag site so a user
// who jumped to a fresh chapter via ChapterSelect (or Contents / anchor
// nav) doesn't get bounced back to their prior reading position across
// the silent-restart. targetSpine < 0 = no override (same as the
// non-spine variant). action is orthogonal (EnableBt when we were
// running with BLE, None otherwise).
void silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction action, int targetSpine);

// v18.9.9.37 (task #22): XTC-viewer defrag variant. Called from
// XtcReaderActivity when malloc for the ~96 KB page buffer fails on a
// fragmented heap. Sets the reader target + defrag magic and carries
// the XTC file path in an RTC slot. Boot dispatch reads the path and
// re-opens the file through ReaderActivity's normal file-open route
// (extension detection lands XtcReaderActivity again on the clean
// heap). v18.9.9.40 (task #25): uses an XTC-scoped defrag magic
// (isXtcDefragRetryContinuation) rather than the shared EPUB flag so
// a cross-activity hop doesn't consume the wrong budget.
void silentRestartToXtcReaderWithDefragRetry(const char* xtcPath);
bool isXtcDefragRetryContinuation();
void clearXtcDefragRetryContinuation();

// Read-and-clear the queued post-boot action. EpubReaderActivity calls
// this once it has finished its own first-paint setup; the returned
// action then dispatches on the next loop tick.
ReaderPostBootAction consumeReaderPostBootAction();

// v18.9.5: non-consuming read of the queued post-boot action. Used to decide
// whether the reader's initial page paint should be skipped -- when a
// post-boot action (EnableBt, OpenLookup, ...) will draw its own popup or
// force its own render on the next tick, painting the page first is a
// wasted flash cycle. Returns None on cold boot / no queued action.
ReaderPostBootAction peekReaderPostBootAction();

// CrumBLE 4.4 task #50: process-lifetime flag set true at setup() when
// the current boot resumes from a silent restart. Used by activities
// to skip cold-boot ceremony that would clobber the pre-reboot popup
// the panel is still holding -- ReaderActivity skips its "Loading..."
// popup, EpubReaderActivity forces FAST instead of HALF refresh on
// its first paint. Does NOT auto-clear so multiple callsites can
// query it during the boot sequence.
bool isContinuingFromSilentReboot();
void clearSilentRebootContinuationFlag();
// v18.9.9.5: true when this boot is the continuation of a Level 1 defrag
// silent-restart triggered by the reader's page-load-refuse path. Reader
// seeds layoutDefragRetryAttempted_ from this at onEnter so a second
// failure in the same book open escalates to Level 2/3 instead of
// hopping through another defrag cycle. Consume-and-clear at book open.
bool isDefragRetryContinuation();
void clearDefragRetryContinuation();
// v18.9.9.10: true when this boot is the continuation of a silent-restart
// whose post-boot action was EnableBt. Reader uses this to decide whether
// the Simple Rendering sidecar (a "needs compat mode WHEN BT is on" hint)
// should activate compat mode. When BT is neither currently enabled nor
// about to be enabled, the sidecar is ignored and the book renders in
// full-prebake mode -- preserving the user's prepared layout.
bool isEnableBtContinuation();

// CrumBLE 4.5: silent restart that lands on the OTA Update activity once the
// boot completes. Used by OtaUpdateActivity's heap pre-flight: a tight-heap
// device that's been used for a while can fail mbedtls SSL setup with
// MBEDTLS_ERR_SSL_ALLOC_FAILED during the GitHub API HTTPS handshake (needs
// ~40-50KB contiguous on top of WiFi's ~58KB share). Silent-restart lands
// on a clean ~115KB heap so the SSL handshake fits.
void silentRestartToOtaUpdate();

// CrumBLE 4.5.3: silent-restart back to BluetoothSettingsActivity. Called
// from the activity when enable or scan fails the heap pre-flight (~66 KB
// free needed for NimBLE init, ~14 KB free + 8 KB MaxAlloc for scan), so
// the user doesn't have to manually power-cycle. The post-boot dispatch
// sets g_postBtSilentReboot=true; the activity checks that flag to avoid
// looping silent-restarts if a fresh boot is somehow still under the floor.
//
// v18.9.9.367: fromReader parameter -- pass true ONLY when the current
// activity really is EpubReaderActivity (i.e. user opened BT from the
// in-book quick-connect drawer). The previous heuristic ("openEpubPath is
// non-empty") was wrong for the common Settings-menu case, because
// openEpubPath tracks the last book read this session, not the active
// activity. Field bug: user in Settings > BT Setup taps enable, silent-
// restart routes returnToReaderAfterBtMagic=true because a book had been
// open earlier, Back-from-BT then jumps user to reader instead of Settings.
void silentRestartToBluetoothSettings(bool fromReader = false);

// v18.9.5: silent-restart landing on Settings root (one level up from the
// BT Setup submenu). Called on Back-from-BT-menu when BT was on -- pairs
// the necessary heap defrag with keeping the user in Settings instead of
// bouncing them out to Home.
void silentRestartToSettings();

// v18.9.9.446: activities that CAN legitimately bad_alloc mid-onEnter
// (Settings rebuildLists is the poster child) call armSilentRestartTarget()
// on entry to pre-declare their landing target. If the terminate handler
// fires while this is armed, v440's target-preservation lands the user
// back on the activity they were entering, not on Home. Cleared by
// clearArmedSilentRestartTarget() on normal exit so a genuine cold boot
// or unrelated reboot doesn't land on the last-entered activity.
// The target int values match SILENT_REBOOT_TARGET_* constants in main.cpp.
void armSilentRestartTarget(uint32_t target);
void clearArmedSilentRestartTarget();

// v18.9: same restart, but arms an RTC magic so the post-restart activity
// auto-enters scan view (after auto-enable). Use when the scan pre-flight
// failed the heap floor -- otherwise the user is dropped on menu row 0 and
// has to tap "Scan & Pair" a second time.
// v18.9.9.367: fromReader default false. Settings > Bluetooth Setup > Scan
// is the only caller today and MUST NOT jump back to reader on Back.
void silentRestartToBluetoothSettingsWithScanIntent(bool fromReader = false);
extern bool g_postBtSilentReboot;

// v18.9: read-only from the activity's onEnter (owner: main.cpp boot
// dispatch). True on the boot immediately following a scan-intent
// silent-restart; false on cold boot and on all other silent-restart
// targets. Consumed and cleared by the activity after use.
extern bool g_postBtSilentRebootScanIntent;

// CrumBLE 4.5.7: one-shot RTC flag set by silentRestartToBluetoothSettings
// when a book is open at the time of the defrag restart. Consumed by
// ActivityManager::goToBluetoothSettings on the post-restart boot to make
// Back bounce to the reader (silentRestartToReaderWithAction) instead of
// dropping the user on Home. Naturally zero on cold boot.
extern uint32_t returnToReaderAfterBtMagic;
constexpr uint32_t RETURN_TO_READER_AFTER_BT_MAGIC = 0xC0BE4502;

// CrumBLE 4.5.4: same pattern as BT, for KOReader auth + OPDS browser.
// Both flip true on a post-recovery boot so the activity's pre-flight
// knows not to re-arm the silent-restart loop (one attempt then real
// error). False on any other entry path.
void silentRestartToKoreaderAuth();
extern bool g_postKoreaderSilentReboot;
void silentRestartToOpdsBrowser();
extern bool g_postOpdsSilentReboot;
// v18.9.9.308: same pattern for Manage Fonts. WiFi.mode(WIFI_STA) needs
// ~50 KB contiguous for 4 RX buffers plus mbedtls handshake for the
// manifest HTTPS fetch. Landing the user back in the wizard preserves
// navigation intent vs plain silentRestart which dumps them on Home.
void silentRestartToFontDownload();
extern bool g_postFontDownloadSilentReboot;
// v18.9.9.336: same pattern for WiFi Selection direct-land. WiFi.mode(WIFI_STA)
// has been observed to null-deref inside wpa_supplicant/eloop.c on an in-book
// fragmented heap. Silent restart lands the user in WifiSelectionActivity on
// a fresh ~150 KB heap where the ESP-IDF WiFi init path fits cleanly.
void silentRestartToWifiSelection();
extern bool g_postWifiSelectionSilentReboot;
// v18.9.9.337: same pattern for ClockSync (NTP sync via WiFi).
void silentRestartToClockSync();
extern bool g_postClockSyncSilentReboot;
// v18.9.9.343: variant fired from HomeActivity's first-entry boot-sync
// check. Same lean-boot path as CLOCK_SYNC but ClockSyncActivity
// silent-restarts back to Home after the sync finishes (or times out),
// so the user gets the clock on Home without interacting with any
// dialog. Only used for the automatic boot-time sync; manual
// Settings > Sync Time still uses silentRestartToClockSync.
void silentRestartToHomeClockSync();
extern bool g_postHomeClockSyncSilentReboot;
// v18.9.9.343: RTC-backed marker cleared on cold boot, set by Home before
// firing silentRestartToHomeClockSync(). Prevents a loop if NTP sync
// fails (halClock.hasValidTime() stays false so the Home entry check
// would otherwise re-trigger every return-to-Home).
bool homeBootClockSyncAlreadyAttempted();
void markHomeBootClockSyncAttempted();

// CrumBLE 4.6: silent restart that resumes mid-OTA -- after the user has
// confirmed install on the "New update available" screen. Skips the check
// phase; URL + size + version are persisted to RTC and pulled back into
// OtaUpdater on the next boot. Used because the install download's own
// HTTPS handshake (separate connection to GitHub's CDN) needs a fresh
// ~50KB contiguous heap budget that the post-check residue eats.
void silentRestartToOtaInstall(const char* url, uint32_t size, const char* version);
// Read-and-clear the queued install state. Returns true if a valid install
// is pending (target=OTA_INSTALL and URL non-empty); fills out the args.
// Safe to call once per boot.
bool consumePendingOtaInstall(char* outUrl, size_t outUrlSize, uint32_t* outSize, char* outVersion,
                              size_t outVersionSize);
