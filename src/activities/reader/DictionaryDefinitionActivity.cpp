#include "DictionaryDefinitionActivity.h"

#include <I18n.h>

#include <sstream>

#include "CrossPointSettings.h"
#include "DictionarySuggestionsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

DictionaryDefinitionActivity::DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::string word, std::string cachePath, int fontId)
    : Activity("DictionaryDefinition", renderer, mappedInput),
      targetWord(std::move(word)),
      cachePath(std::move(cachePath)),
      fontId(fontId) {}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
  performLookup();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  wrappedLines.clear();
  wrappedLines.shrink_to_fit();
  definition.clear();
  definition.shrink_to_fit();
}

void DictionaryDefinitionActivity::performLookup() {
  definition = Dictionary::lookup(targetWord);

  if (definition.empty()) {
    auto stems = Dictionary::getStemVariants(targetWord);
    for (const auto& stem : stems) {
      definition = Dictionary::lookup(stem);
      if (!definition.empty()) {
        targetWord = stem;
        break;
      }
    }
  }

  // FIX: As soon as we have the definition, immediately return the RAM to the system!
  Dictionary::freeMemory();

  if (definition.empty()) {
    notFound = true;
  } else {
    LookupHistory::addWord(cachePath, targetWord);
    wrapText();
  }

  isLoading = false;
  requestUpdate();
}

void DictionaryDefinitionActivity::wrapText() {
  wrappedLines.clear();

  // FIX: Pre-allocate memory to avoid heap fragmentation during line wrapping
  wrappedLines.reserve(50);

  if (definition.empty()) return;

  const int margin = 20;
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  std::stringstream ss(definition);
  std::string paragraph;
  while (std::getline(ss, paragraph, '\n')) {
    if (paragraph.empty()) {
      wrappedLines.push_back("");
      continue;
    }

    auto pLines = renderer.wrappedText(fontId, paragraph.c_str(), maxWidth, 1000);
    wrappedLines.insert(wrappedLines.end(), pLines.begin(), pLines.end());
  }

  lineHeight = renderer.getLineHeight(fontId);

  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int startY = margin + titleH + (margin * 2);

  const int bottomMarginForHints = 55;
  const int availableHeight = renderer.getScreenHeight() - startY - bottomMarginForHints;

  linesPerPage = availableHeight / lineHeight;
  maxScroll = std::max(0, static_cast<int>(wrappedLines.size()) - linesPerPage);
}

void DictionaryDefinitionActivity::loop() {
  if (isLoading) return;

  bool needsUpdate = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (scrollOffset > 0) {
      scrollOffset = std::max(0, scrollOffset - linesPerPage + 1);
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (scrollOffset < maxScroll) {
      scrollOffset = std::min(maxScroll, scrollOffset + linesPerPage - 1);
      needsUpdate = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (notFound) {
      // If word not found, pressing Confirm opens the Suggestions list
      startActivityForResult(
          std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, targetWord, cachePath),
          [this](const ActivityResult& result) {
            // When returning from suggestions, we finish the definition view too
            ActivityResult fwResult;
            setResult(std::move(fwResult));
            finish();
          });
      return;
    } else {
      ActivityResult result;
      setResult(std::move(result));
      finish();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    setResult(std::move(result));
    finish();
  }

  if (needsUpdate) requestUpdate();
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int margin = 20;
  int currentY = margin;

  if (isLoading) {
    renderer.drawText(fontId, margin, currentY, tr(STR_LOOKING_UP));
  } else if (notFound) {
    std::string notFoundMsg = std::string(tr(STR_WORD_NOT_FOUND)) + targetWord;
    renderer.drawText(fontId, margin, currentY, notFoundMsg.c_str());

    currentY += renderer.getLineHeight(fontId) * 2;
    renderer.drawText(fontId, margin, currentY, tr(STR_PRESS_CONFIRM_SUGGESTIONS));
  } else {
    renderer.drawText(UI_12_FONT_ID, margin, currentY, targetWord.c_str(), EpdFontFamily::BOLD);

    int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, targetWord.c_str(), EpdFontFamily::BOLD);
    int titleH = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.fillRect(margin, currentY + titleH + 4, titleWidth, 3, true);

    currentY += titleH + (margin * 2);

    int linesToDraw = std::min(linesPerPage, static_cast<int>(wrappedLines.size()) - scrollOffset);
    for (int i = 0; i < linesToDraw; ++i) {
      renderer.drawText(fontId, margin, currentY, wrappedLines[scrollOffset + i].c_str());
      currentY += lineHeight;
    }

    if (scrollOffset > 0) {
      renderer.drawText(fontId, renderer.getScreenWidth() - margin - 20, margin * 2, "^");
    }
    if (scrollOffset < maxScroll) {
      renderer.drawText(fontId, renderer.getScreenWidth() - margin - 20, renderer.getScreenHeight() - 60, "v");
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_SCROLL), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}