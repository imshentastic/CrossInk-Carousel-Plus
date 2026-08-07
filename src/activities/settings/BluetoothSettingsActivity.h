#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "MappedInputManager.h"
#include "util/ButtonNavigator.h"

// CrumBLE 4.5.5: rewritten as a 1:1 port of upstream crosspoint-reader
// feat-bluetooth (Free-Ink/freeink-sdk) BluetoothSettingsActivity. Three
// primary views (Menu / Scan / Paired) with dynamic menu rows that appear
// only when the relevant action makes sense. Two CrumBLE-only sub-views
// (LearnKeys, DebugMonitor) are preserved as-is because they have no
// upstream equivalent.
class BluetoothSettingsActivity : public Activity {
 private:
  enum class View {
    Menu,
    Scan,
    Paired,
    ButtonMap,    // 4.5.5: upstream-style WaitForKey -> SelectFunction loop
    DebugMonitor,
  };
  // ButtonMap sub-states. WaitForKey: prompt the user to press a remote key;
  // SelectFunction: a key was captured, pick which local virtual button it
  // should drive.
  enum class MapStep { WaitForKey, SelectFunction };

  // Menu row actions. Subset of upstream; we have a single-bonded model so
  // PairedDevices points at the one bonded slot (or is absent when there's
  // no bond yet). MapButtons opens the LearnKeys sub-view.
  enum class Action {
    ToggleBt,
    Scan,
    Disconnect,
    PairedDevices,
    MapButtons,
#ifdef ENABLE_BT_DEBUG_MONITOR
    DebugMonitor,
#endif
  };
  struct MenuRow {
    Action action;
    const char* label;  // inline literal -- string-table additions deferred
  };

  // ---- View / navigation state ----------------------------------------
  View view = View::Menu;
  std::vector<MenuRow> menuRows;
  int menuIndex = 0;
  int scanIndex = 0;
  int pairedIndex = 0;
  ButtonNavigator buttonNavigator;

  // Transient status banner shown above the button hints (connect result,
  // forget confirmation, scan-heap-floor refusal, etc.). Auto-clears.
  std::string banner;
  unsigned long bannerUntil = 0;

  // Set when a connect() has been issued and we're polling the BluetoothHID
  // Manager for the async result.
  bool awaitingConnect = false;
  // 4.5.5: remembered across the async-connect window so we can persist the
  // bond into SETTINGS on success. The Paired view doesn't need these
  // (address already in SETTINGS) but the Scan view does.
  std::string pendingConnectAddress;
  std::string pendingConnectName;
  unsigned long awaitingConnectStartedAt = 0;
  // Guards Paired view's hold-to-forget so it fires once per hold and
  // suppresses the tap-to-connect on the same press.
  bool pairedActionTaken = false;
  // Throttle the e-ink repaint while the scan list is animating.
  unsigned long lastScanAnimMs = 0;

  // ---- ButtonMap sub-view state (1:1 port of upstream BleButtonMapActivity)
  MapStep mapStep = MapStep::WaitForKey;
  uint8_t pendingKeyKind = 0xFF;    // 0 = special key (reserved), 1 = HID usage
  uint8_t pendingKeyValue = 0;
  uint8_t functionIndex = 0;        // selected row in the SelectFunction list

  // ---- CrumBLE-only: debug monitor state -------------------------------
  // These counters are written from the BLE HID input callback, which runs on
  // the nimble_host task, and read by render() on the main loop. The race is
  // benign for a diagnostic readout (worst case one frame shows a stale count),
  // and the alternative -- a lock around a display update -- is not worth it.
  uint16_t debugLastKeycode = 0;
  uint32_t debugEventCount = 0;
  unsigned long debugLastEventMs = 0;
  // Set by that same callback to mean "the numbers changed, repaint". The
  // callback must NOT call requestUpdate() itself: it would drive a panel
  // refresh from the BLE task. loop() consumes this on the main loop, the same
  // way the ButtonMap view polls pendingKeyKind. Without it the Debug Monitor
  // painted once on entry and never again -- key events reached the serial log
  // but the screen never moved.
  std::atomic<bool> debugDirty{false};
  unsigned long debugLastRepaintMs = 0;
  static constexpr uint8_t kDebugUniqueKeyMax = 8;
  uint8_t debugUniqueKeys[kDebugUniqueKeyMax] = {0};
  uint16_t debugUniqueCounts[kDebugUniqueKeyMax] = {0};
  uint8_t debugUniqueCount = 0;

  // ---- Ctor-passed contracts ------------------------------------------
  // When true, the activity auto-finishes once a connect succeeds. Used by
  // the in-book reader's quick-connect entry point so the user is dropped
  // back into the book the instant the remote links.
  bool fromReader = false;
  // 4.5.5: tear BT down on exit. Default true matches the historical
  // Main-Settings-entry behavior (NimBLE holds ~60 KB; Home/Bookshelf
  // OOM'd if left running). Reader entry points pass false because the
  // user is going back to a book that wants the BT connection alive --
  // disabling here meant a "BT goes off when I peek at its settings"
  // regression vs CrumBLE pre-4.5.5.
  bool disableOnExit = true;
  const std::function<void()> onComplete;
  BluetoothHIDManager* btMgr = nullptr;

 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onComplete,
                                     const bool fromReader = false,
                                     const bool disableOnExit = true)
      : Activity("BluetoothSettings", renderer, mappedInput),
        fromReader(fromReader),
        disableOnExit(disableOnExit),
        onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // ---- Menu helpers ----------------------------------------------------
  void rebuildMenuRows();
  void handleMenuConfirm();
  void setBanner(const char* text, unsigned long durationMs = 2000);
  void startScanView();
  // Pre-flight the NimBLE heap before issuing a scan. Sets `banner` and
  // returns false if heap is below the floor. CrumBLE-specific; upstream's
  // freeink-sdk handles this internally.
  bool checkScanHeapOrBanner();

  // ---- View renderers --------------------------------------------------
  void renderMenu();
  void renderScan();
  void renderPaired();
  void renderButtonMap();
  void renderDebugMonitor();
  // Persist the captured (kind, value) bound to `button` into SETTINGS.bleKeyMap.
  // One-key-per-action: any existing slot pointing at the same button is dropped
  // first so a button can't have two keys. Returns false if all 12 slots are
  // already used by other bindings.
  bool assignCapturedKey(uint8_t button);
  // Friendly label for a captured (kind, value) pair shown in the SelectFunction
  // header. Writes a null-terminated string into out.
  void describeMapKey(uint8_t kind, uint8_t value, char* out, size_t outLen) const;

  // ---- Connect-flow helpers -------------------------------------------
  // Returns the name of the currently connected device (or empty if none).
  std::string connectedDeviceName() const;
  // Returns true if we have a remembered bonded device (single-bonded model
  // -- SETTINGS.bleBondedDeviceAddr[0] != 0).
  bool hasBondedDevice() const;
  // Drop the current pairing record from SETTINGS, plus disconnect if live.
  void forgetBondedDevice();
};
