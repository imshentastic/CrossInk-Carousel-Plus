#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_task_wdt.h>

#include <cstddef>

#include "BluetoothHIDManager.h"
#include "CollectionsStore.h"
#include "CrossPointState.h"
#include "LibraryIndex.h"
#include "MappedInputManager.h"
#include "SeriesIndex.h"
#include "NetworkModeSelectionActivity.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "network/CrossPointWebServer.h"
#include "network/FirmwareFlasher.h"
#include <HalStorage.h>
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_INF("WEBACT", "Free heap at onEnter: %d bytes (maxAlloc %d)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // CrumBLE: tear NimBLE down before File Transfer comes up. NimBLE holds
  // ~58 KB of stack state while connected, and the FT web server runs with
  // very little headroom on x4 / even less on x3. Symptom we're guarding
  // against: "phone can't load FT page after personal hotspot" and "x3
  // frozen on Hotspot Mode" -- both consistent with the TX-buffer / WiFi
  // alloc starving when NimBLE is still holding its slab. BT remote is
  // irrelevant in FT anyway (the user is interacting via their phone),
  // so a synchronous teardown here costs the user nothing. No-op when
  // BT is already off (the common non-reader case). Matches the
  // exitToHomeWithPopup teardown pattern in Activity.cpp.
  {
    auto& bt = BluetoothHIDManager::getInstance();
    if (bt.isEnabled()) {
      LOG_INF("WEBACT", "Disabling NimBLE before web server start (~58 KB reclaim)");
      bt.disable();
      LOG_INF("WEBACT", "Free heap after NimBLE host deinit: %d bytes (maxAlloc %d)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    // CrumBLE: aggressive controller-layer teardown. BluetoothHIDManager::
    // disable() calls NimBLEDevice::deinit(false) which only releases the
    // NimBLE *host* stack -- the ESP32-C3 BT *controller* keeps its static
    // buffers (~25 KB) allocated. We never need BT back this boot because
    // onExit silentRestarts the device, so it's safe to tear the controller
    // down too and call esp_bt_mem_release to return its memory to the
    // general heap. Conditional on the controller actually being up; on a
    // fresh boot where BT was never enabled, these are no-ops.
    const esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();
    if (btStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
      esp_bt_controller_disable();
    }
    if (btStatus != ESP_BT_CONTROLLER_STATUS_IDLE) {
      esp_bt_controller_deinit();
    }
    // mem_release returns the controller's static buffers permanently.
    // After this call, esp_bt_controller_init() will fail until reboot --
    // exactly what we want since onExit triggers a silentRestart anyway.
    esp_bt_mem_release(ESP_BT_MODE_BLE);
    LOG_INF("WEBACT", "Free heap after BT controller release: %d bytes (maxAlloc %d)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  // CrumBLE: free the loaded SD-card font before WiFi/web-server come up. File
  // transfer runs with very little heap (~26 KB free while serving), and a
  // stalled/failed TX-buffer alloc there is what wedges the server. Releasing
  // the SD font (if one is loaded) reclaims that headroom; it's reloaded
  // automatically when the reader resumes. No-op for built-in fonts.
  sdFontSystem.releaseLoadedFont(renderer);

  // CrumBLE 4.4: release the page-DOM heap reserve (commit 403de139, 18 KB
  // chunk lazily acquired on first chapter open to guarantee the
  // deserialize allocator a contiguous slot under BT-induced fragmentation).
  // It was previously only released for BT-enable, so a reader -> home -> FT
  // session arrived here with 18 KB of contiguous heap permanently locked
  // away -- which exactly matched the WS upload heap-pressure regression
  // (MinFree bottoming at ~1-4 KB mid-upload instead of the pre-v4.3
  // ~12-15 KB it ran at). FT doesn't read pages; releasing here reclaims
  // the headroom for WS/SD/lwIP. The reserve will be lazily re-acquired by
  // Section::loadPageFromSectionFile the next time the user opens a book,
  // gated on MaxAlloc > 30 KB so it never wedges a tight-heap restart.
  // releasePageHeapReserveForBtEnable() is just the misleadingly-named
  // unconditional release primitive -- nothing BT-specific about it.
  if (Section::pageHeapReserveHeld()) {
    LOG_INF("WEBACT", "Releasing page-DOM heap reserve (18 KB) for FT activity");
    Section::releasePageHeapReserveForBtEnable();
    LOG_INF("WEBACT", "Free heap after page-reserve release: %d bytes (maxAlloc %d)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  // CrumBLE: the in-RAM LibraryIndex (up to tens of KB for a big library) is
  // dead weight while the web server runs and is a major reason free heap sits
  // at only ~25 KB here. Release it now for serving headroom; onExit marks it
  // stale so it's rebuilt (and picks up any just-uploaded books) on the next
  // Recently Added / All Books visit.
  LibraryIndex::getInstance().releaseMemory();
  LOG_INF("WEBACT", "Free heap after index release: %d bytes (maxAlloc %d)",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // CrumBLE: also drop SeriesIndex + CollectionsStore in-RAM state.
  // SeriesIndex's pooled stringPool can hold tens of KB for libraries
  // with lots of series-tagged books; CollectionsStore holds the
  // collections vector + finished/new path caches. Both reload on
  // the silentRestart that exitToOrigin triggers in onExit, so we
  // don't strand persistent state.
  SeriesIndex::getInstance().releaseMemory();
  CollectionsStore::getInstance().releaseMemory();
  LOG_INF("WEBACT", "Free heap after series/collections release: %d bytes (maxAlloc %d)",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // CrumBLE: pre-flight heap check AFTER all reclamations.
  //
  // Threshold history:
  //   40 KB -- original, scoped to WiFi softAP + DNS server startup. User
  //            still hit "freezes when phone loads /files" because WiFi
  //            connect alone consumed ~25 KB of maxAlloc after passing
  //            this gate, and serving the 30 KB gzipped FilesPage burned
  //            another 6 KB on top -- the device wound up at maxAlloc
  //            ~6 KB with the render task starving.
  //   55 KB -- intermediate, refused users with ~51 KB maxAlloc whose
  //            sessions would have actually worked. Too conservative.
  //   45 KB -- current X4 threshold. Math from observed costs:
  //              wifi connect : -25 KB maxAlloc
  //              serve /files : -6 KB maxAlloc
  //              render task  : ~14 KB floor
  //              -> 25 + 6 + 14 = 45 KB minimum safe entry maxAlloc
  //            The per-page-serve guard (sendBufferGzip < 14 KB free)
  //            is the secondary net for any case that slips through.
  //   32 KB -- X3 threshold. The X3's 528x792 frame buffer (52 KB vs the
  //            X4's 48 KB) plus its larger e-ink controller buffers leave
  //            ~10 KB less contiguous heap at home. Observed FT-entry
  //            maxAlloc on a fresh-boot X3 lands at 36-40 KB; the 45 KB
  //            gate was permanently refusing FT entry. The secondary
  //            per-serve guard + silentRestart auto-recovery already
  //            catch the failure mode the higher threshold was guarding
  //            against, so the lower X3 gate trades a small mid-session
  //            recovery risk for the ability to actually use FT at all.
  // CrumBLE 4.2: consume the silent-reboot mode hint up front so the
  // low-heap recovery path below can also use it as a loop guard.
  //   1 = previous boot was in JOIN_NETWORK, restore that mode.
  //   2 = previous boot was in CREATE_HOTSPOT, restore that mode.
  //  99 = previous boot silent-restarted FOR low-heap recovery; if the
  //       pre-flight is still failing after a clean reboot we don't
  //       loop, we fall through to the alert + exitToOrigin so the user
  //       sees a real error instead of an infinite restart.
  constexpr uint32_t SILENT_REBOOT_LOWHEAP_RECOVERY_HINT = 99;
  const uint32_t modeHint = consumeSilentRebootFtModeHint();
  const bool justLowHeapRestarted = (modeHint == SILENT_REBOOT_LOWHEAP_RECOVERY_HINT);

  const uint32_t FT_MIN_MAX_ALLOC = gpio.deviceIsX3() ? 32000U : 45000U;
  if (ESP.getMaxAllocHeap() < FT_MIN_MAX_ALLOC) {
    if (!justLowHeapRestarted) {
      // CrumBLE 4.2: convert the "Reboot the device" alert into an
      // automatic silent-restart to FT. Symptom we're fixing: user was
      // in a heap-tight state (e.g. just connected BT on an SD-font
      // book), exited to FT, the NimBLE/controller/SD-font/index/series
      // releases above weren't enough to defragment the heap, pre-flight
      // failed, and the user had to physically reboot. With the silent
      // restart they instead see a brief loading popup (~3-5 s) and
      // land in a fresh-boot FT with a clean heap -- same total wait,
      // zero user action required.
      LOG_ERR("WEBACT", "FT pre-flight: maxAlloc=%u below %u, silent-restarting to recover",
              ESP.getMaxAllocHeap(), FT_MIN_MAX_ALLOC);
      setSilentRebootFtModeHint(SILENT_REBOOT_LOWHEAP_RECOVERY_HINT);
      silentRestartToFileTransfer();
      return;  // ESP.restart() in silentRestartToFileTransfer doesn't return
    }
    // Already silent-restarted once and still failing -- something is
    // genuinely wrong (corrupt SD, hardware issue, etc.). Don't loop.
    // Fall back to the original alert + exit.
    LOG_ERR("WEBACT", "FT pre-flight: maxAlloc=%u below %u after recovery restart, falling back to alert",
            ESP.getMaxAllocHeap(), FT_MIN_MAX_ALLOC);
    strncpy(APP_STATE.pendingAlertTitle, tr(STR_LOW_MEMORY_FT_TITLE), sizeof(APP_STATE.pendingAlertTitle) - 1);
    strncpy(APP_STATE.pendingAlertBody, tr(STR_LOW_MEMORY_FT_BODY), sizeof(APP_STATE.pendingAlertBody) - 1);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    exitToOrigin();
    return;
  }

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  // CrumBLE: if the previous boot just silentRestarted from this same
  // activity (auto-recovery from a low-heap serve), skip the network
  // mode picker entirely and dive back into the mode the user already
  // chose. Mirrors what would happen if they were to back out + re-
  // enter + re-pick the same mode by hand, except invisibly. Mode
  // hint values: 1 = JOIN_NETWORK, 2 = CREATE_HOTSPOT, 99 = low-heap
  // recovery (no mode auto-restore; mode picker shows normally).
  if (modeHint == 1) {
    LOG_INF("WEBACT", "Auto-restore: JOIN_NETWORK from silent-reboot hint");
    onNetworkModeSelected(NetworkMode::JOIN_NETWORK);
    return;
  }
  if (modeHint == 2) {
    LOG_INF("WEBACT", "Auto-restore: CREATE_HOTSPOT from silent-reboot hint");
    onNetworkModeSelected(NetworkMode::CREATE_HOTSPOT);
    return;
  }

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             exitToOrigin();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // CrumBLE: books may have just been uploaded over the file-transfer
  // web UI (USB or hotspot). Mark the LibraryIndex stale so the next
  // visit to Recently Added / All Books re-walks SD and discovers them.
  // The walk itself is lazy — onExit stays snappy.
  LibraryIndex::getInstance().markStale();

  state = WebServerActivityState::SHUTTING_DOWN;

  // Stop local services before disconnecting/restarting WiFi.
  stopWebServer();
  MDNS.end();
  if (dnsServer) {
    LOG_DBG("WEBACT", "Stopping DNS server...");
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
  delay(50);

  // Skip reboot if WiFi was never activated (e.g. user backed out of mode selection).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestart();
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    startActivityForResult(
        std::make_unique<CalibreConnectActivity>(renderer, mappedInput), [this](const ActivityResult& result) {
          state = WebServerActivityState::MODE_SELECTION;

          startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                                 [this](const ActivityResult& result) {
                                   if (result.isCancelled) {
                                     exitToOrigin();
                                   } else {
                                     onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                                   }
                                 });
        });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Start mDNS for hostname resolution
    if (MDNS.begin(AP_HOSTNAME)) {
      LOG_DBG("WEBACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
    }

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               exitToOrigin();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    exitToOrigin();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  if (MDNS.begin(AP_HOSTNAME)) {
    LOG_DBG("WEBACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
  } else {
    LOG_DBG("WEBACT", "WARNING: mDNS failed to start");
  }

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  LOG_DBG("WEBACT", "DNS server started for captive portal");

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  // Give it the renderer so /api/reader-render-info can compute the reader's
  // viewport + emSize for the optimizer's .pxc baking.
  webServer->setRenderer(&renderer);
  // CrumBLE: settings-JSON pre-build was tried; held ~12 KB at runtime and
  // starved later page serves into a guard-trip restart loop. /api/settings
  // returns 503 in this build (the device-side Settings UI is the canonical
  // path); /api/wifi, /api/opds, /api/files all work normally.
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    exitToOrigin();
  }
}

void CrossPointWebServerActivity::exitToOrigin() {
  if (returnBookPath.empty()) {
    onGoHome();
    return;
  }

  activityManager.goToReader(returnBookPath, true);
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    LOG_DBG("WEBACT", "Stopping web server...");
    webServer->stop();
    LOG_DBG("WEBACT", "Web server stopped");
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      esp_task_wdt_reset();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 500;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        // CrumBLE: as soon as a handler tripped the low-heap guard, stop
        // processing further requests. Browsers fire several /api/* calls
        // in parallel after a page load -- if we keep serving them under
        // a 18 KB free / 15 KB maxAlloc budget the next allocator call
        // craters before our auto-recovery silentRestart gets to run.
        if (peekFtRestartRequest()) break;
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          esp_task_wdt_reset();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Force trigger an update of which buttons are being pressed so be have accurate state
          // for back button checking
          mappedInput.update();
          // Check for exit button inside loop for responsiveness
          if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            exitToOrigin();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // CrumBLE 4.6 LAN-OTA: WS handler queued an install. The pending
    // firmware-pending.bin has already been size-validated; we now stop
    // serving HTTP/WS (the long blocking flash + reboot would drop those
    // connections anyway) and call firmware_flash::flashFromSdPath which
    // validates the image, streams it to the next OTA partition, and
    // flips the bootloader. Reboot on success; on failure delete the bad
    // file and surface an alert + fall back to FT.
    if (consumeFirmwareInstallRequest()) {
      LOG_INF("WEBACT", "LAN-OTA install requested -- stopping servers and flashing");
      // Render device-side "Installing firmware..." screen so the user sees
      // something on the e-ink during the ~30-60s blocking flash. E-ink
      // holds the image without further updates, so a one-shot render
      // before flashFromSdPath is enough.
      {
        const auto& metrics = UITheme::getInstance().getMetrics();
        const auto pageWidth = renderer.getScreenWidth();
        const auto pageHeight = renderer.getScreenHeight();
        renderer.clearScreen();
        GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                       "Installing Firmware", nullptr);
        const auto h10 = renderer.getLineHeight(UI_10_FONT_ID);
        const auto centerY = pageHeight / 2;
        renderer.drawCenteredText(UI_10_FONT_ID, centerY - h10, "Writing new firmware to flash...");
        renderer.drawCenteredText(UI_10_FONT_ID, centerY + h10 / 2, "Device will restart automatically.");
        renderer.drawCenteredText(UI_10_FONT_ID, centerY + h10 * 2, "Do not unplug.");
        renderer.displayBuffer();
      }
      // Give the WS server a beat to flush the INSTALL_QUEUED reply we just
      // sent before we tear down sockets -- otherwise the frontend never
      // sees the ack and hangs waiting for it.
      delay(150);
      stopWebServer();
      MDNS.end();
      if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
        dnsServer = nullptr;
      }
      const firmware_flash::Result fr = firmware_flash::flashFromSdPath(
          "/.crosspoint/firmware-pending.bin", nullptr, nullptr, false);
      // CrumBLE 4.6 re-anchor: do NOT delete the SD bin here. After this
      // restart the device boots from ota_1 -- main.cpp::setup() detects
      // the bin still on SD + non-ota_0 running partition and re-flashes
      // to ota_0 so future USB-flashes land on the expected partition.
      // The relocate pass deletes the bin on success. If THIS install
      // itself failed (no flash happened), wipe the bin so a bogus retry
      // doesn't loop.
      if (fr == firmware_flash::Result::OK) {
        LOG_INF("WEBACT", "LAN-OTA flash OK -- restarting; re-anchor pass will run on next boot");
        delay(500);
        ESP.restart();  // never returns
        return;
      }
      Storage.remove("/.crosspoint/firmware-pending.bin");
      LOG_ERR("WEBACT", "LAN-OTA flash failed: %s", firmware_flash::resultName(fr));
      strncpy(APP_STATE.pendingAlertTitle, "Update failed", sizeof(APP_STATE.pendingAlertTitle) - 1);
      strncpy(APP_STATE.pendingAlertBody, firmware_flash::resultName(fr), sizeof(APP_STATE.pendingAlertBody) - 1);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      exitToOrigin();
      return;
    }

    // CrumBLE: sendBufferGzip flagged a low-heap serve; the response has
    // already gone out (a small "Reconnecting..." page with an 8s meta-
    // refresh). silentRestart to FT now so the device comes back with
    // a fresh heap before the phone's next refresh fires.
    if (consumeFtRestartRequest()) {
      LOG_INF("WEBACT", "Auto-recovery: silentRestart to FT (free=%u maxAlloc=%u)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      // Stash the current mode so the next boot's FT onEnter skips the
      // mode picker and goes straight back to the same flow (auto-
      // connect to last SSID for JOIN, or just restart softAP for
      // HOTSPOT). 1 = JOIN_NETWORK, 2 = CREATE_HOTSPOT.
      setSilentRebootFtModeHint(isApMode ? 2u : 1u);
      // Mirror onExit cleanup that matters before restart: stop our own
      // services so WiFi isn't holding sockets mid-restart.
      stopWebServer();
      MDNS.end();
      if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
        dnsServer = nullptr;
      }
      delay(50);
      silentRestartToFileTransfer();  // never returns
      return;
    }

    // Handle exit on Back button (also check outside loop)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      exitToOrigin();
      return;
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);

    if (state == WebServerActivityState::SERVER_RUNNING) {
      GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                        connectedSSID.c_str());
      renderServerRunning();
    } else {
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  if (!isApMode) {
    renderWifiIndicator(metrics.topPadding + metrics.headerHeight);
  }

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    // AP mode display
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for Wifi
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

    // Show network name
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      connectedSSID.c_str());

    startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;

    // Show primary URL (hostname)
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";

    // Show QR code for URL
    const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);

    // Show IP address as fallback
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      hostnameUrl.c_str());
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 100,
                      ipUrl.c_str());
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode display (original behavior)
    // std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for URL
    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds((pageWidth - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    // Show web server URL prominently
    renderer.drawCenteredText(UI_10_FONT_ID, startY, webInfo.c_str(), true);
    startY += height10 + 5;

    // Also show hostname URL
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY, hostnameUrl.c_str(), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
