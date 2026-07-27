#include "ActivityManager.h"

#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include "SilentRestart.h"

#include <algorithm>

#include "CollectionsStore.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/AlertActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "home/RecentBooksGridActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/WifiSelectionActivity.h"
#include "settings/ClockSyncActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/FontDownloadActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/KOReaderAuthActivity.h"
#include "settings/BluetoothSettingsActivity.h"
#include "settings/OtaUpdateActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

void ActivityManager::begin() {
  xTaskCreate(&renderTaskTrampoline, "ActivityManagerRender",
              16384,  // Stack size — increased from 8192; createSectionFile() puts ChapterHtmlSlimParser (~700 bytes)
                      // on stack during silentIndexNextChapterIfNeeded
              this,   // Parameters
              1,      // Priority
              &renderTaskHandle  // Task handle
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    // v18.9.9.433: skip render entirely when the framebuffer has been
    // released (WS upload in progress). GfxRenderer's direct frameBuffer[]
    // writes would crash on null. E-ink panel retains the last painted
    // image so nothing visibly changes; when the buffer is reallocated
    // (via reboot on FT exit) the next boot repaints from scratch.
    if (currentActivity && display.getFrameBuffer() != nullptr) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&renderStateMux);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&renderStateMux);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    mappedInput.setPowerAsConfirmInReaderMode(currentActivity->allowPowerAsConfirmInReaderMode());
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  } else {
    mappedInput.setPowerAsConfirmInReaderMode(false);
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Queue an update to ensure the popped activity gets re-rendered.
        // Do not block here: result handlers may transiently take RenderLock while
        // reconciling state, and a synchronous wait at this point can trip the
        // deadlock guard even though the queued repaint is sufficient.
        if (pendingAction == PendingAction::None) {
          lock.unlock();
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (APP_STATE.hasPendingAlert.load(std::memory_order_acquire) && pendingAction == PendingAction::None) {
    APP_STATE.hasPendingAlert.store(false, std::memory_order_relaxed);
    pushActivity(std::make_unique<AlertActivity>(renderer, mappedInput));
  }

  if (requestedUpdate) {
    requestedUpdate = false;
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer(std::string returnBookPath) {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, std::move(returnBookPath)));
}

void ActivityManager::goToOtaUpdate() {
  replaceActivity(std::make_unique<OtaUpdateActivity>(renderer, mappedInput));
}

void ActivityManager::goToBluetoothSettings() {
  // CrumBLE 4.5.3: replaceActivity (not push) -- used by the silent-restart
  // dispatch at boot, where we want BT settings to BE the root activity
  // rather than stacked over Home.
  //
  // v18.9.1: bring back the return-to-reader routing scoped to Back-from-BT
  // (v17 had removed it after unrelated regressions with the
  // reader-with-EnableBt bounce). Field flow this restores:
  //   in-book -> drawer BT quick connect -> BT enable fails floor
  //   -> silentRestartToBluetoothSettings sets returnToReaderAfterBtMagic
  //      because APP_STATE.openEpubPath was non-empty at restart time
  //   -> post-restart lands in BT settings, but as ROOT (book context lost)
  //   -> user maps buttons / connects, presses Back
  //   -> WITHOUT this branch: default onComplete is popActivity, and the
  //      disableOnExit=true default disables BT then silent-restarts to
  //      Home (v18.9 fix) -- dropping the user out of their book
  //   -> WITH this branch: onComplete silent-restarts to reader with no
  //      post-action, and disableOnExit=false keeps BT alive for the book
  const bool returnToReader = (returnToReaderAfterBtMagic == RETURN_TO_READER_AFTER_BT_MAGIC);
  if (returnToReader) {
    returnToReaderAfterBtMagic = 0;
    // v18.9.5.5: exitOnSuccessfulConnect=false so a successful connect leaves
    // the user on BT settings (matches reader-menu semantics -- they may want
    // to Map Buttons, forget a device, etc). v18.9.1 had this as TRUE, which
    // finish()ed the activity on connect -- and since this is a replaceActivity
    // root, empty stack fell through to Home. Users were being ejected from
    // both the BT menu AND the reader when all they wanted was to connect.
    //
    // v18.9.5.7: onComplete restarts to reader with post-action = EnableBt so
    // NimBLE comes back up and reconnects to the bonded remote after boot.
    // v18.9.5.5 had this as None -- silent-restart killed NimBLE and the
    // book resumed with BT cold + remote disconnected. EnableBt reuses the
    // reader's own pendingBleQuickConnect_ QC handler, which does the enable
    // + connectToDevice + HID subscribe sequence against a fresh heap.
    // disableOnExit=false is now cosmetic (the reboot ends NimBLE either
    // way) but kept for clarity of intent.
    // 4.7.2: fromReader=true so a SECOND restart from inside this session
    // (scan pre-flight) re-arms returnToReaderAfterBtMagic instead of
    // clearing it -- Back must keep leading to the book however many
    // heap-recovery hops the session takes. The prewarm gate skips the
    // post-restart case via g_postBtSilentReboot, preserving the 4.5.5
    // enable-floor fix.
    replaceActivity(std::make_unique<BluetoothSettingsActivity>(
        renderer, mappedInput,
        [] { silentRestartToReaderWithAction(ReaderPostBootAction::EnableBt); },
        /*fromReader=*/true,
        /*disableOnExit=*/false));
    return;
  }
  replaceActivity(std::make_unique<BluetoothSettingsActivity>(
      renderer, mappedInput, [this] { popActivity(); }, /*fromReader=*/false));
}

void ActivityManager::goToKoreaderAuth() {
  // CrumBLE 4.5.4: replaceActivity post-silent-restart so the auth
  // wizard is the root activity, matching the BT pattern.
  replaceActivity(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput));
}

void ActivityManager::goToFontDownload() {
  // v18.9.9.308: same pattern as goToKoreaderAuth -- boot dispatch for
  // SILENT_REBOOT_TARGET_FONT_DOWNLOAD lands the user back in the
  // Manage Fonts wizard on a fresh heap, instead of dumping them on
  // Home and forcing them to re-navigate Settings > Reader > Font >
  // Manage Fonts.
  replaceActivity(std::make_unique<FontDownloadActivity>(renderer, mappedInput));
}

void ActivityManager::goToWifiSelection() {
  // v18.9.9.336: same pattern as goToFontDownload. Fresh-heap re-launch
  // of the WiFi picker after a silentRestartToWifiSelection tripped the
  // wpa_supplicant crash guard.
  replaceActivity(std::make_unique<WifiSelectionActivity>(renderer, mappedInput));
}

void ActivityManager::goToClockSync() {
  // v18.9.9.337: fresh-heap re-launch of the Clock Sync activity after
  // silentRestartToClockSync tripped the same wpa_supplicant crash guard.
  replaceActivity(std::make_unique<ClockSyncActivity>(renderer, mappedInput));
}

// v18.9.9.335: goToSettings moved below goToReader; see that block for the
// instrumented single definition. Placeholder here would duplicate-define.

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  if (SETTINGS.recentBooksView == CrossPointSettings::RECENT_BOOKS_GRID) {
    replaceActivity(std::make_unique<RecentBooksGridActivity>(renderer, mappedInput));
  } else {
    replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
  }
}

void ActivityManager::goToBookshelf(std::string collectionId) {
  // Default to whatever the user last left active in the carousel header.
  if (collectionId.empty()) {
    collectionId = CollectionsStore::getInstance().getActiveId();
  }
  replaceActivity(std::make_unique<RecentBooksGridActivity>(renderer, mappedInput, std::move(collectionId)));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path, const bool suppressBackRelease) {
  SET_CHECKPOINT("activityMgr:goToReader");
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), suppressBackRelease));
}

void ActivityManager::goToSettings() {
  SET_CHECKPOINT("activityMgr:goToSettings");
  replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  const bool canSnapshotOverlay = currentActivity && currentActivity->canSnapshotForSleepOverlay();
  replaceActivity(
      std::make_unique<SleepActivity>(renderer, mappedInput, canSnapshotOverlay, getCurrentBookPath(), fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks" || activityName == "RecentBooksGrid") {
      // RecentBooksGrid (#81 Bookshelf) shares the icon-bar entry with the
      // legacy RecentBooks list view, so returning from either lands on
      // the same Bookshelf icon.
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const {
  if (currentActivity && currentActivity->isReaderActivity()) {
    return true;
  }

  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity && activity->isReaderActivity(); });
}

bool ActivityManager::canSnapshotForSleepOverlay() const {
  return currentActivity && currentActivity->canSnapshotForSleepOverlay();
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

std::string ActivityManager::getCurrentBookPath() const {
  if (currentActivity) {
    const std::string path = currentActivity->getCurrentBookPath();
    if (!path.empty()) {
      return path;
    }
  }

  for (auto it = stackActivities.rbegin(); it != stackActivities.rend(); ++it) {
    if (*it) {
      const std::string path = (*it)->getCurrentBookPath();
      if (!path.empty()) {
        return path;
      }
    }
  }

  return {};
}

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::notifyBeforeDeepSleep() {
  // Forward to the current activity AND anything stacked underneath.
  // The reader might not be the top of the stack (e.g. an action menu
  // is open on top of it) but still wants its session committed.
  if (currentActivity) currentActivity->onBeforeDeepSleep();
  for (auto& stacked : stackActivities) {
    if (stacked) stacked->onBeforeDeepSleep();
  }
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
RequestUpdateResult ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return RequestUpdateResult::Rejected;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&renderStateMux);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&renderStateMux);

  if (isRenderTask) {
    LOG_ERR("ACT", "requestUpdateAndWait() called from render task; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  if (alreadyWaiting) {
    LOG_ERR("ACT", "requestUpdateAndWait() called while another task is waiting; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  // Cannot call while holding RenderLock or it will cause a deadlock
  if (holdingRenderLock) {
    LOG_ERR("ACT", "requestUpdateAndWait() called while holding RenderLock; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return RequestUpdateResult::Rendered;
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 * Checks if renderingMutex is held by any task, including the calling task.
 *
 * @return true if renderingMutex has an owner (any task), false otherwise.
 *
 * @note Must not be called from ISR context — xSemaphoreGetMutexHolder is not ISR-safe.
 */
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; }

bool RenderLock::heldByCurrentTask() {
  return xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle();
}

bool RenderLock::tryLockFor(TickType_t ticks) {
  return xSemaphoreTake(activityManager.renderingMutex, ticks) == pdTRUE;
}

void RenderLock::forceUnlock() { xSemaphoreGive(activityManager.renderingMutex); }
