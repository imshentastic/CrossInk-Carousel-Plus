#include "ClockSyncActivity.h"

#include <Arduino.h>  // millis(), delay()
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClockSyncActivity::onEnter() {
  Activity::onEnter();
  state = SYNCING;
  syncedTime[0] = '\0';
  requestUpdate();
}

void ClockSyncActivity::onExit() {
  Activity::onExit();
  // CrumBLE 4.4: if we brought up WiFi just for this sync, drop it on the
  // way out so the activity doesn't leak a session. Skipped when WiFi was
  // already up at entry (some other flow owns it).
  if (wifiActivatedByUs) {
    WiFi.disconnect(false);
  }
}

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    // CrumBLE 4.4: previously this bailed immediately with NO_WIFI even
    // when the user had saved networks. The settings entry implies "do
    // the sync for me" -- requiring them to first navigate to WiFi
    // settings, pick a network, wait for it to connect, then come back
    // here is poor UX. Try auto-connecting to saved networks. Initial
    // version only tried lastConnectedSsid + the first credential, but
    // pre-mechanism saved networks don't have a lastConnectedSsid value
    // and the first credential may not be in range. Now iterate through
    // ALL saved credentials in priority order, short timeout per attempt
    // so the worst-case wait stays bounded.
    const auto& creds = WIFI_STORE.getCredentials();
    if (creds.empty()) {
      LOG_INF("CLK", "Manual sync requested but no saved WiFi networks");
      state = NO_WIFI;
      requestUpdate();
      return;
    }
    // Build an attempt order: lastConnectedSsid first (most likely still
    // in range), then the rest in storage order. Avoids retrying the same
    // SSID twice when lastConnectedSsid is present.
    const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
    std::vector<const WifiCredential*> attemptOrder;
    attemptOrder.reserve(creds.size());
    if (!lastSsid.empty()) {
      if (const WifiCredential* lastCred = WIFI_STORE.findCredential(lastSsid)) {
        attemptOrder.push_back(lastCred);
      }
    }
    for (const auto& c : creds) {
      if (attemptOrder.empty() || c.ssid != attemptOrder.front()->ssid) {
        attemptOrder.push_back(&c);
      }
    }

    WiFi.mode(WIFI_STA);
    wifiActivatedByUs = true;
    // 6s per network. Most home networks connect in 2-4s. Worst case
    // with 8 saved networks: ~48s, but in practice the first or second
    // attempt succeeds because at most one network is usually in range.
    constexpr uint32_t kPerAttemptMs = 6000;
    bool connected = false;
    for (const WifiCredential* cred : attemptOrder) {
      LOG_INF("CLK", "Auto-connecting to '%s' for clock sync", cred->ssid.c_str());
      if (cred->password.empty()) {
        WiFi.begin(cred->ssid.c_str());
      } else {
        WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      }
      const uint32_t startMs = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startMs < kPerAttemptMs) {
        delay(200);
      }
      if (WiFi.status() == WL_CONNECTED) {
        LOG_INF("CLK", "Auto-connected to '%s' in %lu ms", cred->ssid.c_str(),
                static_cast<unsigned long>(millis() - startMs));
        connected = true;
        break;
      }
      LOG_INF("CLK", "  failed (status=%d), trying next", static_cast<int>(WiFi.status()));
      WiFi.disconnect(false);
      delay(100);
    }
    if (!connected) {
      LOG_INF("CLK", "All %u saved networks failed -- bailing",
              static_cast<unsigned>(attemptOrder.size()));
      state = NO_WIFI;
      requestUpdate();
      return;
    }
  }

  const bool ok = halClock.syncFromNTP();
  if (!ok) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // Mark as synced so the auto-sync hook stops firing on future WiFi connects.
  SETTINGS.clockHasBeenSynced = 1;
  SETTINGS.saveToFile();

  // Read the freshly synced time back for the user-facing confirmation.
  char buf[9];
  if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    snprintf(syncedTime, sizeof(syncedTime), "%s", buf);
  }
  state = SUCCESS;
  requestUpdate();
}

void ClockSyncActivity::loop() {
  if (state == SYNCING) {
    // First-tick: render the "Syncing..." screen, then perform the (blocking) sync.
    // requestUpdateAndWait below forces the render before we block on WiFi.
    requestUpdateAndWait();
    runSync();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void ClockSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_SYNC));

  const int midY = pageHeight / 2;

  switch (state) {
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CLOCK_SYNCING));
      break;
    case SUCCESS: {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_OK), true, EpdFontFamily::BOLD);
      if (syncedTime[0] != '\0') {
        char line[32];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), syncedTime);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      break;
    }
    case NO_WIFI:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_NO_WIFI), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_SYNC_NO_WIFI_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
