#pragma once

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  OtaUpdater updater;

  // CrumBLE 4.5: heap defragmentation reserve. Grabbed in onEnter (before
  // WiFi.mode allocates ~37KB) and released right before each HTTPS request
  // so WiFi's LWIP/mbedTLS scratch fragments AROUND it, leaving a guaranteed
  // ~50KB contiguous chunk for the SSL handshake / X.509 cert chain math.
  // Without this the post-WiFi MaxAlloc drops to ~36KB and cert verify OOMs
  // at MPI alloc inside RSA bignum parsing (-0x10 / MPI_ALLOC_FAILED).
  void* heapReserve = nullptr;
  size_t heapReserveSize = 0;
  void acquireHeapReserve();
  void releaseHeapReserve();

  void onWifiSelectionComplete(bool success);

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
