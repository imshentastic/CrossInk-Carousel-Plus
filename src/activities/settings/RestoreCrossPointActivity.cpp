#include "RestoreCrossPointActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <HalPowerManager.h>

namespace {
// CrumBLE 4.5.4: hardcoded latest-release redirect for the upstream
// CrossPoint repo. GitHub's /releases/latest/download/<asset> URL pattern
// 302-redirects to the actual signed S3 object whatever the current
// release tag is, so we don't have to fetch + parse the release JSON
// like CrumBLE's own OtaUpdater does. The asset filename "firmware.bin"
// matches CrossPoint's release convention; if they rename it, this
// constant moves and the rest of the activity stays untouched.
constexpr const char* kCrossPointFirmwareUrl =
    "https://github.com/crosspoint-reader/crosspoint-reader/releases/latest/download/firmware.bin";

constexpr uint16_t kMinBatteryPercent = 50;
constexpr uint32_t kHoldDurationMs = 5000;
}  // namespace

void RestoreCrossPointActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("RCP", "RestoreCrossPointActivity entered. URL=%s", kCrossPointFirmwareUrl);

  const uint16_t battery = powerManager.getBatteryPercentage();
  if (battery < kMinBatteryPercent) {
    LOG_INF("RCP", "Battery %u%% < %u%% gate", battery, kMinBatteryPercent);
    state_ = BATTERY_LOW;
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("RCP", "WiFi not connected (status=%d)", static_cast<int>(WiFi.status()));
    state_ = NO_WIFI;
    requestUpdate();
    return;
  }
  state_ = READY;
  requestUpdate();
}

void RestoreCrossPointActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state_ != DOWNLOADING) finish();
    return;
  }

  if (state_ == READY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      holdStartMs_ = millis();
      lastRenderedHoldPct_ = 999;
      state_ = HOLDING;
      requestUpdate();
    }
    return;
  }

  if (state_ == HOLDING) {
    // Released early -> abort the hold, return to READY (must restart the 5s).
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      LOG_INF("RCP", "Confirm released at %ums (< %ums) -- aborting hold", millis() - holdStartMs_, kHoldDurationMs);
      state_ = READY;
      requestUpdate();
      return;
    }
    const uint32_t elapsed = millis() - holdStartMs_;
    if (elapsed >= kHoldDurationMs) {
      LOG_INF("RCP", "Hold confirmed (%ums). Starting download.", elapsed);
      state_ = DOWNLOADING;
      requestUpdate();
      // Synchronous: blocks until install completes or fails. esp_https_ota
      // restarts the chip into the new OTA partition on success, so we
      // don't return from here in the happy path.
      updater_.preloadInstallState(kCrossPointFirmwareUrl, 0, "crosspoint-latest");
      const auto err = updater_.installUpdate(
          [](void* ctx) {
            // Progress callback fires for each chunk; force a re-render.
            static_cast<RestoreCrossPointActivity*>(ctx)->requestUpdate();
          },
          this);
      if (err != OtaUpdater::OK) {
        LOG_ERR("RCP", "Restore failed: err=%d", static_cast<int>(err));
        state_ = FAILED;
        requestUpdate();
      }
      return;
    }
    // Throttle re-renders to one per percent of hold progress.
    const uint32_t pct = (elapsed * 100) / kHoldDurationMs;
    if (pct != lastRenderedHoldPct_) {
      lastRenderedHoldPct_ = pct;
      requestUpdate();
    }
    return;
  }

  if (state_ == DOWNLOADING) {
    const unsigned int pct = updater_.getTotalSize() > 0
                                 ? static_cast<unsigned int>((updater_.getProcessedSize() * 100) / updater_.getTotalSize())
                                 : 0;
    if (pct != lastUpdaterPercentage_) {
      lastUpdaterPercentage_ = pct;
      requestUpdate();
    }
    return;
  }
}

void RestoreCrossPointActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CAT_RECOVERY));

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineHeight) / 2;

  switch (state_) {
    case CHECKING_GATES:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_LOADING));
      break;
    case BATTERY_LOW:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_BATTERY_LOW), true, EpdFontFamily::BOLD);
      break;
    case NO_WIFI:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_NO_WIFI), true, EpdFontFamily::BOLD);
      break;
    case READY: {
      renderer.drawCenteredText(UI_10_FONT_ID, top - lineHeight, tr(STR_RECOVERY_CONFIRM_TITLE), true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + metrics.verticalSpacing, tr(STR_RECOVERY_CONFIRM_BODY));
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight * 2 + metrics.verticalSpacing * 2,
                                kCrossPointFirmwareUrl);
      break;
    }
    case HOLDING: {
      renderer.drawCenteredText(UI_10_FONT_ID, top - lineHeight, tr(STR_RECOVERY_HOLD_TO_CONFIRM), true,
                                EpdFontFamily::BOLD);
      const uint32_t elapsed = millis() - holdStartMs_;
      const int pct = static_cast<int>(std::min<uint32_t>(elapsed * 100 / kHoldDurationMs, 100));
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, top + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          pct, 100);
      break;
    }
    case DOWNLOADING: {
      renderer.drawCenteredText(UI_10_FONT_ID, top - lineHeight, tr(STR_RECOVERY_DOWNLOADING), true,
                                EpdFontFamily::BOLD);
      const int pct = updater_.getTotalSize() > 0
                          ? static_cast<int>((updater_.getProcessedSize() * 100) / updater_.getTotalSize())
                          : 0;
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, top + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          pct, 100);
      renderer.drawCenteredText(UI_10_FONT_ID,
                                top + metrics.verticalSpacing * 2 + metrics.progressBarHeight + lineHeight,
                                tr(STR_RECOVERY_FLASHING));
      break;
    }
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_FAILED), true, EpdFontFamily::BOLD);
      break;
  }
}
