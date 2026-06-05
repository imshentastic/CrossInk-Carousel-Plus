#include "DictionaryIndexBuildActivity.h"

#include <Arduino.h>  // millis() for the redraw throttle
#include <I18n.h>

#include <string>

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
  lastRedrawMs_ = millis();
  requestUpdateAndWait();
  buildIndex();
}

void DictionaryIndexBuildActivity::buildIndex() {
  // CrumBLE 4.2: feed an animated dot row via the onProgress callback.
  // Throttled to >=2.5 s between redraws because each requestUpdateAndWait
  // blocks the scan for one eink refresh (~500 ms). At that cadence a 20 s
  // scan picks up ~7 redraws (~3.5 s overhead) -- enough to communicate
  // "still alive" without doubling the wait. Earlier attempts updated on
  // every onProgress (every SPARSE_INTERVAL = 512 words = ~200 ticks for a
  // 100K-word dict) and dragged the scan past 60 s.
  const bool ok = Dictionary::loadIndex([this](int /*percent*/) {
    const uint32_t now = millis();
    if (now - lastRedrawMs_ < 2500) return;
    lastRedrawMs_ = now;
    dotCount_ = (dotCount_ % 4) + 1;
    requestUpdateAndWait();
  });

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
  // Animated dot beacon. Two blank rows below the hint so it reads as a
  // separate progress marker, not a continuation. dotCount_ cycles 1..4
  // each time the throttle in buildIndex() fires.
  y += renderer.getLineHeight(UI_12_FONT_ID) * 2;
  const std::string dots(dotCount_, '.');
  renderer.drawText(UI_12_FONT_ID, margin, y, dots.c_str());
}
