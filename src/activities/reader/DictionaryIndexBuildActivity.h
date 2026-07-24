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

  // v18.9.9.237: replaced 1-4 dot cycle with a real % progress bar so the
  // user can tell the device isn't frozen during long scans (a 181 MB
  // dict took 3.5 minutes in field testing; the dot animation was too
  // static to signal aliveness at that duration). percent_ mirrors the
  // onProgress callback's value directly (Dictionary::loadIndex computes
  // it as bytes-scanned/total * 100). lastRedrawMs_ throttles redraws
  // because each requestUpdateAndWait blocks the scan for one eink
  // refresh (~500 ms) -- without the throttle, the redraws would
  // noticeably extend the scan duration.
  int percent_ = 0;
  uint32_t lastRedrawMs_ = 0;
};
