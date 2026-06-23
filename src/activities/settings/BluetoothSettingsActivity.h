#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <string>

#include "activities/Activity.h"
#include "MappedInputManager.h"

class BluetoothSettingsActivity : public Activity {
 private:
  enum class ViewMode {
    MAIN_MENU,
    DEVICE_LIST,
    LEARN_KEYS,
    DEBUG_MONITOR
  };

  enum class LearnStep {
    WAIT_PREV,
    WAIT_NEXT,
    WAIT_TEST,
    DONE
  };

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";
  unsigned long lastScanTime = 0;
  // Throttles the e-ink re-render for the animated "Searching..." dots.
  unsigned long lastScanAnimMs = 0;
  LearnStep learnStep = LearnStep::WAIT_PREV;
  uint8_t pendingLearnKey = 0;
  uint8_t pendingLearnIndex = 0xFF;
  uint8_t learnedPrevKey = 0;
  uint8_t learnedNextKey = 0;
  uint8_t learnedReportIndex = 2;
  unsigned long learnTestDeadlineMs = 0;
  bool learnTestForwardSeen = false;
  bool learnTestBackSeen = false;
  uint16_t learnTestForwardCount = 0;
  uint16_t learnTestBackCount = 0;
  uint16_t debugLastKeycode = 0;
  uint32_t debugEventCount = 0;
  unsigned long debugLastEventMs = 0;
  static constexpr uint8_t kDebugUniqueKeyMax = 8;
  uint8_t debugUniqueKeys[kDebugUniqueKeyMax] = {0};
  uint16_t debugUniqueCounts[kDebugUniqueKeyMax] = {0};
  uint8_t debugUniqueCount = 0;
  bool exitOnSuccessfulConnect = false;

 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onComplete,
                                     const bool exitOnSuccessfulConnect = false)
      : Activity("BluetoothSettings", renderer, mappedInput),
        exitOnSuccessfulConnect(exitOnSuccessfulConnect),
        onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleMainMenuInput();
  void handleDeviceListInput();
  void handleLearnInput();
  void handleDebugInput();
  void renderMainMenu();
  void renderDeviceList();
  void renderLearnKeys();
  void renderDebugMonitor();
  std::string getSignalStrengthIndicator(const int32_t rssi) const;
  
  const std::function<void()> onComplete;
};
