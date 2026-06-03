#include "DictionaryIndexBuildActivity.h"

#include <I18n.h>

#include "fontIds.h"
#include "util/Dictionary.h"

DictionaryIndexBuildActivity::DictionaryIndexBuildActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("DictionaryIndexBuild", renderer, mappedInput) {}

void DictionaryIndexBuildActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
  buildIndex();
}

void DictionaryIndexBuildActivity::buildIndex() {
  // Synchronous. The eink will hold the "Building..." message painted by
  // the render() pass above for the ~5-10s the scan takes; we don't try
  // to animate a percent counter because partial-screen redraws on this
  // panel are slow enough to noticeably extend the wait.
  const bool ok = Dictionary::loadIndex();

  ActivityResult result;
  result.isCancelled = !ok;
  setResult(std::move(result));
  finish();
}

void DictionaryIndexBuildActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int margin = 20;
  renderer.drawText(UI_12_FONT_ID, margin, margin, tr(STR_DICT_INDEX_BUILDING));
}
