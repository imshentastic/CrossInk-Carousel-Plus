#include "DictionaryWordSelectActivity.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap()
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "BookmarkStore.h"  // BOOKMARK_PREVIEW_MAX
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

DictionaryWordSelectActivity::DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::unique_ptr<Page> page, int fontId, int marginLeft,
                                                           int marginTop, std::string cachePath, uint8_t orientation,
                                                           std::string nextPageFirstWord, Mode mode)
    : Activity("DictionaryWordSelect", renderer, mappedInput),
      page(std::move(page)),
      fontId(fontId),
      marginLeft(marginLeft),
      marginTop(marginTop),
      cachePath(std::move(cachePath)),
      orientation(orientation),
      nextPageFirstWord(std::move(nextPageFirstWord)),
      mode_(mode) {}

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

  // Pre-allocate vectors to prevent massive heap fragmentation.
  // CrumBLE tuning: reserve(350) is ~22 KB on a fragmented heap with BLE
  // connected (NimBLE ~58 KB, maxAlloc often drops to 7-10 KB), which
  // bad_allocs and reboots the device. Reduced to 80 (~5 KB) -- covers
  // most pages and re-allocates from there for long ones. Exceptions
  // are disabled (-fno-exceptions), so a failing allocation would call
  // __terminate() and reboot. Defense in depth against the LOOKUP
  // entry's heap pre-flight: re-check max contiguous alloc here, and
  // bail out clean if it slipped. Render falls back to showing the
  // page with no highlight (handled by the rows-empty guard).
  constexpr uint32_t EXTRACT_MIN_MAX_ALLOC = 8000;
  if (ESP.getMaxAllocHeap() < EXTRACT_MIN_MAX_ALLOC) {
    LOG_ERR("DICT", "extractWords: maxAlloc=%u below %u; aborting word build",
            ESP.getMaxAllocHeap(), EXTRACT_MIN_MAX_ALLOC);
    return;
  }
  words.reserve(80);
  rows.reserve(20);

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
    // CrumBLE: rows are pushed per Y-line, but wordIndices only gets
    // entries for lookup-able tokens. A row of pure punctuation /
    // styled glyphs leaves wordIndices empty, and starting at row 0
    // with an empty wordIndices vector faults the first render --
    // rows[0].wordIndices[0] derefs OOB on an empty vector. Skip
    // forward to the first row that actually has selectable words.
    currentRow = 0;
    currentWordInRow = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
      if (!rows[r].wordIndices.empty()) {
        currentRow = static_cast<int>(r);
        break;
      }
    }
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
    if (rows.empty() || currentRow < 0 || currentRow >= static_cast<int>(rows.size()) ||
        rows[currentRow].wordIndices.empty() || currentWordInRow < 0 ||
        currentWordInRow >= static_cast<int>(rows[currentRow].wordIndices.size())) {
      // No selectable word -- ignore the press.
    } else {
      int selectedWordIdx = rows[currentRow].wordIndices[currentWordInRow];

      if (mode_ == Mode::Lookup) {
        std::string wordToLookup = words[selectedWordIdx].lookupText;
        startActivityForResult(
            std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, wordToLookup, cachePath, fontId),
            [this](const ActivityResult& result) { requestUpdate(); });
      } else if (mode_ == Mode::HighlightSingleWord) {
        // One-tap mode for cross-page END pick. Emit start==end + just
        // the raw word's text as preview; the reader combines this with
        // the held start anchor to build the actual saved highlight.
        ActivityResult result;
        HighlightRangeResult hr;
        hr.startWordIndex = selectedWordIdx;
        hr.endWordIndex = selectedWordIdx;
        hr.previewText = words[selectedWordIdx].text;
        result.data = hr;
        setResult(std::move(result));
        finish();
      } else {
        // HighlightRange (same-page): first Confirm anchors the start;
        // second Confirm finishes with the range. Anchor lives until exit.
        if (highlightAnchorWordIdx_ < 0) {
          highlightAnchorWordIdx_ = selectedWordIdx;
          requestUpdate();
        } else {
          ActivityResult result;
          HighlightRangeResult hr;
          hr.startWordIndex = std::min(highlightAnchorWordIdx_, selectedWordIdx);
          hr.endWordIndex = std::max(highlightAnchorWordIdx_, selectedWordIdx);
          hr.previewText = buildPreviewBetween(hr.startWordIndex, hr.endWordIndex);
          result.data = hr;
          setResult(std::move(result));
          finish();
        }
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    // CrumBLE: in HighlightRange mode with the anchor placed, Back means
    // "hold the start for later" instead of cancel -- emit a result with
    // startWordIndex=anchor and endWordIndex=-1. Reader stores this as
    // pendingHighlightStart_ and surfaces FINISH / CANCEL in the menu.
    // Lookup mode and pre-anchor HighlightRange still treat Back as cancel.
    if (mode_ == Mode::HighlightRange && highlightAnchorWordIdx_ >= 0 &&
        highlightAnchorWordIdx_ < static_cast<int>(words.size())) {
      HighlightRangeResult hr;
      hr.startWordIndex = highlightAnchorWordIdx_;
      hr.endWordIndex = -1;  // signal: anchor only
      hr.previewText = words[highlightAnchorWordIdx_].text;
      result.data = hr;
    } else {
      result.isCancelled = true;
    }
    setResult(std::move(result));
    finish();
  }

  if (selectionChanged) requestUpdate();
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  page->render(renderer, fontId, marginLeft, marginTop);

  // CrumBLE: belt and suspenders -- extractWords now skips empty rows,
  // but if currentRow lands on one anyway (defensive) or words is empty
  // entirely, skip the highlight pass instead of dereferencing OOB.
  if (!rows.empty() && currentRow >= 0 && currentRow < static_cast<int>(rows.size()) &&
      !rows[currentRow].wordIndices.empty() && currentWordInRow >= 0 &&
      currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size())) {
    int selectedWordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const int lineHeight = renderer.getLineHeight(fontId);

    auto drawSingleWordBox = [&](int index) {
      const WordInfo& word = words[index];
      int boxX = word.screenX;
      int boxY = word.screenY;
      int boxWidth = word.width;

      renderer.fillRect(boxX, boxY + lineHeight + 2, boxWidth, 3, true);
      renderer.fillRect(boxX, boxY - 3, boxWidth, 1, true);
      renderer.fillRect(boxX - 3, boxY - 3, 2, lineHeight + 8, true);
      renderer.fillRect(boxX + boxWidth + 1, boxY - 3, 2, lineHeight + 8, true);
    };

    // HighlightRange mode + anchor placed: render an underline across
    // every word in the inclusive range [anchor, cursor]. Single-word
    // box still used to mark the cursor itself so the user sees where
    // the next Confirm will end the range.
    if (mode_ == Mode::HighlightRange && highlightAnchorWordIdx_ >= 0 &&
        highlightAnchorWordIdx_ < static_cast<int>(words.size())) {
      const int lo = std::min(highlightAnchorWordIdx_, selectedWordIdx);
      const int hi = std::max(highlightAnchorWordIdx_, selectedWordIdx);
      for (int i = lo; i <= hi && i < static_cast<int>(words.size()); ++i) {
        const WordInfo& w = words[i];
        // Thicker underline (3 px) under each in-range word; lets the
        // selection be visually distinct from the cursor's box. Skips
        // continuation slots to avoid double-marking hyphenated halves.
        if (w.continuationOf != -1) continue;
        renderer.fillRect(w.screenX, w.screenY + lineHeight + 2, w.width, 3, true);
      }
    }

    drawSingleWordBox(selectedWordIdx);
    if (words[selectedWordIdx].continuationIndex != -1) {
      drawSingleWordBox(words[selectedWordIdx].continuationIndex);
    }
    if (words[selectedWordIdx].continuationOf != -1) {
      drawSingleWordBox(words[selectedWordIdx].continuationOf);
    }
  }

  // CrumBLE: button hints differ by mode and state.
  //   Lookup        : Cancel / Lookup     /.../ Prev-Next
  //   Range (pre-)  : Cancel / Start      /.../ Prev-Next
  //   Range (held)  : Hold   / End        /.../ Prev-Next
  //                   ^ Back changes meaning once an anchor is placed.
  //   SingleWord    : Cancel / End        /.../ Prev-Next
  const char* btn1Label = tr(STR_CANCEL);
  const char* btn2Label = tr(STR_LOOKUP);
  if (mode_ == Mode::HighlightRange) {
    btn2Label = (highlightAnchorWordIdx_ < 0) ? tr(STR_HIGHLIGHT_START) : tr(STR_HIGHLIGHT_END);
    if (highlightAnchorWordIdx_ >= 0) btn1Label = tr(STR_HIGHLIGHT_HOLD);
  } else if (mode_ == Mode::HighlightSingleWord) {
    btn2Label = tr(STR_HIGHLIGHT_END);
  }
  const auto labels = mappedInput.mapLabels(btn1Label, btn2Label, "", tr(STR_PREV_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string DictionaryWordSelectActivity::buildPreviewBetween(int a, int b) const {
  const int lo = std::min(a, b);
  const int hi = std::max(a, b);
  if (lo < 0 || hi >= static_cast<int>(words.size())) return {};

  std::string out;
  out.reserve(BOOKMARK_PREVIEW_MAX);
  for (int i = lo; i <= hi; ++i) {
    // Skip continuation halves so hyphenated words aren't duplicated.
    if (words[i].continuationOf != -1) continue;
    if (!out.empty()) out += ' ';
    out += words[i].text;
    // Cap early once the string would exceed storage (-1 for NUL,
    // -3 for the ellipsis). Saves cycles on long ranges.
    if (out.size() >= BOOKMARK_PREVIEW_MAX - 4) break;
  }
  if (out.size() > BOOKMARK_PREVIEW_MAX - 1) {
    out.resize(BOOKMARK_PREVIEW_MAX - 4);
    out += "...";
  }
  return out;
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