#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <cstdio>
#include <cstring>

#include <HalGPIO.h>  // BTN_* constants for the button-map UI

#include <algorithm>

#include "../reader/EpubReaderActivity.h"  // prewarmReaderTextBuffer
#include "CrossPointSettings.h"
#include "DeviceProfiles.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// CrumBLE 4.5.5: rich button-map function table. Six map to CrumBLE's
// virtual buttons (HalGPIO::BTN_*, there's no separate PageForward/PageBack
// on this hardware -- Up/Down doubles as page navigation in the reader).
// The 7th is a special "action" sentinel (0xFE, out of HalGPIO::BTN_*
// range 0-6) that the button injector lambda in main.cpp routes to the
// same FORCE_REFRESH path the Power button's short-press uses -- gives the
// user a way to map a remote button to a screen-ghost cleanup refresh.
struct MapFn {
  uint8_t button;
  const char* label;
};
constexpr uint8_t kBtnActionRefreshScreen = 0xFE;
constexpr MapFn kMapFns[] = {
    {HalGPIO::BTN_DOWN,        "Page Forward / Down"},
    {HalGPIO::BTN_UP,          "Page Back / Up"},
    {HalGPIO::BTN_CONFIRM,     "Confirm"},
    {HalGPIO::BTN_BACK,        "Back"},
    {HalGPIO::BTN_RIGHT,       "Right"},
    {HalGPIO::BTN_LEFT,        "Left"},
    {kBtnActionRefreshScreen,  "Refresh Screen"},
};
constexpr uint8_t kMapFnCount = sizeof(kMapFns) / sizeof(kMapFns[0]);
}  // namespace

// CrumBLE 4.5.5: rewrite ported from upstream crosspoint-reader feat-bluetooth
// (Free-Ink/freeink-sdk) BluetoothSettingsActivity. See the .h for view/action
// model. CrumBLE-specific additions kept: checkScanHeapOrBanner pre-flight,
// learn-keys wizard sub-view, optional debug monitor sub-view, ctor params
// (onComplete, fromReader) that all call sites depend on.

namespace {
constexpr unsigned long kBannerMs = 2000;
constexpr unsigned long kForgetHoldMs = 1200;  // hold Confirm this long in Paired view to forget
constexpr uint32_t kScanMs = 10000;
constexpr unsigned long kScanAnimIntervalMs = 700;  // e-ink-safe repaint rate
}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  view = View::Menu;
  menuIndex = 0;
  scanIndex = 0;
  pairedIndex = 0;
  banner.clear();
  bannerUntil = 0;
  awaitingConnect = false;
  pairedActionTaken = false;
  lastScanAnimMs = 0;

  // Reset button-map + debug-monitor state so a previous entry's leftovers
  // don't leak across re-entries.
  mapStep = MapStep::WaitForKey;
  pendingKeyKind = 0xFF;
  pendingKeyValue = 0;
  functionIndex = 0;
  debugLastKeycode = 0;
  debugEventCount = 0;
  debugLastEventMs = 0;
  debugUniqueCount = 0;
  memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
  memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));

  btMgr = &BluetoothHIDManager::getInstance();

  // v18.9.9.370: BT is now on-demand. Anyone entering this activity is either
  // scanning + pairing, checking status, or debugging -- every use-case wants
  // BT on. Unconditional auto-enable removes the "toggle BT then scan" two-
  // step and matches the leave-menu-turns-BT-off symmetry (disableOnExit).
  // Persistent SETTINGS.bluetoothEnabled becomes an "auto-connect on reader
  // open" intent flag, not a "BT stays on 24/7" flag.
  const bool autoEnableForBtRestart = g_postBtSilentReboot && !btMgr->isEnabled();
  if (!btMgr->isEnabled()) {
    // CrumBLE Phase 1 fast-open: pre-grow the reader glyph buffer before
    // NimBLE claims its ~58 KB so the first text page after returning to
    // the reader can allocate its page buffer.
    //
    // CrumBLE 4.5.5: only do the prewarm when this activity was opened from
    // the in-book quick-connect path. From Settings / silent-restart-recover,
    // the reader is not in the activity stack -- prewarming here just eats
    // ~50 KB of contiguous heap right before NimBLE wants 50 KB of its own,
    // tripping the new maxAlloc floor in enable(). Field log: user toggled
    // BT -> first enable failed the floor -> silentRestartToBluetoothSettings
    // -> post-restart onEnter prewarmed -> enable failed again under the
    // same floor -> BT stayed off after the "screen flash", forcing a
    // second manual toggle. The reader's own onEnter will re-prewarm next
    // time we open a book.
    // 4.7.2: !g_postBtSilentReboot keeps the 4.5.5 fix intact now that the
    // post-restart recover path also carries fromReader=true -- prewarming
    // right before enable() on a recover boot tripped the maxAlloc floor.
    if (fromReader && !g_postBtSilentReboot) {
      EpubReaderActivity::prewarmReaderTextBuffer(renderer);
    }
    if (btMgr->enable()) {
      setBanner("Bluetooth restored");
      // 4.5.5: catch up persistence. The pre-silent-restart saveToFile may
      // have deferred (low-heap), so disk still reads bluetoothEnabled = 0.
      // Now that post-restart heap is healthy, persist the user's intent
      // so a power-cycle (or a normal exit) brings BT back next boot.
      if (autoEnableForBtRestart && !SETTINGS.bluetoothEnabled) {
        SETTINGS.bluetoothEnabled = 1;
        SETTINGS.saveToFile();
      }
    } else {
      // Don't reset SETTINGS.bluetoothEnabled here -- the user explicitly
      // asked for BT on, and our auto-restore is best-effort. Leaving the
      // intent persisted lets the next entry retry under a fresh heap.
      setBanner("Failed to restore BT");
    }
  }
  // v18.9.9.370: dropped the "BT-on-but-SETTINGS-off resync" branch. Under
  // the on-demand model this activity's onEnter is the ONLY thing that
  // enables BT (setting-toggle keeps the persistent intent alive but doesn't
  // control real BT state anymore), so entering with BT-on-and-SETTINGS-off
  // isn't reachable in normal flow.

  rebuildMenuRows();

  // v18.9: if the user tapped "Scan & Pair" and the scan pre-flight
  // silent-restarted us here, jump straight into scan view. Otherwise the
  // NO_REFRESH restart looks like a "quick refresh" and drops focus on
  // menu row 0 -- indistinguishable from "the tap did nothing." One-shot.
  if (g_postBtSilentRebootScanIntent) {
    g_postBtSilentRebootScanIntent = false;
    if (btMgr && btMgr->isEnabled()) {
      startScanView();
      return;
    }
  }

  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  if (btMgr) {
    btMgr->setLearnInputCallback(nullptr);
    btMgr->setInputCallback(nullptr);
    // CrumBLE 4.5.5: gated by the ctor's disableOnExit flag. Main Settings
    // entry still passes true so Home/Bookshelf gets a clean heap (NimBLE
    // holds ~60 KB; Home OOM'd under that load historically). Reader
    // entry points (reader menu, drawer's BT-flow bounce) pass false --
    // user is going back to a book that wants the link alive, and
    // disabling here meant a "BT drops every time I peek at its settings"
    // regression vs CrumBLE pre-4.5.5.
    if (disableOnExit && btMgr->isEnabled()) {
      btMgr->disable();
      // v18.9: NimBLE disable frees ~60 KB but through many small free()s,
      // leaving heap deeply fragmented. Home's cover-shelf load right after
      // has been observed to OOM/crash on Back-to-Home. Silent-restart on
      // a clean heap instead of unwinding into a starving Home.
      // v18.9.5: target Settings instead of Home so Back-from-BT-menu goes
      // one level up in the menu tree (matches every other Back on the
      // device) rather than jumping the user all the way out.
      Activity::onExit();
      silentRestartToSettings();
      return;  // never returns
    }
  }
  Activity::onExit();
}

// ============================================================================
// Menu (dynamic rows)
// ============================================================================

void BluetoothSettingsActivity::rebuildMenuRows() {
  menuRows.clear();
  menuRows.reserve(6);
  // BT on/off is always the first row.
  menuRows.push_back({Action::ToggleBt, "Bluetooth"});
  if (btMgr && btMgr->isEnabled()) {
    menuRows.push_back({Action::Scan, "Scan & Pair"});
    if (!btMgr->getConnectedDevices().empty()) {
      menuRows.push_back({Action::Disconnect, "Disconnect"});
    }
    if (hasBondedDevice()) {
      menuRows.push_back({Action::PairedDevices, "Paired Device"});
    }
    menuRows.push_back({Action::MapButtons, "Map Buttons"});
#ifdef ENABLE_BT_DEBUG_MONITOR
    menuRows.push_back({Action::DebugMonitor,
                        btMgr->isDebugCaptureEnabled() ? "Debug Monitor (ON)" : "Debug Monitor"});
#endif
  }
  if (menuIndex >= static_cast<int>(menuRows.size())) menuIndex = 0;
}

void BluetoothSettingsActivity::handleMenuConfirm() {
  if (!btMgr || menuRows.empty()) return;
  const Action action = menuRows[menuIndex].action;
  switch (action) {
    case Action::ToggleBt: {
      if (btMgr->isEnabled()) {
        if (btMgr->disable()) {
          SETTINGS.bluetoothEnabled = 0;
          SETTINGS.saveToFile();
          setBanner("Bluetooth disabled");
        } else {
          setBanner("Failed to disable");
        }
      } else {
        EpubReaderActivity::prewarmReaderTextBuffer(renderer);
        if (btMgr->enable()) {
          SETTINGS.bluetoothEnabled = 1;
          SETTINGS.saveToFile();
          setBanner("Bluetooth enabled");
        } else {
          // CrumBLE 4.5.3 ported through 4.5.5: enable() refuses below the
          // free/maxAlloc floors that NimBLE's controller_init demands. A
          // fresh reboot lands us with ~150 KB free / ~94 KB maxAlloc and
          // the next click works. Silent-restart back to BT settings to
          // recover instead of just showing "Failed". g_postBtSilentReboot
          // guards against an infinite loop if a fresh boot can't clear
          // the floor either (vanishingly unlikely).
          if (!g_postBtSilentReboot) {
            LOG_INF("BT", "Enable failed (free=%u maxAlloc=%u) -- silent-restart to recover",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            // Persist intent so the post-boot auto-restore brings BT up.
            SETTINGS.bluetoothEnabled = 1;
            SETTINGS.saveToFile();
            // 4.7.2: always restart back INTO BT settings. The old
            // from-reader branch restarted into the READER, which
            // stranded users mid-setup with BT on but nothing paired;
            // fromReader now only routes where Back lands (book vs
            // Settings) via returnToReaderAfterBtMagic.
            silentRestartToBluetoothSettings(fromReader);
            // never returns
          }
          setBanner(btMgr->lastError.empty() ? "Failed to enable" : btMgr->lastError.c_str(), 4000);
        }
      }
      rebuildMenuRows();
      requestUpdate();
      break;
    }
    case Action::Scan:
      startScanView();
      break;
    case Action::Disconnect: {
      const auto connected = btMgr->getConnectedDevices();
      for (const auto& addr : connected) {
        btMgr->disconnectFromDevice(addr);
      }
      setBanner("Disconnected");
      rebuildMenuRows();
      requestUpdate();
      break;
    }
    case Action::PairedDevices:
      view = View::Paired;
      pairedIndex = 0;
      pairedActionTaken = false;
      requestUpdate();
      break;
    case Action::MapButtons:
      // Mapping needs a connected remote so the device can hear its HID keys.
      if (btMgr->getConnectedDevices().empty()) {
        setBanner("Connect a remote first");
        requestUpdate();
        return;
      }
      view = View::ButtonMap;
      mapStep = MapStep::WaitForKey;
      pendingKeyKind = 0xFF;
      pendingKeyValue = 0;
      functionIndex = 0;
      btMgr->setLearnInputCallback([this](uint8_t keycode, uint8_t /*reportIndex*/) {
        if (view == View::ButtonMap && mapStep == MapStep::WaitForKey && keycode != 0 && keycode != 0xFF) {
          pendingKeyKind = 1;  // HID usage code
          pendingKeyValue = keycode;
        }
      });
      setBanner("Press a remote button to map");
      requestUpdate();
      break;
#ifdef ENABLE_BT_DEBUG_MONITOR
    case Action::DebugMonitor:
      if (!btMgr->isDebugCaptureEnabled()) btMgr->setDebugCaptureEnabled(true);
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
            if (debugUniqueCounts[i] < 65535) debugUniqueCounts[i]++;
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
      view = View::DebugMonitor;
      requestUpdate();
      break;
#endif
  }
}

void BluetoothSettingsActivity::startScanView() {
  if (!checkScanHeapOrBanner()) {
    requestUpdate();
    return;
  }
  view = View::Scan;
  scanIndex = 0;
  awaitingConnect = false;
  btMgr->startScan(kScanMs);
  setBanner(tr(STR_SCANNING));
  requestUpdate();
}

bool BluetoothSettingsActivity::checkScanHeapOrBanner() {
  // CrumBLE 4.4: NimBLE running + a book in the background can leave free
  // heap < 7 KB / MaxAlloc < 5 KB -- not enough for the scan's onResult
  // result list + post-scan picker strings. Silent-restart back to this
  // activity when below floor; g_postBtSilentReboot guards against looping.
  constexpr uint32_t kScanMinFreeHeap = 14u * 1024u;
  constexpr uint32_t kScanMinMaxAlloc = 8u * 1024u;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap >= kScanMinFreeHeap && maxAlloc >= kScanMinMaxAlloc) return true;
  if (!g_postBtSilentReboot) {
    LOG_INF("BT", "BT scan pre-flight low (free=%u maxAlloc=%u) -- silent-restart to recover heap",
            freeHeap, maxAlloc);
    // 4.7.2: both origins restart into BT settings WITH scan intent so
    // the user lands straight back in the scan view instead of being
    // bounced into the book; fromReader keeps Back returning to the
    // book rather than Home.
    silentRestartToBluetoothSettingsWithScanIntent(fromReader);
    // never returns
  }
  LOG_ERR("BT", "BT scan pre-flight refused even after silent-restart: free=%u maxAlloc=%u (need %u/%u)",
          freeHeap, maxAlloc, kScanMinFreeHeap, kScanMinMaxAlloc);
  setBanner("Memory low. Power-cycle the device.");
  return false;
}

void BluetoothSettingsActivity::setBanner(const char* text, unsigned long durationMs) {
  banner = text ? text : "";
  bannerUntil = millis() + (durationMs > 0 ? durationMs : kBannerMs);
}

// ============================================================================
// loop (input + async polling)
// ============================================================================

void BluetoothSettingsActivity::loop() {
  // Clear an expired banner so the hint line returns to its usual content.
  if (bannerUntil > 0 && millis() > bannerUntil) {
    banner.clear();
    bannerUntil = 0;
    requestUpdate();
  }

  // ButtonMap / debug sub-views own their own input + render flow.
  if (view == View::ButtonMap) {
    // ---- WaitForKey: a remote key arrives, advance to SelectFunction ----
    if (mapStep == MapStep::WaitForKey && pendingKeyKind != 0xFF) {
      mapStep = MapStep::SelectFunction;
      functionIndex = 0;
      requestUpdate();
    }

    // Back leaves the sub-view at any step.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (btMgr) btMgr->setLearnInputCallback(nullptr);
      view = View::Menu;
      rebuildMenuRows();
      requestUpdate();
      return;
    }

    // ---- SelectFunction: scroll through kMapFns, Confirm assigns -------
    if (mapStep == MapStep::SelectFunction) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
          mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        functionIndex = ButtonNavigator::nextIndex(functionIndex, kMapFnCount);
        requestUpdate();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                 mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        functionIndex = ButtonNavigator::previousIndex(functionIndex, kMapFnCount);
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (assignCapturedKey(kMapFns[functionIndex].button)) {
          // v18.4: distinguish "saved to disk" from "saved to RAM only,
          // heap too low to persist." Before, a deferred save was silent
          // -- user thought mapping was persisted but on next reboot it
          // was gone. Now we tell them explicitly to disconnect BT (frees
          // ~50KB) or reboot so the deferred save can catch up.
          if (SETTINGS.hasDeferredSave()) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "Mapped %s (heap low, disk write deferred - disconnect BT or reboot to persist)",
                     kMapFns[functionIndex].label);
            setBanner(buf, 5000);
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Mapped %s", kMapFns[functionIndex].label);
            setBanner(buf, 2500);
          }
        } else {
          setBanner("Map full (12 slots)", 3000);
        }
        // Loop back to capture the next key so the user can map multiple buttons
        // in one session.
        mapStep = MapStep::WaitForKey;
        pendingKeyKind = 0xFF;
        pendingKeyValue = 0;
        requestUpdate();
      }
    }
    return;
  }
#ifdef ENABLE_BT_DEBUG_MONITOR
  if (view == View::DebugMonitor) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && btMgr) {
      const bool next = !btMgr->isDebugCaptureEnabled();
      btMgr->setDebugCaptureEnabled(next);
      setBanner(next ? "Debug capture: ON" : "Debug capture: OFF");
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (btMgr) btMgr->setInputCallback(nullptr);
      view = View::Menu;
      rebuildMenuRows();
      requestUpdate();
      return;
    }
    return;
  }
#endif

  // ---- Watch for async connect result --------------------------------
  if (awaitingConnect && btMgr) {
    // CrumBLE single-bonded model: success = at least one connected device.
    if (!btMgr->getConnectedDevices().empty()) {
      awaitingConnect = false;
      // 4.5.5: persist the bond. Without this, SETTINGS.bleBondedDeviceAddr
      // stays empty after a fresh pair and the in-book drawer's BT quick-
      // connect falls into the "no bonded -> launch pairing UI" branch
      // instead of "have bonded -> reconnect directly", so the user has to
      // re-pair on every launch. The Paired view path doesn't reach this
      // code (its connect target was already SETTINGS.bleBondedDeviceAddr).
      if (!pendingConnectAddress.empty()) {
        strncpy(SETTINGS.bleBondedDeviceAddr, pendingConnectAddress.c_str(),
                sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
        SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
        strncpy(SETTINGS.bleBondedDeviceName, pendingConnectName.c_str(),
                sizeof(SETTINGS.bleBondedDeviceName) - 1);
        SETTINGS.bleBondedDeviceName[sizeof(SETTINGS.bleBondedDeviceName) - 1] = '\0';
        SETTINGS.bleBondedDeviceAddrType = 0;
        SETTINGS.saveToFile();
        btMgr->setBondedDevice(pendingConnectAddress, pendingConnectName);
      }
      char buf[64];
      const std::string& name = pendingConnectName.empty() ? connectedDeviceName() : pendingConnectName;
      snprintf(buf, sizeof(buf), "Connected: %s (saved)", name.empty() ? "device" : name.c_str());
      setBanner(buf);
      pendingConnectAddress.clear();
      pendingConnectName.clear();
      // 4.7.2: no auto-exit on connect. The user stays in the BT menu for
      // scan / button-mapping follow-ups and leaves with an explicit Back.
      view = View::Menu;
      rebuildMenuRows();
      requestUpdate();
    } else if (awaitingConnectStartedAt > 0 && (millis() - awaitingConnectStartedAt) > 15000) {
      // 4.5.5: connect timeout. No takeConnectFailure equivalent in our
      // manager -- without this, awaitingConnect would stick if NimBLE
      // failed silently, blocking subsequent button presses on the menu.
      awaitingConnect = false;
      pendingConnectAddress.clear();
      pendingConnectName.clear();
      setBanner("Connect timed out", 3000);
      requestUpdate();
    }
  }

  // Repaint scan view as devices stream in (or the spinner-equivalent ticks).
  if (view == View::Scan && btMgr && btMgr->isScanning()) {
    if (millis() - lastScanAnimMs > kScanAnimIntervalMs) {
      lastScanAnimMs = millis();
      requestUpdate();
    }
  }

  // ---- Back returns from a sub-view, or finishes from Menu -----------
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::Menu) {
      if (onComplete) onComplete();
      return;
    }
    if (view == View::Scan && btMgr && btMgr->isScanning()) {
      btMgr->stopScan();
    }
    view = View::Menu;
    rebuildMenuRows();
    requestUpdate();
    return;
  }

  // ---- List navigation: Up/Down (and Left/Right alias) ---------------
  const int count = view == View::Menu                  ? static_cast<int>(menuRows.size())
                    : view == View::Scan
                        ? static_cast<int>(btMgr ? btMgr->getDiscoveredDevices().size() : 0)
                        : (hasBondedDevice() ? 1 : 0);  // Paired -> single-bonded model
  int* idx = view == View::Menu ? &menuIndex : view == View::Scan ? &scanIndex : &pairedIndex;
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  if (count > 0 && nextPressed) {
    *idx = ButtonNavigator::nextIndex(*idx, count);
    requestUpdate();
  } else if (count > 0 && prevPressed) {
    *idx = ButtonNavigator::previousIndex(*idx, count);
    requestUpdate();
  }

  // ---- Paired view: tap to connect, HOLD to forget -------------------
  if (view == View::Paired) {
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      if (!pairedActionTaken && mappedInput.getHeldTime() >= kForgetHoldMs && hasBondedDevice()) {
        forgetBondedDevice();
        setBanner(tr(STR_FORGET_BUTTON));
        pairedActionTaken = true;
        view = View::Menu;
        rebuildMenuRows();
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!pairedActionTaken && !awaitingConnect && hasBondedDevice()) {
        awaitingConnect = true;
        setBanner(tr(STR_CONNECTING));
        const std::string addr = SETTINGS.bleBondedDeviceAddr;
        btMgr->connectToDevice(addr);
        requestUpdate();
      }
      pairedActionTaken = false;
    }
    return;
  }

  // ---- Menu / Scan: Confirm ------------------------------------------
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (view == View::Menu) {
      handleMenuConfirm();
    } else if (view == View::Scan && btMgr) {
      if (!awaitingConnect) {
        if (btMgr->isScanning()) btMgr->stopScan();
        const auto& devices = btMgr->getDiscoveredDevices();
        if (scanIndex >= 0 && scanIndex < static_cast<int>(devices.size())) {
          const auto& d = devices[scanIndex];
          awaitingConnect = true;
          pendingConnectAddress = d.address;
          pendingConnectName = d.name;
          awaitingConnectStartedAt = millis();
          setBanner(tr(STR_CONNECTING));
          btMgr->connectToDevice(d.address);
          requestUpdate();
        }
      }
    }
    return;
  }
}

// ============================================================================
// Helpers (single-bonded model)
// ============================================================================

std::string BluetoothSettingsActivity::connectedDeviceName() const {
  if (!btMgr) return {};
  // BluetoothHIDManager::getConnectedDevices() returns just addresses; reach
  // into the device list to find the name. Single-connected model in
  // practice (CrumBLE only ever pairs one HID).
  const auto addrs = btMgr->getConnectedDevices();
  if (addrs.empty()) return {};
  // The manager stores names alongside addresses internally; SETTINGS holds
  // the last bonded name as a friendlier fallback.
  if (SETTINGS.bleBondedDeviceName[0] != '\0') return SETTINGS.bleBondedDeviceName;
  return addrs.front();
}

bool BluetoothSettingsActivity::hasBondedDevice() const {
  return SETTINGS.bleBondedDeviceAddr[0] != '\0';
}

void BluetoothSettingsActivity::forgetBondedDevice() {
  if (btMgr) {
    const std::string addr = SETTINGS.bleBondedDeviceAddr;
    if (!addr.empty() && btMgr->isConnected(addr.c_str())) {
      btMgr->disconnectFromDevice(addr);
    }
  }
  SETTINGS.bleBondedDeviceAddr[0] = '\0';
  SETTINGS.bleBondedDeviceName[0] = '\0';
  SETTINGS.saveToFile();
}

// ============================================================================
// render dispatch
// ============================================================================

void BluetoothSettingsActivity::render(RenderLock&&) {
  if (view == View::ButtonMap) {
    renderButtonMap();
    return;
  }
#ifdef ENABLE_BT_DEBUG_MONITOR
  if (view == View::DebugMonitor) {
    renderDebugMonitor();
    return;
  }
#endif

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH));

  // Sub-header: live status. Mirrors upstream: connected name OR "Not
  // connected" (CrumBLE-extended: "Disabled" when BT is off).
  std::string status;
  if (btMgr && btMgr->isEnabled()) {
    const std::string name = connectedDeviceName();
    status = name.empty() ? "Not connected" : name;
  } else {
    status = "Disabled";
  }
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    status.c_str());

  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect listRect{0, topOffset, pageWidth, contentHeight};

  if (view == View::Menu) {
    renderMenu();
  } else if (view == View::Scan) {
    renderScan();
  } else {
    renderPaired();
  }

  // Transient banner above the button hints.
  if (!banner.empty()) {
    GUI.drawHelpText(renderer,
                     Rect{0, pageHeight - metrics.buttonHintsHeight - 22, pageWidth, 20},
                     banner.c_str());
  } else if (view == View::Paired && hasBondedDevice()) {
    // Surface the hold-to-forget hint when there's nothing else to say.
    GUI.drawHelpText(renderer,
                     Rect{0, pageHeight - metrics.buttonHintsHeight - 22, pageWidth, 20},
                     "Hold Confirm to forget");
  }

  const char* confirmHint = view == View::Menu ? tr(STR_SELECT) : tr(STR_CONNECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmHint, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  // Avoid unused-variable warnings when the menu/scan/paired branch above
  // doesn't reach for listRect itself (the sub-renderers reach for it via
  // their own metrics calls, which is fine -- this just keeps the symbol
  // live in case a future renderer wants it).
  (void)listRect;
}

void BluetoothSettingsActivity::renderMenu() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect listRect{0, topOffset, pageWidth, contentHeight};
  GUI.drawList(
      renderer, listRect, static_cast<int>(menuRows.size()), menuIndex,
      [this](int i) { return std::string(menuRows[i].label); },
      nullptr, nullptr,
      [this](int i) -> std::string {
        // Show On/Off chip next to the BT toggle; bonded-name chip next to
        // PairedDevices. Other rows have no value.
        const Action a = menuRows[i].action;
        if (a == Action::ToggleBt) {
          return std::string(btMgr && btMgr->isEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        if (a == Action::PairedDevices && hasBondedDevice()) {
          return std::string(SETTINGS.bleBondedDeviceName);
        }
        return std::string("");
      },
      true);
}

void BluetoothSettingsActivity::renderScan() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const auto& devices = btMgr ? btMgr->getDiscoveredDevices() : std::vector<BluetoothDevice>{};
  if (devices.empty()) {
    GUI.drawHelpText(renderer, Rect{0, topOffset, pageWidth, 24},
                     btMgr && btMgr->isScanning() ? tr(STR_SCANNING) : "No HID devices found");
    return;
  }
  GUI.drawList(
      renderer, Rect{0, topOffset, pageWidth, contentHeight},
      static_cast<int>(devices.size()), scanIndex,
      [&devices](int i) {
        // Devices are HID-only post 4.5.5 filter. Show name first, fall back
        // to address; RSSI as a value chip.
        return devices[static_cast<size_t>(i)].name;
      },
      nullptr, nullptr,
      [&devices](int i) -> std::string {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", devices[static_cast<size_t>(i)].rssi);
        return std::string(buf);
      },
      false);
}

void BluetoothSettingsActivity::renderPaired() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (!hasBondedDevice()) {
    GUI.drawHelpText(renderer, Rect{0, topOffset, pageWidth, 24}, "No paired device");
    return;
  }
  // Single-bonded model: one row.
  GUI.drawList(
      renderer, Rect{0, topOffset, pageWidth, contentHeight}, 1, pairedIndex,
      [](int) { return std::string(SETTINGS.bleBondedDeviceName); },
      nullptr, nullptr, nullptr, false);
}

// ============================================================================
// Sub-view renderers (CrumBLE-only -- preserved verbatim from prior version)
// ============================================================================

void BluetoothSettingsActivity::describeMapKey(uint8_t kind, uint8_t value, char* out, size_t outLen) const {
  if (!out || outLen == 0) return;
  // CrumBLE captures HID usage codes today; the kind == 0 (special key) branch
  // is here so a future SpecialKey decoder can plug in without churning the UI.
  if (kind == 1) {
    snprintf(out, outLen, "Key 0x%02X", static_cast<unsigned>(value));
  } else if (kind == 0) {
    snprintf(out, outLen, "Special %u", static_cast<unsigned>(value));
  } else {
    snprintf(out, outLen, "Unknown");
  }
}

bool BluetoothSettingsActivity::assignCapturedKey(uint8_t button) {
  using Entry = CrossPointSettings::BleKeyMapEntry;
  auto& map = SETTINGS.bleKeyMap;
  const uint8_t kind = pendingKeyKind;
  const uint8_t value = pendingKeyValue;

  // One key per action: drop any other key currently bound to this action so
  // the same virtual button can't be triggered by two different remote keys.
  std::replace_if(
      std::begin(map), std::end(map),
      [&](const Entry& e) { return e.button == button && !(e.keyKind == kind && e.keyValue == value); },
      Entry{});

  // Reuse the slot already bound to this key, else find the first free slot.
  auto* slot = std::find_if(std::begin(map), std::end(map), [&](const Entry& e) {
    return e.button != 0xFF && e.keyKind == kind && e.keyValue == value;
  });
  if (slot == std::end(map)) {
    slot = std::find_if(std::begin(map), std::end(map),
                        [](const Entry& e) { return e.button == 0xFF || e.keyKind == 0xFF; });
  }
  if (slot == std::end(map)) return false;  // table full

  slot->keyKind = kind;
  slot->keyValue = value;
  slot->button = button;
  SETTINGS.saveToFile();
  // Return true = in-RAM assignment landed (slot was available). Save
  // outcome is signalled separately via SETTINGS.hasDeferredSave() so the
  // caller can distinguish "table full" (return false) from "captured but
  // heap too low to persist" (return true, hasDeferredSave() true).
  return true;
}

void BluetoothSettingsActivity::renderButtonMap() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Map Buttons");

  if (mapStep == MapStep::WaitForKey) {
    GUI.drawSubHeader(
        renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
        "Press a remote button");
    // List currently-mapped bindings so the user sees progress.
    const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
    int row = 0;
    for (const auto& e : SETTINGS.bleKeyMap) {
      if (e.button == 0xFF || e.keyKind == 0xFF) continue;
      char keyName[24];
      describeMapKey(e.keyKind, e.keyValue, keyName, sizeof(keyName));
      const char* fnName = "?";
      for (uint8_t i = 0; i < kMapFnCount; i++) {
        if (kMapFns[i].button == e.button) {
          fnName = kMapFns[i].label;
          break;
        }
      }
      char line[64];
      snprintf(line, sizeof(line), "%s  ->  %s", keyName, fnName);
      GUI.drawHelpText(renderer, Rect{0, topOffset + row * 22, pageWidth, 20}, line);
      row++;
    }
    if (row == 0) {
      GUI.drawHelpText(renderer, Rect{0, topOffset, pageWidth, 22},
                       "No bindings yet -- press any remote key to start");
    }
  } else {
    char captured[24];
    describeMapKey(pendingKeyKind, pendingKeyValue, captured, sizeof(captured));
    GUI.drawSubHeader(
        renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, captured);
    const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
    GUI.drawList(
        renderer, Rect{0, topOffset, pageWidth, contentHeight}, kMapFnCount, functionIndex,
        [](int i) { return std::string(kMapFns[i].label); },
        nullptr, nullptr, nullptr, false);
  }

  if (!banner.empty()) {
    GUI.drawHelpText(renderer, Rect{0, pageHeight - metrics.buttonHintsHeight - 22, pageWidth, 20}, banner.c_str());
  }

  const char* confirm = mapStep == MapStep::SelectFunction ? tr(STR_SELECT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDebugMonitor() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Bluetooth Debug");
  const char* sub = btMgr && btMgr->isDebugCaptureEnabled() ? "Capture ON" : "Capture OFF";
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, sub);

  char l1[64], l2[64], l3[64], l4[64];
  unsigned int connectedCount = btMgr ? static_cast<unsigned int>(btMgr->getConnectedDevices().size()) : 0;
  snprintf(l1, sizeof(l1), "Connected: %u", connectedCount);
  snprintf(l2, sizeof(l2), "Key events: %u", static_cast<unsigned>(debugEventCount));
  snprintf(l3, sizeof(l3), "Unique keys: %u", static_cast<unsigned>(debugUniqueCount));
  snprintf(l4, sizeof(l4), "Last key: 0x%02X", static_cast<unsigned>(debugLastKeycode & 0xFF));
  const int base = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight;
  renderer.drawCenteredText(UI_12_FONT_ID, base + 24, l1);
  renderer.drawCenteredText(UI_12_FONT_ID, base + 48, l2);
  renderer.drawCenteredText(UI_12_FONT_ID, base + 72, l3);
  renderer.drawCenteredText(UI_12_FONT_ID, base + 96, l4);
  if (debugLastEventMs > 0) {
    char eventAge[64];
    snprintf(eventAge, sizeof(eventAge), "Last event: %lus ago", (millis() - debugLastEventMs) / 1000);
    renderer.drawCenteredText(UI_10_FONT_ID, base + 114, eventAge);
  }
  const int uniqueStartY = base + 132;
  if (debugUniqueCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY, "No key presses captured yet");
  } else {
    uint8_t order[kDebugUniqueKeyMax] = {0};
    for (uint8_t i = 0; i < debugUniqueCount; i++) order[i] = i;
    for (uint8_t i = 0; i + 1 < debugUniqueCount; i++) {
      uint8_t best = i;
      for (uint8_t j = i + 1; j < debugUniqueCount; j++) {
        if (debugUniqueCounts[order[j]] > debugUniqueCounts[order[best]]) best = j;
      }
      if (best != i) {
        const uint8_t tmp = order[i];
        order[i] = order[best];
        order[best] = tmp;
      }
    }
    const uint8_t renderCount = debugUniqueCount < 4 ? debugUniqueCount : 4;
    for (uint8_t i = 0; i < renderCount; i++) {
      const uint8_t idx = order[i];
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
  if (!banner.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, banner.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
