#pragma once

#include "activities/Activity.h"

// CrumBLE: explicit, user-consented driver for the one-time ~10s
// StarDict index scan. Shown after the LOOKUP entry prompt is accepted
// and before DictionaryWordSelectActivity launches, so the user knows
// exactly when the device is busy and why. Mirrors the loading-screen
// pattern of DictionaryDefinitionActivity: render() paints the status
// message, onEnter() calls requestUpdate() and then synchronously runs
// Dictionary::loadIndex(). On return the activity finishes with a
// success flag in ActivityResult::isCancelled (false = built OK).
class DictionaryIndexBuildActivity final : public Activity {
 public:
  DictionaryIndexBuildActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  void buildIndex();

  // CrumBLE 4.2: animated progress beacon for the one-time index scan.
  // dotCount_ cycles 1..4 each time the throttle in onProgress fires;
  // render() paints that many '.' on a row below the hint so the user
  // sees the device is still alive during the 10-30 s scan. lastRedrawMs_
  // throttles redraws because each requestUpdateAndWait blocks the scan
  // for one eink refresh (~500 ms) -- without the throttle, the
  // animation would noticeably extend the scan duration.
  uint8_t dotCount_ = 1;
  uint32_t lastRedrawMs_ = 0;
};
