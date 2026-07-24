#include "Activity.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap()
#include <BluetoothHIDManager.h>
#include <I18n.h>

#include "ActivityManager.h"
#include "SilentRestart.h"
#include "boot_sleep/SleepActivity.h"
#include "components/UITheme.h"
#include "home/HomeActivity.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

RequestUpdateResult Activity::requestUpdateAndWait() { return activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::exitToHomeWithPopup() {
  // Cache the clean current page for the deep-sleep screensaver-cycle path
  // BEFORE drawing the "Going home..." popup, so transparent sleep PNGs show
  // the page (not the popup) behind them when the user cycles screensavers.
  // Only readers contribute — the cycle background is meant to be a book page.
  //
  // v18.9.9.273: force a fresh render before snapshotting. When the caller
  // arrived here via a menu action (user in reader -> opens drawer / top
  // menu -> picks Go Home), the menu sub-activity has closed but the
  // reader's re-render is only QUEUED (ActivityManager pops -> runs the
  // result handler BEFORE re-rendering the newly-top activity). The
  // framebuffer therefore still carries the menu overlay pixels at
  // this moment, and snapshotting captures them -- next wake with a
  // transparent sleep PNG shows the menu as the background instead
  // of the book page. Blocking on requestUpdateAndWait forces the
  // reader to draw its clean page first.
  //
  // Safety: if requestUpdateAndWait returns Rejected (deadlock guard
  // fired -- e.g. caller holds a RenderLock, or is somehow the render
  // task itself), skip the snapshot entirely rather than capture the
  // dirty framebuffer. Cache stays at whatever the previous reader
  // session wrote, which is guaranteed to be a book page.
  if (canSnapshotForSleepOverlay()) {
    if (requestUpdateAndWait() == RequestUpdateResult::Rendered) {
      SleepActivity::snapshotFramebufferForCycle();
    } else {
      LOG_INF("ACT", "exitToHomeWithPopup: skipping cycle-cache snapshot -- render deferred");
    }
  }
  // FAST_REFRESH (drawPopup's default mode) gives the user instant
  // visual feedback before the activity teardown begins. Without
  // this, the reader's long-tail exit (BLE shutdown, session save,
  // activity replace, carousel render) leaves the panel frozen on
  // the last reader page for ~700 ms.
  //
  // CrumBLE 4.4: on a tight heap, FAST_REFRESH's custom LUT can produce
  // a dim / partially-inverted popup -- the controller's view of the
  // panel state diverges from the framebuffer when BW backup
  // compression or display-buffer allocations fail. Fall back to
  // HALF_REFRESH in that case: ~770 ms instead of instant, but the
  // popup actually renders correctly. Threshold mirrors other
  // heap-pre-flight checks in the reader (~32 KB MaxAlloc floor).
  constexpr uint32_t kGoingHomePopupHealthyMaxAlloc = 32u * 1024u;
  const auto popupRefresh = ESP.getMaxAllocHeap() >= kGoingHomePopupHealthyMaxAlloc
                                ? HalDisplay::FAST_REFRESH
                                : HalDisplay::HALF_REFRESH;
  GUI.drawPopup(renderer, tr(STR_GOING_HOME), /*minTextWidth=*/0, /*leftAlignText=*/false, popupRefresh);
  // v18.9.9.205: the popup is on-panel — tell Home so its carousel warmup
  // runs silently behind it instead of stacking a "Loading" popup on top.
  HomeActivity::noteGoingHomePopupShown();
  // CrumBLE: tear NimBLE down synchronously BEFORE the Home transition. Home's
  // Flow shelf renders on a separate task and would otherwise race the reader's
  // deferred BLE disable -- rendering while NimBLE still holds ~58 KB, which
  // OOM-crashed the shelf's vector/string allocations on a fragmented heap. Doing
  // it here frees that heap first so Home renders cleanly. No-op when BLE is off
  // (the common non-reader case); drawPopup above already gave instant feedback,
  // mirroring the drawer's drawPopup-then-BLE-op pattern (safe outside a render lock).
  auto& bt = BluetoothHIDManager::getInstance();
  if (bt.isEnabled()) {
    bt.disable();
  }
  // v18.9.9.396: heap-guard for the Home render. Symmetric to v383's book-
  // open guard. Field log after a CJK reading session: free=19508 max-
  // Alloc=14836 at Home entry, then slow-render carousel=5264 shelf=9425
  // total=14765 ms -- Home took ~15s to appear because every cover thumb
  // decoded from SD (cached JPEG path needs ~20 KB, unavailable at 14 KB
  // maxAlloc) and the shelf fell into the same slow-path.
  //
  // Fresh boot has ~85 KB free / 61 KB maxAlloc, and Home comes back in
  // ~500 ms. So when we detect the low-heap condition here, silentRestart
  // (target=Home) trades ~2 s of boot for ~15 s of Home render.
  //
  // Thresholds mirror the book-open guard so the two watchdogs agree on
  // "fragmented enough that a big render will drag". If the render task
  // is currently locked (deadlock guard), skip the restart -- the ESP.
  // restart() call is unsafe under a held render lock.
  constexpr uint32_t kHomeExitMinFree = 40u * 1024u;
  constexpr uint32_t kHomeExitMinMaxAlloc = 30u * 1024u;
  const uint32_t freeNow = ESP.getFreeHeap();
  const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
  if (freeNow < kHomeExitMinFree || maxAllocNow < kHomeExitMinMaxAlloc) {
    LOG_INF("ACT",
            "exitToHomeWithPopup heap-guard: free=%u<%u OR maxAlloc=%u<%u -- silent-restart to home for fresh heap",
            freeNow, static_cast<unsigned>(kHomeExitMinFree),
            maxAllocNow, static_cast<unsigned>(kHomeExitMinMaxAlloc));
    silentRestart();  // never returns
    return;
  }
  activityManager.goHome();
}

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
