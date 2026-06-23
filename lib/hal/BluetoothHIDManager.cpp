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
  if (_enabled) {
    LOG_DBG("BT", "Already enabled");
    return true;
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
  constexpr uint32_t kMinFreeHeapForEnable = 66 * 1024;
  const uint32_t freeHeap = esp_get_free_heap_size();
  if (freeHeap < kMinFreeHeapForEnable) {
    LOG_ERR("BT", "Refusing to enable Bluetooth: free heap %u < %u needed for NimBLE", freeHeap,
            static_cast<unsigned>(kMinFreeHeapForEnable));
    lastError = "Not enough memory to enable Bluetooth";
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
  
  // Initialize NimBLE stack
  NimBLEDevice::init("CrossPoint");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // +9dBm
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);
  NimBLEDevice::setSecurityAuth(true, false, true);
  
  _enabled = true;
  lastError = "";
  
  LOG_INF("BT", "Bluetooth enabled successfully");
  loadState();
  return true;
}

bool BluetoothHIDManager::tryEnableIfRequested() {
  if (!_enableLaterRequested) return false;
  if (_enabled) {
    _enableLaterRequested = false;  // someone else already brought it back up
    return false;
  }

  if (!enable()) {
    // enable() refused -- typically free heap is just below the NimBLE threshold
    // right after a heap-heavy chapter build. KEEP _enableLaterRequested set so we
    // retry on a later tick once the heap recovers (it usually does within a
    // second as the build's transient memory frees). Clearing it here -- as we
    // used to -- left the bonded remote permanently off after a borderline miss,
    // so the user appeared unable to reconnect.
    LOG_DBG("BT", "tryEnableIfRequested: enable() deferred (%s); will retry when heap recovers", lastError.c_str());
    return false;
  }
  _enableLaterRequested = false;  // enabled successfully

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

  // Disconnect all devices
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
  NimBLEDevice::deinit(true);

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
  
  std::string address = advertisedDevice->getAddress().toString();
  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();
  
  // Check if device advertises HID service
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));
  
  // Check if we already have this device
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi; // Update RSSI
      if (isHID) dev.isHID = true;
      return;
    }
  }

  // CrumBLE 4.4: cap discovered devices. On a busy RF environment we see 20+
  // results and the post-scan picker activity allocates a std::vector<std::
  // string> with a "name (addr) RSSI" entry per device -- on the X3's NimBLE-
  // squeezed heap (NimBLE eats ~58 KB), even 20 entries hit the picker's
  // grow-on-build with MaxAlloc < entry size and abort(). Cap at 12 so the
  // picker fits comfortably; weakest-RSSI entry is evicted to keep the most
  // promising ones. HID devices (remotes) get eviction priority over non-HID.
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
      NimBLEClient* freshClient = NimBLEDevice::createClient(bleAddress);
      if (freshClient) {
        if (hadExistingClient) {
          LOG_INF("BT", "Reconnect with existing client failed; using fresh client");
        }
        pClient = freshClient;
        pClient->setSelfDelete(false, false);
        pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
        pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY,
                                     BLE_CONN_TIMEOUT, BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
        pClient->setClientCallbacks(&clientCallbacks);
      }

      if (!pClient->connect(bleAddress)) {
        lastError = "Connection failed";
        LOG_ERR("BT", "Failed to connect to %s (after retry)", address.c_str());
        return false;
      }
      LOG_INF("BT", "Connect succeeded on retry for %s", address.c_str());
    }

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
    
    // Subscribe to ALL Report characteristics with notify capability
    LOG_INF("BT", "Subscribing to %d Report characteristics...", reportChars.size());
    size_t successfulSubscriptions = 0;
    
    for (size_t i = 0; i < reportChars.size(); i++) {
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
    lastError = "Connected";
    return true;
}

void BluetoothHIDManager::noteClientDisconnect(int reason) {
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
  if (since < EARLY_DISCONNECT_MS) {
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

    if (device->profile == nullptr && likelyFree2Press && isPressed) {
      // Free 2 may not emit a clean initial release frame; arm on first valid press.
      device->hasSeenRelease = true;
      LOG_DBG("BT", "Arming auto-detect on first valid Free2 code: 0x%02X", keycode);
    }

    releaseInjectedButton();
    device->lastButtonState = isPressed;
    device->lastHIDKeycode = keycode;
    return;
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
  lastReconnectAttempt = now;

  if (_bondedDeviceAddress.empty()) {
    LOG_DBG("BT", "AutoReconnect skipped: no bonded device configured");
    return;
  }

  LOG_INF("BT", "Button activity detected while disconnected, reconnecting to bonded device %s",
          _bondedDeviceAddress.c_str());

  if (connectToDevice(_bondedDeviceAddress)) {
    LOG_INF("BT", "Reconnected to bonded device %s", _bondedDeviceAddress.c_str());
  } else {
    LOG_ERR("BT", "Reconnect to bonded device %s failed: %s", _bondedDeviceAddress.c_str(), lastError.c_str());
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

