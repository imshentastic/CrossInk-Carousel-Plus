#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "activities/Activity.h"

struct WordInfo {
  std::string text;
  std::string lookupText;
  int screenX;
  int screenY;
  int width;
  int rowIndex;

  bool isHyphenatedLineEnd = false;
  int continuationIndex = -1;
  int continuationOf = -1;

  WordInfo(std::string t, int x, int y, int w, int r)
      : text(std::move(t)), screenX(x), screenY(y), width(w), rowIndex(r) {}
};

struct RowInfo {
  int16_t y;
  std::vector<int> wordIndices;
};

class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        std::string cachePath, uint8_t orientation, std::string nextPageFirstWord);

  void onEnter() override;
  void loop() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  std::unique_ptr<Page> page;
  int fontId;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  uint8_t orientation;
  std::string nextPageFirstWord;

  std::vector<WordInfo> words;
  std::vector<RowInfo> rows;
  int currentRow = 0;
  int currentWordInRow = 0;

  void extractWords();
  void mergeHyphenatedWords();
  int findClosestWordIndexInRow(int rowIndex, int targetX) const;
};