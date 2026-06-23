#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage = KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();

  // CrumBLE 4.5.4: heap pre-flight. WiFi.begin alone needs ~58 KB free +
  // ~30 KB MaxAlloc, and the mbedtls HTTPS handshake for the KOReader
  // sync server's auth POST needs another ~40-50 KB contiguous on top.
  // Mid-reading session, free heap is often ~50 KB / MaxAlloc ~25 KB --
  // the user used to see "Memory low. Restart device." and have to
  // power-cycle. Now: silent-restart to come back on a clean ~150 KB
  // free heap, then the auth completes. g_postKoreaderSilentReboot
  // guards against an infinite loop if even a fresh boot is somehow
  // under the floor.
  constexpr uint32_t kAuthMinFreeHeap = 66u * 1024u;
  constexpr uint32_t kAuthMinMaxAlloc = 48u * 1024u;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if ((freeHeap < kAuthMinFreeHeap || maxAlloc < kAuthMinMaxAlloc) && !g_postKoreaderSilentReboot) {
    LOG_INF("KOR", "KOReader auth pre-flight low (free=%u maxAlloc=%u, need %u/%u) -- silent-restart to recover heap",
            freeHeap, maxAlloc, kAuthMinFreeHeap, kAuthMinMaxAlloc);
    silentRestartToKoreaderAuth();
    return;  // never returns, but appease the linter
  }

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void KOReaderAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}
