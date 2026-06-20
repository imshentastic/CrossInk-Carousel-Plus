#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <cstring>

#include "../reader/EpubReaderActivity.h"  // prewarmReaderTextBuffer
#include "CrossPointSettings.h"
#include "DeviceProfiles.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// CrumBLE 4.4: pre-flight before starting a BT scan. With NimBLE up and a
// book open in the background, free heap can be < 7 KB / MaxAlloc < 5 KB --
// not enough for the scan callback to grow its result list + the post-scan
// picker activity to allocate per-device strings. The result was an abort()
// after "Scan complete, found N devices". Returns true (ok to scan) when
// there's enough headroom; otherwise sets `outError` and returns false so
// the caller can show the user a clear message rather than crashing.
bool checkScanHeapOrError(std::string& outError) {
  // Need to cover: scan onResult callbacks (~12 * std::string copies) +
  // picker activity (vector<MenuItem> with ~80 B per entry) + render path.
  constexpr uint32_t kScanMinFreeHeap = 14u * 1024u;
  constexpr uint32_t kScanMinMaxAlloc = 8u * 1024u;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap >= kScanMinFreeHeap && maxAlloc >= kScanMinMaxAlloc) {
    return true;
  }
  LOG_ERR("BT", "BT scan pre-flight refused: free=%u maxAlloc=%u (need %u/%u)",
          freeHeap, maxAlloc, kScanMinFreeHeap, kScanMinMaxAlloc);
  outError = "Memory low. Restart device, then scan before opening a book.";
  return false;
}
}  // namespace

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();
  
  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  lastScanTime = 0;
  pendingLearnKey = 0;
  pendingLearnIndex = 0xFF;
  learnedPrevKey = 0;
  learnedNextKey = 0;
  learnedReportIndex = 2;
  learnTestDeadlineMs = 0;
  learnTestForwardSeen = false;
  learnTestBackSeen = false;
  learnTestForwardCount = 0;
  learnTestBackCount = 0;
  debugLastKeycode = 0;
  debugEventCount = 0;
  debugLastEventMs = 0;
  debugUniqueCount = 0;
  memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
  memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
  learnStep = LearnStep::WAIT_PREV;
  
  // Get BLE manager instance
  btMgr = &BluetoothHIDManager::getInstance();
  LOG_INF("BT", "BluetoothHIDManager ready");
  
  // Restore Bluetooth persistent state on entry
  if (SETTINGS.bluetoothEnabled && !btMgr->isEnabled()) {
    LOG_INF("BT", "Restoring Bluetooth from settings (enabled)");
    // CrumBLE Phase 1 fast-open: pre-grow the reader glyph buffer before
    // NimBLE claims its ~58 KB. Without this, the first text page after
    // returning to the reader can't allocate its page buffer, glyphs
    // starve, and BLE supervision-timeouts within seconds. Cheap no-op
    // when we're not in a reader context.
    EpubReaderActivity::prewarmReaderTextBuffer(renderer);
    if (btMgr->enable()) {
      lastError = "Bluetooth restored";
    } else {
      lastError = "Failed to restore BT";
      SETTINGS.bluetoothEnabled = 0;
    }
  } else if (!SETTINGS.bluetoothEnabled && btMgr->isEnabled()) {
    LOG_INF("BT", "Disabling Bluetooth per settings (disabled)");
    btMgr->disable();
    lastError = "Bluetooth disabled per settings";
  }
  
  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  if (btMgr) {
    btMgr->setLearnInputCallback(nullptr);
    btMgr->setInputCallback(nullptr);
  }
  Activity::onExit();
}

void BluetoothSettingsActivity::loop() {
  // Use the Back *release* edge, not the press edge: every other activity in
  // the stack (EpubReaderMenu, EpubReader) also handles Back on release. If
  // this activity exits on the press edge, the same physical tap's release
  // arrives at whichever activity is now current, popping a second time —
  // sending the user from BT settings → reader menu → book in one tap and
  // sometimes triggering the reader's onExit BLE auto-disable.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (viewMode == ViewMode::DEVICE_LIST) {
      // Return to main menu
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) {
        btMgr->stopScan();
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::LEARN_KEYS) {
      if (btMgr) {
        btMgr->setLearnInputCallback(nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (learnStep != LearnStep::DONE) {
        lastError = "Learn mode canceled";
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::DEBUG_MONITOR) {
      if (btMgr) {
        btMgr->setInputCallback(nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      requestUpdate();
      return;
    } else {
      if (onComplete) onComplete();
      return;
    }
  }

  // Check if scan completed
  if (btMgr && viewMode == ViewMode::DEVICE_LIST && !btMgr->isScanning() && lastScanTime > 0) {
    if (millis() - lastScanTime > 500) { // Small delay to see final results
      lastScanTime = 0;
      requestUpdate();
    }
  }

  if (viewMode == ViewMode::MAIN_MENU) {
    handleMainMenuInput();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    handleDeviceListInput();
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    handleDebugInput();
  } else {
    handleLearnInput();
  }
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  constexpr int kMainMenuItemCount =
#ifdef ENABLE_BT_DEBUG_MONITOR
      8;
#else
      7;
#endif

  constexpr int kToggleBluetoothIndex = 0;
  constexpr int kReconnectBondedIndex = 1;
  constexpr int kDisconnectDevicesIndex = 2;
  constexpr int kScanForDevicesIndex = 3;
  constexpr int kRemoteSetupWizardIndex = 4;
#ifdef ENABLE_BT_DEBUG_MONITOR
  constexpr int kDebugMonitorIndex = 5;
  constexpr int kClearLearnedKeysIndex = 6;
  constexpr int kForgetBondedRemoteIndex = 7;
#else
  constexpr int kClearLearnedKeysIndex = 5;
  constexpr int kForgetBondedRemoteIndex = 6;
#endif

  // Match Reader Options / Controls Options convention: both side U/D and
  // front L/R drive the same Up/Down navigation. Hint labels (line ~675)
  // render this as "Up/Down" since that's the conceptual operation.
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                            mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                            mappedInput.wasPressed(MappedInputManager::Button::Right);
  if (prevPressed) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : (kMainMenuItemCount - 1);
    requestUpdate();
  } else if (nextPressed) {
    selectedIndex = (selectedIndex < (kMainMenuItemCount - 1)) ? selectedIndex + 1 : 0;
    requestUpdate();
  }
  
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!btMgr) {
      lastError = "BLE not available";
      LOG_ERR("BT", "BLE manager not available");
      requestUpdate();
      return;
    }

    if (selectedIndex == kToggleBluetoothIndex) {
      // Toggle Bluetooth
      if (btMgr->isEnabled()) {
        LOG_INF("BT", "Disabling Bluetooth...");
        if (btMgr->disable()) {
          lastError = "Bluetooth disabled";
          SETTINGS.bluetoothEnabled = 0;
          SETTINGS.saveToFile();
        } else {
          lastError = "Failed to disable";
        }
      } else {
        LOG_INF("BT", "Enabling Bluetooth...");
        // CrumBLE Phase 1 fast-open: pre-grow the reader glyph buffer
        // before NimBLE eats heap. See onEnter restore above for the
        // full reasoning.
        EpubReaderActivity::prewarmReaderTextBuffer(renderer);
        if (btMgr->enable()) {
          lastError = "Bluetooth enabled";
          SETTINGS.bluetoothEnabled = 1;
          SETTINGS.saveToFile();
        } else {
          lastError = btMgr->lastError.empty() ? "Failed to enable" : btMgr->lastError;
        }
      }
      requestUpdate();
    } else if (selectedIndex == kReconnectBondedIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else if (SETTINGS.bleBondedDeviceAddr[0] == '\0') {
        lastError = "No bonded remote saved";
      } else if (btMgr->isConnected(SETTINGS.bleBondedDeviceAddr)) {
        lastError = "Bonded remote already connected";
      } else {
        LOG_INF("BT", "Reconnecting to bonded remote %s (%s)", SETTINGS.bleBondedDeviceName,
                SETTINGS.bleBondedDeviceAddr);
        lastError = "Reconnecting...";
        requestUpdate();

        const unsigned long reconnectStart = millis();
        const bool reconnected = btMgr->connectToDevice(SETTINGS.bleBondedDeviceAddr);
        if (reconnected) {
          lastError = std::string("Reconnected to ") +
                      (SETTINGS.bleBondedDeviceName[0] ? SETTINGS.bleBondedDeviceName : "bonded remote");
        } else {
          lastError = btMgr->lastError.empty() ? "Reconnect failed" : btMgr->lastError;
        }
        // connectToDevice can block 2-3 s. If the user impatiently tapped
        // Back during the freeze, swallow that release so it doesn't pop us
        // out of the BT menu (and cascade into a reader exit + BLE disable).
        if (millis() - reconnectStart > 500) {
          mappedInput.suppressNextBackRelease();
        }
        // Auto-pop back to the reader on success when the caller opted in
        // (in-book menu uses exitOnSuccessfulConnect=true). Signals
        // autoExitParent so the EpubReaderMenu launching us also finishes
        // itself, taking the user straight back to the book.
        if (reconnected && exitOnSuccessfulConnect) {
          MenuResult result;
          result.autoExitParent = true;
          setResult(ActivityResult{result});
          finish();
          return;
        }
      }
      requestUpdate();
    } else if (selectedIndex == kDisconnectDevicesIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else {
        const auto& connectedDevices = btMgr->getConnectedDevices();
        if (connectedDevices.empty()) {
          lastError = "No devices connected";
        } else {
          std::vector<std::string> deviceAddresses = connectedDevices;
          for (const auto& addr : deviceAddresses) {
            btMgr->disconnectFromDevice(addr);
          }
          lastError = "Disconnected";
        }
      }
      requestUpdate();
    } else if (selectedIndex == kScanForDevicesIndex) {
      // Start scan and switch to device list
      if (btMgr->isEnabled()) {
        if (checkScanHeapOrError(lastError)) {
          btMgr->startScan(10000);
          lastScanTime = millis();
          viewMode = ViewMode::DEVICE_LIST;
          selectedIndex = 0;
          lastError = "";
        }
      } else {
        lastError = "Enable BT first";
      }
      requestUpdate();
    } else if (selectedIndex == kRemoteSetupWizardIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else if (btMgr->getConnectedDevices().empty()) {
        lastError = "Connect a remote first";
      } else {
        viewMode = ViewMode::LEARN_KEYS;
        learnStep = LearnStep::WAIT_PREV;
        pendingLearnKey = 0;
        pendingLearnIndex = 0xFF;
        learnedPrevKey = 0;
        learnedNextKey = 0;
        learnedReportIndex = 2;
        learnTestDeadlineMs = 0;
        learnTestForwardSeen = false;
        learnTestBackSeen = false;
        learnTestForwardCount = 0;
        learnTestBackCount = 0;
        btMgr->setLearnInputCallback([this](uint8_t keycode, uint8_t reportIndex) {
          if (viewMode == ViewMode::LEARN_KEYS && keycode > 0 && reportIndex != 0xFF) {
            pendingLearnKey = keycode;
            pendingLearnIndex = reportIndex;
          }
        });
        lastError = "Wizard: press FORWARD button";
      }
      requestUpdate();
    }
#ifdef ENABLE_BT_DEBUG_MONITOR
    else if (selectedIndex == kDebugMonitorIndex) {
      if (!btMgr->isDebugCaptureEnabled()) {
        btMgr->setDebugCaptureEnabled(true);
      }
      debugLastKeycode = 0;
      debugEventCount = 0;
      debugLastEventMs = 0;
      debugUniqueCount = 0;
      memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
      memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
      btMgr->setInputCallback([this](uint16_t keycode) {
        debugLastKeycode = keycode & 0xFF;
        debugEventCount++;
        debugLastEventMs = millis();

        const uint8_t code = static_cast<uint8_t>(keycode & 0xFF);
        bool found = false;
        for (uint8_t i = 0; i < debugUniqueCount; i++) {
          if (debugUniqueKeys[i] == code) {
            if (debugUniqueCounts[i] < 65535) {
              debugUniqueCounts[i]++;
            }
            found = true;
            break;
          }
        }

        if (!found && debugUniqueCount < kDebugUniqueKeyMax) {
          debugUniqueKeys[debugUniqueCount] = code;
          debugUniqueCounts[debugUniqueCount] = 1;
          debugUniqueCount++;
        }
      });
      viewMode = ViewMode::DEBUG_MONITOR;
      lastError = "BT debug monitor";
      requestUpdate();
    }
#endif
    else if (selectedIndex == kClearLearnedKeysIndex) {
      DeviceProfiles::clearCustomProfile();
      lastError = "Learned mapping cleared";
      requestUpdate();
    } else if (selectedIndex == kForgetBondedRemoteIndex) {
      SETTINGS.bleBondedDeviceAddr[0] = '\0';
      SETTINGS.bleBondedDeviceName[0] = '\0';
      SETTINGS.bleBondedDeviceAddrType = 0;
      SETTINGS.saveToFile();
      btMgr->setBondedDevice("", "");
      lastError = "Bonded remote cleared";
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::handleLearnInput() {
  if (pendingLearnKey != 0) {
    const uint8_t capturedKey = pendingLearnKey;
    const uint8_t capturedIndex = pendingLearnIndex;
    pendingLearnKey = 0;
    pendingLearnIndex = 0xFF;

    if (learnStep == LearnStep::WAIT_PREV) {
      learnedNextKey = capturedKey;  // Wizard step 1 = forward/next
      learnedReportIndex = (capturedIndex == 0xFF) ? 2 : capturedIndex;
      learnStep = LearnStep::WAIT_NEXT;
      char buf[96];
      snprintf(buf, sizeof(buf), "Forward=0x%02X @byte[%u], press BACK", learnedNextKey,
               static_cast<unsigned>(learnedReportIndex));
      lastError = buf;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_NEXT) {
      if (capturedKey == learnedNextKey) {
        lastError = "Back key must be different";
        requestUpdate();
        return;
      }

      learnedPrevKey = capturedKey;  // Wizard step 2 = back/prev
      learnStep = LearnStep::WAIT_TEST;
      learnTestDeadlineMs = millis() + 10000;
      learnTestForwardSeen = false;
      learnTestBackSeen = false;
      learnTestForwardCount = 0;
      learnTestBackCount = 0;
      char buf[96];
      snprintf(buf, sizeof(buf), "Test 10s: press both keys, then Confirm to save");
      lastError = buf;
      requestUpdate();
      return;
    }

    if (learnStep == LearnStep::WAIT_TEST) {
      if (capturedKey == learnedNextKey) {
        learnTestForwardSeen = true;
        if (learnTestForwardCount < 65535) {
          learnTestForwardCount++;
        }
      } else if (capturedKey == learnedPrevKey) {
        learnTestBackSeen = true;
        if (learnTestBackCount < 65535) {
          learnTestBackCount++;
        }
      }

      char buf[96];
      snprintf(buf, sizeof(buf), "Test Fwd:%s(%u) Back:%s(%u)", learnTestForwardSeen ? "OK" : "--",
               static_cast<unsigned>(learnTestForwardCount), learnTestBackSeen ? "OK" : "--",
               static_cast<unsigned>(learnTestBackCount));
      lastError = buf;
      requestUpdate();
      return;
    }
  }

  if (learnStep == LearnStep::WAIT_TEST && millis() > learnTestDeadlineMs) {
    if (btMgr) {
      btMgr->setLearnInputCallback(nullptr);
    }
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    lastError = "Wizard timed out (not saved)";
    requestUpdate();
    return;
  }

  if (learnStep == LearnStep::WAIT_TEST && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    DeviceProfiles::setCustomProfile(learnedPrevKey, learnedNextKey, learnedReportIndex);
    if (btMgr) {
      const auto& connected = btMgr->getConnectedDevices();
      for (const auto& addr : connected) {
        DeviceProfiles::setCustomProfileForDevice(addr, learnedPrevKey, learnedNextKey, learnedReportIndex);
      }
      btMgr->setLearnInputCallback(nullptr);
    }
    learnStep = LearnStep::DONE;
    char buf[96];
    snprintf(buf, sizeof(buf), "Saved! Back=0x%02X Fwd=0x%02X", learnedPrevKey, learnedNextKey);
    lastError = buf;

    // On successful wizard completion, return immediately to menu (or back to book).
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (exitOnSuccessfulConnect) {
      MenuResult result;
      result.autoExitParent = true;
      setResult(ActivityResult{result});
      finish();
      return;
    }

    requestUpdate();
    return;
  }

  if (learnStep == LearnStep::DONE && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (btMgr) {
      btMgr->setLearnInputCallback(nullptr);
    }
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    requestUpdate();
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  const auto& devices = btMgr->getDiscoveredDevices();
  const auto& connectedDevices = btMgr->getConnectedDevices();
  
  // Calculate menu items: devices + "Refresh" + "Disconnect" (if connected)
  int menuItems = devices.size() + 1; // +1 for Refresh
  if (!connectedDevices.empty()) {
    menuItems++; // +1 for Disconnect
  }
  int maxIndex = menuItems - 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    requestUpdate();
  }
  
  // Left/Right for back/refresh
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Go back to main menu
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (btMgr && btMgr->isScanning()) {
      btMgr->stopScan();
    }
    requestUpdate();
    return;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Quick rescan
    if (!checkScanHeapOrError(lastError)) {
      requestUpdate();
      return;
    }
    LOG_INF("BT", "Quick rescan...");
    lastError = "Scanning...";
    btMgr->startScan(10000);
    lastScanTime = millis();
    selectedIndex = 0;
    requestUpdate();
    return;
  }
  
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Check if "Refresh" is selected
    if (selectedIndex == static_cast<int>(devices.size())) {
      if (!checkScanHeapOrError(lastError)) {
        requestUpdate();
        return;
      }
      LOG_INF("BT", "Refreshing scan...");
      lastError = "Scanning...";
      btMgr->startScan(10000);
      lastScanTime = millis();
      selectedIndex = 0;
      requestUpdate();
      return;
    }
    
    // Check if "Disconnect" is selected
    if (!connectedDevices.empty() && selectedIndex == static_cast<int>(devices.size()) + 1) {
      LOG_INF("BT", "Disconnecting from all devices...");
      // Make a copy of addresses to avoid iterator invalidation
      std::vector<std::string> deviceAddresses = connectedDevices;
      for (const auto& addr : deviceAddresses) {
        LOG_DBG("BT", "Disconnecting from %s", addr.c_str());
        btMgr->disconnectFromDevice(addr);
      }
      lastError = "Disconnected";
      selectedIndex = 0;
      requestUpdate();
      return;
    }
    
    // Otherwise, connect to selected device
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
      const auto& device = devices[selectedIndex];
      
      LOG_INF("BT", "Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
      lastError = "Connecting...";
      requestUpdate();
      
      if (btMgr->connectToDevice(device.address)) {
        strncpy(SETTINGS.bleBondedDeviceAddr, device.address.c_str(), sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
        SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
        strncpy(SETTINGS.bleBondedDeviceName, device.name.c_str(), sizeof(SETTINGS.bleBondedDeviceName) - 1);
        SETTINGS.bleBondedDeviceName[sizeof(SETTINGS.bleBondedDeviceName) - 1] = '\0';
        SETTINGS.bleBondedDeviceAddrType = 0;
        SETTINGS.saveToFile();
        btMgr->setBondedDevice(device.address, device.name);

        lastError = "Bluetooth enabled";
        LOG_INF("BT", "Successfully connected to %s", device.name.c_str());
        if (exitOnSuccessfulConnect) {
          MenuResult result;
          result.autoExitParent = true;
          setResult(ActivityResult{result});
          finish();
          return;
        }
      } else {
        lastError = btMgr->lastError.empty() ? "Connection failed" : btMgr->lastError;
        LOG_ERR("BT", "Failed to connect: %s", lastError.c_str());
      }
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  if (viewMode == ViewMode::MAIN_MENU) {
    renderMainMenu();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    renderDeviceList();
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    renderDebugMonitor();
  } else {
    renderLearnKeys();
  }
}

void BluetoothSettingsActivity::handleDebugInput() {
  if (!btMgr) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool next = !btMgr->isDebugCaptureEnabled();
    btMgr->setDebugCaptureEnabled(next);
    lastError = next ? "BT debug capture: ON" : "BT debug capture: OFF";
    requestUpdate();
    return;
  }
}

void BluetoothSettingsActivity::renderMainMenu() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header with Bluetooth title
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH));

  // Status subheader
  std::string statusLine;
  if (btMgr) {
    if (btMgr->isEnabled()) {
      auto connDevices = btMgr->getConnectedDevices();
      if (!connDevices.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Enabled, %zu device(s) connected", connDevices.size());
        statusLine = buf;
      } else {
        statusLine = "Enabled, no devices connected";
      }
    } else {
      statusLine = "Disabled";
    }
  } else {
    statusLine = "Error initializing Bluetooth";
  }
  
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    statusLine.c_str());

  int listOffsetY = 0;
  if (btMgr && btMgr->isEnabled() && SETTINGS.bleBondedDeviceName[0] != '\0') {
    const int nameY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 4;
    std::string deviceLine = std::string("Remote: ") + SETTINGS.bleBondedDeviceName;
    deviceLine = renderer.truncatedText(UI_10_FONT_ID, deviceLine.c_str(), pageWidth - metrics.contentSidePadding * 2);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, nameY, deviceLine.c_str(), true);
    listOffsetY = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }

  // Use GUI.drawList for consistent formatting with main settings
  const char* items[] = {
    btMgr && btMgr->isEnabled() ? "Disable Bluetooth" : "Enable Bluetooth",
    "Reconnect Bonded Remote",
    "Disconnect Device(s)",
    "Scan for Devices",
    "Remote Setup Wizard",
#ifdef ENABLE_BT_DEBUG_MONITOR
    btMgr && btMgr->isDebugCaptureEnabled() ? "Disable BT Debug Capture" : "Enable BT Debug Capture",
#endif
    "Clear Learned Keys",
    "Forget Bonded Remote"
  };

  std::vector<std::string> itemLabels;
  for (int i = 0; i < static_cast<int>(sizeof(items) / sizeof(items[0])); i++) {
    itemLabels.push_back(items[i]);
  }

  GUI.drawList(
      renderer,
      Rect{0,
           metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing + listOffsetY,
           pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2 + listOffsetY)},
      static_cast<int>(itemLabels.size()), selectedIndex,
      [&itemLabels](int index) { return itemLabels[index]; }, nullptr, nullptr,
      [this](int i) {
        if (i == 0) {
          return std::string(btMgr && btMgr->isEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        if (i == 1 && SETTINGS.bleBondedDeviceName[0] != '\0') {
          return renderer.truncatedText(UI_10_FONT_ID, SETTINGS.bleBondedDeviceName,
                                        renderer.getScreenWidth() - UITheme::getInstance().getMetrics().contentSidePadding * 4);
        }
        return std::string("");
      },
      true);

  if (!lastError.empty()) {
    std::string statusText = renderer.truncatedText(UI_10_FONT_ID, lastError.c_str(),
                                                    pageWidth - metrics.contentSidePadding * 2);
    const int statusY = pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, statusY, statusText.c_str(), true);
  }

  // Button hints — front L/R are aliased to Up/Down navigation just like
  // Reader Options / Controls, so label them as Up/Down to match.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDeviceList() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (!btMgr) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Bluetooth error");
    return;
  }

  const auto& devices = btMgr->getDiscoveredDevices();
  const auto& connectedDevices = btMgr->getConnectedDevices();

  // Header with device count
  char countStr[32];
  snprintf(countStr, sizeof(countStr), btMgr->isScanning() ? tr(STR_SCANNING) : "Found %zu", devices.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, 
                 tr(STR_BLUETOOTH), countStr);

  // Subheader with scan status
  std::string subheaderText;
  if (btMgr->isScanning()) {
    subheaderText = "Searching for devices...";
  } else {
    if (devices.empty()) {
      subheaderText = "No devices found";
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d device(s) available", (int)devices.size());
      subheaderText = buf;
    }
  }
  
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    subheaderText.c_str());

  // Build device list labels. `GUI.drawList()` already paginates based on
  // `selectedIndex`, so keep the full device list here and let the user scroll
  // through every discovered device instead of truncating after the first page.
  std::vector<std::string> deviceLabels;
  std::vector<std::string> deviceValues;
  char buf[128];

  if (!devices.empty()) {
    for (const auto& device : devices) {
      const bool connected = btMgr->isConnected(device.address.c_str());

      // Device name with indicators
      const char* connSymbol = connected ? "[*] " : "";
      const char* hidSymbol = device.isHID ? "[HID] " : "";
      snprintf(buf, sizeof(buf), "%s%s%s", connSymbol, hidSymbol, device.name.c_str());
      deviceLabels.push_back(buf);

      // RSSI/signal strength
      const std::string signalBars = getSignalStrengthIndicator(device.rssi);
      snprintf(buf, sizeof(buf), "%s (%d dBm)", signalBars.c_str(), device.rssi);
      deviceValues.push_back(buf);
    }
  }

  // Add action buttons after the full device list.
  deviceLabels.push_back("< Rescan >");
  deviceValues.push_back("");
  
  if (!connectedDevices.empty()) {
    deviceLabels.push_back("< Disconnect All >");
    deviceValues.push_back("");
  }
  
  // Render the list using GUI.drawList for consistency
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      deviceLabels.size(), selectedIndex,
      [&deviceLabels](int index) { return deviceLabels[index]; }, nullptr, nullptr,
      [&deviceValues](int i) { return i < (int)deviceValues.size() ? deviceValues[i] : std::string(""); },
      true);

  // Help text
  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                   "Up/Down: Scroll | Right: Rescan");

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), tr(STR_DIR_LEFT), tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string BluetoothSettingsActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // BLE RSSI tends to be lower than WiFi at similar distance.
  // Use BLE-friendly thresholds so nearby remotes are not shown as always weak.
  if (rssi >= -60) {
    return "||||";  // Excellent
  }
  if (rssi >= -70) {
    return " |||";  // Good
  }
  if (rssi >= -80) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void BluetoothSettingsActivity::renderLearnKeys() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Remote Setup Wizard");

  const char* stepText = "Press FORWARD button";
  if (learnStep == LearnStep::WAIT_NEXT) {
    stepText = "Press BACK button";
  } else if (learnStep == LearnStep::WAIT_TEST) {
    stepText = "Test both buttons (10s)";
  } else if (learnStep == LearnStep::DONE) {
    stepText = "Learning complete";
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    stepText);

  char line1[64];
  char line2[64];
  snprintf(line1, sizeof(line1), "Forward key: %s", learnedNextKey ? "captured" : "waiting");
  snprintf(line2, sizeof(line2), "Back key: %s", learnedPrevKey ? "captured" : "waiting");

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 32,
                            line1);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 56,
                            line2);

  if (learnedNextKey || learnedPrevKey) {
    char line3[48];
    char line4[64];
    if (learnStep == LearnStep::WAIT_TEST) {
      unsigned int remaining = (learnTestDeadlineMs > millis()) ? (learnTestDeadlineMs - millis()) / 1000 : 0;
      snprintf(line3, sizeof(line3), "Time left: %us", remaining);
      snprintf(line4, sizeof(line4), "Fwd:%u Back:%u", static_cast<unsigned>(learnTestForwardCount),
               static_cast<unsigned>(learnTestBackCount));
      renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 100,
                                line4);
    } else {
      snprintf(line3, sizeof(line3), "Report byte: [%u]", static_cast<unsigned>(learnedReportIndex));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 80,
                              line3);
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                        (learnStep == LearnStep::DONE || learnStep == LearnStep::WAIT_TEST)
                          ? tr(STR_SELECT)
                          : "",
                                            "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDebugMonitor() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Bluetooth Debug");

  std::string sub = btMgr && btMgr->isDebugCaptureEnabled() ? "Capture ON" : "Capture OFF";
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    sub.c_str());

  char line1[64];
  char line2[64];
  char line3[64];
  char line4[64];

  unsigned int connectedCount = btMgr ? static_cast<unsigned int>(btMgr->getConnectedDevices().size()) : 0;
  snprintf(line1, sizeof(line1), "Connected: %u", connectedCount);
  snprintf(line2, sizeof(line2), "Key events: %u", static_cast<unsigned>(debugEventCount));
  snprintf(line3, sizeof(line3), "Unique keys: %u", static_cast<unsigned>(debugUniqueCount));
  snprintf(line4, sizeof(line4), "Last key: 0x%02X", static_cast<unsigned>(debugLastKeycode & 0xFF));

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 24,
                            line1);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 48,
                            line2);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 72,
                            line3);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 96,
                            line4);

  if (debugLastEventMs > 0) {
    char eventAgeLine[64];
    snprintf(eventAgeLine, sizeof(eventAgeLine), "Last event: %lus ago", (millis() - debugLastEventMs) / 1000);
    renderer.drawCenteredText(UI_10_FONT_ID,
                              metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 114,
                              eventAgeLine);
  }

  const int uniqueStartY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 132;
  if (debugUniqueCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY, "No key presses captured yet");
  } else {
    uint8_t sortedIndices[kDebugUniqueKeyMax] = {0};
    for (uint8_t i = 0; i < debugUniqueCount; i++) {
      sortedIndices[i] = i;
    }

    for (uint8_t i = 0; i + 1 < debugUniqueCount; i++) {
      uint8_t best = i;
      for (uint8_t j = i + 1; j < debugUniqueCount; j++) {
        const uint16_t bestCount = debugUniqueCounts[sortedIndices[best]];
        const uint16_t candidateCount = debugUniqueCounts[sortedIndices[j]];
        if (candidateCount > bestCount) {
          best = j;
        }
      }
      if (best != i) {
        const uint8_t tmp = sortedIndices[i];
        sortedIndices[i] = sortedIndices[best];
        sortedIndices[best] = tmp;
      }
    }

    const uint8_t renderCount = (debugUniqueCount < 4) ? debugUniqueCount : 4;
    for (uint8_t i = 0; i < renderCount; i++) {
      const uint8_t idx = sortedIndices[i];
      char keyLine[64];
      snprintf(keyLine, sizeof(keyLine), "Key 0x%02X  x%u", static_cast<unsigned>(debugUniqueKeys[idx]),
               static_cast<unsigned>(debugUniqueCounts[idx]));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(i) * 16, keyLine);
    }

    if (debugUniqueCount > renderCount) {
      char moreLine[48];
      snprintf(moreLine, sizeof(moreLine), "+%u more keys", static_cast<unsigned>(debugUniqueCount - renderCount));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(renderCount) * 16, moreLine);
    }
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}