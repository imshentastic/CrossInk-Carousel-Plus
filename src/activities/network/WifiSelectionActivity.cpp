#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#ifndef SIMULATOR
#include <esp_err.h>
#include <esp_wifi.h>
#endif

#include <map>

#include "CrossPointSettings.h"
#include "ReadingStats.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

#ifndef SIMULATOR
uint8_t sLastStaDisconnectReason = 0;
bool sConnectionAttemptLoggingActive = false;
bool sWifiEventLoggingRegistered = false;
// v18.9.9.419: track consecutive AUTH_EXPIRE (reason=2) events. On silent-
// reboot to FT the AP often holds stale association state for our old
// session and rejects our new auth attempts for 10-15 seconds. Field log
// showed 12 AUTH_EXPIRE events @ ~1s each before the AP finally released
// the old state. checkConnectionStatus reads these counters and triggers
// a full WiFi driver reset after 3 in a row -- forces the AP to see us
// as a genuinely new client on the next auth.
uint8_t sAuthExpireStreak = 0;
uint32_t sLastAuthExpireMs = 0;
bool sAuthExpireKickApplied = false;

void logWifiStationEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (!sConnectionAttemptLoggingActive) {
    return;
  }

  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      LOG_INF("WIFI", "STA event: connected to AP");
      sAuthExpireStreak = 0;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      const uint8_t* ip = reinterpret_cast<const uint8_t*>(&info.got_ip.ip_info.ip.addr);
      LOG_INF("WIFI", "STA event: got IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t reason = info.wifi_sta_disconnected.reason;
      if (reason == 0) {
        reason = WIFI_REASON_UNSPECIFIED;
      }
      sLastStaDisconnectReason = reason;
      LOG_INF("WIFI", "STA event: disconnected reason=%u(%s)", reason,
              WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason)));
      if (reason == WIFI_REASON_AUTH_EXPIRE) {
        ++sAuthExpireStreak;
        sLastAuthExpireMs = millis();
      } else {
        sAuthExpireStreak = 0;
      }
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      LOG_INF("WIFI", "STA event: lost IP");
      break;
    default:
      break;
  }
}

void ensureWifiEventLoggingRegistered() {
  if (sWifiEventLoggingRegistered) {
    return;
  }
  WiFi.onEvent(logWifiStationEvent, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(logWifiStationEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(logWifiStationEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(logWifiStationEvent, ARDUINO_EVENT_WIFI_STA_LOST_IP);
  sWifiEventLoggingRegistered = true;
}
#else
void ensureWifiEventLoggingRegistered() {}
#endif

const char* wifiStatusName(const wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID_AVAIL";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
#ifndef SIMULATOR
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
#endif
    case WL_DISCONNECTED:
      return "DISCONNECTED";
#ifndef SIMULATOR
    case WL_NO_SHIELD:
      return "NO_SHIELD";
    case WL_STOPPED:
      return "STOPPED";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
#endif
    default:
      return "UNKNOWN";
  }
}

bool wifiStatusIsConnectionFailure(const wl_status_t status) {
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    return true;
  }
#ifndef SIMULATOR
  return status == WL_CONNECTION_LOST;
#else
  return false;
#endif
}

const char* wifiAuthName(const int authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
#ifndef SIMULATOR
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA_PSK";
#endif
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2_PSK";
#ifndef SIMULATOR
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI_PSK";
    case WIFI_AUTH_OWE:
      return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:
      return "WPA3_ENT_192";
#endif
    default:
      return "UNKNOWN";
  }
}

}  // namespace

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();
  SET_CHECKPOINT("wifiSel:onEnter");

  // v18.9.9.336: silent-restart pre-flight. WiFi.mode(WIFI_STA) has been
  // observed to null-deref inside wpa_supplicant/eloop.c on an in-book
  // fragmented heap (RTC checkpoint wifi:mode-STA -> Guru Meditation Load
  // access fault at MEPC 0x4218dbf2). Fresh boot heap (~90 KB free /
  // 60 KB maxAlloc) reliably clears the ESP-IDF WiFi init path.
  //
  // v18.9.9.350: MOVED before ensureWifiEventLoggingRegistered(). The event-
  // handler registration eats ~50 KB of ESP-IDF WiFi driver init, which
  // dropped a 106 KB entry heap to 52 KB and tripped the preflight AFTER
  // we'd already committed the memory. Field symptom: FT auto-restore
  // JOIN_NETWORK spawned WifiSelection with plenty of heap, but the
  // preflight fired anyway and silent-restarted (losing the FT return
  // context, so the connect eventually landed on Home not back in FT).
  // Checking heap BEFORE the WiFi driver init reflects true fragmentation.
  constexpr uint32_t kWifiEnterMinFree = 55U * 1024U;
  constexpr uint32_t kWifiEnterMinMaxAlloc = 40U * 1024U;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  // v18.9.9.353: skip the preflight when WiFi is ALREADY in STA mode --
  // the caller (e.g. FT auto-restore path in CrossPointWebServerActivity)
  // has already called WiFi.mode(WIFI_STA), which is the exact call the
  // preflight was designed to protect. Re-checking heap after that
  // ~50 KB commitment is too late: WiFi is either already init'd
  // successfully (no crash to protect against) or the panic already
  // happened. Silent-restart in this case just loses the caller's
  // return context (FT ends up on Home instead of getting the connect
  // callback).
  const bool wifiAlreadyStaMode = (WiFi.getMode() & WIFI_STA) != 0;
  if (!g_postWifiSelectionSilentReboot && !wifiAlreadyStaMode &&
      (freeHeap < kWifiEnterMinFree || maxAlloc < kWifiEnterMinMaxAlloc)) {
    LOG_INF("WIFI",
            "WifiSelection heap pre-flight tripped (free=%u<%u OR maxAlloc=%u<%u); "
            "silent-restart-to-self to avoid wpa_supplicant crash",
            freeHeap, static_cast<unsigned>(kWifiEnterMinFree),
            maxAlloc, static_cast<unsigned>(kWifiEnterMinMaxAlloc));
    silentRestartToWifiSelection();
    return;  // never reached
  }
  if (wifiAlreadyStaMode) {
    LOG_INF("WIFI", "WifiSelection: WiFi already in STA mode (from caller), skipping preflight (free=%u maxAlloc=%u)",
            freeHeap, maxAlloc);
  }
  // Consume the flag so a subsequent in-session re-entry gets the
  // pre-flight again (if the heap re-degraded between the boot and the
  // user's second WiFi visit).
  g_postWifiSelectionSilentReboot = false;

  ensureWifiEventLoggingRegistered();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;
  lastConnectionStatusLogTime = 0;
  lastLoggedWifiStatus = -1;

  // Cache MAC address for display
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[64];
  snprintf(macStr, sizeof(macStr), "%s %02x-%02x-%02x-%02x-%02x-%02x", tr(STR_MAC_ADDRESS), mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  cachedMacAddress = std::string(macStr);

  // Trigger first update to show scanning message
  requestUpdate();

  // Attempt to auto-connect to the last network
  if (allowAutoConnect) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto* cred = WIFI_STORE.findCredential(lastSsid);
      if (cred) {
        LOG_INF("WIFI", "Auto-connect candidate: ssid=%s saved=1", lastSsid.c_str());
        selectedSSID = cred->ssid;
        enteredPassword = cred->password;
        selectedRequiresPassword = !cred->password.empty();
        usedSavedPassword = true;
        autoConnecting = true;
        attemptConnection();
        requestUpdate();
        return;
      }
    }
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan() {
  autoConnecting = false;
  state = WifiSelectionState::SCANNING;
  networks.clear();
  requestUpdate();

  // Set WiFi mode to station
  LOG_INF("WIFI", "Starting WiFi scan (mode=%d status=%d/%s heap=%u maxAlloc=%u)", static_cast<int>(WiFi.getMode()),
          static_cast<int>(WiFi.status()), wifiStatusName(WiFi.status()), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  SET_CHECKPOINT("wifi:mode-STA");
  WiFi.mode(WIFI_STA);
  SET_CHECKPOINT("wifi:disconnect");
  WiFi.disconnect();
  delay(100);

  // Start async scan
  SET_CHECKPOINT("wifi:scanNetworks");
  const int scanStartResult = WiFi.scanNetworks(true);  // true = async scan
  SET_CHECKPOINT("wifi:scan-requested");
  LOG_INF("WIFI", "WiFi scan requested (result=%d)", scanStartResult);
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    LOG_INF("WIFI", "WiFi scan failed");
    state = WifiSelectionState::NETWORK_LIST;
    requestUpdate();
    return;
  }

  LOG_INF("WIFI", "WiFi scan complete: rawNetworks=%d", scanResult);

  // Scan complete, process results
  // Use a map to deduplicate networks by SSID, keeping the strongest signal
  std::map<std::string, WifiNetworkInfo> uniqueNetworks;
  int hiddenNetworks = 0;
  int duplicateNetworks = 0;

  for (int i = 0; i < scanResult; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    const int32_t rssi = WiFi.RSSI(i);
    const int authMode = WiFi.encryptionType(i);

    // Skip hidden networks (empty SSID)
    if (ssid.empty()) {
      hiddenNetworks++;
      continue;
    }

    // Check if we've already seen this SSID
    auto it = uniqueNetworks.find(ssid);
    if (it != uniqueNetworks.end()) {
      duplicateNetworks++;
    }
    if (it == uniqueNetworks.end() || rssi > it->second.rssi) {
      // New network or stronger signal than existing entry
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (authMode != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      uniqueNetworks[ssid] = network;
    }

    LOG_DBG("WIFI", "Scan result: ssid=%s rssi=%d auth=%s saved=%d", ssid.c_str(), rssi, wifiAuthName(authMode),
            WIFI_STORE.hasSavedCredential(ssid));
  }

  // Convert map to vector
  networks.clear();
  for (const auto& pair : uniqueNetworks) {
    // cppcheck-suppress useStlAlgorithm
    networks.push_back(pair.second);
  }

  // Sort: saved-password networks first, then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    return a.rssi > b.rssi;
  });

  WiFi.scanDelete();
  LOG_INF("WIFI", "WiFi scan usable networks=%zu hidden=%d duplicates=%d", networks.size(), hiddenNetworks,
          duplicateNetworks);
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];
  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;
  // Manual selection -> fresh retry budget for the new network. (The
  // previous network's failures shouldn't count against this one.)
  autoConnectRetryCount = 0;

  // Check if we have saved credentials for this network
  const auto* savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_INF("WIFI", "Selected network: ssid=%s encrypted=%d saved=1 rssi=%d", selectedSSID.c_str(),
            selectedRequiresPassword, network.rssi);
    LOG_DBG("WiFi", "Using saved password for %s, length: %zu", selectedSSID.c_str(), enteredPassword.size());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    // Show password entry
    state = WifiSelectionState::PASSWORD_ENTRY;
    // Don't allow screen updates while changing activity
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
                                                                   "",  // No initial text
                                                                   64,  // Max password length
                                                                   InputType::Password),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               state = WifiSelectionState::NETWORK_LIST;
                             } else {
                               enteredPassword = std::get<KeyboardResult>(result.data).text;
                               // state will be updated in next loop iteration
                             }
                           });
  } else {
    // Connect directly for open networks
    LOG_INF("WIFI", "Selected open network: ssid=%s rssi=%d", selectedSSID.c_str(), network.rssi);
    attemptConnection();
  }
}

void WifiSelectionActivity::attemptConnection() {
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  lastConnectionStatusLogTime = 0;
  lastLoggedWifiStatus = -1;
#ifndef SIMULATOR
  sLastStaDisconnectReason = 0;
  sConnectionAttemptLoggingActive = false;
  sAuthExpireStreak = 0;
  sLastAuthExpireMs = 0;
  sAuthExpireKickApplied = false;
#endif
  requestUpdate();

  LOG_INF("WIFI", "Connecting to ssid=%s auto=%d saved=%d encrypted=%d passProvided=%d heap=%u maxAlloc=%u",
          selectedSSID.c_str(), autoConnecting, usedSavedPassword, selectedRequiresPassword, !enteredPassword.empty(),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect

#ifndef SIMULATOR
  // v18.9.9.431: WiFi runtime override. v430 confirmed that sdkconfig
  // CONFIG_ESP_WIFI_* trims are ignored -- Arduino's WiFi.mode() calls
  // esp_wifi_init(WIFI_INIT_CONFIG_DEFAULT()) which materializes the
  // full-fat default buffer counts (10 static RX, 32 dynamic RX/TX,
  // AMPDU enabled). To actually apply our trims, call esp_wifi_init()
  // ourselves with a custom wifi_init_config_t BEFORE Arduino's implicit
  // init runs -- if we succeed, Arduino's subsequent call to
  // esp_wifi_init() will return ESP_ERR_WIFI_INIT_STATE (already inited)
  // and its own default cfg is ignored. Target savings: ~10-15 KB of
  // RX buffer pool + AMPDU block-ack state. One-shot; safe if it fails.
  {
    static bool sCustomWifiInitAttempted = false;
    if (!sCustomWifiInitAttempted && WiFi.getMode() == WIFI_MODE_NULL) {
      sCustomWifiInitAttempted = true;
      wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
      cfg.static_rx_buf_num = 4;
      cfg.dynamic_rx_buf_num = 8;
      cfg.dynamic_tx_buf_num = 8;
      cfg.ampdu_rx_enable = false;
      cfg.ampdu_tx_enable = false;
      cfg.mgmt_sbuf_num = 8;
      const uint32_t freeBefore = ESP.getFreeHeap();
      const esp_err_t err = esp_wifi_init(&cfg);
      const uint32_t freeAfter = ESP.getFreeHeap();
      if (err == ESP_OK) {
        LOG_INF("WIFI",
                "Runtime WiFi init OK: rx=4/8 tx=8 ampdu=off mgmt=8 (heap %u -> %u, delta=%d)",
                freeBefore, freeAfter, static_cast<int>(freeBefore - freeAfter));
      } else {
        LOG_INF("WIFI", "Runtime WiFi init skipped: %s (Arduino may have inited first)",
                esp_err_to_name(err));
      }
    }
  }
#endif

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);  // Abort any in-progress SDK auto-connect and clear NVS-saved SSID
  delay(100);
#ifndef SIMULATOR
  sLastStaDisconnectReason = 0;
  sConnectionAttemptLoggingActive = true;
#endif

  // Set hostname so routers show "CrossPoint-Reader-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());

  wl_status_t beginStatus = WL_IDLE_STATUS;
  if (selectedRequiresPassword && !enteredPassword.empty()) {
    beginStatus = WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    beginStatus = WiFi.begin(selectedSSID.c_str());
  }
  LOG_INF("WIFI", "WiFi.begin returned status=%d/%s", static_cast<int>(beginStatus), wifiStatusName(beginStatus));
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();
  const unsigned long now = millis();

  if (lastLoggedWifiStatus != static_cast<int>(status) ||
      now - lastConnectionStatusLogTime >= CONNECTION_STATUS_LOG_INTERVAL_MS) {
    LOG_INF("WIFI", "Connection poll: elapsed=%lums status=%d/%s rssi=%d", now - connectionStartTime,
            static_cast<int>(status), wifiStatusName(status), status == WL_CONNECTED ? WiFi.RSSI() : 0);
    lastLoggedWifiStatus = static_cast<int>(status);
    lastConnectionStatusLogTime = now;
  }

#ifndef SIMULATOR
  // v18.9.9.419: AUTH_EXPIRE cascade kick. On silent-reboot-to-FT the AP
  // often refuses re-auth for 10-15 s while it clears the stale association
  // from our previous session. The internal WiFi driver just keeps retrying
  // silently. Once we've seen 3 AUTH_EXPIRE events in a row within the last
  // ~4 s, cycle the STA driver hard -- disconnect(true, true) + mode(OFF)
  // + short delay + mode(STA) + begin() again. That kicks the AP into
  // seeing us as a fresh client and skips ~10 s of waiting for the old
  // association to naturally time out. One-shot per connect attempt.
  if (!sAuthExpireKickApplied && sAuthExpireStreak >= 3 &&
      now - sLastAuthExpireMs < 4000) {
    LOG_INF("WIFI",
            "AUTH_EXPIRE kick: %u consecutive AUTH_EXPIRE events -- cycling STA driver to force fresh association",
            static_cast<unsigned>(sAuthExpireStreak));
    sAuthExpireKickApplied = true;
    sAuthExpireStreak = 0;
    // Suspend event-driven counter updates while we tear the stack down;
    // the disconnect/mode-off calls will fire their own STA_DISCONNECTED
    // events which we don't want to count against a future streak.
    sConnectionAttemptLoggingActive = false;
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    delay(50);
    sConnectionAttemptLoggingActive = true;
    sLastStaDisconnectReason = 0;
    // Re-issue the same begin() the initial attemptConnection() used.
    if (selectedRequiresPassword && !enteredPassword.empty()) {
      WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
    } else {
      WiFi.begin(selectedSSID.c_str());
    }
  }
#endif

  if (status == WL_CONNECTED) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    autoConnecting = false;
#ifndef SIMULATOR
    sConnectionAttemptLoggingActive = false;
#endif
    LOG_INF("WIFI", "Connected to ssid=%s ip=%s rssi=%d", selectedSSID.c_str(), connectedIP.c_str(), WiFi.RSSI());
    // Clean connect -> reset the auto-retry budget for the NEXT failure
    // cycle. Without this, a network that took 2 retries this session
    // would start the next failure cycle with only 1 retry left.
    autoConnectRetryCount = 0;

    // Sync RTC from NTP on the first successful WiFi connection only. The DS3231
    // drifts ~2 ppm so one sync is enough; users can force a re-sync from
    // Settings > Sync & Network > Sync Time.
    // v18.9.9.292: also do the sync on X4 (no DS3231) so the ESP32
    // system clock gets set for ReadingStats. syncFromNTP no longer
    // early-returns on !_available; it skips the DS3231 write instead.
    // v18.9.9.347: gate on !halClock.hasValidTime() instead of
    // !halClock.isAvailable(). On X4, isAvailable() always returns
    // false (no DS3231), so the old gate ALWAYS ran NTP on every WiFi
    // connect within a session -- including the FT auto-restore
    // JOIN_NETWORK path. Each SNTP setup leaks ~15-25 KB that the
    // subsequent FT web-server allocs couldn't absorb, hitting OOM +
    // std::terminate at wifiSel:onEnter. hasValidTime() checks the
    // ESP32 SoC system clock which survives within a boot cycle, so
    // this reduces to "NTP once per cold boot on X4" -- matching the
    // X3 "NTP once per SD-persisted clockHasBeenSynced" cadence.
    if (!SETTINGS.clockHasBeenSynced || !halClock.hasValidTime()) {
      SET_CHECKPOINT("wifiSel:ntpBegin");
      if (halClock.syncFromNTP()) {
        SET_CHECKPOINT("wifiSel:ntpDone");
        if (halClock.isAvailable()) {
          SETTINGS.clockHasBeenSynced = 1;
          SETTINGS.saveToFile();
        }
        ReadingStats::reevaluateClockStatus();
      }
    }
    SET_CHECKPOINT("wifiSel:postNtp");

    // Save this as the last connected network - SD card operations need lock as
    // we use SPI for both
    {
      RenderLock lock(*this);
      WIFI_STORE.setLastConnectedSsid(selectedSSID);
    }

    // If we entered a new password, ask if user wants to save it
    // Otherwise, immediately complete so parent can start web server
    if (!usedSavedPassword && !enteredPassword.empty()) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      requestUpdate();
    } else {
      // Using saved password or open network - complete immediately
      if (allowAutoConnect) {
        LOG_DBG("WIFI",
                "Connected with saved/open credentials, "
                "completing immediately");
        onComplete(true);
      } else {
        LOG_DBG("WIFI", "Connected from manual network settings, showing connected status");
        state = WifiSelectionState::CONNECTED;
        requestUpdate();
      }
    }
    return;
  }

  if (wifiStatusIsConnectionFailure(status)) {
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    LOG_INF("WIFI", "Connection failed: ssid=%s status=%d/%s elapsed=%lums", selectedSSID.c_str(),
            static_cast<int>(status), wifiStatusName(status), now - connectionStartTime);
#ifndef SIMULATOR
    if (sLastStaDisconnectReason != 0) {
      LOG_INF("WIFI", "Last disconnect reason: %u(%s)", sLastStaDisconnectReason,
              WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(sLastStaDisconnectReason)));
    }
    sConnectionAttemptLoggingActive = false;
#endif
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }

  // Check for timeout
  if (millis() - connectionStartTime > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect();
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    LOG_INF("WIFI", "Connection timed out: ssid=%s elapsed=%lums lastStatus=%d/%s", selectedSSID.c_str(),
            millis() - connectionStartTime, static_cast<int>(status), wifiStatusName(status));
#ifndef SIMULATOR
    if (sLastStaDisconnectReason != 0) {
      LOG_INF("WIFI", "Last disconnect reason before timeout: %u(%s)", sLastStaDisconnectReason,
              WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(sLastStaDisconnectReason)));
    }
    sConnectionAttemptLoggingActive = false;
#endif
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }
}

void WifiSelectionActivity::loop() {
  if ((state == WifiSelectionState::SCANNING || state == WifiSelectionState::CONNECTING ||
       state == WifiSelectionState::AUTO_CONNECTING) &&
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
#ifndef SIMULATOR
    sConnectionAttemptLoggingActive = false;
#endif
    WiFi.disconnect();
    onComplete(false);
    return;
  }

  // Check scan progress
  if (state == WifiSelectionState::SCANNING) {
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    checkConnectionStatus();
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      }
      // Complete - parent will start web server
      onComplete(true);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      onComplete(true);
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was selected)
      startWifiScan();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      startWifiScan();
    }
    return;
  }

  if (state == WifiSelectionState::CONNECTED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onComplete(true);
    }
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    // CrumBLE 4.5.5+: silent-retry on transient failure for auto-connect /
    // saved-password attempts. AUTH_EXPIRE / HANDSHAKE_TIMEOUT during a
    // post-restart auto-reconnect is almost always the AP being briefly
    // busy (recovering from its own load, mid-scan from another client,
    // etc.) rather than a credential problem. Previously this routed
    // straight to FORGET_PROMPT, which is destructive UX -- one transient
    // blip and the user is one click away from losing the saved password
    // that was perfectly correct. Now we silently retry the same network
    // up to kMaxAutoRetries times before falling back to the network list,
    // and we NEVER auto-offer to forget. The Forget affordance stays
    // reachable via the explicit Left-button shortcut in the network list
    // for users who genuinely want to remove a credential.
    if ((autoConnecting || usedSavedPassword) &&
        autoConnectRetryCount < kMaxAutoRetries) {
      autoConnectRetryCount++;
      LOG_INF("WIFI",
              "Auto-retry %d/%d after transient failure on %s; retrying same network",
              autoConnectRetryCount, kMaxAutoRetries, selectedSSID.c_str());
      attemptConnection();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Out of retry budget (or this was a manual attempt that failed).
      // Go back to the network list either way -- the user can re-pick
      // the same network to start a fresh attempt cycle, or pick a
      // different one. No auto-forget prompt; that path is only
      // user-initiated via the Left-button shortcut on a saved-network
      // entry in the list.
      autoConnecting = false;
      autoConnectRetryCount = 0;
      state = WifiSelectionState::NETWORK_LIST;
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Handle navigation
    buttonNavigator.onNext([this] {
      selectedNetworkIndex = ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size());
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      selectedNetworkIndex = ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size());
      requestUpdate();
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(RenderLock&&) {
  // Don't render if we're in PASSWORD_ENTRY state - we're just transitioning
  // from the keyboard subactivity back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    return;
  }

  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // Draw header
  char countStr[32];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), networks.size());
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WIFI_NETWORKS), countStr);
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      cachedMacAddress.c_str());

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting(&screen, &metrics);  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case WifiSelectionState::SAVE_PROMPT:
      renderSavePrompt(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed(&screen, &metrics);
      break;
    case WifiSelectionState::FORGET_PROMPT:
      renderForgetPrompt(&screen, &metrics);
      break;
    case WifiSelectionState::PASSWORD_ENTRY:
      break;  // Handled by early return above
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) const {
  if (networks.empty()) {
    // No networks found or scan failed
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = screen->y + (screen->height - height) / 2;
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, tr(STR_NO_NETWORKS));
    UITheme::drawCenteredText(renderer, *screen, SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  } else {
    int contentTop =
        screen->y + metrics->topPadding + metrics->headerHeight + metrics->tabBarHeight + metrics->verticalSpacing;
    int contentHeight = screen->height - contentTop - metrics->verticalSpacing * 2;
    GUI.drawList(
        renderer, Rect{screen->x, contentTop, screen->width, contentHeight}, static_cast<int>(networks.size()),
        selectedNetworkIndex, [this](int index) { return networks[index].ssid; }, nullptr, nullptr,
        [this](int index) {
          auto network = networks[index];
          return std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
                 getSignalStrengthIndicator(network.rssi);
        });
  }

  GUI.drawHelpText(renderer,
                   Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
                   tr(STR_NETWORK_LEGEND));

  const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;

  if (state == WifiSelectionState::SCANNING) {
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, tr(STR_SCANNING));
  } else {
    UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_CONNECTING), true,
                              EpdFontFamily::BOLD);

    std::string ssidInfo = std::string(tr(STR_TO_PREFIX)) + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo.replace(22, ssidInfo.length() - 22, "...");
    }
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 4) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderSavePrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());

  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, tr(STR_SAVE_PASSWORD));

  // Draw Yes/No buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = screen->x + (screen->width - totalWidth) / 2;

  // Draw "Yes" button
  if (savePromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_YES)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_YES));
  }

  // Draw "No" button
  if (savePromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_NO)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_NO));
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_FORGET_NETWORK), true,
                            EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());

  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, tr(STR_FORGET_AND_REMOVE));

  // Draw Cancel/Forget network buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = screen->x + (screen->width - totalWidth) / 2;

  // Draw "Cancel" button
  if (forgetPromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_CANCEL)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_CANCEL));
  }

  // Draw "Forget network" button
  if (forgetPromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_FORGET_BUTTON)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_FORGET_BUTTON));
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::onComplete(const bool connected) {
  ActivityResult result;
  result.isCancelled = !connected;
  if (connected) {
    result.data = WifiResult{true, selectedSSID, connectedIP};
  }
  setResult(std::move(result));
  finish();
}
