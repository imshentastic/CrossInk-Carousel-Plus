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
  // CrumBLE: the same word-picking machinery powers two flows:
  //   - Lookup: tap a word, dive into DictionaryDefinitionActivity.
  //   - HighlightRange: tap a word to anchor, navigate to expand the
  //     selection, tap again to finish. Emits a HighlightRangeResult
  //     with both word indices + the joined preview text.
  enum class Mode { Lookup, HighlightRange };

  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        std::string cachePath, uint8_t orientation, std::string nextPageFirstWord,
                                        Mode mode = Mode::Lookup);

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
  Mode mode_;

  std::vector<WordInfo> words;
  std::vector<RowInfo> rows;
  int currentRow = 0;
  int currentWordInRow = 0;

  // HighlightRange mode: word index in `words` of the first Confirm
  // (selection anchor). -1 means start hasn't been picked yet -- cursor
  // navigation moves a single word as in Lookup. Once set, the cursor
  // and the anchor define an inclusive range that's rendered as a
  // contiguous highlight underline rather than the single-word box.
  int highlightAnchorWordIdx_ = -1;

  void extractWords();
  void mergeHyphenatedWords();
  int findClosestWordIndexInRow(int rowIndex, int targetX) const;

  // Build the preview text from the inclusive word range [a, b] in the
  // current page's words vector. Joins raw word text with single spaces
  // and truncates to BOOKMARK_PREVIEW_MAX-1 chars with a trailing "..."
  // if the joined text is longer. Order-agnostic (handles b < a).
  std::string buildPreviewBetween(int a, int b) const;
};