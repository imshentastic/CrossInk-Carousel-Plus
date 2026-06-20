#include "OtaUpdateActivity.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap()
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstring>

#include <algorithm>

namespace {
// Target reserve size: a hair over the mbedtls SSL handshake + X.509 verify
// peak (~40-50KB for RSA-2048 cert chains). Allocating this much before WiFi
// goes up forces LWIP/mbedTLS scratch to fragment around it, so when we free
// the reserve right before esp_http_client_perform, mbedTLS has the chunk it
// needs in one piece. The 16KB floor avoids holding crumbs (any reserve
// smaller than the actual handshake peak doesn't help and just wastes heap).
constexpr size_t kHeapReserveTarget = 50u * 1024u;
constexpr size_t kHeapReserveMinUseful = 16u * 1024u;
constexpr size_t kHeapReserveHeadroom = 8u * 1024u;  // leave for non-WiFi allocs
}  // namespace

#include "AppVersion.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

void OtaUpdateActivity::acquireHeapReserve() {
  if (heapReserve) return;
  const size_t maxAvail = ESP.getMaxAllocHeap();
  if (maxAvail < kHeapReserveMinUseful + kHeapReserveHeadroom) {
    LOG_INF("OTA", "Heap reserve: maxAlloc=%u too small, skipping", maxAvail);
    return;
  }
  const size_t want = std::min(kHeapReserveTarget, maxAvail - kHeapReserveHeadroom);
  heapReserve = heap_caps_malloc(want, MALLOC_CAP_8BIT);
  if (heapReserve) {
    heapReserveSize = want;
    LOG_INF("OTA", "Heap reserve: held %u bytes (maxAlloc before=%u after=%u)", want, maxAvail,
            ESP.getMaxAllocHeap());
  } else {
    LOG_INF("OTA", "Heap reserve: alloc failed for %u bytes (maxAlloc=%u)", want, maxAvail);
  }
}

void OtaUpdateActivity::releaseHeapReserve() {
  if (!heapReserve) return;
  const size_t freed = heapReserveSize;
  heap_caps_free(heapReserve);
  heapReserve = nullptr;
  heapReserveSize = 0;
  LOG_INF("OTA", "Heap reserve: released %u bytes (maxAlloc=%u)", freed, ESP.getMaxAllocHeap());
}

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    releaseHeapReserve();
    finish();
    return;
  }

  // Release the boot-time heap reserve right before the HTTPS handshake so
  // mbedtls gets the guaranteed contiguous chunk it needs for X.509 verify.
  releaseHeapReserve();

  // CrumBLE 4.6: install-pending mode -- a prior boot completed
  // checkForUpdate and the user confirmed install. We silent-restarted to
  // give the install download its own fresh heap. Skip the check and jump
  // directly to install with the cached URL/version.
  if (installPending_) {
    installPending_ = false;
    LOG_INF("OTA", "WiFi reconnected for install -- skipping check, going straight to install");
    requestInstallFromLoop_ = true;
    {
      RenderLock lock(*this);
      state = WAITING_CONFIRMATION;  // loop() picks up requestInstallFromLoop_ and transitions
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OTA", "Checking update screen could not be rendered synchronously; aborting update check");
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
  requestUpdate(true);
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // CrumBLE 4.5: heap pre-flight. mbedtls SSL setup needs ~40-50KB
  // contiguous on top of WiFi's ~58KB share for the GitHub API HTTPS
  // handshake; on a used-for-a-while device the post-WiFi MaxAlloc can
  // collapse below that and the handshake bails with -0x7F00
  // (MBEDTLS_ERR_SSL_ALLOC_FAILED), surfaced to the user as the
  // unhelpful "Update Failed". Threshold ~80KB MaxAlloc before WiFi
  // leaves headroom for the SSL allocs after WiFi takes its bite.
  // Silent-restart to OTA so we re-enter with a clean ~115KB heap.
  // isContinuingFromSilentReboot prevents an infinite restart loop if
  // the fresh-boot heap still isn't enough (genuinely broken state).
  constexpr uint32_t kOtaPreflightMaxAlloc = 80u * 1024u;
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (maxAlloc < kOtaPreflightMaxAlloc && !isContinuingFromSilentReboot()) {
    LOG_INF("OTA",
            "Heap pre-flight: maxAlloc=%u below %u; silent-restarting to OTA so the SSL handshake fits",
            maxAlloc, kOtaPreflightMaxAlloc);
    silentRestartToOtaUpdate();
    return;
  }
  if (isContinuingFromSilentReboot()) {
    LOG_INF("OTA", "Post-silent-restart entry: maxAlloc=%u (proceeding)", maxAlloc);
    clearSilentRebootContinuationFlag();
  }

  // CrumBLE 4.6: if a prior boot completed checkForUpdate and the user
  // confirmed install, we silent-restarted to give the install download a
  // fresh heap. Pull the cached URL/size/version back into OtaUpdater so
  // installUpdate can run without re-doing the check.
  char pendingUrl[256];
  uint32_t pendingSize = 0;
  char pendingVersion[40];
  if (consumePendingOtaInstall(pendingUrl, sizeof(pendingUrl), &pendingSize, pendingVersion,
                               sizeof(pendingVersion))) {
    LOG_INF("OTA", "Install pending from prior boot: version=%s size=%u", pendingVersion, pendingSize);
    updater.preloadInstallState(pendingUrl, pendingSize, pendingVersion);
    installPending_ = true;
  }

  // Grab the defrag reserve BEFORE WiFi.mode allocates -- so WiFi's LWIP
  // buffers + esp_netif state fragment around the reserve instead of through
  // the only large contiguous block. Released right before the HTTPS handshake
  // (in onWifiSelectionComplete), giving mbedTLS a guaranteed ~50KB chunk.
  acquireHeapReserve();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    // CrumBLE: show CrumBLE's own version (CRUMBLE_VERSION) since the
    // update is being pulled from imshentastic/CrumBLE's releases.
    // Strip the "crumble-" tag prefix off the new-version display so it
    // reads as "4.1.0" not "crumble-v4.1.0".
    const char* newVersionDisplay = updater.getLatestVersion().c_str();
    constexpr char tagPrefix[] = "crumble-";
    if (strncmp(newVersionDisplay, tagPrefix, sizeof(tagPrefix) - 1) == 0) {
      newVersionDisplay += sizeof(tagPrefix) - 1;
    }
    if (*newVersionDisplay == 'v' || *newVersionDisplay == 'V') ++newVersionDisplay;
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CRUMBLE_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + newVersionDisplay).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the bytes line below stays at the same Y it was at when the activity drew its own percent.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_POWER_ON_HINT));
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::runInstall() {
  {
    RenderLock lock(*this);
    state = UPDATE_IN_PROGRESS;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OTA", "Update progress screen could not be rendered synchronously; aborting OTA install");
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }
  // Heap reserve around install is handled inside OtaUpdater::installUpdate
  // (acquired before esp_https_ota_begin to defrag URL/HTTP client init,
  // released right after begin so perform has the freed block for TLS).
  LOG_INF("OTA", "Pre-install heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  const auto res = updater.installUpdate(
      [](void* ctx) {
        // immediate=true notifies the render task directly. The default deferred path only
        // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
        // installUpdate() blocks this task.
        static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
      },
      this);

  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update failed: %d", res);
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = FINISHED;
  }
  const auto renderResult = requestUpdateAndWait();
  if (renderResult == RequestUpdateResult::Rendered) {
    // Hold the completion screen briefly so the user sees it, then restart.
    delay(3000);
  } else {
    LOG_ERR("OTA", "Completion screen could not be rendered synchronously; restarting without sync confirmation");
  }
  {
    RenderLock lock(*this);
    state = SHUTTING_DOWN;
  }
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    // CrumBLE 4.6: install-pending path. We came back from a silent-restart
    // that was triggered after the user confirmed install on a prior boot.
    // Skip the Confirm wait -- consent was given pre-restart -- and fire
    // installUpdate immediately on the now-fresh heap.
    if (requestInstallFromLoop_) {
      requestInstallFromLoop_ = false;
      LOG_INF("OTA", "Auto-installing post-silent-restart (user already confirmed)");
      runInstall();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // CrumBLE 4.6: silent-restart between check and install so the install
      // download's HTTPS handshake runs on a fresh ~94KB heap. The check
      // residue (LWIP keep-alive + http client state) eats ~17KB that mbedtls
      // SSL setup needs. The URL/size/version are persisted to RTC; the
      // next boot's onEnter pulls them back and goes straight to install.
      LOG_INF("OTA", "User confirmed install -- silent-restarting to fresh heap for install handshake");
      // WiFi is up and holding LWIP state. Disconnect cleanly so the
      // post-restart boot doesn't race the prior session's tear-down.
      WiFi.disconnect(false);
      delay(30);
      silentRestartToOtaInstall(updater.getOtaUrl().c_str(),
                                static_cast<uint32_t>(updater.getOtaSize()),
                                updater.getLatestVersion().c_str());
      return;  // unreachable: silentRestartToOtaInstall calls ESP.restart()
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
