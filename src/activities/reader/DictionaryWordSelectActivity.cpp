#include "DictionaryWordSelectActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

DictionaryWordSelectActivity::DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::unique_ptr<Page> page, int fontId, int marginLeft,
                                                           int marginTop, std::string cachePath, uint8_t orientation,
                                                           std::string nextPageFirstWord)
    : Activity("DictionaryWordSelect", renderer, mappedInput),
      page(std::move(page)),
      fontId(fontId),
      marginLeft(marginLeft),
      marginTop(marginTop),
      cachePath(std::move(cachePath)),
      orientation(orientation),
      nextPageFirstWord(std::move(nextPageFirstWord)) {}

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  extractWords();
  mergeHyphenatedWords();
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  Activity::onExit();

  // FIX: Aggressively free all resources and defragment heap before returning to the reader
  words.clear();
  words.shrink_to_fit();

  rows.clear();
  rows.shrink_to_fit();

  // Explicitly reset the unique_ptr to free the parsed DOM memory immediately
  page.reset();
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  rows.clear();
  if (!page) return;

  // FIX: Pre-allocate vectors to prevent massive heap fragmentation.
  // An average screen has ~250-300 words. Vector reallocation without reserve()
  // creates a "Swiss cheese" heap, eventually starving the E-ink renderer.
  words.reserve(350);
  rows.reserve(40);

  int currentRowIndex = -1;
  int16_t lastY = -1;

  for (const auto& element : page->elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;

    const auto& line = static_cast<const PageLine&>(*element);
    auto block = line.getBlock();
    if (!block) continue;

    const auto& lineWords = block->getWords();
    if (lineWords.empty()) continue;

    const auto& wordXpos = block->getWordXpos();
    const auto& wordStyles = block->getWordStyles();

    int16_t currentY = marginTop + line.yPos;
    if (currentY != lastY) {
      rows.push_back({currentY, {}});
      currentRowIndex++;
      lastY = currentY;
    }

    for (size_t i = 0; i < lineWords.size(); ++i) {
      const std::string& raw = lineWords[i];
      int16_t baseX = marginLeft + line.xPos + wordXpos[i];

      std::string prefix = "";
      std::string current_word = "";

      auto emitWord = [&]() {
        if (current_word.empty()) return;

        int pre_w = prefix.empty() ? 0 : renderer.getTextWidth(fontId, prefix.c_str(), wordStyles[i]);
        int word_w = renderer.getTextWidth(fontId, current_word.c_str(), wordStyles[i]);

        std::string lookup = current_word;
        while (!lookup.empty() && lookup.back() == '\'') lookup.pop_back();
        while (!lookup.empty() && lookup.front() == '\'') lookup.erase(0, 1);

        if (!lookup.empty()) {
          int wordIndex = static_cast<int>(words.size());
          words.emplace_back(current_word, baseX + pre_w, currentY, word_w, currentRowIndex);
          words.back().lookupText = lookup;
          rows[currentRowIndex].wordIndices.push_back(wordIndex);
        }
      };

      for (size_t c = 0; c < raw.length();) {
        unsigned char ch = static_cast<unsigned char>(raw[c]);

        if (ch == 0xE2 && c + 2 < raw.length() && static_cast<unsigned char>(raw[c + 1]) == 0x80) {
          emitWord();
          prefix += current_word + raw.substr(c, 3);
          current_word = "";
          c += 3;
          continue;
        }

        bool isWordChar = (ch > 127 || std::isalnum(ch) || ch == '\'');

        if (isWordChar) {
          current_word += raw[c];
          c++;
        } else {
          emitWord();
          prefix += current_word + raw[c];
          current_word = "";
          c++;
        }
      }
      emitWord();

      if (i == lineWords.size() - 1 && !raw.empty() && raw.back() == '-') {
        if (!words.empty() && words.back().rowIndex == currentRowIndex) {
          words.back().isHyphenatedLineEnd = true;
        }
      }
    }
  }

  if (!words.empty()) {
    currentRow = 0;
    currentWordInRow = 0;
  }
}

int DictionaryWordSelectActivity::findClosestWordIndexInRow(int rowIndex, int targetX) const {
  if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()) || rows[rowIndex].wordIndices.empty()) {
    return 0;
  }
  const auto& rowWords = rows[rowIndex].wordIndices;
  int bestIndex = 0;
  int minDistance = 99999;
  for (size_t i = 0; i < rowWords.size(); ++i) {
    const auto& word = words[rowWords[i]];
    int wordCenterX = word.screenX + (word.width / 2);
    int distance = std::abs(wordCenterX - targetX);
    if (distance < minDistance) {
      minDistance = distance;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

void DictionaryWordSelectActivity::loop() {
  if (rows.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  bool selectionChanged = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (currentWordInRow > 0) {
      currentWordInRow--;
      selectionChanged = true;
    } else if (currentRow > 0) {
      currentRow--;
      currentWordInRow = static_cast<int>(rows[currentRow].wordIndices.size()) - 1;
      selectionChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size()) - 1) {
      currentWordInRow++;
      selectionChanged = true;
    } else if (currentRow < static_cast<int>(rows.size()) - 1) {
      currentRow++;
      currentWordInRow = 0;
      selectionChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (currentRow > 0) {
      int currentWordIdx = rows[currentRow].wordIndices[currentWordInRow];
      int targetX = words[currentWordIdx].screenX + (words[currentWordIdx].width / 2);
      currentRow--;
      currentWordInRow = findClosestWordIndexInRow(currentRow, targetX);
      selectionChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (currentRow < static_cast<int>(rows.size()) - 1) {
      int currentWordIdx = rows[currentRow].wordIndices[currentWordInRow];
      int targetX = words[currentWordIdx].screenX + (words[currentWordIdx].width / 2);
      currentRow++;
      currentWordInRow = findClosestWordIndexInRow(currentRow, targetX);
      selectionChanged = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int selectedWordIdx = rows[currentRow].wordIndices[currentWordInRow];
    std::string wordToLookup = words[selectedWordIdx].lookupText;

    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, wordToLookup, cachePath, fontId),
        [this](const ActivityResult& result) { requestUpdate(); });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  if (selectionChanged) requestUpdate();
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!rows.empty()) {
    int selectedWordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const int lineHeight = renderer.getLineHeight(fontId);

    auto drawHighlight = [&](int index) {
      const WordInfo& word = words[index];
      int boxX = word.screenX;
      int boxY = word.screenY;
      int boxWidth = word.width;

      renderer.fillRect(boxX, boxY + lineHeight + 2, boxWidth, 3, true);
      renderer.fillRect(boxX, boxY - 3, boxWidth, 1, true);
      renderer.fillRect(boxX - 3, boxY - 3, 2, lineHeight + 8, true);
      renderer.fillRect(boxX + boxWidth + 1, boxY - 3, 2, lineHeight + 8, true);
    };

    drawHighlight(selectedWordIdx);

    if (words[selectedWordIdx].continuationIndex != -1) {
      drawHighlight(words[selectedWordIdx].continuationIndex);
    }
    if (words[selectedWordIdx].continuationOf != -1) {
      drawHighlight(words[selectedWordIdx].continuationOf);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_LOOKUP), tr(STR_UP_DOWN), tr(STR_PREV_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void DictionaryWordSelectActivity::mergeHyphenatedWords() {
  if (words.empty()) return;
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i].isHyphenatedLineEnd && i + 1 < words.size()) {
      std::string merged = words[i].lookupText + words[i + 1].lookupText;
      words[i].lookupText = merged;
      words[i + 1].lookupText = merged;
      words[i].continuationIndex = static_cast<int>(i + 1);
      words[i + 1].continuationOf = static_cast<int>(i);
    }
  }
}