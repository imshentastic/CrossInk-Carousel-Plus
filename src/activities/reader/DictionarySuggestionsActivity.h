#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

class DictionarySuggestionsActivity final : public Activity {
 public:
  explicit DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string word,
                                         std::string cachePath);

  void onEnter() override;
  void loop() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  std::string targetWord;
  std::string cachePath;
  std::vector<std::string> suggestions;

  int selectedIndex = 0;
  bool isLoading = true;

  void findSuggestions();
};