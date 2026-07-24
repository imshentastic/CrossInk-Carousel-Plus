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
#include <HalGPIO.h>  // v18.9.9.117: for gpio.deviceIsX3() in pre-flight
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

  // v18.9.9.373: mark the entry time so the heap watchdog can enforce
  // a minimum cooldown between silent-restart-to-FT attempts.
  activityEnteredAtMs_ = millis();
  heapLowStreakStartMs_ = 0;

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
    // v18.9.9.110: BT memory reservation is now released at BOOT (not here)
    // via our strong btInUse() override in main.cpp. If the previous session
    // silent-restarted into FT (silentRebootTarget == FT), Arduino releases
    // the ~25 KB of BT static memory before app_main. So this runtime call
    // is now a small ~1.6 KB extra cleanup that mostly no-ops.
    esp_bt_mem_release(ESP_BT_MODE_BLE);
    extern bool g_bleControllerMemReleased;
    g_bleControllerMemReleased = true;
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

  // CrumBLE 4.5.4: also dump the renderer's image cache. On a device
  // that's been showing the home screen with shelf covers loaded, the
  // image cache can hold ~30-50 KB of cover bitmaps that the FT mode
  // doesn't need. Field log showed maxAlloc=6900 at WEBACT entry on a
  // device that booted clean, meaning the cover decode churn during
  // shelf paint had fragmented the heap beyond what BT/index/series
  // release could recover. Wiping the image cache is the last reclaim
  // that targets the cover-decode residue specifically.
  renderer.clearImageCache();
  LOG_INF("WEBACT", "Free heap after image-cache clear: %d bytes (maxAlloc %d)",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // CrumBLE 4.5.4: also release the SD card fonts. The primary SD font +
  // UI fallback together hold ~30-40 KB resident (headers, intervals,
  // mini bitmap caches), scattered through the heap. None of it is
  // needed in FT mode: the web server uses small UI strings drawn with
  // the always-loaded built-in fonts. On exit, ensureLoaded() + ensure
  // FallbackLoaded() are called from main.cpp's per-tick poll and the
  // fonts re-resident before the user's next home render. Field log:
  // WEBACT entered with maxAlloc=14324 (well below the 45 KB pre-flight
  // floor), and even after BT/index/series/cache clears the maxAlloc
  // stayed at 14324 -- the SD-font fragmentation was the dominant
  // residue.
  sdFontSystem.releaseLoadedFont(renderer);
  // CrumBLE 4.5.4: also release the UI fallback. The fallback was the
  // dominant fragmentation contributor -- a permanently-resident
  // LXGW @14pt in the carousel/UI path. Suppression flag prevents the
  // per-tick poll from immediately reloading it. ALWAYS clear the flag
  // on FT exit (see exitToOrigin / silent restart paths).
  sdFontSystem.setFallbackSuppressed(true);
  sdFontSystem.releaseFallback(renderer);
  LOG_INF("WEBACT", "Free heap after SD font + fallback release: %d bytes (maxAlloc %d)",
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
  // v18.9.9.86: also latch on the html-serve low-heap flag (see main.cpp
  // g_ftHtmlServeLowHeapRestart). One-shot: read-and-clear so a subsequent
  // fresh FT entry doesn't inherit "we just recovered."
  extern uint32_t g_ftHtmlServeLowHeapRestart;
  const bool htmlServeLowHeapPrev = (g_ftHtmlServeLowHeapRestart == 1);
  g_ftHtmlServeLowHeapRestart = 0;
  // v18.9.9.117: modeHint 1 or 2 also implies "we just restarted with the
  // user's mode preserved" so the pre-flight in onNetworkModeSelected must
  // not fire again (loop protection). Any silent-restart into FT (whether
  // for html-serve low heap or for the new post-mode-selection pre-flight)
  // sets this so a second failure falls through to alert + exit.
  const bool justLowHeapRestarted = (modeHint == SILENT_REBOOT_LOWHEAP_RECOVERY_HINT) ||
                                    (modeHint == 1) || (modeHint == 2) || htmlServeLowHeapPrev;
  justLowHeapRestarted_ = justLowHeapRestarted;  // v18.9.9.83: latch for mid-serve check

  // CrumBLE 4.5.4: REVERTED to 45000/32000. An earlier lowering to
  // 25000/20000/16000 was based on observed steady-state heap, but
  // didn't account for WiFi.begin's allocation need: esp_wifi_init
  // consumes ~50 KB contiguous, so a 20 KB pre-flight pass lets the
  // device proceed to WiFi.begin which then OOMs and produces the
  // ASSOC_EXPIRE storm (field log: heap dropped 56548 -> 2572 over
  // WiFi.begin, then connection timed out). The 45 KB floor really
  // is the minimum that lets WiFi succeed -- the lower thresholds
  // for HTML serve / API guard (in CrossPointWebServer.cpp) are fine
  // because those run POST-WiFi when heap has already recovered.
  // v18.9.9.116: REVERT v115's threshold lowering. The pre-flight silent-
  // restart IS beneficial: it wipes cover-buffer/carousel/framebuffer state
  // that isn't otherwise released at FT-enter, and the post-restart boot
  // reaches 60+K maxAlloc vs the cold-path 38K. v115's log proved skipping
  // the restart left us at 12K/10K Post-webServer-begin vs 15K/13K WITH
  // the restart. Restore 45000 for X4 -- the "unnecessary" restart is
  // actually the difference between FT working and stalling.
  // v18.9.9.117: pre-flight moved to onNetworkModeSelected(). Firing here
  // (at onEnter, BEFORE the user has committed to a mode) wasted a ~3 s
  // reboot cycle even when the user just wanted to see the mode picker
  // before backing out. Now the check fires ONLY after the user picks
  // Join Network / Hotspot -- at which point the ~3 s wait is meaningful
  // (they're committing to WiFi.begin's ~50 KB alloc anyway).

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

  // CrumBLE 4.5.4: user explicitly exited FT -- clear the panic-recovery
  // flag so a later unrelated panic doesn't auto-restart back into FT
  // they no longer want to be in. Reset is idempotent if no upload was
  // active. Counter resets too, restoring full auto-resume budget for
  // the next FT session.
  setFtUploadInProgress(false);

  // CrumBLE 4.5.4: lift the UI-fallback suppression so the per-tick
  // poll restores the fallback before the user's next home render.
  // No-op on silent-restart exits (they reboot, suppression flag
  // resets to default false anyway).
  sdFontSystem.setFallbackSuppressed(false);

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

  // v18.9.9.120: UNCONDITIONAL silent-restart on FT mode commit (Pattern 1
  // from user's arch discussion). Rationale: Home page optimizations
  // (shelf-strip snapshot, LyraFlowTheme side-tile cache ~24 KB, carousel
  // frame cache) fragment the heap during a Home session. Even after the
  // Home::onExit release, fragmentation persists and FT serves at a
  // constrained heap that struggles with api-files + follow-up XHRs
  // (/api/settings-page, /api/wifi-networks). Silent-restarting on mode
  // commit gives FT a truly fresh heap identical to boot-into-FT state,
  // matching v55's post-cover-guard-restart baseline that shipped fast
  // FT loads. Home optimizations kept 100%. Cost: ~3s "Loading FT..."
  // popup once per FT session at a natural user-commit point.
  // Skip only when we came via boot dispatch (post-restart) to avoid
  // an infinite loop; the mode hint mechanism carries the picked mode
  // across the reboot so the user picks once.
  if (mode != NetworkMode::CONNECT_CALIBRE && !justLowHeapRestarted_) {
    LOG_INF("WEBACT",
            "onNetworkModeSelected: unconditional silent-restart to FT for fresh heap (mode hint=%u)",
            (unsigned)(mode == NetworkMode::CREATE_HOTSPOT ? 2u : 1u));
    setSilentRebootFtModeHint(mode == NetworkMode::CREATE_HOTSPOT ? 2u : 1u);
    silentRestartToFileTransfer();
    return;  // ESP.restart() doesn't return
  }

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
    // v18.9.9.265: stop BLE BEFORE WiFi init. ESP32-C3 shares one radio
    // between BLE + WiFi. When WiFi driver init runs with NimBLE
    // resident (~52 KB heap), the WiFi RX/TX pool sizes get clamped to
    // fit the remaining budget -- and stay clamped for this WiFi
    // session even if we drop BLE later. Ported from CrossPoint
    // feat-bluetooth c1a396c1. Match action: synchronous disable so
    // WiFi.mode() runs with BLE memory already released.
    if (BluetoothHIDManager::getInstance().isEnabled()) {
      LOG_INF("WEBACT", "Disabling BT before WiFi STA init to free radio+heap");
      BluetoothHIDManager::getInstance().disable();
    }
    // STA mode - launch WiFi selection
    // v18.9.9.355: DROPPED the pre-emptive WiFi.mode(WIFI_STA) call.
    // WifiSelectionActivity's connect path already sets WIFI_STA before
    // WiFi.begin(). Doing it HERE ate ~50 KB of driver-init heap
    // BEFORE WifiSelection ran, and WifiSelection then had only ~30 KB
    // free -- which trickled through onWifiSelectionComplete leaving
    // startWebServer at ~15 KB (needs 40 KB+ for routes). Result:
    // /api/files rendered zero rows, all requests soft-503'd. Letting
    // WifiSelection own the WiFi.mode call keeps FT's heap ~50 KB
    // higher for startWebServer.
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

    // v18.9.9.82: instrument the post-connect path to identify which
    // allocation is bad_alloc'ing on marginal heap. Field repro: WiFi
    // connects at ~25 KB free, then 17 ms later std::terminate at
    // free=2608. Somewhere in [mDNS begin, webServer new, webServer->begin]
    // ~22 KB got consumed.
    LOG_INF("WEBACT", "Post-connect diag: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // v18.9.9.92: mDNS SKIPPED on STA path. It costs ~5.5 KB free / ~4 KB
    // maxAlloc, which on X4 is exactly the headroom that separates
    // "TCP accept can serve HTTP" from "silent connection reset at
    // maxAlloc=4852". Device is reachable at the raw IP shown on the
    // mode-status panel, so .local access is a nicety users can trade
    // for FT actually working. Bookmarked `.local` URLs will need to
    // be updated to the IP once (or resolved via router DHCP hostname).
    LOG_INF("WEBACT", "mDNS skipped on STA path (v91: heap-headroom trade); IP-only access");

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

  // v18.9.9.265: same rationale as the STA branch above -- stop BLE
  // BEFORE WiFi init so the WiFi RX/TX pool sizing runs against fresh
  // heap. Ported from CrossPoint feat-bluetooth c1a396c1.
  if (BluetoothHIDManager::getInstance().isEnabled()) {
    LOG_INF("WEBACT", "Disabling BT before WiFi AP init to free radio+heap");
    BluetoothHIDManager::getInstance().disable();
  }
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
  LOG_INF("WEBACT", "startWebServer entry: free=%u maxAlloc=%u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  LOG_INF("WEBACT", "Post-webServer-new diag: free=%u maxAlloc=%u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // Give it the renderer so /api/reader-render-info can compute the reader's
  // viewport + emSize for the optimizer's .pxc baking.
  webServer->setRenderer(&renderer);
  // CrumBLE: settings-JSON pre-build was tried; held ~12 KB at runtime and
  // starved later page serves into a guard-trip restart loop. /api/settings
  // returns 503 in this build (the device-side Settings UI is the canonical
  // path); /api/wifi, /api/opds, /api/files all work normally.
  webServer->begin();
  LOG_INF("WEBACT", "Post-webServer-begin diag: free=%u maxAlloc=%u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiRssi = isApMode ? 0 : WiFi.RSSI();
    lastWifiBars = isApMode ? 0 : barsForRssi(lastWifiRssi, 0);

    // v18.9.9.432: reclaim the ~52 KB framebuffer secondary. FT's UI is a
    // static QR/IP screen: no differential AA rendering, only occasional
    // RSSI/bars cosmetic repaints (already suppressed during active WS
    // uploads by the loop() gate below). swapBuffers() and displayBuffer()
    // handle a null secondary correctly (no swap; driver re-seeds RAM
    // planes). onExit always reboots via silentRestart(), which re-allocs
    // both buffers on next boot, so no matching realloc is needed here.
    const uint32_t freeBefore = ESP.getFreeHeap();
    const uint32_t maxBefore = ESP.getMaxAllocHeap();
    display.releaseSecondaryFrameBuffer();
    LOG_INF("WEBACT",
            "Released framebuffer secondary: free %u -> %u (+%d), maxAlloc %u -> %u",
            freeBefore, ESP.getFreeHeap(),
            static_cast<int>(ESP.getFreeHeap()) - static_cast<int>(freeBefore),
            maxBefore, ESP.getMaxAllocHeap());

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
        // v18.9.9.421: skip cosmetic repaints during an active WS upload.
        // Each requestUpdate() drives the activity's render() and a panel
        // refresh (X3_DRF ~380-930 ms on X3), which pauses the WS loop
        // and eats maxAlloc for the framebuffer flush. Over a 10 MB
        // upload the RSSI/bars flap can trigger 15-30 repaints, adding
        // ~10-15 s of dead time AND transiently starving the heap the
        // WS BIN handler needs. WiFi loss handling (consecutive-
        // disconnect abandonment via onGoHome) still runs -- only the
        // "bars changed" cosmetic repaint is suppressed while uploading.
        const bool uploadActiveForRepaintGate =
            webServer && webServer->getWsUploadStatus().inProgress;
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
          // v18.9.9.300: cache RSSI so render() can show a weak-signal
          // warning. Repaint on either bars change OR when we cross the
          // weak-signal threshold in either direction.
          const bool wasWeak = lastWifiRssi < 0 && lastWifiRssi > -100 && lastWifiRssi < -70;
          const bool nowWeak = rssi < 0 && rssi > -100 && rssi < -70;
          if (bars != lastWifiBars || wasWeak != nowWeak) {
            lastWifiBars = bars;
            lastWifiRssi = rssi;
            repaint = true;
          } else {
            lastWifiRssi = rssi;
          }
        }
        if (repaint && !uploadActiveForRepaintGate) requestUpdate();
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

      // CrumBLE 4.5.5: critical-heap watchdog. Field log: an HP CJK upload
      // finished cleanly, then the post-upload phase (font stream + library
      // re-walk attempts + browser probing .crosspoint cache state) drained
      // heap to free=5864 maxAlloc=1780. The NEXT incoming HTTP request
      // entered WebServer::_parseForm, which allocates a std::unordered_map
      // for headers, hit std::bad_alloc, and -- because the Arduino runtime
      // is built without exception handling for application code paths --
      // landed in std::terminate -> abort(). Device panic'd, reboot, the
      // freshly-uploaded EPUB wasn't visible in FT.
      //
      // We can't intercept inside _parseForm (it's library code). But we
      // CAN refuse to call handleClient when heap is already below the
      // floor parseForm needs. If we hit that floor, trigger a clean
      // silent-restart back to FT instead of letting the next request
      // crash the process. The user re-enters FT on a fresh heap; in-
      // flight uploads are lost but the device stays alive.
      //
      // v18.9.9.90: pre-emptive heap-floor + silent-restart-to-FT removed.
      // History: v83 added a 3072-byte floor to prevent bad_alloc in
      // _parseForm; v88 lowered it to 1500 after v87's chunked serve; v90
      // field logs showed maxAlloc dipping below 1500 mid-session and
      // silent-restarting even when the next request would have parsed
      // fine, breaking the browser's WS session and open pages every
      // time. Real bad_allocs are caught by std::terminate ->
      // hard-restart with panel resync (v18.9.9.84), which is one clean
      // reboot per actual failure vs. dozens of unnecessary reboots
      // under the pre-emptive floor. If _parseForm turns out to
      // bad_alloc under some workload, reintroduce a narrower guard
      // (e.g. floor + hasClient() check + multi-poll persistence).

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

    // v18.9.9.372: passive critical-heap watchdog. sendBufferGzip's per-
    // serve guard only fires when a PROGMEM page is being served -- if
    // the heap collapses AFTER a serve (WS state pinning, post-upload
    // library walk, browser polling /api/heap while device chokes), the
    // device sits at ~3 KB free indefinitely. Browser sees "reconnecting"
    // because every new HTTP conn needs ~2 KB for the TCP socket.
    //
    // Poll free heap once per loop tick; if it stays under a critical
    // floor for kHeapLowConsecutiveTicks in a row, silent-restart to FT
    // to give the browser a natural recovery point (same flow the
    // per-serve guard uses -- mode hint preserves the network mode so
    // the reload lands on FilesPage automatically).
    //
    // Consecutive-tick requirement avoids restarting on transient dips
    // (e.g. mid-request allocation spike that resolves in <1 second).
    // Never fires during an active WS upload -- restarting mid-upload
    // aborts the browser's upload state and loses whatever bytes were
    // in flight. Never fires when a request handler already scheduled
    // a per-serve restart (peekFtRestartRequest).
    // v18.9.9.373: raised threshold from 6 KB -> 10 KB, added time-based
    // cooldown that lets the watchdog fire again 30 s after a prior
    // silent-restart. Field log: 40 seconds of low-heap thrash with the
    // old code refusing to restart because `justLowHeapRestarted_` was
    // still latched and no "large serve" had counted since restart --
    // browser only polls small API endpoints, none of which trigger the
    // progress counter. Time-based cooldown handles that flow.
    // v18.9.9.374: DROP the justLowHeapRestarted_ gate. It's set true on
    // EVERY post-mode-select boot (modeHint 1 or 2), not just after a
    // real heap-recovery restart -- so the 30 s cooldown was applying
    // even on the first FT session after user picked Join Network. That
    // meant the browser sat at 6 KB free for 30 seconds every time.
    // Replace with a simpler "minimum time since entry" gate: give the
    // page 12 s to load + settle, then any 2 s low-heap streak triggers
    // restart. Worst-case restart-every-14s loop is preferable to
    // 30-second browser stalls on every FT session.
    {
      // v18.9.9.426: cut floor 10 KB -> 5 KB. Field bug: watchdog kept
      // tripping during normal Optimize activity -- after optimizer.js
      // serve + WS pad grab + api-reader-render-info request, free heap
      // legitimately dips to 7-9 KB for several seconds, well under the
      // old 10 KB floor. The watchdog then restarted the WS server
      // mid-optimize, browser saw "cannot fetch". 5 KB is genuinely
      // stuck-territory; normal serve activity should recover above it.
      constexpr uint32_t kHeapLowFloor = 5u * 1024u;
      constexpr uint32_t kHeapLowStreakMs = 2000;   // sustained-low duration
      constexpr uint32_t kMinTimeSinceEntryMs = 12000;  // let page load + settle first
      const uint32_t nowMs = millis();
      const uint32_t freeNow = ESP.getFreeHeap();
      const bool uploadActive = webServer && webServer->getWsUploadStatus().inProgress;
      const bool peerPending = peekFtRestartRequest();
      if (!uploadActive && !peerPending && freeNow < kHeapLowFloor) {
        if (heapLowStreakStartMs_ == 0) {
          heapLowStreakStartMs_ = nowMs;
        }
        const uint32_t streakMs = nowMs - heapLowStreakStartMs_;
        const bool minEntryElapsed = (activityEnteredAtMs_ != 0 &&
                                      (nowMs - activityEnteredAtMs_) >= kMinTimeSinceEntryMs);
        if (streakMs >= kHeapLowStreakMs) {
          if (!minEntryElapsed) {
            LOG_DBG("WEBACT",
                    "Heap watchdog: free=%u < %u for %u ms; skipping (min time since entry not elapsed)",
                    freeNow, kHeapLowFloor, streakMs);
          } else {
            LOG_INF("WEBACT",
                    "Heap watchdog tripped: free=%u < %u for %u ms -- silent-restart to FT",
                    freeNow, kHeapLowFloor, streakMs);
            setSilentRebootFtModeHint(isApMode ? 2u : 1u);
            extern uint32_t g_ftHtmlServeLowHeapRestart;
            g_ftHtmlServeLowHeapRestart = 1;
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
        }
      } else {
        heapLowStreakStartMs_ = 0;  // heap recovered
      }
    }

    // v18.9.9.417: fragmentation-during-upload watchdog. The heap-low block
    // above deliberately gates out `uploadActive` because we don't want to
    // interrupt a healthy in-progress upload. But that leaves a gap: an
    // upload can be alive (WS accepting connections, free heap fine) while
    // maxAlloc is chronically fragmented -- every BIN frame dips below
    // v402's 6 KB hard floor and gets refused, so the browser retries
    // forever at ~10-20 s per file. Field-observed: uploading Harry Potter
    // CJK, ~700 small files, ~85% of them got 3-10 retries each, taking
    // hours instead of minutes. maxAlloc sat at 11764 for 1000+ seconds
    // with no automatic recovery.
    // Fix: if maxAlloc stays below 15 KB for 60+ seconds during an active
    // upload, silent-restart to FT. Browser's WS retry + server RESUME:N
    // continues from the fsync'd offset on the fresh boot's clean heap.
    // Threshold 15 KB is chosen so the trigger doesn't fire under normal
    // upload heap use (v417 field data showed 17-22 KB maxAlloc in a
    // healthy CJK upload run); a sustained dip below indicates true
    // fragmentation, not transient allocation.
    {
      // v18.9.9.421: tightened from 15 KB / 60 s to 18 KB / 30 s. Field
      // observation: maxAlloc drifted at 10-17 KB for the entire upload
      // and never recovered -- 60 s of "wait for recovery" was pure
      // wasted time before we finally silent-restarted. At 18 KB / 30 s
      // we catch chronic fragmentation twice as fast, and the 18 KB
      // threshold sits between the WS BIN floor (20 KB, v421) and the
      // observed crash zone (17 KB) so we defrag BEFORE the floor
      // starts refusing frames.
      constexpr uint32_t kUploadFragMaxAllocFloor = 18u * 1024u;
      constexpr uint32_t kUploadFragStreakMs = 30u * 1000u;
      const uint32_t nowMs = millis();
      const bool uploadActive = webServer && webServer->getWsUploadStatus().inProgress;
      if (uploadActive) {
        const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
        if (maxAllocNow < kUploadFragMaxAllocFloor) {
          if (uploadFragStreakStartMs_ == 0) {
            uploadFragStreakStartMs_ = nowMs;
          }
          const uint32_t streakMs = nowMs - uploadFragStreakStartMs_;
          if (streakMs >= kUploadFragStreakMs) {
            LOG_INF("WEBACT",
                    "Upload-fragmentation watchdog tripped: maxAlloc=%u < %u for %u ms during upload -- silent-restart to FT for defrag",
                    maxAllocNow, kUploadFragMaxAllocFloor, streakMs);
            setSilentRebootFtModeHint(isApMode ? 2u : 1u);
            extern uint32_t g_ftHtmlServeLowHeapRestart;
            g_ftHtmlServeLowHeapRestart = 1;
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
        } else {
          uploadFragStreakStartMs_ = 0;
        }
      } else {
        uploadFragStreakStartMs_ = 0;
      }
    }

    // v18.9.9.384: post-large-upload defrag restart. WS DONE handler arms
    // this after a >= 1 MB upload finishes and heap is fragmented. Uses
    // the same silent-restart-to-FT path as the low-heap watchdog above,
    // but timed to fire AFTER the DONE TXT frame has flushed to the browser
    // (1500ms delay armed in the handler). Browser's post-upload workflow
    // reconnects via its retry loop and lands on the fresh boot's clean heap.
    // v18.9.9.394: idle-restart. If no HTTP request or WS event has arrived
    // for kFtIdleRestartMs while an upload is NOT in progress, silent-restart
    // to FT. Catches states the passive low-heap watchdog can't see:
    //   - WS accept path wedged (stale client slot from a prior restart) but
    //     HTTP still healthy, so heap is fine and the watchdog never trips.
    //   - Slow heap creep from cumulative HTTP asset serves that never dips
    //     below the critical floor but leaves us with maxAlloc ~16 KB.
    //   - Browser tab closed silently -- next entry lands on a clean boot.
    // Gates: skip during upload (wsUploadInProgress), and require the user
    // to have been in FT long enough that a restart doesn't just re-loop
    // (kMinTimeSinceEntryMs already enforced by the heap watchdog block above
    // -- reuse activityEnteredAtMs_ here so the two share the same floor).
    {
      constexpr uint32_t kFtIdleRestartMs = 15u * 60u * 1000u;  // 15 min
      constexpr uint32_t kFtWsWedgeRestartMs = 3u * 60u * 1000u;  // 3 min
      constexpr uint32_t kMinUptimeForIdleRestart = 60u * 1000u;
      extern uint32_t g_ftLastActivityAtMs;
      extern uint32_t g_ftLastWsEventAtMs;
      const uint32_t now = millis();
      // v18.9.9.398: WS-wedge detector. Fires when the browser is actively
      // hitting HTTP endpoints (g_ftLastActivityAtMs is recent) but no WS
      // event has landed in 3 min. Real-world case: browser has stale
      // pre-v395 JS + probes /api/status on every WS retry (keeping the
      // 15-min idle timer forever reset), while every WS handshake times
      // out at 30 s. Silent-restart-to-FT clears whatever wedge is blocking
      // the WS accept (stale client slot / LWIP state / anything similar).
      const bool httpRecent = (g_ftLastActivityAtMs != 0 &&
                               (now - g_ftLastActivityAtMs) < 60u * 1000u);
      if (!isFtUploadInProgress() &&
          activityEnteredAtMs_ != 0 &&
          (now - activityEnteredAtMs_) >= kMinUptimeForIdleRestart &&
          httpRecent &&
          g_ftLastWsEventAtMs != 0 &&
          (now - g_ftLastWsEventAtMs) >= kFtWsWedgeRestartMs) {
        LOG_INF("WEBACT",
                "WS-wedge detected: HTTP active but no WS event for %u ms; silent-restart to FT",
                now - g_ftLastWsEventAtMs);
        setSilentRebootFtModeHint(isApMode ? 2u : 1u);
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
      if (!isFtUploadInProgress() &&
          activityEnteredAtMs_ != 0 &&
          (now - activityEnteredAtMs_) >= kMinUptimeForIdleRestart &&
          g_ftLastActivityAtMs != 0 &&
          (now - g_ftLastActivityAtMs) >= kFtIdleRestartMs) {
        LOG_INF("WEBACT",
                "Idle-restart firing: no FT activity for %u ms (>= %u); silent-restart to FT to refresh heap",
                now - g_ftLastActivityAtMs, static_cast<unsigned>(kFtIdleRestartMs));
        setSilentRebootFtModeHint(isApMode ? 2u : 1u);
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
    }

    if (consumeFtDefragRestartIfDue()) {
      LOG_INF("WEBACT",
              "Post-upload defrag restart firing: free=%u maxAlloc=%u -- silent-restart to FT",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      setSilentRebootFtModeHint(isApMode ? 2u : 1u);
      extern uint32_t g_ftHtmlServeLowHeapRestart;
      g_ftHtmlServeLowHeapRestart = 1;
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

    // CrumBLE 4.5.2: WS upload DONE flagged the library as needing a
    // refresh. Wait until no upload is in progress (so an N-book upload
    // burst triggers ONE walk, not N), then markStale + ensureWalked.
    // The walk picks up new files AND runs populateAuthorKeysIfNeeded
    // (which reads the WASM prebake's book.bin author or falls back to
    // OPF peek for sideloaded EPUBs). Net result: Sort by Author works
    // on the new book the moment it lands in Home, without the user
    // needing to open it first.
    if (consumePendingLibraryRefreshRequest()) {
      // The flag was set when the last WS upload's DONE handler ran, which
      // also sets wsUploadInProgress=false in the same callback -- so by
      // the time we consume here, the upload has already settled. If a NEW
      // upload sneaks in between consume and walk, the walk picks up that
      // file too (incremental SD scan) and the next upload's DONE re-sets
      // the flag, which we'll consume again next iteration. Safe to run
      // the walk unconditionally.
      //
      // CrumBLE 4.5.4: heap-aware gating. After a sequence of WS upload +
      // SD font fetch + chapter prebake, heap is typically heavily
      // fragmented with maxAlloc in the 12-22 KB range. populateAuthorKeys
      // (which the walk drives) opens each EPUB's ZIP + parses content.opf
      // -- the ZIP open alone needs ~30 KB contiguous for the inflate
      // dictionary. Under-heap, that throws std::bad_alloc and crashes
      // the whole device (field log: PANIC right after 'Upload settled'
      // following a 4.3 MB SD font fetch). Skip the walk and re-set the
      // pending flag so the NEXT loop iteration retries. Heap recovers
      // as transient WS / WiFi state drains, usually within seconds.
      // CrumBLE 4.5.4: heap-aware deferring + attempts cap.
      //   - Threshold: 30 KB (was 35 KB). The original 35 KB was set without
      //     evidence; field log shows the post-upload heap pinned at 5-8 KB
      //     maxAlloc for minutes, never crossing 35 KB. 30 KB is the actual
      //     ZIP-inflate dictionary need + 2 KB safety.
      //   - Cap: after kMaxDeferAttempts retries (~30 s wall clock at 1 Hz),
      //     just give up. The walk will run on next boot's natural startup
      //     instead. New book stays at the bottom of Sort-by-Author until
      //     then -- a minor UX miss, far better than the field log spam.
      //   - Self-debounce: only one deferred-log line per second (instead
      //     of the 2 Hz tick rate) so the serial log stays readable.
      constexpr uint32_t kMinMaxAllocForWalk = 30 * 1024;
      constexpr uint32_t kMaxDeferAttempts = 60;
      constexpr uint32_t kDeferLogIntervalMs = 1000;
      static uint32_t deferAttempts = 0;
      static uint32_t lastDeferLogMs = 0;
      const uint32_t curMaxAlloc = ESP.getMaxAllocHeap();
      if (curMaxAlloc < kMinMaxAllocForWalk) {
        deferAttempts++;
        if (deferAttempts > kMaxDeferAttempts) {
          LOG_INF("WEBACT",
                  "Upload settled -- GIVING UP on library re-walk after %u attempts (maxAlloc still %u); "
                  "new book will pick up author keys on next boot",
                  static_cast<unsigned>(kMaxDeferAttempts), curMaxAlloc);
          deferAttempts = 0;
          // Do NOT re-request: caller has accepted the loss.
        } else {
          if (millis() - lastDeferLogMs >= kDeferLogIntervalMs) {
            LOG_INF("WEBACT",
                    "Upload settled -- DEFERRING library re-walk (maxAlloc=%u < %u, attempt %u/%u)",
                    curMaxAlloc, static_cast<unsigned>(kMinMaxAllocForWalk), static_cast<unsigned>(deferAttempts),
                    static_cast<unsigned>(kMaxDeferAttempts));
            lastDeferLogMs = millis();
          }
          requestLibraryRefresh();
        }
      } else {
        LOG_INF("WEBACT", "Upload settled -- re-walking library to refresh author keys (maxAlloc=%u, attempts=%u)",
                curMaxAlloc, static_cast<unsigned>(deferAttempts));
        deferAttempts = 0;
        LibraryIndex::getInstance().markStale();
        LibraryIndex::getInstance().ensureWalked();
      }
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
      // v18.9.9.86: loop-break if we already silent-restarted this cycle and
      // are still failing -- restarting again just loops if we can't make ANY
      // progress. v18.9.9.169 refinement: allow the restart when we HAVE made
      // progress since the last restart (>= 1 large serve completed). That's
      // the prebake-WASM-after-serving-optimizer.js case -- the "loop" heuristic
      // was too coarse and blocked recovery when the heap was legitimately
      // fresh enough to serve intermediate assets but couldn't sustain the
      // 1.3 MB WASM. If NO large serve completed, we ARE stuck and should stop.
      extern uint32_t g_ftLargeServesSinceRestart;
      const uint32_t progress = g_ftLargeServesSinceRestart;
      if (justLowHeapRestarted_ && progress == 0) {
        LOG_ERR("WEBACT",
                "html-serve low-heap AFTER a prior silent-restart AND no progress since (free=%u maxAlloc=%u); "
                "not restarting again to avoid loop -- user should back out to Home",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return;
      }
      if (justLowHeapRestarted_) {
        LOG_INF("WEBACT",
                "html-serve low-heap AFTER a prior silent-restart but %u large serve(s) completed since -- "
                "restart allowed (heap can recover post-boot)",
                progress);
      }
      LOG_INF("WEBACT", "Auto-recovery: silentRestart to FT (free=%u maxAlloc=%u)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      // v18.9.9.86: preserve the mode hint so post-boot still auto-connects
      // to WiFi (user doesn't have to re-select JOIN_NETWORK). Latching the
      // low-heap-recovery state uses g_ftHtmlServeLowHeapRestart (below), a
      // separate RTC_NOINIT flag consulted in onEnter — that way this second
      // hit path can bail without stomping the auto-restore behavior.
      setSilentRebootFtModeHint(isApMode ? 2u : 1u);
      extern uint32_t g_ftHtmlServeLowHeapRestart;
      g_ftHtmlServeLowHeapRestart = 1;  // consumed in onEnter (v18.9.9.86 loop-break)
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

  // v18.9.9.300: on-device weak-WiFi warning. Matches the web UI banner
  // that fires below -70 dBm. Without this the device UI silently shows
  // "connected" while file uploads stall or truncate mid-transfer.
  // Threshold -70 dBm matches the browser-side banner threshold so
  // users get consistent signal from both the phone and the device.
  if (!isApMode && lastWifiRssi < 0 && lastWifiRssi > -100 && lastWifiRssi < -70) {
    char warning[80];
    snprintf(warning, sizeof(warning),
             "Weak WiFi (%d dBm). Move closer to router.", lastWifiRssi);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, warning,
                       true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing;
  }
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
