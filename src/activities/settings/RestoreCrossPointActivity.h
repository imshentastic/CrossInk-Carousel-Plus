#pragma once

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

// Emergency "Restore CrossPoint" flash. Buried at Settings -> System ->
// Recovery -> Restore CrossPoint so accidental selection takes deliberate
// drilling. Once entered, the activity gates on:
//   1. Battery >= 50% (mid-flash brownout would brick)
//   2. WiFi connected (no WiFi-pairing flow inside Recovery -- user must
//      connect through the normal Network settings first)
//   3. 5-second hold-to-confirm on the OK button (progress bar fills)
// Then downloads the latest crosspoint-reader release via esp_https_ota
// straight into the OTA partition and reboots into it. The CrumBLE
// install becomes the "previous" OTA slot and is reachable again only
// via SD-card flash (same as any other OTA from this codebase).
class RestoreCrossPointActivity : public Activity {
  enum State {
    CHECKING_GATES,
    BATTERY_LOW,
    NO_WIFI,
    READY,
    HOLDING,
    DOWNLOADING,
    FAILED,
  };

  State state_ = CHECKING_GATES;
  uint32_t holdStartMs_ = 0;
  uint32_t lastRenderedHoldPct_ = 999;
  unsigned int lastUpdaterPercentage_ = 999;
  OtaUpdater updater_;

 public:
  RestoreCrossPointActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RestoreCrossPoint", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == DOWNLOADING || state_ == HOLDING; }
};
