#include "Activity.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap()
#include <BluetoothHIDManager.h>
#include <I18n.h>

#include "ActivityManager.h"
#include "boot_sleep/SleepActivity.h"
#include "components/UITheme.h"

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
  if (canSnapshotForSleepOverlay()) {
    SleepActivity::snapshotFramebufferForCycle();
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
  activityManager.goHome();
}

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
