#pragma once

#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"

// Web server activity states
enum class WebServerActivityState {
  MODE_SELECTION,  // Choosing between Join Network and Create Hotspot
  WIFI_SELECTION,  // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,     // Starting Access Point mode
  SERVER_RUNNING,  // Web server is running and handling requests
  SHUTTING_DOWN    // Shutting down server and WiFi
};

/**
 * CrossPointWebServerActivity is the entry point for file transfer functionality.
 * It:
 * - First presents a choice between "Join a Network" (STA), "Connect to Calibre", and "Create Hotspot" (AP)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the CrossPointWebServer when connected
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class CrossPointWebServerActivity final : public Activity {
  WebServerActivityState state = WebServerActivityState::MODE_SELECTION;
  std::string returnBookPath;

  // Network mode
  NetworkMode networkMode = NetworkMode::JOIN_NETWORK;
  bool isApMode = false;

  // Web server - owned by this activity
  std::unique_ptr<CrossPointWebServer> webServer;
  // v18.9.9.83: latched at onEnter from the SILENT_REBOOT_LOWHEAP_RECOVERY_HINT
  // sentinel so the mid-serve floor-breach check (loop() → handleClient path)
  // can see we already tried the silent-restart path once. Prevents an infinite
  // low-heap silent-restart loop when the fresh-boot heap still can't fit the
  // web server allocation.
  bool justLowHeapRestarted_ = false;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // v18.9.9.372/v373: passive heap watchdog. Timestamp when the current
  // low-heap streak began (0 = not currently low). Once the streak
  // exceeds a threshold duration, silent-restart to FT.
  uint32_t heapLowStreakStartMs_ = 0;
  // v18.9.9.417: parallel streak-start for the upload-fragmentation
  // watchdog. Non-zero while maxAlloc has been below the fragmentation
  // floor for an active upload; resets to 0 when maxAlloc recovers or
  // when no upload is in flight.
  uint32_t uploadFragStreakStartMs_ = 0;
  // v18.9.9.373: timestamp when this FT activity was entered (post-boot
  // dispatch or fresh open). Used to enforce a minimum cooldown between
  // silent-restart-to-FT attempts.
  uint32_t activityEnteredAtMs_ = 0;

  // Sustained WiFi-loss tracking; abandon only after WIFI_ABANDON_MS.
  int consecutiveDisconnects = 0;
  unsigned long firstDisconnectAt = 0;
  static constexpr unsigned long WIFI_ABANDON_MS = 5UL * 60UL * 1000UL;

  // Cached signal-strength bracket (0..4) for the WiFi indicator.
  int lastWifiBars = 0;
  // v18.9.9.300: last polled RSSI in dBm (0 = not connected / not measured).
  // Cached so render() can show a weak-signal warning without hitting the
  // WiFi API each frame.
  int lastWifiRssi = 0;

  void renderServerRunning() const;
  void renderWifiIndicator(int subHeaderTop) const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startWebServer();
  void stopWebServer();
  void exitToOrigin();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::string returnBookPath = {})
      : Activity("CrossPointWebServer", renderer, mappedInput), returnBookPath(std::move(returnBookPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
