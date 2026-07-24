#pragma once

// FreeInk SDK — BLE HID host (singleton).
//
// Pairs with and connects to a Bluetooth Low Energy HID peripheral (central
// role) and exposes translated key events (printable chars + a SpecialKey enum)
// plus scan/pair/connect controls for a settings UI. One peripheral at a time.
//
// Capability-gated: the real NimBLE implementation compiles only when
// FREEINK_CAP_BLE_HID_HOST is set (and the firmware adds NimBLE-Arduino to its
// lib_deps); otherwise every method links a stub so callers need no #ifdefs and
// no BLE code is pulled in. This header is deliberately NimBLE-free so consumers
// (and host builds) never include the BLE stack just to see the API.
//
// Memory: all storage is fixed-capacity (no std::vector / heap in the hot path).
// BLE callbacks run on the NimBLE host task and hand data to the app through a
// small spinlock-guarded ring; drain it from the main loop with popKey().
//
// BLE-only — the ESP32-C3/S3 has no Bluetooth Classic radio, so Classic-only HID
// peripherals cannot connect.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace freeink {

// Non-character keys an editor/UI cares about. Printable keys arrive as `ch`.
enum class SpecialKey : uint8_t {
  None = 0,
  Enter,
  Backspace,
  Tab,
  Escape,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
};

// One decoded key press (or auto-repeat). `pressed` is always true today — the
// host emits on the press edge and synthesizes repeats while a key is held; key
// releases are tracked internally for repeat but not surfaced.
struct KeyEvent {
  char ch = 0;                          // printable ASCII, or 0 for a special key
  uint8_t keycode = 0;                  // raw HID usage id
  uint8_t mods = 0;                     // HID modifier bitmask (ctrl/shift/alt/gui)
  SpecialKey special = SpecialKey::None;
  bool pressed = true;
};

// A BLE device seen during a scan.
struct DiscoveredDevice {
  char addr[18] = {0};  // "AA:BB:CC:DD:EE:FF"
  // v18.9.9.95: name[32] -> name[20]. Names past 19 chars truncate in the
  // scan-result picker (cosmetic). Compounded over kMaxDiscovered=4 = 48 B.
  char name[20] = {0};  // falls back to the address when no name was received
  int rssi = 0;
  uint8_t addrType = 0;  // BLE address type, needed to reconnect
  bool hasName = false;  // true when the advertised name was actually received
  bool hid = false;      // advertises the HID service (0x1812)
  bool connectable = false;
};

// A BLE HID peripheral the host has bonded with (persisted in NVS for auto-reconnect).
struct PairedHidDevice {
  char addr[18] = {0};
  // v18.9.9.95: name[32] -> name[20], same rationale. Over kMaxBonds=4 = 48 B.
  char name[20] = {0};
  uint8_t addrType = 0;
};
using PairedKeyboard = PairedHidDevice;  // Backward-compatible SDK name.

class BleKeyboardHost {
 public:
  // CrumBLE patch v18.9.9.95: further shrink for FT-headroom parity with
  // pre-BT-port (v18.9.9.82 already shrunk kMaxDiscovered 24 -> 8; measured
  // BT-port static RAM cost is 5.4 KB, and we need ~5 KB back at FT-time
  // to restore reliable TCP serve). kMaxDiscovered 8 -> 4: pairing UI now
  // shows the 4 strongest RSSI devices, enough for typical single-remote
  // pairing scenarios. kKeyQueueLen 16 -> 8: still 8-event auto-repeat ring.
  // kMaxBonds stays at 4 to keep multi-device bonding capacity intact.
  static constexpr uint8_t kMaxDiscovered = 4;
  static constexpr uint8_t kMaxBonds = 4;
  static constexpr uint8_t kKeyQueueLen = 8;

  static BleKeyboardHost& getInstance();

  // Init the NimBLE central, security (Just Works bonding), and load the saved
  // pairing list. Safe to call once. Returns false if BLE init failed or the
  // capability is compiled out.
  bool begin(const char* hostName = "FreeInk");

  // Fully tear down the BLE stack: stop scanning, drop the link, delete the
  // connection task, and NimBLEDevice::deinit() so the NimBLE host + controller
  // RAM (tens of KB) is returned to the heap. Use this — not disconnect() — when
  // the user turns Bluetooth off, so memory-hungry work (e.g. EPUB inflate) can
  // allocate again. Bonds persist in NVS; begin() re-inits cleanly afterwards.
  // Must run at normal CPU frequency (controller deinit), like begin().
  void end();

  // Pump per main-loop iteration: drives auto-reconnect and key auto-repeat.
  // Cheap; never blocks.
  void poll();

  // True while the NimBLE stack is initialized (between a successful begin() and
  // end()). Lets the app gate CPU-frequency and lifecycle decisions on whether BLE
  // is actually resident, independent of the user's on/off preference.
  bool isRunning() const { return begun_; }

  // --- Discovery -------------------------------------------------------------
  void startScan(uint32_t ms = 5000);
  void stopScan();
  bool isScanning() const { return scanning_; }
  uint8_t deviceCount() const { return deviceCount_; }
  const DiscoveredDevice& device(uint8_t i) const;
  // Free scan bookkeeping after connecting, to reclaim RAM while writing.
  void releaseScanResults();

  // --- Connection ------------------------------------------------------------
  // Begin an async connect to a scanned/bonded address. isConnected() flips once
  // the link is encrypted and the HID input report is subscribed.
  bool connect(const char* addr);
  void disconnect();
  bool isConnected() const { return connected_; }
  bool isConnecting() const { return connecting_; }
  const char* connectedName() const { return connName_; }
  bool takeConnectFailure(char* out, size_t outLen);
  bool takePairingPasskey(uint32_t& out);

  // --- Pairings (persisted) --------------------------------------------------
  uint8_t pairedCount() const { return bondCount_; }
  const PairedHidDevice& paired(uint8_t i) const;
  void forget(const char* addr);

  // --- Translated input ------------------------------------------------------
  // Pop the next key event. Returns false when the queue is empty.
  bool popKey(KeyEvent& out);

  // CrumBLE patch: raw-report observer, called from the NimBLE-host task each
  // time a HID input report arrives, before BleHid's own standard-keyboard
  // parser runs. Consumers use this to apply device-profile framing (e.g. IINE
  // Game Brick puts its keycode in byte[4] with a bitmask; Free3-R uses byte[0])
  // that BleHid's generic parser can't decode. Plain function pointer (no
  // heap, no std::function) — the observer runs on a BLE callback and must
  // not block or allocate. Pass nullptr to unhook.
  using RawReportObserver = void (*)(const uint8_t* data, size_t len);
  void setRawReportObserver(RawReportObserver obs) { rawObs_ = obs; }

  // CrumBLE patch: link lifecycle observers. Fires on the NimBLE host task
  // after BleHid's own state (connected_, connName_) is updated. Consumers
  // use these to bookkeep app-level state (e.g. synthesize a
  // ConnectedDevice entry for BluetoothHIDManager's report processor, arm
  // reason-520 early-drop alerts). Plain function pointers.
  using LinkUpObserver = void (*)(const char* addr, const char* name);
  using LinkDownObserver = void (*)(int reason);
  void setLinkUpObserver(LinkUpObserver obs) { linkUpObs_ = obs; }
  void setLinkDownObserver(LinkDownObserver obs) { linkDownObs_ = obs; }

  // --- Internal: called by the NimBLE backend (not for app use). These keep the
  // public header free of NimBLE types — the .cpp translates BLE objects into
  // these plain calls. -------------------------------------------------------
  void onScanResultIngest(const char* addr, const char* name, int rssi, uint8_t type, bool hid, bool connectable);
  void onReportIngest(const uint8_t* data, size_t len);
  void onLinkUp(const char* addr, const char* name, uint8_t type);
  void onLinkDown(int reason = 0);  // CrumBLE: reason forwarded to linkDownObs_
  void onConnectFailed(const char* reason);
  void onPairingPasskey(uint32_t passkey);

 private:
  void enqueue(const KeyEvent& ev);    // ring push (spinlock-guarded)
  void emitUsage(uint8_t usage, uint8_t mods);  // translate + enqueue
  void persistBonds();
  void loadBonds();
  BleKeyboardHost() = default;
  BleKeyboardHost(const BleKeyboardHost&) = delete;
  BleKeyboardHost& operator=(const BleKeyboardHost&) = delete;

  // Plain, NimBLE-free state shared by both the real and stub builds. The NimBLE
  // objects, spinlock, and connection task live file-static in the .cpp so this
  // header pulls in nothing.
  DiscoveredDevice devices_[kMaxDiscovered];
  uint8_t deviceCount_ = 0;
  PairedHidDevice bonds_[kMaxBonds];
  uint8_t bondCount_ = 0;
  KeyEvent ring_[kKeyQueueLen];
  volatile uint8_t ringHead_ = 0;  // next write
  volatile uint8_t ringTail_ = 0;  // next read
  // v18.9.9.95: shrunk from [32]/[48] to [24]/[24]. Device names past 23
  // chars truncate in the connection popup (cosmetic only). Failure messages
  // past 23 chars truncate in the toast (also cosmetic).
  char connName_[24] = {0};
  char connectFailure_[24] = {0};
  volatile uint32_t pairingPasskey_ = 0;
  volatile bool connected_ = false;
  volatile bool connecting_ = false;
  volatile bool connectFailed_ = false;
  volatile bool pairingPasskeyReady_ = false;
  volatile bool scanning_ = false;
  bool begun_ = false;

  // Key auto-repeat: HID delivers one report per state change, so holding a key
  // (backspace, arrows) only sends a single press. The backend records the held
  // usage here and poll() synthesizes repeats after an initial delay.
  volatile uint8_t heldUsage_ = 0;
  volatile uint8_t heldMods_ = 0;
  volatile uint32_t heldSince_ = 0;
  volatile uint32_t lastRepeat_ = 0;
  uint8_t prevKeys_[6] = {0};  // backend-task only

  // CrumBLE raw-report observer (see setRawReportObserver above). Read on the
  // NimBLE task; written from the main task — atomic pointer write is fine on
  // ESP32-C3 (aligned single-word).
  RawReportObserver rawObs_ = nullptr;
  LinkUpObserver linkUpObs_ = nullptr;
  LinkDownObserver linkDownObs_ = nullptr;
};

}  // namespace freeink

// App-friendly accessors. BleKbd is kept for source compatibility.
#define BleHid ::freeink::BleKeyboardHost::getInstance()
#define BleKbd ::freeink::BleKeyboardHost::getInstance()
