#pragma once

#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();              // home screen
void silentRestartToReader();      // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToFileTransfer();// goes straight back to File Transfer activity

// CrumBLE: optional mode hint persisted across silentRestartToFileTransfer
// so the FT activity skips the mode-picker on auto-recover.
// Valid values: 0 = no hint, 1 = JOIN_NETWORK, 2 = CREATE_HOTSPOT.
void setSilentRebootFtModeHint(uint32_t mode);
uint32_t consumeSilentRebootFtModeHint();

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
};

// Restart + queue OpenDefinition with a word string. Allocation-free.
void silentRestartToReaderWithDefinition(const char* word);

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

// Read-and-clear the queued resume spine. EpubReaderActivity calls this in
// onEnter after the progress.bin load; if a value was queued it overrides
// currentSpineIndex and forces nextPageNumber = 0. Returns -1 if no resume
// was queued.
int consumePendingResumeSpine();

// Restart + queue the action. Allocation-free (just RTC writes + ESP.restart)
// so it can be called from low-heap contexts. Caller may draw a contextual
// popup before calling -- the e-ink panel retains the last frame across the
// reboot, so the user sees that popup until the reader paints.
void silentRestartToReaderWithAction(ReaderPostBootAction action);

// Read-and-clear the queued post-boot action. EpubReaderActivity calls
// this once it has finished its own first-paint setup; the returned
// action then dispatches on the next loop tick.
ReaderPostBootAction consumeReaderPostBootAction();

// CrumBLE 4.4 task #50: process-lifetime flag set true at setup() when
// the current boot resumes from a silent restart. Used by activities
// to skip cold-boot ceremony that would clobber the pre-reboot popup
// the panel is still holding -- ReaderActivity skips its "Loading..."
// popup, EpubReaderActivity forces FAST instead of HALF refresh on
// its first paint. Does NOT auto-clear so multiple callsites can
// query it during the boot sequence.
bool isContinuingFromSilentReboot();
void clearSilentRebootContinuationFlag();

// CrumBLE 4.5: silent restart that lands on the OTA Update activity once the
// boot completes. Used by OtaUpdateActivity's heap pre-flight: a tight-heap
// device that's been used for a while can fail mbedtls SSL setup with
// MBEDTLS_ERR_SSL_ALLOC_FAILED during the GitHub API HTTPS handshake (needs
// ~40-50KB contiguous on top of WiFi's ~58KB share). Silent-restart lands
// on a clean ~115KB heap so the SSL handshake fits.
void silentRestartToOtaUpdate();
