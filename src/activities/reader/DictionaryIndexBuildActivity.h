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
};
