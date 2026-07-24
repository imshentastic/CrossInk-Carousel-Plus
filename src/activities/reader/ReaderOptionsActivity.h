#pragma once
#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "util/ButtonNavigator.h"
#include "util/SettingsViewCache.h"

class ReaderOptionsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int settingsCount = 0;
  std::vector<SettingInfo> settings;
  // CrumBLE: set when onEnter could not build the settings list because the
  // heap was too fragmented (typical mid-BLE-read, after image-heavy pages).
  // render() draws an explanatory message instead of an empty list, and
  // loop() ignores Confirm so we don't index into an empty vector. Same
  // shape as BookSettingsDrawerActivity's low-heap fallback.
  bool lowHeap_ = false;
  // v18.9.9.50 (task #35): view-only fallback populated from the SD-cached
  // settings snapshot when the live build refuses on tight heap. Non-empty
  // means we're rendering a read-only list from the cache; user can scroll
  // and inspect but any edit tap triggers a silent-restart-with-
  // OpenReaderOptions so the fresh boot heap can safely build the live
  // list for editing.
  std::vector<SettingsViewRow> viewRows_;
  bool viewMode_ = false;

  void rebuildSettingsList();
  void toggleCurrentSetting();
  // Renders the value column for a cached row -- the view-mode analog
  // of the inline lambda in render() that reads from settings[].
  std::string viewRowValueText(const SettingsViewRow& row) const;

 public:
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderOptions", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
