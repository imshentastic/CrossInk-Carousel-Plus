#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

class LookedUpWordsActivity final : public Activity {
 public:
  explicit LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string cachePath);

  void onEnter() override;
  void loop() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  std::string cachePath;
  std::vector<std::string> words;

  int selectedIndex = 0;
  int scrollOffset = 0;
  int linesPerPage = 0;

  bool isLoading = true;
  bool confirmingDelete = false;

  void loadHistory();
  void deleteSelectedWord();
};