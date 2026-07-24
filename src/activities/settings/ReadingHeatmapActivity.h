#pragma once

#include "activities/Activity.h"

// v18.9.9.290: standalone reading heatmap view. Renders a GitHub-style
// contribution grid (7 rows × ~12 weeks) coloured by daily reading
// minutes. Data comes from ReadingStats::loadAggregates(). Access via
// Settings > System (or wherever wired up); no Home integration yet.
//
// Rendering: fill each cell with one of 4 tones based on minutes bucket
// (0, low, medium, high). Bucket thresholds are adaptive to the max
// value in view so a light reader still sees contrast.
class ReadingHeatmapActivity final : public Activity {
 public:
  explicit ReadingHeatmapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingHeatmap", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
