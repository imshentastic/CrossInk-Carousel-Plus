#pragma once
#include <atomic>
#include <cstdint>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  std::string favoriteSleepImagePath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // CrumBLE 4.4: last firmware version that ran on this device. Compared
  // against CRUMBLE_VERSION at boot so we can run one-shot post-update
  // tasks (currently: sweep thumb_failed_v3_*.marker files so users whose
  // cover gen failed under an earlier-firmware bug -- e.g. the EOCD scan
  // window bump -- get their covers back automatically).
  std::string lastCrumbleVersion;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
  uint16_t pendingBookmarkSpine = UINT16_MAX;
  float pendingBookmarkProgress = -1.0f;

  // Set by background move task on failure; read and cleared by ActivityManager to show AlertActivity.
  // Title/body are written before the flag is set to ensure they are visible when flag is read.
  std::atomic<bool> hasPendingAlert{false};
  std::atomic<bool> pendingAlertGoHomeOnBack{false};
  char pendingAlertTitle[64] = {};
  char pendingAlertBody[256] = {};

  // v18.9.9.23: the currently-open reader is in Simple Rendering / Compat
  // mode. Set by EpubReaderActivity when simpleRenderingActive_ turns true;
  // cleared on onExit (or on entry if the new open isn't in compat). Read
  // by SettingsActivity so the four settings compat overrides (embedded
  // style, images, bionic reading, guide reading) show a "· Compat" suffix
  // and refuse to toggle -- so the user isn't confused by "Images: display"
  // reading enabled while the reader is drawing without images.
  bool readerCompatModeActive = false;

  // v18.9.9.174: set by reader onExit when the last-rendered page had images
  // (cover page, illustration, etc.). Read + cleared by HomeActivity::onEnter
  // to force pendingFullRefresh -- otherwise the light refresh cycle on Home
  // paint leaves visible ghosting of the reader image behind the shelf.
  bool readerExitedFromImagePage = false;

  // v18.9.9.346: set by reader render whenever the just-rendered page
  // actually needed the grayscale (LSB/MSB) plane -- true for pages
  // with images, or when textAntiAliasing is on with any glyphs. Read
  // by SleepActivity::renderOverlaySleepScreen to gate whether to run
  // the grayscale pass under a sleep overlay. Highlight-only pages
  // (sparse-ink lattice) set this false, so sleep skips the two extra
  // full-screen refreshes (triple-flash bug).
  bool lastReaderPageNeededGrayscale = false;

  // v18.9.9.293: set by any full-screen activity whose exit clobbers
  // Home's shelf-snapshot buffer (Reading Heatmap, and potentially
  // future extended-stats views). Home::onEnter reads + clears it and
  // forces pendingFullRefresh so the shelf renders from scratch instead
  // of trying to fast-refresh from a stale snapshot that now contains
  // the heatmap grid pixels.
  bool pendingHomeFullRefresh = false;

  // v18.9.9.58: which render path the currently-open book is on.
  // 0 = PreparedLayout (fingerprint matches prebake manifest; images render
  //     from .pxc cache, sections load from sections-prebake/ if the live
  //     fingerprint drifted).
  // 1 = CustomSettings (user picked "keep my settings" at the prompt, or no
  //     prebake exists for this book).
  // Compat state is scoped to this path: a book whose PreparedLayout works
  // fine can still legitimately need compat under CustomSettings, and
  // vice versa. Read by ReaderOptionsActivity's compat toggle so it
  // read/writes the correct per-path sidecar. Not persisted -- lives in
  // memory + RTC_NOINIT (silentRebootReaderActivePath) across silent
  // restarts.
  uint8_t readerActivePath = 1;

  // v18.9.9.27: signal from ReaderOptionsActivity to the reader that the
  // user just toggled Compatibility Mode. The reader-menu return handler
  // treats this like any other reader-layout setting change (forces
  // settingsChanged = true so the section rebuilds on the next render).
  // ReaderOptionsActivity sets it after writing/clearing the sidecar;
  // EpubReaderMenuActivity clears it after consuming.
  bool compatModeChanged = false;

  // v18.9.9.25: one-shot latch preventing infinite silent-restart loops when
  // the Book Settings drawer's ensureSettingsSrcBuilt() keeps refusing on a
  // book that's genuinely too heap-hungry for the drawer even on a clean
  // boot. Set when we fire silentRestartToReaderOpeningDrawerAt from the
  // refuse path; cleared on EpubReaderActivity::onExit. If the drawer
  // refuses a second time in the same book open, we fall through instead
  // of silent-restarting again.
  bool drawerHeapRestartTriedThisBook = false;

  // v18.9.9.59: user manually turned OFF compat mode via drawer or Reader
  // Options during the current book open. If Layer 2 auto-writes the compat
  // sidecar back on within the same session, the reader arms the RTC
  // toast flag so the post-silent-restart landing shows a "Compatibility
  // Mode required" popup -- the user's intent is respected in the first
  // place, but they get to see WHY the mode reactivated. In-memory only;
  // cleared on book close (EpubReaderActivity::onExit).
  bool compatUserDisabledThisSession = false;

  // v18.9.9.312: opt-in flag for activities that don't mutate the library
  // (Settings menus, Book/Reading stats views, Bookmarks list, Clock Sync,
  // etc). When set true by an activity's onExit before returning to Home,
  // HomeActivity::onEnter skips several expensive invalidations: cover
  // reload probes, shelf snapshot invalidation, and the shelf covers
  // "loaded" bit. Cuts ~500-1500 ms off return-to-Home for read-only
  // side trips. Cleared by HomeActivity::onEnter after consumption so
  // the next return defaults back to the safe (full-invalidate) path.
  // Set via APP_STATE.preserveHomeStateOnReturn = true; -- keep opt-in
  // to avoid stale-cache bugs if a new modifying activity forgets to
  // clear it.
  bool preserveHomeStateOnReturn = false;

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
