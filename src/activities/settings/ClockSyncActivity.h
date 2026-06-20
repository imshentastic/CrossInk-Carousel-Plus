#pragma once

#include "activities/Activity.h"

// Manual NTP resync action. Runs a forced sync (bypassing the once-per-device debounce),
// reports success/failure, then waits for Back. Auto-connects to a saved WiFi network if
// one is available and WiFi isn't already up. Disconnects on exit only if the activity
// brought WiFi up itself (leaves a user-initiated session alone).
class ClockSyncActivity final : public Activity {
 public:
  explicit ClockSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { SYNCING, SUCCESS, NO_WIFI, FAILED };
  State state = SYNCING;
  char syncedTime[16] = {0};
  // CrumBLE 4.4: tracks whether we initiated the WiFi connection (vs found
  // it already up). When true, we disconnect in onExit so the activity
  // doesn't leak a network session the user didn't ask for.
  bool wifiActivatedByUs = false;

  void runSync();
};
