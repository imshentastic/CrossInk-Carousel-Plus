#include "DictionaryIndexBuildActivity.h"

#include <I18n.h>

#include "fontIds.h"
#include "util/Dictionary.h"

DictionaryIndexBuildActivity::DictionaryIndexBuildActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("DictionaryIndexBuild", renderer, mappedInput) {}

void DictionaryIndexBuildActivity::onEnter() {
  Activity::onEnter();
  // requestUpdateAndWait blocks until the render task physically paints
  // the "Building..." message. requestUpdate() alone only sets a flag,
  // and the render task can't service it because the activity task is
  // about to be tied up in buildIndex() for the full scan duration. The
  // first attempt used requestUpdate(); the user saw the prompt screen
  // remain visible the whole 20s before the device crashed -- the
  // transition to this activity never repainted.
  requestUpdateAndWait();
  buildIndex();
}

void DictionaryIndexBuildActivity::buildIndex() {
  // Synchronous. The eink holds the "Building..." message painted by
  // the requestUpdateAndWait() pass in onEnter for the whole scan; we
  // don't try to animate a percent counter because eink redraws are
  // slow enough on this panel to noticeably extend the wait.
  const bool ok = Dictionary::loadIndex();

  ActivityResult result;
  result.isCancelled = !ok;
  setResult(std::move(result));
  finish();
}

void DictionaryIndexBuildActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int margin = 20;
  int y = margin;
  renderer.drawText(UI_12_FONT_ID, margin, y, tr(STR_DICT_INDEX_BUILDING));
  y += renderer.getLineHeight(UI_12_FONT_ID) * 2;
  renderer.drawText(UI_12_FONT_ID, margin, y, tr(STR_DICT_INDEX_BUILDING_HINT));
}
