#include "BluetoothHIDManager.h"
#include <algorithm>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <esp_system.h>  // esp_get_free_heap_size (NimBLE-enable heap gate)

#if defined(ARDUINO) && __has_include(<esp32-hal-bt-mem.h>)
// Arduino-ESP32 3.x releases BT controller memory during startup unless a
// Bluetooth library marks it as in use before app_main(). NimBLE-Arduino does
// not do that automatically in this build, which can crash later in
// `NimBLEDevice::init()` / `esp_bt_controller_init()` when Bluetooth is enabled
// from the settings UI on ESP32-C3. Pulling in this header sets the core's
// `_btLibraryInUse` flag early via a constructor and keeps BLE memory reserved.
#include <esp32-hal-bt-mem.h>
#endif

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";
static const char* HID_INFO_UUID = "2A4A";
static const char* HID_REPORT_MAP_UUID = "2A4B";
static const char* HID_PROTOCOL_MODE_UUID = "2A4E";

static constexpr uint8_t GAMEBRICK_ACTION_A_CODE = 0xF1;
static constexpr uint8_t GAMEBRICK_ACTION_B_CODE = 0xF2;

// v18.9.9.195: NimBLE bond-store diagnostic. Reason-520 MIC-failure on first
// connect per session is masked by v21's one-shot reconnect. To identify the
// real cause we need to see what NimBLE has persisted at each boot/enable and
// whether the bond record changes across a fail-then-succeed reconnect pair.
// CONFIG_BT_NIMBLE_NVS_PERSIST=1 in nimconfig.h is on by default, so bonds
// SHOULD survive a cold boot -- this log tells us whether they actually do.
static void logBondStoreDiag(const char* tag) {
  const int n = NimBLEDevice::getNumBonds();
  LOG_INF("BTBOND", "%s: %d bond(s) persisted", tag, n);
  for (int i = 0; i < n; ++i) {
    NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
    LOG_INF("BTBOND", "  [%d] addr=%s type=%u", i,
            addr.toString().c_str(), (unsigned)addr.getType());
  }
}

namespace {
// BLE intervals are in 1.25ms units and timeout is in 10ms units.
// Keep latency at 0 for low input lag while allowing a longer supervision timeout
// to reduce disconnects at marginal range.
constexpr uint16_t BLE_CONN_MIN_INTERVAL = 12;   // 15ms
constexpr uint16_t BLE_CONN_MAX_INTERVAL = 24;   // 30ms
constexpr uint16_t BLE_CONN_LATENCY = 0;
constexpr uint16_t BLE_CONN_TIMEOUT = 600;       // 6s
constexpr uint16_t BLE_CONN_SCAN_INTERVAL = 60;
constexpr uint16_t BLE_CONN_SCAN_WINDOW = 30;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 10000;
constexpr unsigned long FREE2_STALE_RELEASE_DEFAULT_MS = 250;
constexpr unsigned long FREE2_STALE_RELEASE_READER_MS = 500;
}

struct ReportMapHints {
  bool hasConsumerPage = false;
  bool hasKeyboardPage = false;
  uint8_t preferredByteIndex = 0xFF;
};

struct ExtractedHIDKey {
  uint8_t keycode = 0x00;
  uint8_t reportIndex = 0xFF;
};

static ExtractedHIDKey extractGenericPageTurnKeycode(const uint8_t* report, size_t length) {
  ExtractedHIDKey result;

  if (!report || length == 0) {
    return result;
  }

  // First pass: prefer known page-turn keycodes anywhere in short reports.
  const size_t scanLen = length < 8 ? length : 8;
  for (size_t i = 0; i < scanLen; i++) {
    const uint8_t code = report[i];
    if (DeviceProfiles::isCommonPageTurnCode(code)) {
      result.keycode = code;
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  // Second pass: typical keyboard report key slots (bytes 2..7)
  for (size_t i = 2; i < scanLen; i++) {
    if (report[i] != 0x00) {
      result.keycode = report[i];
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  // Final fallback for non-keyboard HID layouts: first non-zero byte.
  for (size_t i = 0; i < scanLen; i++) {
    if (report[i] != 0x00) {
      result.keycode = report[i];
      result.reportIndex = static_cast<uint8_t>(i);
      return result;
    }
  }

  return result;
}

static uint8_t classifyFree2Direction(const uint8_t keycode) {
  if (keycode == DeviceProfiles::FREE2_FORWARD_A || keycode == DeviceProfiles::FREE2_FORWARD_B ||
      keycode == DeviceProfiles::FREE2_FORWARD_C || keycode == DeviceProfiles::FREE2_FORWARD_D) {
    return 0x01;
  }

  if (keycode == DeviceProfiles::FREE2_BACK_A || keycode == DeviceProfiles::FREE2_BACK_B ||
      keycode == DeviceProfiles::FREE2_BACK_C || keycode == DeviceProfiles::FREE2_BACK_D) {
    return 0x00;
  }

  return 0xFF;
}

static bool isFree2Profile(const DeviceProfiles::DeviceProfile* profile) {
  if (profile == nullptr || profile->name == nullptr) {
    return false;
  }

  return strcmp(profile->name, "Free2-M") == 0 || strcmp(profile->name, "Free2 Style") == 0;
}

static ReportMapHints parseReportMapHints(const std::string& map) {
  ReportMapHints hints;
  if (map.empty()) {
    return hints;
  }

  for (size_t i = 0; i + 1 < map.size(); i++) {
    const uint8_t b = static_cast<uint8_t>(map[i]);
    const uint8_t next = static_cast<uint8_t>(map[i + 1]);

    // Usage Page (1 byte value)
    if (b == 0x05) {
      if (next == 0x0C) {
        hints.hasConsumerPage = true;
      } else if (next == 0x07) {
        hints.hasKeyboardPage = true;
      }
    }
  }

  // Heuristic preferred byte index:
  // keyboard-like reports commonly place keycode at byte[2], consumer-control
  // reports are often compact and keycode-like values appear at byte[1].
  if (hints.hasKeyboardPage) {
    hints.preferredByteIndex = 2;
  } else if (hints.hasConsumerPage) {
    hints.preferredByteIndex = 1;
  }

  return hints;
}

// Global static for singleton
static BluetoothHIDManager* g_instance = nullptr;

// Scan callbacks for NimBLE 2.x - keep as static to ensure it stays alive
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (g_instance) {
      // onScanResult expects non-const pointer, need to cast
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    } else {
      LOG_ERR("BT", "onResult called but g_instance is NULL!");
    }
  }
  
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    // Async scan finished: clear _scanning so the UI shows results.
    if (g_instance) {
      g_instance->onScanComplete(reason);
    }
  }
};

// Static instance to keep callbacks alive during scan
static ScanCallbacks scanCallbacks;

// Client connection callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    LOG_INF("BT", "Client connected: %s", pClient->getPeerAddress().toString().c_str());
  }
  
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    LOG_ERR("BT", "Client disconnected: %s (reason: %d)", pClient->getPeerAddress().toString().c_str(), reason);
    BluetoothHIDManager::getInstance().noteClientDisconnect(reason);
  }
};

BluetoothHIDManager& BluetoothHIDManager::getInstance() {
  if (!g_instance) {
    g_instance = new BluetoothHIDManager();
    LOG_INF("BT", "BluetoothHIDManager instance created");
  }
  return *g_instance;
}

BluetoothHIDManager::BluetoothHIDManager() {
  LOG_DBG("BT", "BluetoothHIDManager constructor");
}

BluetoothHIDManager::~BluetoothHIDManager() {
  cleanup();
}

void BluetoothHIDManager::cleanup() {
  if (_enabled) {
    disable();
  }
}

bool BluetoothHIDManager::enable() {
  SET_CHECKPOINT("bt:enable-entry");
  if (_enabled) {
    LOG_DBG("BT", "Already enabled");
    return true;
  }

  // v18.9.9.252: refuse if the boot BT-off release (main.cpp v245 branch,
  // or the FT-enter release) already released the BLE controller memory
  // this boot. esp_bt_mem_release is one-way per boot -- once fired, no
  // path in this boot can esp_bt_controller_init() again without
  // load-faulting inside bt_controller_deinit_internal's queue walk (the
  // v251 QC-crash class). Setting lastError here routes callers through
  // their normal enable-failure fallback, which silent-restarts to a
  // fresh boot with bluetoothEnabled=1 -- the v245 branch skips the
  // release, and enable() succeeds naturally on that boot.
  extern bool g_bleControllerMemReleased;
  if (g_bleControllerMemReleased) {
    LOG_ERR("BT", "Refusing to enable Bluetooth: BLE controller memory released this boot -- restart needed");
    lastError = "BT released this boot; restart needed";
    return false;
  }

  // Refuse to bring NimBLE up without enough free heap for it (controller + host
  // are ~58 KB). Initializing below that threshold previously HUNG the device:
  // esp_bt_controller_init() can't get its memory and the task blocks (seen after
  // a low-memory chapter rebuild re-enabled BLE at ~56 KB free). Callers treat a
  // false return as "stayed off"; the user can retry once heap recovers (e.g. on
  // a lighter page or from the reader's Bluetooth menu).
  // CrumBLE 4.3 note: tried lowering to 60 KB so a pre-enable warm page
  // cache could hold the ~18 KB Page DOM through enable. NimBLE actually
  // needs MORE than 60 KB at the controller-init step (device hangs at
  // "Enabling Bluetooth..." with no further output), so the floor stays
  // at 66 KB. The warm-before-enable path is reverted accordingly.
  // v18.9.9.202: restored to 66 KB after reverting v199's dictionary picker
  // (which cost ~150 B static + settings-view cache growth on X3 and forced
  // v201's 65 KB stopgap). With the picker gone, X3 is back within the
  // documented safe range.
  // v18.9.9.281: was 66 KB, tuned to pre-v280 NimBLE that consumed ~55 KB
  // at init. v280's controller + host pool trims (BT_CTRL_BLE_MAX_ACT
  // 6->1, MSYS 12/24 -> 3/6, ACL 24->2, EVT 30->4, MAX_CONNECTIONS 3->1,
  // HOST_TASK_STACK 5120->2560, ATT_MAX_PREP 64->2) cut init cost to
  // ~20 KB. 30 KB gives ~10 KB safety margin. Symptom of an over-
  // aggressive floor: quick-connect refused with "free heap X < Y" while
  // X is comfortably above the actual runtime cost (v280 user report:
  // refused at 67216 free vs 67584 gate -- 368 bytes short of a value
  // that was overkill by 35 KB post-shrink).
  constexpr uint32_t kMinFreeHeapForEnable = 30 * 1024;
  // CrumBLE 4.5.5: contiguous-heap floor for the ESP-IDF BT controller init.
  // History: a 4.5.5 dev build set this at 50 KB after a one-off crash with
  // free=76900 / maxAlloc=45044 (controller_init failed + rollback double-
  // freed a semaphore, esp-idf bug). Field testing showed 50 KB refused on
  // a perfectly normal in-book quick-connect at maxAlloc=47092 -- a state
  // CrumBLE 4.5.4 and earlier handled fine without any floor. Lowered to
  // 40 KB which still gives controller_init the contiguous space it
  // typically needs while not blocking the steady-state reader case.
  // Trade-off: ~5 KB closer to the documented crash point. If we see a
  // recurrence, raise back toward 45-48 KB or pair with a defrag step.
  // v18.9.9.281: was 40 KB. Same reasoning as kMinFreeHeapForEnable --
  // controller_init's biggest contiguous chunk was the BLE_MAX_ACT
  // slot table (~15 KB at MAX_ACT=6). v280 shrunk MAX_ACT to 1, so
  // the largest contiguous demand is now the NimBLE host task stack
  // (2560 B) or ACL pool blocks; 22 KB stays well above either.
  constexpr uint32_t kMinMaxAllocForEnable = 22 * 1024;
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap < kMinFreeHeapForEnable) {
    LOG_ERR("BT", "Refusing to enable Bluetooth: free heap %u < %u needed for NimBLE", freeHeap,
            static_cast<unsigned>(kMinFreeHeapForEnable));
    lastError = "Not enough memory to enable Bluetooth";
    return false;
  }
  if (maxAlloc < kMinMaxAllocForEnable) {
    LOG_ERR("BT", "Refusing to enable Bluetooth: maxAlloc %u < %u needed for controller_init", maxAlloc,
            static_cast<unsigned>(kMinMaxAllocForEnable));
    lastError = "Memory fragmented; restart needed";
    return false;
  }

  LOG_INF("BT", "Enabling Bluetooth...");
  // CrumBLE 4.4 post-bisect: fresh auto-reconnect budget per BT cycle.
  _autoReconnectConsumedThisCycle = false;
  _autoReconnectPending = false;

  // CRITICAL: Disable WiFi when enabling Bluetooth
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BT", "Disabling WiFi to enable Bluetooth (mutual exclusion)");
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }
  
  // CrumBLE 4.5.5: clamp CPU to normal speed across NimBLE init. The
  // controller-init step takes the interrupt-WDT path that hangs at the
  // 10 MHz low-power frequency -- field reports (and upstream CrossPoint's
  // feat-bluetooth investigation) trace one class of "device freezes when
  // turning on Bluetooth" to this. The Lock scopes itself off as soon as
  // init returns; if power-saving was the active mode before, it resumes
  // on the next loop tick.
  {
    HalPowerManager::Lock powerLock;
    NimBLEDevice::init("CrossPoint");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // +9dBm
    NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);
    NimBLEDevice::setSecurityAuth(true, false, true);
    // v18.9.9.195: dump bond store immediately after init so we can see what
    // survived the previous power-off. Expected pattern: cold boot after a
    // successful bond => 1 entry (the Free3-R remote). If 0 shows up here on
    // a cold boot, NVS bond persistence is broken (root cause of reason 520).
    logBondStoreDiag("post-NimBLE-init");
  }

  _enabled = true;
  // Deliberate enable (BT menu entry, quick-connect) is an explicit "try now":
  // never make the user wait out a backoff earned by earlier failures.
  resetAutoReconnectBackoff();
  // v18.9.9.48: successful init clears the skip-teardown flag so callers
  // don't keep silent-restarting after we've re-entered a clean state.
  _nimbleStateSkippedTeardown = false;
  lastError = "";

  LOG_INF("BT", "Bluetooth enabled successfully");
  loadState();
  return true;
}

bool BluetoothHIDManager::tryEnableIfRequested() {
  if (!_enableLaterRequested) return false;
  SET_CHECKPOINT("bt:tryEnableIfRequested");
  if (_enabled) {
    _enableLaterRequested = false;  // someone else already brought it back up
    _enableLaterFirstAttemptMs = 0;
    _enableLaterLastAttemptMs = 0;
    return false;
  }

  // CrumBLE 4.5.6: rate-limit + give-up. Main loop calls this every tick.
  // Without the rate limit, a refused enable() spammed the log at ~10 ms
  // intervals. Without a give-up, a book whose steady-state heap can't fit
  // NimBLE (37 KB free vs 67 KB needed) would spin forever after the reader
  // fell to streaming glyphs post-BT-cycle.
  constexpr unsigned long kMinRetryIntervalMs = 500;
  constexpr unsigned long kGiveUpAfterMs = 6000;
  const unsigned long now = millis();
  if (_enableLaterLastAttemptMs != 0 && (now - _enableLaterLastAttemptMs) < kMinRetryIntervalMs) {
    return false;
  }
  if (_enableLaterFirstAttemptMs == 0) {
    _enableLaterFirstAttemptMs = now;
  }
  _enableLaterLastAttemptMs = now;

  if (!enable()) {
    // Rate-limited retry so a borderline miss (heap recovers in ~1 s) still
    // reconnects, but a hopeless case (post-heavy-book render) gives up cleanly.
    if ((now - _enableLaterFirstAttemptMs) >= kGiveUpAfterMs) {
      LOG_INF("BT",
              "tryEnableIfRequested: giving up after %lu ms of refused enable() -- heap can't fit NimBLE"
              " (user can re-enable manually from BT menu)",
              now - _enableLaterFirstAttemptMs);
      _enableLaterRequested = false;
      _enableLaterFirstAttemptMs = 0;
      _enableLaterLastAttemptMs = 0;
      // v18.9.6c: signal give-up so callers (reader) can escalate to a
      // silent-restart-with-EnableBt. One-shot; cleared on takeEnableGaveUpAlert.
      _enableGaveUpAlertPending = true;
    } else {
      LOG_DBG("BT",
              "tryEnableIfRequested: enable() deferred (%s); will retry (elapsed=%lu ms)",
              lastError.c_str(), now - _enableLaterFirstAttemptMs);
    }
    return false;
  }
  _enableLaterRequested = false;
  _enableLaterFirstAttemptMs = 0;
  _enableLaterLastAttemptMs = 0;

  // checkAutoReconnect() gates on a local button press because in its usual
  // "remote got out of range / disconnected on its own" scenario the
  // firmware can't tell whether the user is actively reading or has set
  // the device down. But this drain path runs after a programmatic
  // disable() that the user didn't ask for (we dropped BLE around a
  // heap-heavy re-layout). They expect the remote to resume without
  // having to wake the reconnect logic with a local button mash, so do
  // the connect ourselves the same way the reader-menu's manual
  // "Reconnect bonded" entry would. _bondedDeviceAddress survives the
  // disable()/enable() cycle (just a std::string on the singleton; we
  // don't clear it in disable()), so it's still valid here.
  if (_bondedDeviceAddress.empty()) {
    LOG_DBG("BT", "tryEnableIfRequested: no bonded device cached; nothing to reconnect");
    return true;
  }
  LOG_INF("BT", "tryEnableIfRequested: reconnecting to bonded device %s", _bondedDeviceAddress.c_str());
  if (!connectToDevice(_bondedDeviceAddress)) {
    // Not fatal — the remote might be powered off or out of range. The
    // user can still hit a local button later to trigger
    // checkAutoReconnect()'s retry path, or pop into the BT settings
    // menu and tap Reconnect manually.
    LOG_INF("BT", "tryEnableIfRequested: bonded reconnect failed (%s); falling back to user-input retry",
            lastError.c_str());
  }
  return true;
}

bool BluetoothHIDManager::disable() {
  if (!_enabled) {
    LOG_DBG("BT", "Already disabled");
    return true;
  }

  LOG_INF("BT", "Disabling Bluetooth...");

  if (_scanning) {
    stopScan();
  }

  // v18.9.9.293: user-initiated disable also cancels any pending auto-
  // reconnect and clears the connection-lost alert flag. Previously an
  // async link drop (e.g. remote fell asleep) could queue the alert
  // between the last render and the disable click, so the user would
  // press "Turn off BT" in settings and STILL see "Bluetooth couldn't
  // stay connected" pop up on the way back to Home. The alert is only
  // meaningful when BT is still enabled and trying to hold the link.
  _autoReconnectPending = false;
  _connectionLostAlertPending = false;

  // CrumBLE 4.5.5: REVERTED to the pre-4.5.5 disable path. Two earlier
  // attempts in this version (the wait-for-DISCONNECTED + explicit
  // deleteClient, then the wait-without-deleteClient) both crashed in
  // NimBLEClient::~NimBLEClient -> std::vector dtor -> heap_caps_free assert
  // when called against the user's actively-connected Free3-R/M remote.
  // The simple disconnect loop + deinit(true) was the proven-working pattern
  // (shipped through 4.5.4) and the user reports field-stable behavior on it.
  // The hypothetical m_pClients slot leak the 4.3 comment described is the
  // lesser evil vs an actual crash on every BT-menu Back press.
  // v18.9.9.47 (task #32): capture whether we needed to actively drop a
  // live connection or whether _connectedDevices was already empty. The
  // "already empty" case happens when the remote (or our own idle-timeout
  // logic) had disconnected earlier and the deferred async cleanup didn't
  // finish before disable() ran. That's the scenario where NimBLE's
  // internal registry still holds stale client state that deinit(true)
  // then tries to free -- crashing in heap_caps_free on a scrambled
  // pointer. The mitigation below uses deinit(false) for that path so
  // stale objects aren't touched.
  const bool hadActiveConnection = !_connectedDevices.empty();
  while (!_connectedDevices.empty()) {
    disconnectFromDevice(_connectedDevices[0].address);
  }

  // CrumBLE 4.3: full deinit (clearAll=true) so NimBLE deletes ALL client
  // objects, services, and bonded-but-disconnected state along with the
  // stack itself. Previously we passed false to preserve client objects for
  // a "fast reconnect" path on the next enable -- but under repeated
  // book-switch + BT-cycle pressure those retained NimBLEClient instances
  // accumulated ~2 KB of stale state per session AND the next enable's
  // reconnect-with-existing-client attempt could fail mid-subscribe with
  // `E (NNN) BLE_INIT: Malloc failed` (NimBLE running out of mbufs during
  // characteristic enumeration), then take ~22 seconds to surrender to the
  // retry-with-fresh-client fallback -- by which point the heap was so
  // drained that the section deserialize floor refused to load the page
  // and we showed "Page load error". With deinit(true) every enable now
  // starts from a clean NimBLE state; the cost is ~50 ms extra on each
  // connectToDevice() because we always go through createClient() instead
  // of getClientByPeerAddress(). The reconnect-with-existing-client path
  // in connectToDevice() still works for the very first connect after a
  // single enable (e.g. lost-link auto-recovery without a disable in
  // between), it just never sees a stale client carried across an
  // explicit disable boundary.
  //
  // CrumBLE 4.5.5: same power lock as enable(). NimBLE deinit hits the
  // same controller path that hangs the interrupt WDT at 10 MHz, so
  // clamp to normal CPU around it. RAII releases as soon as deinit
  // returns; power-saving resumes on the next loop tick.
  //
  // Also ported from upstream: retry deinit once if isInitialized() is
  // still true after the first call. Stop/scan races can leave the host
  // partially initialised, and a no-op enable() afterwards is then
  // running on a half-torn-down stack.
  {
    // v18.9.9.285: try deinit(false) -- the ONE deinit path that skips the
    // client std::vector destruction that crashed EVERY previous attempt
    // (v18.9.9.34/44/46/47/282 with deinit(true), and CrumBLE 4.5.5's two
    // deleteClient variants which invoke the same ~NimBLEClient destructor).
    // deinit(false) tears down the NimBLE host stack + controller but leaves
    // NimBLEClient objects allocated on the C++ heap. Cost: ~2 KB per
    // enable/disable cycle leaks (accumulates until reboot). Benefit: we
    // recover ~30 KB of host-stack RAM instead of holding it hostage until
    // silent-restart. If this ALSO crashes in heap_caps_free, revert to
    // skip-deinit in the block below (currently commented out) and update
    // project-crumble-nimble-deinit-crash memory to strike this approach.
    const uint32_t freeBefore = ESP.getFreeHeap();
    const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
    LOG_INF("BT", "disable: attempting NimBLEDevice::deinit(false) (hadActiveConn=%d free=%u maxAlloc=%u)",
            hadActiveConnection ? 1 : 0, freeBefore, maxAllocBefore);

    const bool ok = NimBLEDevice::deinit(false);
    LOG_INF("BT", "disable: deinit(false) returned %d, isInitialized=%d",
            ok ? 1 : 0, NimBLEDevice::isInitialized() ? 1 : 0);

    _nimbleStateSkippedTeardown = false;
    const uint32_t freeAfter = ESP.getFreeHeap();
    const uint32_t maxAllocAfter = ESP.getMaxAllocHeap();
    LOG_INF("BT", "disable: post-deinit(false) free=%u (+%d) maxAlloc=%u (+%d)",
            freeAfter, static_cast<int>(freeAfter) - static_cast<int>(freeBefore),
            maxAllocAfter, static_cast<int>(maxAllocAfter) - static_cast<int>(maxAllocBefore));

    // Fallback path if the above ever regresses -- keep the code around
    // so a single-line revert restores the v283 skip-deinit behavior.
    // (void)hadActiveConnection;
    // _nimbleStateSkippedTeardown = true;
    // LOG_INF("BT", "disable: SKIPPED NimBLEDevice::deinit (fallback path)");
  }

  _enabled = false;
  lastError = "";

  LOG_INF("BT", "Bluetooth disabled");
  return true;
}

void BluetoothHIDManager::startScan(uint32_t durationMs) {
  if (!_enabled || _scanning) {
    LOG_DBG("BT", "Cannot scan: enabled=%d scanning=%d", _enabled, _scanning);
    return;
  }
  
  LOG_INF("BT", "Starting BLE scan for %lu ms", durationMs);
  _scanning = true;
  _discoveredDevices.clear();
  
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    LOG_ERR("BT", "Failed to get scan object");
    _scanning = false;
    lastError = "Scan failed";
    return;
  }
  
  // Use static callbacks object to ensure it stays alive
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  
  // Async: NimBLE auto-stops after durationMs (ms) and fires onScanEnd. Was a
  // blocking delay() that froze the UI for the whole scan. (false = clear results)
  bool started = pScan->start(durationMs, false);

  if (!started) {
    LOG_ERR("BT", "Failed to start scan!");
    _scanning = false;
    lastError = "Scan failed";
    return;
  }

  LOG_INF("BT", "Scan started (async, %lu ms)", durationMs);
}

void BluetoothHIDManager::stopScan() {
  if (!_scanning) return;
  
  LOG_INF("BT", "Stopping scan");
  
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan) {
    pScan->stop();
  }
  
  _scanning = false;
}

void BluetoothHIDManager::onScanResult(NimBLEAdvertisedDevice* advertisedDevice) {
  if (!advertisedDevice) return;

  // Check if device advertises HID service
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));

  // CrumBLE 4.5.5: HID-only scan filter (ported from upstream feat-bluetooth /
  // freeink-sdk BleKeyboardHost). The scan callback used to add every BLE
  // advertiser (phones, headphones, smart bulbs, AirTags...) to
  // _discoveredDevices with HID devices merely getting eviction priority.
  // The list still showed everything to the user, and finding the actual
  // page-turner among 8 noise entries was the consistent UX complaint. Now
  // non-HID advertisements are dropped at the callback boundary -- the
  // settings list only ever sees pairable remotes/keyboards.
  if (!isHID) return;

  std::string address = advertisedDevice->getAddress().toString();
  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();

  // Check if we already have this device
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi; // Update RSSI
      dev.isHID = true;
      return;
    }
  }

  // CrumBLE 4.4: cap discovered devices. On a busy RF environment we see 20+
  // results and the post-scan picker activity allocates a std::vector<std::
  // string> with a "name (addr) RSSI" entry per device -- on the X3's NimBLE-
  // squeezed heap (NimBLE eats ~58 KB), even 20 entries hit the picker's
  // grow-on-build with MaxAlloc < entry size and abort(). Cap at 12 so the
  // picker fits comfortably; weakest-RSSI entry is evicted to keep the most
  // promising ones. (4.5.5: now that non-HID is filtered out at the top, the
  // requireNonHid eviction-priority pass below is a no-op -- every retained
  // entry is HID. Kept as a safety net in case the filter is ever relaxed.)
  constexpr size_t MAX_SCAN_RESULTS = 12;
  if (_discoveredDevices.size() >= MAX_SCAN_RESULTS) {
    // Find weakest entry to evict. Prefer evicting non-HID over HID so a real
    // remote isn't dropped in favour of nearby phones/headphones.
    auto pickEvictionTarget = [this](bool requireNonHid) {
      auto worst = _discoveredDevices.end();
      int worstRssi = INT32_MAX;
      for (auto it = _discoveredDevices.begin(); it != _discoveredDevices.end(); ++it) {
        if (requireNonHid && it->isHID) continue;
        if (it->rssi < worstRssi) { worstRssi = it->rssi; worst = it; }
      }
      return worst;
    };
    auto victim = pickEvictionTarget(/*requireNonHid=*/true);
    if (victim == _discoveredDevices.end()) {
      victim = pickEvictionTarget(/*requireNonHid=*/false);
    }
    // Only evict if the new device beats the weakest -- otherwise just drop.
    if (victim == _discoveredDevices.end() || rssi <= victim->rssi) {
      LOG_DBG("BT", "Scan cap reached, dropping weaker new device RSSI=%d", rssi);
      return;
    }
    _discoveredDevices.erase(victim);
  }

  // Add new device
  BluetoothDevice device;
  device.address = address;
  device.name = name.empty() ? "Unknown" : name;
  device.rssi = rssi;
  device.isHID = isHID;

  _discoveredDevices.push_back(device);

  // Named devices first, then stronger RSSI; stable to avoid jitter as results stream in.
  std::stable_sort(_discoveredDevices.begin(), _discoveredDevices.end(),
                   [](const BluetoothDevice& a, const BluetoothDevice& b) {
                     const bool aNamed = a.name != "Unknown";
                     const bool bNamed = b.name != "Unknown";
                     if (aNamed != bNamed) return aNamed;  // named first
                     return a.rssi > b.rssi;               // stronger signal first
                   });

  LOG_DBG("BT", "Found device: %s (%s) RSSI:%d HID:%d",
          device.name.c_str(), device.address.c_str(), rssi, isHID);
}

void BluetoothHIDManager::onScanComplete(int reason) {
  _scanning = false;
  LOG_INF("BT", "Scan ended (reason=%d), found %d devices", reason,
          static_cast<int>(_discoveredDevices.size()));
}

bool BluetoothHIDManager::connectToDevice(const std::string& address) {
  if (!_enabled) {
    LOG_ERR("BT", "Cannot connect: Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    return false;
  }
  
  // Check if already connected
  if (isConnected(address.c_str())) {
    LOG_INF("BT", "Already connected to %s", address.c_str());
    return true;
  }
  
  LOG_INF("BT", "Connecting to device %s", address.c_str());
  
  NimBLEAddress bleAddress(address, BLE_ADDR_PUBLIC);

    // Reuse existing disconnected client objects to avoid NimBLE deleteClient() on this target.
    NimBLEClient* pClient = NimBLEDevice::getClientByPeerAddress(bleAddress);
    const bool hadExistingClient = (pClient != nullptr);
    if (!pClient) {
      pClient = NimBLEDevice::getDisconnectedClient();
      if (pClient) {
        pClient->setPeerAddress(bleAddress);
      }
    }
    if (!pClient) {
      pClient = NimBLEDevice::createClient(bleAddress);
    }

    if (!pClient) {
      lastError = "Failed to create BLE client";
      LOG_ERR("BT", "Failed to create BLE client");
      return false;
    }

    // Keep client lifetime under manager control so disconnect callbacks do not free it in NimBLE context.
    pClient->setSelfDelete(false, false);
    pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
    pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                                 BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);

    if (!pClient->isConnected()) {
      pClient->deleteServices();
    }
    
    // Set connection callbacks
    static ClientCallbacks clientCallbacks;
    pClient->setClientCallbacks(&clientCallbacks);
    
    // Connect to device. First attempt frequently times out for game-pad
    // peripherals that haven't been awake for a while: the controller is
    // still discovering / advertising and our scan-window misses it. User
    // pattern is reliably "first quick-connect fails, second one works".
    //
    // CrumBLE 4.4 task #28-follow: always retry at least once with a fresh
    // client + a small cool-down between attempts. The fresh-client path
    // also covers the existing-client-from-previous-session case (where
    // stale state in NimBLE's host stack rejects the reconnect).
    if (!pClient->connect(bleAddress)) {
      LOG_INF("BT", "Initial connect attempt failed for %s; retrying after cool-down", address.c_str());
      // ~300 ms cool-down: long enough for the controller to settle a
      // pending advertising window, short enough that the user doesn't
      // perceive a stall on the rare-but-real case where the first attempt
      // actually races RF noise rather than peripheral state. NimBLE
      // controller event loop keeps ticking through delay().
      delay(300);
      // v18.9.9.34 (task #20): retry on the SAME pClient rather than
      // creating a fresh one. The previous fresh-client fallback swapped
      // pClient without disposing the old object; since the old client
      // still had setSelfDelete(false, false) it stayed in NimBLE's
      // internal registry with dangling references to our static
      // ClientCallbacks. Later disable() -> NimBLEDevice::deinit(true)
      // walked the registry to tear down all clients and hit
      // heap_caps_free with a stale/scrambled pointer -- the crash we saw
      // in the v18.9.9.33 field log (assert "free() target pointer is
      // outside heap areas" during NimBLE ble_hs_stop). The 4.5.5 comment
      // above ("attempts to explicitly deleteClient crashed") means we
      // can't paper over the leak with a manual disposal either -- the
      // safe path is to never leak in the first place. The Free3-R
      // remote in test reliably reconnects on the second attempt with
      // the same client after the 300 ms settle, so this doesn't cost
      // us the retry success we were getting from the fresh client.
      if (!pClient->connect(bleAddress)) {
        lastError = "Connection failed";
        LOG_ERR("BT", "Failed to connect to %s (after retry)", address.c_str());
        return false;
      }
      LOG_INF("BT", "Connect succeeded on retry for %s (same client)", address.c_str());
    }
    (void)hadExistingClient;  // preserved for future diagnostic use

    const bool connParamsUpdated =
        pClient->updateConnParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
    LOG_INF("BT", "Connection params update request: %d", connParamsUpdated);

    const bool dataLenUpdated = pClient->setDataLen(251);
    LOG_INF("BT", "Data length extension request (251): %d", dataLenUpdated);

    const int connectedRssi = pClient->getRssi();
    LOG_INF("BT", "Connected RSSI for %s: %d dBm", address.c_str(), connectedRssi);
    
    // Get HID service
    NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
    if (!pService) {
      lastError = "HID service not found";
      LOG_ERR("BT", "Device %s doesn't have HID service", address.c_str());
      pClient->disconnect();
      return false;
    }

    // Attempt to force Report Protocol mode (0x01) when supported.
    // Some remotes behave inconsistently unless protocol mode is explicit.
    if (auto* pProtocolMode = pService->getCharacteristic(HID_PROTOCOL_MODE_UUID)) {
      if (pProtocolMode->canWrite() || pProtocolMode->canWriteNoResponse()) {
        uint8_t reportMode = 0x01;
        const bool protocolSet = pProtocolMode->writeValue(&reportMode, 1, false);
        LOG_INF("BT", "Protocol mode write (Report=0x01): %d", protocolSet);
      }
    }

    ReportMapHints reportHints;
    if (auto* pReportMap = pService->getCharacteristic(HID_REPORT_MAP_UUID)) {
      if (pReportMap->canRead()) {
        std::string reportMap = pReportMap->readValue();
        reportHints = parseReportMapHints(reportMap);
        LOG_INF("BT", "Report map hints: keyboard=%d consumer=%d preferredByte=%d len=%u",
                reportHints.hasKeyboardPage, reportHints.hasConsumerPage,
                static_cast<int>(reportHints.preferredByteIndex), static_cast<unsigned>(reportMap.size()));
      }
    }
    
    LOG_INF("BT", "Found HID service, enumerating report characteristics...");
    
    // BLE HID has multiple report characteristics (input, output, feature)
    // We need to find one that supports NOTIFY or INDICATE (input report)
    // In NimBLE 2.x, getCharacteristics() returns std::vector<NimBLERemoteCharacteristic*>
    auto pCharacteristics = pService->getCharacteristics(true);
    NimBLERemoteCharacteristic* pReportChar = nullptr;
    
    int reportCount = 0;
    std::vector<NimBLERemoteCharacteristic*> reportChars;
    
    for (auto it = pCharacteristics.begin(); it != pCharacteristics.end(); ++it) {
      auto* pChar = *it;
      LOG_DBG("BT", "Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d",
              pChar->getUUID().toString().c_str(),
              pChar->canRead(), pChar->canWrite(), pChar->canNotify(), pChar->canIndicate());
      
      if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
        reportCount++;
        
        // Check if this report supports notify or indicate (input report)
        if (pChar->canNotify() || pChar->canIndicate()) {
          reportChars.push_back(pChar);
          LOG_INF("BT", "Added Report char #%d for subscription", reportCount);
        }
      }
    }
    
    if (reportChars.empty()) {
      lastError = "No input report characteristic found";
      LOG_ERR("BT", "No Report characteristic with notify/indicate found");
      pClient->disconnect();
      return false;
    }
    
    // v15.4 tried capping to 2 Report chars for ~500B heap + fragmentation
    // savings. REVERTED in v18.9: field test on Free3-R "volume icon mode"
    // showed only the leftmost button (0x07, sent on Report char #1) was
    // received. Middle (0x08) and right (0x09) buttons use OTHER Report
    // characteristics (typically consumer control on chars #3+) that we
    // stopped listening to. Real functional regression. Reverting to all
    // notify-capable chars means we hear every button on the remote.
    // ~500B heap trade is nothing vs "your remote only half works."
    LOG_INF("BT", "Subscribing to %d Report characteristics...", reportChars.size());
    const size_t subscribeCount = reportChars.size();
    size_t successfulSubscriptions = 0;

    for (size_t i = 0; i < subscribeCount; i++) {
      auto* pChar = reportChars[i];
      
      // Clear stale CCCD state on reused clients where possible.
      (void)pChar->unsubscribe();

      // Use notifications when available, otherwise indications.
      const bool useNotify = pChar->canNotify();
      bool subResult = pChar->subscribe(useNotify, onHIDNotify);
      LOG_INF("BT", "Report char #%d subscribe (%s) result: %d", i + 1, useNotify ? "notify" : "indicate",
              subResult);
      if (subResult) {
        successfulSubscriptions++;
      }
      
      if (!subResult) {
        LOG_INF("BT", "Failed to subscribe to Report char #%d (continuing)", i + 1);
      }
    }

    if (successfulSubscriptions == 0) {
      lastError = "Failed to subscribe to input reports";
      LOG_ERR("BT", "No HID report subscriptions succeeded for %s", address.c_str());
      pClient->disconnect();
      return false;
    }
    
    LOG_INF("BT", "Subscribed to %u/%u HID Report characteristics",
            static_cast<unsigned>(successfulSubscriptions), static_cast<unsigned>(reportChars.size()));
    
    // Save connection with activity timestamp
    ConnectedDevice connDev;
    connDev.address = address;
    connDev.client = pClient;
    connDev.reportChars = reportChars;
    connDev.connectedTime = millis();
    connDev.subscribed = true;
    connDev.lastActivityTime = millis();  // Initialize activity timer
    connDev.wasConnected = true;  // Mark for auto-reconnect if disconnected
    // CrumBLE: a fresh link starts clean -- record when it came up and clear any
    // stale intentional-disconnect latch so a real early drop isn't suppressed.
    _lastConnectMillis = millis();
    _intentionalDisconnect = false;
    connDev.descriptorHasKeyboardPage = reportHints.hasKeyboardPage;
    connDev.descriptorHasConsumerPage = reportHints.hasConsumerPage;
    connDev.descriptorSuggestedIndex = reportHints.preferredByteIndex;
    
    // Detect device profile
    // First, try to find the device in scan results to get its name
    bool foundInScan = false;
    for (const auto& dev : _discoveredDevices) {
      if (dev.address == address) {
        connDev.name = dev.name;
        foundInScan = true;
        LOG_INF("BT", "Device found in scan results: %s (%s)", dev.name.c_str(), address.c_str());
        break;
      }
    }
    
    if (!foundInScan) {
      LOG_INF("BT", "Device not in scan results (may be previously paired): %s", address.c_str());
      if (connDev.name.empty() && !_bondedDeviceAddress.empty() && _bondedDeviceAddress == address &&
          !_bondedDeviceName.empty()) {
        connDev.name = _bondedDeviceName;
        LOG_INF("BT", "Using bonded device name hint: %s", connDev.name.c_str());
      }
    }
    
    // Profile matching priority:
    //  1. MAC-prefix exact match  (hardware ID, precise – always wins)
    //  2. Per-device learned profile by full MAC address
    //  3. User-learned global custom profile (explicitly taught by the user)
    //     → only if the name-matched known profile is NOT marked strictProfile
    //  4. Fuzzy name-pattern match  (last resort – can produce false positives)
    connDev.profile = DeviceProfiles::findDeviceProfile(address.c_str(), nullptr);

    DeviceProfiles::DeviceProfile perDeviceProfile;
    const bool hasPerDeviceProfile = DeviceProfiles::getCustomProfileForDevice(address, perDeviceProfile);

    if (!connDev.profile) {
      // Check if a name-matched profile exists and whether it is strict.
      const DeviceProfiles::DeviceProfile* nameMatch =
          DeviceProfiles::findDeviceProfile(nullptr, connDev.name.c_str());
      const bool nameMatchIsStrict = nameMatch && nameMatch->strictProfile;

      if (hasPerDeviceProfile && !nameMatchIsStrict) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleBackKeycode = perDeviceProfile.pageUpCode;
        connDev.simpleForwardKeycode = perDeviceProfile.pageDownCode;
        LOG_INF("BT", "Using per-device learned profile for %s: up=0x%02X down=0x%02X idx=%u", address.c_str(),
          perDeviceProfile.pageUpCode, perDeviceProfile.pageDownCode,
          static_cast<unsigned>(perDeviceProfile.reportByteIndex));
      }

      // Prefer the user's learned mapping over a non-strict name-based guess.
      const auto* customProfile = DeviceProfiles::getCustomProfile();
      if (!connDev.profile && customProfile && !nameMatchIsStrict) {
        connDev.profile = customProfile;
        LOG_INF("BT", "Using learned custom profile (overrides non-strict name match): up=0x%02X dn=0x%02X",
                customProfile->pageUpCode, customProfile->pageDownCode);
      } else if (!connDev.profile && nameMatch) {
        connDev.profile = nameMatch;
        if (nameMatchIsStrict) {
          LOG_INF("BT", "Using strict name-matched profile '%s' (custom profile bypassed)",
                  nameMatch->name);
        } else {
          LOG_INF("BT", "Using name-matched profile '%s' (no custom profile set)", nameMatch->name);
        }
      }
    }
    
    if (connDev.profile) {
      LOG_INF("BT", "Using device profile: %s (byte[%d] for keycode)", 
              connDev.profile->name, connDev.profile->reportByteIndex);
      connDev.simpleFallbackEnabled = false;
    } else {
      LOG_INF("BT", "No known profile matched for %s, will auto-detect from HID codes", address.c_str());
      if (!connDev.simpleFallbackEnabled) {
        connDev.simpleFallbackEnabled = true;
        connDev.simpleForwardKeycode = 0x00;
        connDev.simpleBackKeycode = 0x00;
      }
      LOG_INF("BT", "Simple fallback enabled for unknown device %s", address.c_str());
    }
    
    auto existing = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                                 [&address](const ConnectedDevice& dev) { return dev.address == address; });
    if (existing != _connectedDevices.end()) {
      *existing = connDev;
    } else {
      _connectedDevices.push_back(connDev);
    }
    
    LOG_INF("BT", "Successfully connected to %s", address.c_str());
    // v18.9.9.195: bond snapshot right after Successfully connected. If a
    // NEW bond was written during this connect, we'll see the count grow.
    logBondStoreDiag("post-Successfully-connected");
    lastError = "Connected";
    return true;
}

void BluetoothHIDManager::noteClientDisconnect(int reason) {
  // v18.9.9.195: log bond state on every disconnect (before the intentional-
  // latch consume) so a reason-520 followed by a successful reconnect leaves
  // three snapshots in the log: post-init, disconnect-520, post-reconnect.
  // If the count/address changes between snapshots, NimBLE re-negotiated the
  // LTK on the second connect -- explaining why reconnect works.
  {
    char tag[48];
    snprintf(tag, sizeof(tag), "onDisconnect reason=%d", reason);
    logBondStoreDiag(tag);
  }
  // A disconnect we triggered ourselves: consume the latch, no alert.
  if (_intentionalDisconnect) {
    _intentionalDisconnect = false;
    return;
  }
  if (_lastConnectMillis == 0) return;
  const unsigned long since = millis() - _lastConnectMillis;
  // A drop in the first SETTLE_MS is almost always bonding/encryption renegotiation
  // on the first connect (the link comes back on its own within a second). Earlier
  // this fired a false "BT couldn't stay connected" alert every first connect.
  //
  // CrumBLE 4.4 post-bisect: HCI reason 520 (0x208 = BLE supervision timeout) is
  // unambiguous — it's the "controller timed the link out under heap pressure"
  // case, NOT renegotiation. Fall through to the auto-reconnect branch even
  // inside the settle window so a drop at ~2s (observed empirically) still
  // triggers the one-shot retry.
  if (since < SETTLE_MS && reason != 520) {
    LOG_DBG("BT", "Link dropped %lums after connect (reason %d); within settle window, no alert",
            since, reason);
    return;
  }
  // A drop in [SETTLE_MS, EARLY_DISCONNECT_MS] is the real "controller timed the
  // link out under heap pressure" case (HCI 0x08 / reason 520). Surface it so the
  // user isn't left wondering why Bluetooth silently went away.
  //
  // v18.9.9.21: reason 520 (supervision timeout under CPU/heap pressure) is
  // treated as auto-reconnectable regardless of session age. The EARLY_
  // DISCONNECT_MS window was designed to distinguish a supervision timeout
  // (retryable) from a walk-away (permanent), but a mid-session
  // long-parse-blocking-BT case (heavy chapter transition, prebake miss,
  // compat mode cold rebuild) can starve the BLE stack past the 10 s window
  // -- the drop is still under pressure, not a walk-away. Field log with a
  // reader that dropped ~6 minutes in during a chapter build showed exactly
  // this pattern. The one-shot _autoReconnectConsumedThisCycle latch caps
  // retries so we don't loop on a real walk-away that keeps re-dropping.
  const bool retryableWindow = since < EARLY_DISCONNECT_MS;
  const bool retryableReason520 = reason == 520;
  if (retryableWindow || retryableReason520) {
    // CrumBLE 4.4 post-bisect: an early reason-520 drop is the
    // "supervision timeout during post-connect render" race. Fire a
    // one-shot auto-reconnect (reader polls takeAutoReconnectRequest()
    // and calls connectToDevice() again). If the second attempt also
    // drops, the alert path takes over -- no infinite retry loop.
    if (!_autoReconnectConsumedThisCycle) {
      LOG_INF("BT", "Link dropped %lums after connect (reason %d); queueing one-shot auto-reconnect",
              since, reason);
      _autoReconnectPending = true;
      _autoReconnectConsumedThisCycle = true;
    } else {
      LOG_INF("BT", "Link dropped %lums after connect (reason %d); auto-reconnect already used, flagging alert",
              since, reason);
      _connectionLostAlertPending = true;
    }
  }
}

bool BluetoothHIDManager::disconnectFromDevice(const std::string& address) {
  LOG_INF("BT", "Disconnecting from device %s", address.c_str());
  // CrumBLE: this is a disconnect WE initiate (user toggle, or the temporary
  // drop-BLE-for-build path). Mark it so the resulting onDisconnect callback
  // doesn't raise the "couldn't stay connected" alert.
  _intentionalDisconnect = true;

  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    if (_buttonInjector && it->activeInjectedButton != 0xFF) {
      _buttonInjector(it->activeInjectedButton, false);
    }
    NimBLEClient* client = it->client;

    // Ensure normal CPU speed during BLE termination to avoid WDT in low-power mode.
    if (client && client->isConnected()) {
      HalPowerManager::Lock lock;
      client->disconnect();
    }

    // Remove from our list
    _connectedDevices.erase(it);
    LOG_INF("BT", "Disconnected from %s", address.c_str());
    return true;
  }
  
  LOG_INF("BT", "Device %s not in connected list", address.c_str());
  return false;
}

bool BluetoothHIDManager::isConnected(const char* address) const {
  if (!address) return false;
  return std::find_if(_connectedDevices.begin(), _connectedDevices.end(), [address](const ConnectedDevice& dev) {
           // dev.address is std::string; operator== with const char* uses strcmp, no allocation.
           return dev.address == address && dev.client && dev.client->isConnected();
         }) != _connectedDevices.end();
}

std::vector<std::string> BluetoothHIDManager::getConnectedDevices() const {
  std::vector<std::string> addresses;
  for (const auto& dev : _connectedDevices) {
    if (dev.client && dev.client->isConnected()) {
      addresses.push_back(dev.address);
    }
  }
  return addresses;
}

ConnectedDevice* BluetoothHIDManager::findConnectedDevice(const std::string& address) {
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    return &(*it);
  }
  return nullptr;
}

void BluetoothHIDManager::processInputEvents() {
  // Input events are processed via notifications callback
  // This method is kept for potential polling-based implementations
}

void BluetoothHIDManager::setInputCallback(std::function<void(uint16_t)> callback) {
  _inputCallback = callback;
  LOG_DBG("BT", "Input callback registered");
}

void BluetoothHIDManager::setLearnInputCallback(std::function<void(uint8_t, uint8_t)> callback) {
  _learnInputCallback = callback;
  LOG_DBG("BT", "Learn input callback registered");
}

void BluetoothHIDManager::setButtonInjector(std::function<void(uint8_t, bool)> injector) {
  _buttonInjector = injector;
  LOG_DBG("BT", "Button injector registered");
}

void BluetoothHIDManager::setReaderContextCallback(std::function<bool()> callback) {
  _readerContextCallback = callback;
  LOG_DBG("BT", "Reader context callback registered");
}

void BluetoothHIDManager::setButtonActivityNotifier(std::function<void(uint8_t)> notifier) {
  _buttonActivityNotifier = notifier;
}

void BluetoothHIDManager::setBondedDevice(const std::string& address, const std::string& name) {
  _bondedDeviceAddress = address;
  _bondedDeviceName = name;
  LOG_INF("BT", "Bonded device set: %s (%s)", _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str());
}

bool BluetoothHIDManager::hasRecentActivity() const {
  // Check if any connected device has had activity in the last 4 minutes
  // This prevents power sleep while using BLE controller
  unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute (240 second) threshold to keep BLE alive
        return true;
      }
    }
  }
  return false;
}

// v18.9.4: returns millis since the newest input across all connected
// devices, or ULONG_MAX when nothing is connected (or no input yet). Used
// by main.cpp to drive the user-set BT auto-disconnect timeout.
unsigned long BluetoothHIDManager::getMillisSinceLastActivity() const {
  if (_connectedDevices.empty()) return ULONG_MAX;
  unsigned long now = millis();
  unsigned long minGap = ULONG_MAX;
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime == 0) continue;
    unsigned long gap = now - device.lastActivityTime;
    if (gap < minGap) minGap = gap;
  }
  return minGap;
}

bool BluetoothHIDManager::hadRecentFree2Input(unsigned long windowMs) const {
  const unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastNormalizedEventMs == 0 || (now - device.lastNormalizedEventMs) > windowMs) {
      continue;
    }

    // Keep the legacy method name for compatibility, but treat any recent BLE
    // page-turner input as a signal to prefer press-driven reader navigation.
    if (isFree2Profile(device.profile) || device.activeInjectedButton != 0xFF || device.lastNormalizedKeycode != 0x00) {
      return true;
    }
  }
  return false;
}

// Static callback for HID notifications
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;
  
  // Get the device address and find the connected device
  ConnectedDevice* device = nullptr;
  if (pChar && pChar->getRemoteService()) {
    auto client = pChar->getRemoteService()->getClient();
    if (client) {
      std::string deviceAddr = client->getPeerAddress().toString();
      device = g_instance->findConnectedDevice(deviceAddr);
    }
  }
  
  if (!device) return;

  const unsigned long nowMs = millis();
  const bool free2Profile = isFree2Profile(device->profile);

  // GameBrick can occasionally miss a release tail, leaving a virtual button
  // latched as pressed. After a long idle gap, clear stale hold state so the
  // next tap is always treated as a fresh press.
  // Keep this comfortably above the reader's 700ms chapter-skip threshold so
  // a legitimate long press is not force-released early.
  if (device->profile && strncmp(device->profile->name, "IINE Game Brick", 15) == 0) {
    constexpr unsigned long STALE_GAMEBRICK_HOLD_RESET_MS = 1200;
    if (device->activeInjectedButton != 0xFF &&
        device->lastNormalizedEventMs > 0 &&
        (nowMs - device->lastNormalizedEventMs) > STALE_GAMEBRICK_HOLD_RESET_MS) {
      if (g_instance->_buttonInjector) {
        g_instance->_buttonInjector(device->activeInjectedButton, false);
      }
      device->activeInjectedButton = 0xFF;
      device->lastButtonState = false;
      device->lastHIDKeycode = 0x00;
      device->lastNormalizedPressed = false;
      device->lastGameBrickActiveKey = 0x00;
      device->gameBrickCenterPressFrames = 0;
      LOG_DBG("BT", "Game Brick: cleared stale held state after %lu ms idle", nowMs - device->lastNormalizedEventMs);
    }
  }
  
  // Update activity timestamp to keep connection alive
  device->lastActivityTime = millis();
  // Only Free2 needs hold-time capping based on BLE activity. Other remotes,
  // including GameBrick, should keep the original virtual hold semantics so
  // long-press chapter skip continues to use the full press duration.
  if (free2Profile && g_instance->_buttonActivityNotifier && device->activeInjectedButton != 0xFF) {
    g_instance->_buttonActivityNotifier(device->activeInjectedButton);
  }


  if (g_instance->_debugCaptureEnabled) {
    char rawBuf[128] = {0};
    size_t offset = 0;
    const size_t dumpLen = length < 8 ? length : 8;
    for (size_t i = 0; i < dumpLen && offset + 4 < sizeof(rawBuf); i++) {
      offset += snprintf(rawBuf + offset, sizeof(rawBuf) - offset, "%02X ", static_cast<unsigned>(pData[i]));
    }
    LOG_INF("BTDBG", "addr=%s len=%u raw=%s", device->address.c_str(), static_cast<unsigned>(length), rawBuf);
  }

  auto releaseInjectedButton = [&]() {
    if (g_instance->_buttonInjector && device->activeInjectedButton != 0xFF) {
      g_instance->_buttonInjector(device->activeInjectedButton, false);
    }
    device->activeInjectedButton = 0xFF;
    device->pendingGameBrickRelease = false;
    device->pendingGameBrickReleaseMs = 0;
    device->pendingGameBrickKeycode = 0x00;
    device->pendingGameBrickButton = 0xFF;
  };
  
  // Extract keycode based on device profile or auto-detect
  uint8_t keycode = 0xFF;
  uint8_t keycodeIndex = 0xFF;
  bool isPressed = false;
  bool isGameBrickProfile = false;
  
  if (length < 1) {
    LOG_DBG("BT", "HID report empty, ignoring");
    return;
  }
  
  // Determine keycode source and press state based on device profile
  if (device->profile) {
    // Use device profile's byte index for keycode
    if (length >= device->profile->reportByteIndex + 1) {
      keycode = pData[device->profile->reportByteIndex];
      keycodeIndex = device->profile->reportByteIndex;
    }

    // For custom/learned profiles: if the fixed-index byte is not one of the learned
    // keycodes, scan the entire report.  This handles remotes where the prev/next buttons
    // send their keycodes at different byte positions, or where they arrive on separate
    // HID report characteristics with their own frame layouts.
    const bool isCustomProfile = (strcmp(device->profile->name, "Custom BLE Remote") == 0);
    if (isCustomProfile &&
        keycode != device->profile->pageUpCode &&
        keycode != device->profile->pageDownCode) {
      for (size_t bi = 0; bi < length && bi < 8; bi++) {
        const uint8_t b = pData[bi];
        if (b == device->profile->pageUpCode || b == device->profile->pageDownCode) {
          keycode = b;
          keycodeIndex = static_cast<uint8_t>(bi);
          LOG_DBG("BT", "Custom profile: found learned code 0x%02X at byte[%u] (vs fixed idx %u)",
                  keycode, static_cast<unsigned>(bi),
                  static_cast<unsigned>(device->profile->reportByteIndex));
          break;
        }
      }
    }

    // For Game Brick: press state from byte[0] bit 0
    // For standard HID keyboards: press state from keycode (non-zero = pressed)
    if (strncmp(device->profile->name, "IINE Game Brick", 15) == 0) {
      isGameBrickProfile = true;
      bool gameBrickStandardMode = false;

      // --- GameBrick V2 report format (confirmed via RAW captures) ---
      // byte[0]   : frame status (0x13 pressed/active, 0x12 release tail)
      // byte[1-2] : 16-bit cycling counter (+125/frame, ~8 ms), NOT button data
      // byte[3]   : horizontal (X) joystick axis, center = 0x98
      // byte[4]   : button / vertical axis
      //               0x08 = idle / joystick center
      //               0x07 = physical UP button (d-pad up)
      //               0x09 = physical DOWN button (d-pad down)
      // LEFT/RIGHT are joystick-only: byte[4]==0x08 with byte[3] offset from 0x98.
      //
      // IMPORTANT: ignore any pre-extracted keycode from profile byte index because
      // byte[2] can naturally pass through 0x07/0x09 and cause false button presses.
      keycode = 0x00;
      keycodeIndex = 0xFF;

      auto isGameBrickSupportedCode = [](uint8_t code) {
         return code == 0x07 || code == 0x09 ||
           code == GAMEBRICK_ACTION_A_CODE ||
           code == GAMEBRICK_ACTION_B_CODE ||
               code == DeviceProfiles::KEYBOARD_UP_ARROW ||
               code == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
               code == DeviceProfiles::KEYBOARD_LEFT_ARROW ||
               code == DeviceProfiles::KEYBOARD_RIGHT_ARROW ||
               code == DeviceProfiles::KEYBOARD_ENTER ||
               code == DeviceProfiles::KEYBOARD_SPACE ||
               code == DeviceProfiles::KEYBOARD_PAGE_UP ||
               code == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
               code == DeviceProfiles::STANDARD_PAGE_UP ||
               code == DeviceProfiles::STANDARD_PAGE_DOWN;
      };

      // Some GameBrick C/T/H modes expose standard keyboard/consumer reports.
      // Prefer that path when a clear standard keycode is present.
      const ExtractedHIDKey generic = extractGenericPageTurnKeycode(pData, length);
      auto isStandardGameBrickCode = [](uint8_t code) {
        return code == DeviceProfiles::KEYBOARD_UP_ARROW ||
               code == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
               code == DeviceProfiles::KEYBOARD_LEFT_ARROW ||
               code == DeviceProfiles::KEYBOARD_RIGHT_ARROW ||
               code == DeviceProfiles::KEYBOARD_ENTER ||
               code == DeviceProfiles::KEYBOARD_SPACE ||
               code == DeviceProfiles::KEYBOARD_PAGE_UP ||
               code == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
               code == DeviceProfiles::STANDARD_PAGE_UP ||
               code == DeviceProfiles::STANDARD_PAGE_DOWN;
      };

      if (isStandardGameBrickCode(generic.keycode)) {
        gameBrickStandardMode = true;
        keycode = generic.keycode;
        keycodeIndex = generic.reportIndex;
      }

      if (!gameBrickStandardMode && length >= 5) {
        // bytes[1,2] form a 16-bit LE cycling counter (~+125/frame, LE).
        // The counter FREEZES to 0x07D0 when any physical button is pressed and
        // remains frozen through the entire press AND release-ramp sequence.
        // Joystick motion keeps the counter cycling freely.
        const uint16_t counter =
            static_cast<uint16_t>(pData[1]) | (static_cast<uint16_t>(pData[2]) << 8);
        const bool counterFrozen = (counter == device->lastGameBrickCounter);
        device->lastGameBrickCounter = counter;

        const bool isReleaseTail = (pData[0] & 0x01) == 0;
        const bool activeFrame = ((pData[0] & 0x01) != 0);
        const bool isDirectionalFreezeWindow = (counter == 0x07D0);

        // Clear the d-pad latch once the counter resumes cycling or a release-tail arrives.
        if (!counterFrozen || isReleaseTail) {
          device->lastGameBrickActiveKey = 0x00;
        }
        const uint8_t b4 = pData[4];
        if (b4 == 0x07 || b4 == 0x09) {
          const bool directionalFreezeWindow =
              isDirectionalFreezeWindow || (counterFrozen && device->lastGameBrickActiveKey != 0x00);
          if (directionalFreezeWindow) {
            // D-pad UP/DOWN uses the special 0x07D0 frozen counter window.
            // While held, the release ramp can cross the opposite code, so latch the
            // first directional code seen until release-tail/counter-change.
            if (device->lastGameBrickActiveKey == 0x00) {
              device->lastGameBrickActiveKey = b4;
            }
            if (b4 == device->lastGameBrickActiveKey) {
              keycode = b4;
              keycodeIndex = 4;
            }
          } else {
            // Non-0x07D0 window: treat 0x07/0x09 as A/B button family.
            // This preserves menu semantics (A=Select, B=Back) outside page-reading context.
            keycode = (b4 == 0x07) ? GAMEBRICK_ACTION_A_CODE : GAMEBRICK_ACTION_B_CODE;
            keycodeIndex = 4;
          }
          device->gameBrickCenterPressFrames = 0;
        } else if (b4 == 0x08) {
          // Joystick horizontal:
          // - usually appears while counter is cycling
          // - can also appear in some frozen windows for horizontal-only presses
          //
          // But while vertical d-pad latch (0x07/0x09 in 0x07D0 window) is active,
          // b4==0x08 frames are release/overshoot noise and must be ignored.
          const bool allowHorizontal = !counterFrozen || device->lastGameBrickActiveKey == 0x00;
          if (!allowHorizontal) {
            // Transitional frame from vertical press/release.
            keycode = 0x00;
            device->gameBrickCenterPressFrames = 0;
          } else {
            const int dx = static_cast<int>(pData[3]) - 0x98;
            // Empirical tuning from logs:
            // RIGHT tends to be stronger than LEFT on some units, so keep LEFT
            // threshold lower to catch weak positive deflections.
            constexpr int kDeadzoneRight = 2;
            constexpr int kDeadzoneLeft = 0;
            if (dx < -kDeadzoneRight) {
              keycode = DeviceProfiles::KEYBOARD_RIGHT_ARROW;
              keycodeIndex = 3;
              device->gameBrickCenterPressFrames = 0;
            } else if (dx > kDeadzoneLeft) {
              keycode = DeviceProfiles::KEYBOARD_LEFT_ARROW;
              keycodeIndex = 3;
              device->gameBrickCenterPressFrames = 0;
            } else if (activeFrame && !counterFrozen && device->lastGameBrickActiveKey == 0x00) {
              // Some GameBrick units appear to emit LEFT as a centered b4==0x08 burst
              // (dx≈0) with a cycling counter. Require several consecutive frames so
              // transitional noise from other keys is ignored.
              if (device->gameBrickCenterPressFrames < 255) {
                device->gameBrickCenterPressFrames++;
              }
              if (device->gameBrickCenterPressFrames >= 6) {
                keycode = DeviceProfiles::KEYBOARD_LEFT_ARROW;
                keycodeIndex = 3;
              }
            } else {
              device->gameBrickCenterPressFrames = 0;
            }
            // else: centered idle → keycode stays 0x00
          }
        } else {
          device->gameBrickCenterPressFrames = 0;
        }
        // All other byte[4] values (ramp overshoot > 0x09 or < 0x07) → 0x00.
      }

      // If nothing found, keycode stays 0x00 → treated as release.

      // Game Brick: accept only stable digital-button report family (0x1x).
      // Ignore noisy transitional frames (commonly 0x2x/0x3x) that can trigger false presses.
      if (gameBrickStandardMode) {
        isPressed = (keycode != 0x00) && isGameBrickSupportedCode(keycode);
      } else {
        const bool stableButtonReport = (pData[0] & 0xF0) == 0x10;
        if (!stableButtonReport) {
          LOG_DBG("BT", "Game Brick: ignoring transitional report byte[0]=0x%02X, keycode=0x%02X", pData[0], keycode);
          // Keep the previous button state intact while skipping transitional frames.
          // Resetting state here can create a duplicate "new press" on the next stable
          // frame, which shows up as a double page-turn.
          return;
        }

        // Press is only valid with a supported decoded code plus active frame bit.
        isPressed = ((pData[0] & 0x01) != 0) && isGameBrickSupportedCode(keycode);
      }

      // Prevent initial stale pressed frame right after subscribe from triggering navigation.
      // Only allow presses after at least one clean release frame has been seen.
      if (!device->hasSeenRelease) {
        if (!isPressed) {
          device->hasSeenRelease = true;
        } else {
          // Some GameBrick variants do not emit an immediate release frame after
          // connect and would otherwise be blocked indefinitely. Arm input on
          // the first valid GameBrick press instead of discarding it.
          device->hasSeenRelease = true;
          LOG_DBG("BT", "Game Brick: arming on first valid press keycode=0x%02X", keycode);
        }
      }

      {
        // Full raw dump so we can reverse-engineer D-pad encoding.
        char rawBuf[64];
        int pos = 0;
        for (size_t ri = 0; ri < length && ri < 8 && pos < 56; ri++) {
          pos += snprintf(rawBuf + pos, sizeof(rawBuf) - pos, "%02X ", pData[ri]);
        }
        LOG_DBG("BT", "Game Brick RAW[%u]: %s=> keycode=0x%02X idx=%u pressed=%d",
                static_cast<unsigned>(length), rawBuf, keycode,
                static_cast<unsigned>(keycodeIndex), isPressed);
      }
    } else {
      // Standard HID keyboards/custom profiles: keycode non-zero = pressed.
      // Normalise 0xFF (= "nothing found in report") to 0x00 so that short
      // release frames (e.g. 1-byte consumer control [0x00]) are treated as
      // a key-release rather than a phantom press.
      if (keycode == 0xFF) {
        keycode = 0x00;
      }
      isPressed = (keycode != 0x00);
      LOG_DBG("BT", "Device %s: keycode=0x%02X, pressed=%d", device->profile->name, keycode, isPressed);
    }
  } else {
    // Auto-detect mode: support a wider range of generic HID remotes.
    const ExtractedHIDKey extracted = extractGenericPageTurnKeycode(pData, length);
    keycode = extracted.keycode;
    keycodeIndex = extracted.reportIndex;

    if (device->descriptorSuggestedIndex != 0xFF && length > device->descriptorSuggestedIndex) {
      const uint8_t hintedCode = pData[device->descriptorSuggestedIndex];
      if (hintedCode != 0x00 && hintedCode != 0xFF &&
          (keycode == 0x00 || keycode == 0xFF || DeviceProfiles::isCommonPageTurnCode(hintedCode))) {
        keycode = hintedCode;
        keycodeIndex = device->descriptorSuggestedIndex;
      }
    }

    // Some remotes emit noisy 0x07/0x09 bytes in parallel with true rolling keycodes.
    // If we selected 0x07/0x09, search the short report for a stronger non-GameBrick code.
    if ((keycode == 0x07 || keycode == 0x09) && length > 0) {
      const size_t scanLen = length < 8 ? length : 8;
      for (size_t i = 0; i < scanLen; i++) {
        const uint8_t candidate = pData[i];
        if (candidate == 0x00 || candidate == 0xFF || candidate == 0x07 || candidate == 0x09) {
          continue;
        }
        if (DeviceProfiles::isCommonPageTurnCode(candidate)) {
          keycode = candidate;
          keycodeIndex = static_cast<uint8_t>(i);
          break;
        }
      }
    }

    // Keep existing GameBrick bit0 press-state behavior when applicable.
    if (length >= 5 && (keycode == 0x07 || keycode == 0x09)) {
      isPressed = ((pData[0] & 0x01) != 0) || (keycode != 0x00);
      LOG_DBG("BT", "Auto-detect (GameBrick-like): keycode=0x%02X, pressed=%d", keycode, isPressed);
    } else {
      isPressed = (keycode != 0x00);
      LOG_DBG("BT", "Auto-detect (generic HID): keycode=0x%02X, pressed=%d", keycode, isPressed);
    }
  }

  // Update release state for startup noise gate
  // When we see the first release (isPressed = false), we enable button injection
  if (!isPressed && !device->hasSeenRelease) {
    device->hasSeenRelease = true;
  }
  
  // Ignore if no valid keycode detected
  if (keycode == 0x00 || keycode == 0xFF) {
    releaseInjectedButton();
    // Track state for transition detection
    device->lastButtonState = isPressed;
    device->lastHIDKeycode = keycode;
    device->lastNormalizedDirection = 0xFF;
    return;
  }
  
  // CRITICAL GATE: Don't inject any buttons until we've seen the first release
  // This prevents startup transient noise from being interpreted as button presses
  if (!device->hasSeenRelease) {
    const bool likelyFree2Press =
        keycode == DeviceProfiles::FREE2_FORWARD_A || keycode == DeviceProfiles::FREE2_FORWARD_B ||
        keycode == DeviceProfiles::FREE2_FORWARD_C || keycode == DeviceProfiles::FREE2_FORWARD_D ||
        keycode == DeviceProfiles::FREE2_BACK_A || keycode == DeviceProfiles::FREE2_BACK_B ||
        keycode == DeviceProfiles::FREE2_BACK_C || keycode == DeviceProfiles::FREE2_BACK_D;

    // CrumBLE 4.5.6: matched-profile arming that ALSO injects this first press
    // (rather than swallowing it). Free3-R and similar remotes emit the same
    // keycode on press AND release frames (byte[4] = 0x70 on both), so the
    // normal `!isPressed` -> hasSeenRelease arming path at the top of this
    // function never fires -- and the Free2 escape below only helps unknown
    // profiles. For Free3-R the profile IS matched (via bonded name lookup)
    // but hasSeenRelease stays false and the first tap gets swallowed here,
    // then every subsequent tap does too. Skip the gate entirely when the
    // profile is trusted: matched profile means startup noise is rare vs a
    // genuine tap, and the user reports first tap always fails on Free3-R.
    if (device->profile != nullptr && isPressed) {
      device->hasSeenRelease = true;
      LOG_DBG("BT", "Arming injection on first press with matched profile '%s' (key=0x%02X)",
              device->profile->name, keycode);
      // Fall through so this first press actually injects instead of being
      // swallowed by the return below.
    } else {
      if (device->profile == nullptr && likelyFree2Press && isPressed) {
        // Free 2 auto-detect: no profile yet, but this looks like a Free2 code.
        // Arm hasSeenRelease so the NEXT press injects (profile detection runs
        // between here and then). First press is still swallowed to avoid
        // treating startup noise as input during auto-detect.
        device->hasSeenRelease = true;
        LOG_DBG("BT", "Arming auto-detect on first valid Free2 code: 0x%02X", keycode);
      }

      releaseInjectedButton();
      device->lastButtonState = isPressed;
      device->lastHIDKeycode = keycode;
      return;
    }
  }

  const uint8_t free2Direction = free2Profile ? classifyFree2Direction(keycode) : 0xFF;

  // Detect button PRESS transition.
  // For most remotes, key changes while held are treated as a new press event.
  // For Game Brick, ignore key-change retriggers while held to avoid duplicate events.
  bool isNewPressEvent =
      isPressed && (!device->lastButtonState || (!isGameBrickProfile && keycode != device->lastHIDKeycode));

  // Free2 reports rolling keycodes while one button is held.
  // Collapse that family to one logical press and ignore family flips until release.
  if (free2Profile && isPressed) {
    if (!device->lastButtonState) {
      device->lastNormalizedDirection = free2Direction;
    } else if (device->lastNormalizedDirection != 0xFF && free2Direction == device->lastNormalizedDirection) {
      isNewPressEvent = false;
    } else if (device->lastNormalizedDirection != 0xFF && free2Direction != 0xFF &&
               free2Direction != device->lastNormalizedDirection) {
      isNewPressEvent = false;
      if (device->activeInjectedButton != 0xFF) {
        keycode = device->lastHIDKeycode;
      }
    }
  }

  if (isGameBrickProfile && isPressed && !isNewPressEvent && keycode == device->lastHIDKeycode &&
      device->lastNormalizedEventMs > 0) {
    constexpr unsigned long GAMEBRICK_REPRESS_IDLE_MS = 220;
    if ((nowMs - device->lastNormalizedEventMs) > GAMEBRICK_REPRESS_IDLE_MS) {
      isNewPressEvent = true;
      device->lastButtonState = false;
      device->lastNormalizedPressed = false;
      // A same-key re-press after this much idle means we missed the previous
      // release frame (BLE callback starved during a heavy chapter render is a
      // common trigger). Force-release the stuck injection so the press path
      // below treats this as a fresh tap; otherwise the `activeInjectedButton`
      // gate at the injector block would silently swallow it.
      if (device->activeInjectedButton != 0xFF) {
        if (g_instance->_buttonInjector) {
          g_instance->_buttonInjector(device->activeInjectedButton, false);
        }
        device->activeInjectedButton = 0xFF;
      }
      LOG_DBG("BT", "Game Brick: promoting same-key re-press after %lu ms idle (key=0x%02X)",
              nowMs - device->lastNormalizedEventMs, keycode);
    }
  }

  // CrumBLE 4.5.5: generalised stuck-state release for any device whose
  // release frame doesn't decode to keycode==0 under its profile.
  // Field log (Free3-R / Custom BLE Remote profile, byte[4] reads the same
  // 0x70 on press AND release frames) showed activeInjectedButton sticking
  // after the first press because no synthetic release ever fired -- second
  // and third presses passed BUTTON PRESSED detection but were silently
  // swallowed by the `activeInjectedButton == 0xFF` gate at the injector.
  // A 2-second idle window is comfortably above any realistic held-button
  // duration (key repeats arrive every ~30 ms) and below the smallest
  // human "tap, wait, tap again" interval. Apply only when activeInjected
  // Button is still set on the same keycode -- i.e. we're stuck.
  if (isPressed && device->activeInjectedButton != 0xFF && keycode == device->lastInjectedKeycode &&
      device->lastInjectionTime > 0) {
    constexpr unsigned long GENERIC_STUCK_RELEASE_IDLE_MS = 2000;
    if ((nowMs - device->lastInjectionTime) > GENERIC_STUCK_RELEASE_IDLE_MS) {
      if (g_instance->_buttonInjector) {
        g_instance->_buttonInjector(device->activeInjectedButton, false);
      }
      device->activeInjectedButton = 0xFF;
      device->lastNormalizedPressed = false;
      isNewPressEvent = true;
      LOG_INF("BT", "Generic stuck-release: clearing latched injection key=0x%02X idle=%lums",
              keycode, nowMs - device->lastInjectionTime);
    }
  }

  if (isNewPressEvent && device->lastNormalizedPressed && device->lastNormalizedKeycode == keycode &&
      (nowMs - device->lastNormalizedEventMs) < 90) {
    isNewPressEvent = false;
    if (g_instance->_debugCaptureEnabled) {
      LOG_INF("BTDBG", "Suppressed jitter duplicate key=0x%02X dt=%lu", keycode,
              nowMs - device->lastNormalizedEventMs);
    }
  }
  if (isNewPressEvent) {
    LOG_INF("BT", ">>> BUTTON PRESSED: keycode=0x%02X <<<", keycode);

    if (g_instance->_learnInputCallback && keycode != 0x00 && keycode != 0xFF && keycodeIndex != 0xFF) {
      g_instance->_learnInputCallback(keycode, keycodeIndex);
    }
    
    // Also call original callback if set
    if (g_instance->_inputCallback) {
      g_instance->_inputCallback(keycode);
    }
  }

  uint8_t mappedButton = isPressed ? g_instance->mapKeycodeToButton(keycode, device) : 0xFF;

  // Free2 can wobble briefly while a key is held, causing opposite-direction flips or
  // transient unmapped frames. Keep the active direction latched during a continuous hold
  // and wait for an actual release before changing direction.
  if (free2Profile && isPressed && device->lastButtonState && device->activeInjectedButton != 0xFF) {
    if (mappedButton == 0xFF) {
      mappedButton = device->activeInjectedButton;
    } else if (mappedButton != device->activeInjectedButton) {
      if (g_instance->_debugCaptureEnabled) {
        LOG_INF("BTDBG", "Hold wobble suppressed: active=%u incoming=%u key=0x%02X", device->activeInjectedButton,
                mappedButton, keycode);
      }
      mappedButton = device->activeInjectedButton;
      isNewPressEvent = false;
    }
  }

  const bool isGameBrickActionKey = isGameBrickProfile &&
                                    (keycode == GAMEBRICK_ACTION_A_CODE || keycode == GAMEBRICK_ACTION_B_CODE);
  const uint8_t gameBrickActionButton = isGameBrickActionKey ? g_instance->mapKeycodeToButton(keycode, device) : 0xFF;

  if (device->pendingGameBrickRelease) {
    if (isPressed && keycode == device->pendingGameBrickKeycode && mappedButton == device->pendingGameBrickButton) {
      device->pendingGameBrickRelease = false;
      device->pendingGameBrickReleaseMs = 0;
      device->pendingGameBrickKeycode = 0x00;
      device->pendingGameBrickButton = 0xFF;
      mappedButton = device->activeInjectedButton;
      isNewPressEvent = false;
    } else if (isPressed && mappedButton != device->pendingGameBrickButton) {
      releaseInjectedButton();
    }
  }

  if (isGameBrickProfile && g_instance->_debugCaptureEnabled && isPressed) {
    const char* keyLabel = "Unknown";
    switch (keycode) {
      case DeviceProfiles::KEYBOARD_UP_ARROW:
        keyLabel = "DPad Up";
        break;
      case DeviceProfiles::KEYBOARD_DOWN_ARROW:
        keyLabel = "DPad Down";
        break;
      case DeviceProfiles::KEYBOARD_LEFT_ARROW:
        keyLabel = "DPad Left";
        break;
      case DeviceProfiles::KEYBOARD_RIGHT_ARROW:
        keyLabel = "DPad Right";
        break;
      case GAMEBRICK_ACTION_A_CODE:
        keyLabel = "A";
        break;
      case GAMEBRICK_ACTION_B_CODE:
        keyLabel = "B";
        break;
      case 0x07:
        keyLabel = "Up";
        break;
      case 0x09:
        keyLabel = "Down";
        break;
      default:
        break;
    }

    const char* actionLabel = "Unmapped";
    switch (mappedButton) {
      case HalGPIO::BTN_UP:
        actionLabel = "Up/PageBack";
        break;
      case HalGPIO::BTN_DOWN:
        actionLabel = "Down/PageForward";
        break;
      case HalGPIO::BTN_LEFT:
        actionLabel = "Left";
        break;
      case HalGPIO::BTN_RIGHT:
        actionLabel = "Right";
        break;
      case HalGPIO::BTN_CONFIRM:
        actionLabel = "Select";
        break;
      case HalGPIO::BTN_BACK:
        actionLabel = "Back";
        break;
      default:
        break;
    }

    LOG_INF("BTDBG", "GameBrick %s (0x%02X) -> %s", keyLabel, keycode, actionLabel);
  }

  if (!isPressed || mappedButton == 0xFF) {
    if (isGameBrickActionKey && device->activeInjectedButton == gameBrickActionButton && gameBrickActionButton != 0xFF) {
      constexpr unsigned long GAMEBRICK_ACTION_RELEASE_GRACE_MS = 110;
      device->pendingGameBrickRelease = true;
      device->pendingGameBrickReleaseMs = nowMs + GAMEBRICK_ACTION_RELEASE_GRACE_MS;
      device->pendingGameBrickKeycode = keycode;
      device->pendingGameBrickButton = gameBrickActionButton;
    } else {
      releaseInjectedButton();
    }
  } else {
    if (device->activeInjectedButton != 0xFF && device->activeInjectedButton != mappedButton) {
      releaseInjectedButton();
    }

    if (g_instance->_buttonInjector && device->activeInjectedButton == 0xFF) {
      if (isGameBrickProfile && device->lastInjectedKeycode == keycode &&
          (millis() - device->lastInjectionTime) < 180) {
        LOG_DBG("BT", "Game Brick: debouncing duplicate key 0x%02X (%lu ms)", keycode,
                millis() - device->lastInjectionTime);
      } else {
      const char* buttonName = "Unknown";
      switch (mappedButton) {
        case HalGPIO::BTN_UP:
          buttonName = "Up/PageBack";
          break;
        case HalGPIO::BTN_DOWN:
          buttonName = "Down/PageForward";
          break;
        case HalGPIO::BTN_LEFT:
          buttonName = "Left";
          break;
        case HalGPIO::BTN_RIGHT:
          buttonName = "Right";
          break;
        case HalGPIO::BTN_CONFIRM:
          buttonName = "Select";
          break;
        case HalGPIO::BTN_BACK:
          buttonName = "Back";
          break;
        default:
          break;
      }
      if (g_instance->_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped key 0x%02X -> %s", keycode, buttonName);
      }
      g_instance->_buttonInjector(mappedButton, true);
      device->activeInjectedButton = mappedButton;
      if (free2Profile && g_instance->_buttonActivityNotifier) {
        // Seed the hold timer on the very first injected Free2 press. This keeps a
        // missing release frame from letting a short tap age into a long-press skip.
        g_instance->_buttonActivityNotifier(mappedButton);
      }
      device->lastInjectionTime = millis();
      device->lastInjectedKeycode = keycode;
      }
    }
  }
  
  // Track the button state and keycode for next time
  device->lastButtonState = isPressed;
  device->lastHIDKeycode = keycode;
  device->lastNormalizedEventMs = nowMs;
  device->lastNormalizedKeycode = keycode;
  device->lastNormalizedPressed = isPressed;
  if (!isPressed) {
    device->lastNormalizedDirection = 0xFF;
  } else if (free2Profile && free2Direction != 0xFF) {
    device->lastNormalizedDirection = free2Direction;
  }
}

uint16_t BluetoothHIDManager::parseHIDReport(uint8_t* data, size_t length) {
  if (length < 3) {
    LOG_ERR("BT", "Invalid HID report length: %d", length);
    return 0;
  }
  
  uint8_t modifier = data[0];
  uint8_t keycode = data[2]; // First key in the report
  
  // If no key pressed (all zeros), return 0
  if (keycode == 0 && modifier == 0) {
    return 0;
  }
  
  // Log non-empty reports only during active debug capture to keep the hot path light.
  if (_debugCaptureEnabled) {
    LOG_INF("BT", "HID Report: mod=0x%02X key=0x%02X", modifier, keycode);
  }
  
  // Combine modifier and keycode (modifier in upper byte, keycode in lower)
  uint16_t combined = (static_cast<uint16_t>(modifier) << 8) | keycode;
  
  return combined;
}

// Map HID keycodes to navigator buttons based on device profile
// Only maps keycodes that match the current device's profile to prevent
// unwanted D-pad or other button inputs from triggering page turns
uint8_t BluetoothHIDManager::mapKeycodeToButton(uint8_t keycode, ConnectedDevice* device) {
  const DeviceProfiles::DeviceProfile* profile = device ? device->profile : nullptr;

  // Log keycode for debugging
  if (keycode != 0x00) {
    LOG_DBG("BT", "mapKeycodeToButton() called with keycode: 0x%02X", keycode);
  }

  // CrumBLE 4.5.5: rich BLE button map override. The application wires a
  // resolver at boot that consults SETTINGS.bleKeyMap and returns the
  // mapped HalGPIO::BTN_* for a (kind, value) pair (or 0xFF if not mapped).
  // Lives in the app layer because the HAL must not depend on
  // CrossPointSettings (src/). kind == 1 == HID usage code (the only kind
  // CrumBLE currently captures via its onReport hook).
  if (_bleKeyMapResolver) {
    const uint8_t mapped = _bleKeyMapResolver(1, keycode);
    if (mapped != 0xFF) {
      return mapped;
    }
  }
  
  // If we have a device profile, ONLY map keycodes specific to that profile
  if (profile) {
    // Free 2 reports a rolling keycode family while button is held.
    // These groups are captured from device logs and map to stable page actions.
    if (strcmp(profile->name, "Free2-M") == 0 || strcmp(profile->name, "Free2 Style") == 0) {
      const bool isForward =
          keycode == 0x1C || keycode == 0xC4 || keycode == 0x6C || keycode == 0xBC;
      const bool isBack =
          keycode == 0xB4 || keycode == 0x0E || keycode == 0x66 || keycode == 0x16;

      if (isForward) {
        LOG_INF("BT", "Free2 rolling-code forward match: 0x%02X", keycode);
        return HalGPIO::BTN_DOWN;
      }

      if (isBack) {
        LOG_INF("BT", "Free2 rolling-code back match: 0x%02X", keycode);
        return HalGPIO::BTN_UP;
      }
    }

    if (strncmp(profile->name, "IINE Game Brick", 15) == 0) {
      bool inReaderContext = false;
      if (_readerContextCallback) {
        inReaderContext = _readerContextCallback();
      }

      // Synthetic A/B mapping:
      // - Menus: A=Confirm, B=Back
      // - Reader: A=PageForward, B=PageBack
      if (keycode == GAMEBRICK_ACTION_A_CODE) {
        return inReaderContext ? HalGPIO::BTN_DOWN : HalGPIO::BTN_CONFIRM;
      }

      if (keycode == GAMEBRICK_ACTION_B_CODE) {
        return inReaderContext ? HalGPIO::BTN_UP : HalGPIO::BTN_BACK;
      }

      // Physical UP button (byte[4]=0x07 = profile->pageDownCode).
      // Maps to BTN_UP in all contexts: navigate up in menus, page-back in reader.
      if (keycode == profile->pageDownCode) {
        return HalGPIO::BTN_UP;
      }

      // Physical DOWN button (byte[4]=0x09 = profile->pageUpCode).
      // Maps to BTN_DOWN in all contexts: navigate down in menus, page-forward in reader.
      if (keycode == profile->pageUpCode) {
        return HalGPIO::BTN_DOWN;
      }

      // Keyboard/consumer-mode directional mappings (C/T/H mode variants).
      if (keycode == DeviceProfiles::KEYBOARD_UP_ARROW ||
          keycode == DeviceProfiles::KEYBOARD_PAGE_UP ||
          keycode == DeviceProfiles::STANDARD_PAGE_DOWN) {
        return HalGPIO::BTN_UP;
      }

      if (keycode == DeviceProfiles::KEYBOARD_DOWN_ARROW ||
          keycode == DeviceProfiles::KEYBOARD_PAGE_DOWN ||
          keycode == DeviceProfiles::STANDARD_PAGE_UP) {
        return HalGPIO::BTN_DOWN;
      }

      // Joystick LEFT/RIGHT (decoded from byte[3] offset when byte[4]=0x08).
      // In non-reader context: emit true LEFT/RIGHT so activities can decide
      // behavior (many menus already treat LEFT/RIGHT as prev/next via ButtonNavigator).
      // In reader context: suppress to avoid accidental exits/actions.
      if (!inReaderContext) {
        if (keycode == DeviceProfiles::KEYBOARD_LEFT_ARROW) return HalGPIO::BTN_LEFT;
        if (keycode == DeviceProfiles::KEYBOARD_RIGHT_ARROW) return HalGPIO::BTN_RIGHT;
      }

      if (keycode == DeviceProfiles::KEYBOARD_ENTER || keycode == DeviceProfiles::KEYBOARD_SPACE) {
        return HalGPIO::BTN_CONFIRM;
      }

      return 0xFF;
    }

    if (keycode == profile->pageUpCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Matched profile pageUpCode 0x%02X (%s) -> PageBack", keycode, profile->name);
      }
      return HalGPIO::BTN_UP;
    } else if (keycode == profile->pageDownCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Matched profile pageDownCode 0x%02X (%s) -> PageForward", keycode, profile->name);
      }
      return HalGPIO::BTN_DOWN;
    }

    // The known profile didn't recognise this keycode. For non-strict (standard layout)
    // profiles, also consult the user-learned custom mapping as a fallback. This covers
    // the common case where a device partially matches a known profile (e.g. its back
    // button matches MINI_KEYBOARD but its forward button uses a different code).
    const bool isStrict = profile->strictProfile;
    if (!isStrict) {
      if (const auto* learned = DeviceProfiles::getCustomProfile()) {
        if (keycode == learned->pageUpCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> PageBack (profile=%s)", keycode, profile->name);
          return HalGPIO::BTN_UP;
        }
        if (keycode == learned->pageDownCode) {
          LOG_INF("BT", "Custom-fallback: 0x%02X -> PageForward (profile=%s)", keycode, profile->name);
          return HalGPIO::BTN_DOWN;
        }
      }
    }

    // Not matched by profile or fallback - ignore
    LOG_DBG("BT", "Keycode 0x%02X not in profile %s (expecting 0x%02X/0x%02X), ignoring",
            keycode, profile->name, profile->pageUpCode, profile->pageDownCode);
    return 0xFF;
  }

  // Learned mappings are only used for unknown devices.
  if (const auto* customProfile = DeviceProfiles::getCustomProfile()) {
    if (keycode == customProfile->pageUpCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> PageBack", keycode);
      }
      return HalGPIO::BTN_UP;
    }
    if (keycode == customProfile->pageDownCode) {
      if (_debugCaptureEnabled) {
        LOG_INF("BT", "Mapped learned key 0x%02X -> PageForward", keycode);
      }
      return HalGPIO::BTN_DOWN;
    }
  }

  // No profile match - use broad common-key mapping for generic remotes/keyboards.
  bool pageForward = false;
  if (DeviceProfiles::mapCommonCodeToDirection(keycode, pageForward)) {
    if (_debugCaptureEnabled) {
      if (pageForward) {
        LOG_INF("BT", "Mapped generic key 0x%02X -> PageForward", keycode);
      } else {
        LOG_INF("BT", "Mapped generic key 0x%02X -> PageBack", keycode);
      }
    }
    return pageForward ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
  }

  if (keycode == 0x00) {
    return 0xFF;
  }

  if (device && device->simpleFallbackEnabled) {
    if (device->simpleForwardKeycode == 0x00) {
      device->simpleForwardKeycode = keycode;
      LOG_INF("BT", "Simple fallback learned FORWARD keycode 0x%02X", keycode);

      if (device->simpleBackKeycode != 0x00) {
        const uint8_t idx = (device->descriptorSuggestedIndex == 0xFF) ? 2 : device->descriptorSuggestedIndex;
        DeviceProfiles::setCustomProfileForDevice(device->address, device->simpleBackKeycode,
                                                  device->simpleForwardKeycode, idx);
      }
      return HalGPIO::BTN_DOWN;
    }

    if (keycode == device->simpleForwardKeycode) {
      return HalGPIO::BTN_DOWN;
    }

    if (device->simpleBackKeycode == 0x00) {
      device->simpleBackKeycode = keycode;
      LOG_INF("BT", "Simple fallback learned BACK keycode 0x%02X", keycode);
      const uint8_t idx = (device->descriptorSuggestedIndex == 0xFF) ? 2 : device->descriptorSuggestedIndex;
      DeviceProfiles::setCustomProfileForDevice(device->address, device->simpleBackKeycode,
                                                device->simpleForwardKeycode, idx);
      return HalGPIO::BTN_UP;
    }

    if (keycode == device->simpleBackKeycode) {
      return HalGPIO::BTN_UP;
    }
  }

  LOG_DBG("BT", "Unmapped keycode: 0x%02X (no profile)", keycode);
  return 0xFF;
}

void BluetoothHIDManager::updateActivity() {
  unsigned long now = millis();

  for (auto& device : _connectedDevices) {
    if (device.pendingGameBrickRelease && device.pendingGameBrickReleaseMs > 0 && now >= device.pendingGameBrickReleaseMs) {
      if (_buttonInjector && device.activeInjectedButton != 0xFF) {
        _buttonInjector(device.activeInjectedButton, false);
      }
      device.activeInjectedButton = 0xFF;
      device.lastButtonState = false;
      device.lastHIDKeycode = 0x00;
      device.lastNormalizedPressed = false;
      device.pendingGameBrickRelease = false;
      device.pendingGameBrickReleaseMs = 0;
      device.pendingGameBrickKeycode = 0x00;
      device.pendingGameBrickButton = 0xFF;
      LOG_DBG("BT", "Game Brick: released deferred action button for %s", device.address.c_str());
    }
  }

  // Fast path: release stale injected buttons promptly for Free2 only.
  // Free2 often omits timely release frames; other remotes should keep their prior behavior.
  for (auto& device : _connectedDevices) {
    if (isFree2Profile(device.profile) && device.activeInjectedButton != 0xFF) {
      const bool inReaderContext = _readerContextCallback && _readerContextCallback();
      const unsigned long staleReleaseMs = inReaderContext ? FREE2_STALE_RELEASE_READER_MS
                                                           : FREE2_STALE_RELEASE_DEFAULT_MS;
      if (now - device.lastActivityTime <= staleReleaseMs) {
        continue;
      }
      if (_buttonInjector) {
        _buttonInjector(device.activeInjectedButton, false);
      }
      device.activeInjectedButton = 0xFF;
      device.lastButtonState = false;
      device.lastHIDKeycode = 0x00;
      LOG_DBG("BT", "Released stale injected button for %s", device.address.c_str());
    }
  }

  // Slow path: connection maintenance every 10 seconds.
  if (now - lastMaintenanceCheck < 10000) {
    return;
  }
  lastMaintenanceCheck = now;

  // Preserve the original stale-release maintenance behavior for non-Free2 devices.
  for (auto& device : _connectedDevices) {
    if (!isFree2Profile(device.profile) && device.activeInjectedButton != 0xFF && now - device.lastActivityTime > 250) {
      if (_buttonInjector) {
        _buttonInjector(device.activeInjectedButton, false);
      }
      device.activeInjectedButton = 0xFF;
      device.lastButtonState = false;
      device.lastHIDKeycode = 0x00;
      LOG_DBG("BT", "Released stale injected button for %s", device.address.c_str());
    }
  }

  // Check for one inactive connection and disconnect it in-place.
  std::string inactiveAddress;
  unsigned long inactiveTimeMs = 0;
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime == 0) {
      continue;
    }

    unsigned long inactiveTime = now - device.lastActivityTime;
    if (inactiveTime > INACTIVITY_TIMEOUT_MS) {
      inactiveAddress = device.address;
      inactiveTimeMs = inactiveTime;
      break;
    }
  }

  if (!inactiveAddress.empty()) {
    LOG_INF("BT", "Device %s inactive for %lu ms, disconnecting", inactiveAddress.c_str(), inactiveTimeMs);
    disconnectFromDevice(inactiveAddress);
  }
}

void BluetoothHIDManager::resetAutoReconnectBackoff() {
  _reconnectFailures = 0;
  _nextReconnectAllowedMs = 0;
}

void BluetoothHIDManager::checkAutoReconnect(bool userInputDetected) {
  if (!_enabled) {
    return;
  }

  static unsigned long lastReconnectCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  unsigned long now = millis();
  
  // Only check every 5 seconds to avoid hammering
  if (now - lastReconnectCheck < 5000) {
    return;
  }
  lastReconnectCheck = now;

  // Remove stale disconnected clients from active list.
  for (auto it = _connectedDevices.begin(); it != _connectedDevices.end();) {
    if (!it->client || !it->client->isConnected()) {
      if (_buttonInjector && it->activeInjectedButton != 0xFF) {
        _buttonInjector(it->activeInjectedButton, false);
      }
      LOG_DBG("BT", "Pruning stale disconnected client entry: %s client=%p", it->address.c_str(), it->client);
      it = _connectedDevices.erase(it);
    } else {
      ++it;
    }
  }

  // Already connected.
  if (!_connectedDevices.empty()) {
    LOG_DBG("BT", "AutoReconnect skipped: already connected");
    return;
  }

  // Reconnect is user-driven while reading: require a local button event.
  if (!userInputDetected) {
    LOG_DBG("BT", "AutoReconnect skipped: no local user input");
    return;
  }

  // Avoid reconnect storms.
  if (now - lastReconnectAttempt < 2000) {
    LOG_DBG("BT", "AutoReconnect skipped: cooldown active (%lu ms)", now - lastReconnectAttempt);
    return;
  }

  // 4.7.4: failure backoff. Each attempt below blocks the loop for 2-3 s when
  // the remote isn't reachable, which reads as "the buttons are laggy". After
  // a failure, hold off progressively (10s, 20s, 40s, ... capped at 5 min)
  // instead of re-freezing on the next press.
  if (_nextReconnectAllowedMs != 0 && now < _nextReconnectAllowedMs) {
    LOG_DBG("BT", "AutoReconnect skipped: backoff for %lu more ms after %u failures",
            _nextReconnectAllowedMs - now, static_cast<unsigned>(_reconnectFailures));
    return;
  }
  lastReconnectAttempt = now;

  if (_bondedDeviceAddress.empty()) {
    LOG_DBG("BT", "AutoReconnect skipped: no bonded device configured");
    return;
  }

  LOG_INF("BT", "Button activity detected while disconnected, reconnecting to bonded device %s",
          _bondedDeviceAddress.c_str());

  if (connectToDevice(_bondedDeviceAddress)) {
    LOG_INF("BT", "Reconnected to bonded device %s", _bondedDeviceAddress.c_str());
    resetAutoReconnectBackoff();
  } else {
    if (_reconnectFailures < 5) _reconnectFailures++;
    // 10s, 20s, 40s, 80s, 160s -- then hold at 300s.
    unsigned long holdMs = 10000UL << (_reconnectFailures - 1);
    if (holdMs > 300000UL) holdMs = 300000UL;
    _nextReconnectAllowedMs = now + holdMs;
    LOG_ERR("BT", "Reconnect to bonded device %s failed: %s (backoff %lu ms, failures=%u)",
            _bondedDeviceAddress.c_str(), lastError.c_str(), holdMs,
            static_cast<unsigned>(_reconnectFailures));
  }
}

void BluetoothHIDManager::saveState() {
  LOG_DBG("BT", "Saving state (stub)");
  // Stub: would save paired devices to file
}

void BluetoothHIDManager::loadState() {
  LOG_DBG("BT", "Loading state (stub)");
  // Stub: would load paired devices from file
}

