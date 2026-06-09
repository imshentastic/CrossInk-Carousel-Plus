#pragma once
// Simulator-only no-op stub for the NimBLE-based BluetoothHIDManager.
//
// The real implementation (lib/hal/BluetoothHIDManager.{h,cpp}) depends on
// NimBLE/ESP32 and is unavailable in the native simulator build (the sim
// lib_ignores `hal`). This stub lets CrumBLE compile + run in the simulator,
// where Bluetooth is simply a no-op (you can't pair a real remote to the host).
// It is only on the include path for env:simulator (-Isrc/simulator/sim_stubs);
// on-device builds use the real lib/hal header.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi = 0;
  bool isHID = false;
};

class BluetoothHIDManager {
 public:
  static BluetoothHIDManager& getInstance() {
    static BluetoothHIDManager instance;
    return instance;
  }

  // Lifecycle
  bool enable() { return false; }
  bool disable() { return true; }
  bool isEnabled() const { return false; }

  // Deferred enable/disable
  void requestDisableLater() {}
  bool tryDisableIfRequested() { return false; }
  void requestEnableLater() {}
  bool tryEnableIfRequested() { return false; }

  // Scanning
  void startScan(uint32_t /*durationMs*/ = 10000) {}
  void stopScan() {}
  bool isScanning() const { return false; }
  const std::vector<BluetoothDevice>& getDiscoveredDevices() const { return _discoveredDevices; }

  // Connection
  bool connectToDevice(const std::string& /*address*/) { return false; }
  bool disconnectFromDevice(const std::string& /*address*/) { return false; }
  bool isConnected(const char* /*address*/) const { return false; }
  std::vector<std::string> getConnectedDevices() const { return {}; }

  // Input handling
  void processInputEvents() {}
  void setInputCallback(std::function<void(uint16_t)> /*cb*/) {}
  void setLearnInputCallback(std::function<void(uint8_t, uint8_t)> /*cb*/) {}
  void setButtonInjector(std::function<void(uint8_t, bool)> /*injector*/) {}
  void setReaderContextCallback(std::function<bool()> /*cb*/) {}
  void setButtonActivityNotifier(std::function<void(uint8_t)> /*notifier*/) {}
  void setDebugCaptureEnabled(bool /*enabled*/) {}
  bool isDebugCaptureEnabled() const { return false; }
  void setBondedDevice(const std::string& /*address*/, const std::string& /*name*/ = "") {}
  void updateActivity() {}
  void checkAutoReconnect(bool /*userInputDetected*/ = false) {}
  bool hasRecentActivity() const { return false; }
  bool hadRecentFree2Input(unsigned long /*windowMs*/ = 1500) const { return false; }

  // State persistence
  void saveState() {}
  void loadState() {}

  std::string lastError;

 private:
  BluetoothHIDManager() = default;
  std::vector<BluetoothDevice> _discoveredDevices;
};
