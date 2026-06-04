#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string word,
                                        std::string cachePath, int fontId);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string targetWord;
  std::string cachePath;
  int fontId;

  std::string definition;
  std::vector<std::string> wrappedLines;

  bool isLoading = true;
  bool notFound = false;

  int scrollOffset = 0;
  int maxScroll = 0;
  int linesPerPage = 0;
  int lineHeight = 0;

  void performLookup();
  void wrapText();
};