#include "DictionarySuggestionsActivity.h"

#include <I18n.h>

#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"

DictionarySuggestionsActivity::DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string word, std::string cachePath)
    : Activity("DictionarySuggestions", renderer, mappedInput),
      targetWord(std::move(word)),
      cachePath(std::move(cachePath)) {}

void DictionarySuggestionsActivity::onEnter() {
  Activity::onEnter();
  isLoading = true;
  requestUpdate();
}

void DictionarySuggestionsActivity::onExit() {
  Activity::onExit();
  // FIX: Explicitly clear and shrink vector to free RAM immediately upon exiting
  suggestions.clear();
  suggestions.shrink_to_fit();
}

void DictionarySuggestionsActivity::findSuggestions() {
  // Limit to top 5 closest matches to prevent memory bloat and long lists
  suggestions = Dictionary::findSimilar(targetWord, 5);

  // FIX: The dictionary index is loaded into RAM here.
  // We MUST explicitly release it back to the system immediately!
  Dictionary::freeMemory();

  isLoading = false;
  requestUpdate();
}

void DictionarySuggestionsActivity::loop() {
  if (isLoading) {
    findSuggestions();
    return;
  }

  if (suggestions.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ActivityResult result;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  bool needsUpdate = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (selectedIndex > 0) {
      selectedIndex--;
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (selectedIndex < static_cast<int>(suggestions.size()) - 1) {
      selectedIndex++;
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Launch Definition for the suggested word
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                               renderer, mappedInput, suggestions[selectedIndex], cachePath, UI_12_FONT_ID),
                           [this](const ActivityResult& result) { requestUpdate(); });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    setResult(std::move(result));
    finish();
  }

  if (needsUpdate) requestUpdate();
}

void DictionarySuggestionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int margin = 20;
  int currentY = margin;

  if (isLoading) {
    std::string loadingStr = std::string(tr(STR_FINDING_SUGGESTIONS)) + " '" + targetWord + "'...";
    renderer.drawText(UI_12_FONT_ID, margin, currentY, loadingStr.c_str());
    renderer.displayBuffer();
    return;
  }

  // Draw Header
  std::string titleStr = std::string(tr(STR_DID_YOU_MEAN)) + " (" + targetWord + ")";
  renderer.drawText(UI_12_FONT_ID, margin, currentY, titleStr.c_str(), EpdFontFamily::BOLD);
  int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.fillRect(margin, currentY + titleH + 4, renderer.getScreenWidth() - (margin * 2), 3, true);

  currentY += titleH + 15;

  if (suggestions.empty()) {
    renderer.drawText(UI_12_FONT_ID, margin, currentY + 20, tr(STR_NO_SIMILAR_WORDS));
  } else {
    int lineHeight = renderer.getLineHeight(UI_12_FONT_ID) + 10;

    for (int i = 0; i < static_cast<int>(suggestions.size()); ++i) {
      bool isSelected = (i == selectedIndex);

      if (isSelected) {
        renderer.fillRect(margin - 5, currentY - 2, renderer.getScreenWidth() - (margin * 2) + 10, lineHeight, true);
      }

      renderer.drawText(UI_12_FONT_ID, margin, currentY, suggestions[i].c_str(), !isSelected);
      currentY += lineHeight;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_UP_DOWN), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}