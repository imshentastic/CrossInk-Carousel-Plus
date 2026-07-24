#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "CollectionsStore.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "LibraryIndex.h"
#include "Logging.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SeriesIndex.h"
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
  // v18.9.9.459: SYNCHRONOUS render so "Authenticating…" is on-panel before
  // performAuthentication releases the framebuffer. requestUpdate() alone
  // is async — the render task may not have serviced it by the time we
  // release the fb, leaving nothing painted during the ~5 s TLS handshake.
  // AndWait blocks until the render + display cycle completes.
  requestUpdateAndWait();

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  // v18.9.9.459: release the ~40 KB framebuffer for the duration of the TLS
  // handshake. Mirrors the FT trick (v432/v433) — mbedtls needs 55 KB
  // contiguous for the cipher-suite tables + cert chain, and the framebuffer
  // is dead weight during network I/O (the "Authenticating…" popup is
  // already painted to the panel; e-ink retains it without a live
  // framebuffer). Field data on v458: 37 KB free at handshake, needed 55.
  // Framebuffer release adds ~40 KB → clears the floor with room to spare.
  // Restored before painting the result so success/failure UI has a valid fb.
  // RenderLock coordinates with the render task so we don't race a paint
  // that would deref frameBuffer=nullptr mid-blit.
  {
    RenderLock lock(*this);
    renderer.releaseFrameBufferForBuild();
  }
  const uint32_t freeAfterRelease = ESP.getFreeHeap();
  LOG_INF("KOR", "TLS-window framebuffer release: free -> %u (maxAlloc=%u)",
          freeAfterRelease, ESP.getMaxAllocHeap());

  const auto result = KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (!renderer.restoreFrameBufferAfterBuild()) {
      LOG_ERR("KOR", "Framebuffer realloc failed after TLS window; forcing restart");
      ESP.restart();
    }
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

  // v18.9.9.371: aggressive teardown BEFORE the pre-flight check so we
  // measure post-reclaim heap. Mirrors CrossPointWebServerActivity's onEnter
  // (FT does the same reclaim for the same reason: WiFi.begin + mbedTLS
  // handshake collectively want ~110 KB, and lean-boot dispatch starts at
  // ~85 KB with sdFont+LibraryIndex+Series+Collections loaded). Field bug:
  // silent-restart landed at 85 KB free, WiFi.begin ate 54 KB, TLS handshake
  // then failed at 27 KB. Releasing sdFont(~10 KB) + LibraryIndex(~7 KB) +
  // Series/Collections(~3 KB) brings us to ~105 KB pre-WiFi, ~51 KB post-
  // WiFi -- tight but above the 50 KB TLS minimum after the mbedTLS floor
  // reduction below.
  {
    const uint32_t freeBefore = ESP.getFreeHeap();
    LibraryIndex::getInstance().releaseMemory();
    SeriesIndex::getInstance().releaseMemory();
    CollectionsStore::getInstance().releaseMemory();
    renderer.clearImageCache();
    sdFontSystem.releaseLoadedFont(renderer);
    sdFontSystem.setFallbackSuppressed(true);
    sdFontSystem.releaseFallback(renderer);
    LOG_INF("KOR", "Pre-network teardown: free %u -> %u bytes (maxAlloc=%u)",
            freeBefore, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  // CrumBLE 4.5.4: heap pre-flight. WiFi.begin alone needs ~58 KB free +
  // ~30 KB MaxAlloc, and the mbedtls HTTPS handshake for the KOReader
  // sync server's auth POST needs another ~40-50 KB contiguous on top.
  // Mid-reading session, free heap is often ~50 KB / MaxAlloc ~25 KB --
  // the user used to see "Memory low. Restart device." and have to
  // power-cycle. Now: silent-restart to come back on a clean ~150 KB
  // free heap, then the auth completes. g_postKoreaderSilentReboot
  // guards against an infinite loop if even a fresh boot is somehow
  // under the floor.
  //
  // v18.9.9.305: raised 66 KB -> 95 KB to match the KOSYNC_TLS_HEAP_FLOOR
  // used by the in-reader SYNC path (EpubReaderActivity.cpp). Field
  // report: user hit "Not enough memory for sync - please retry" on the
  // auth activity even though the 66 KB pre-flight passed. Same drain
  // that motivated the reader-side bump: wifi.connect + mbedtls
  // cert-chain load consume ~40 KB BETWEEN the pre-flight click and
  // the actual TLS handshake, so 66 KB clears here but the handshake
  // starves at ~26 KB inside KOReaderSyncClient::authenticate. 95 KB
  // leaves ~55 KB for TLS after the drain, matching MIN_HEAP_FOR_TLS
  // in KOReaderSyncClient.cpp.
  constexpr uint32_t kAuthMinFreeHeap = 95u * 1024u;
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
