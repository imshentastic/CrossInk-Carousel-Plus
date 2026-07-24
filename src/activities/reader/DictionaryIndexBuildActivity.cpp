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
  // v18.9.9.237: throttled progress-bar redraw. onProgress fires very
  // often during the scan; each requestUpdateAndWait blocks the scan
  // for one eink refresh (~500 ms) so we cap redraws at ~2 s intervals.
  // A 20 s scan gets ~10 redraws (~5 s overhead, 25% penalty). A 3.5 min
  // scan gets ~100 redraws (~50 s overhead, 24% penalty). The progress
  // bar makes each redraw useful (visible % movement) instead of the
  // v236 dot cycle which read as "still stuck at the same 3 dots" for
  // long scans.
  const bool ok = Dictionary::loadIndex([this](int percent) {
    const uint32_t now = millis();
    if (percent > percent_) percent_ = percent;  // always advance; never snap back
    if (now - lastRedrawMs_ < 2000) return;
    lastRedrawMs_ = now;
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
  // v18.9.9.268: wrap the hint text -- "Please do not power off the
  // device. Larger dictionaries take longer." doesn't fit on one line
  // at UI_12 on the X3 panel, so the tail got clipped mid-word. Use
  // wrappedText + a short line loop so the whole message renders.
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int maxTextWidth = renderer.getScreenWidth() - 2 * margin;
  const auto hintLines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_DICT_INDEX_BUILDING_HINT),
                                                maxTextWidth, /*maxLines=*/4);
  for (const auto& line : hintLines) {
    renderer.drawText(UI_12_FONT_ID, margin, y, line.c_str());
    y += lineH;
  }
  y += lineH;  // spacer before progress bar (was * 2 previously)

  // v18.9.9.237: progress bar. Outline rect + filled inner rect
  // proportional to percent_. Percent text drawn below the bar.
  const int barX = margin;
  const int barY = y;
  const int barW = renderer.getScreenWidth() - 2 * margin;
  const int barH = 16;
  // 2-pixel outline: draw four filled strips instead of a stroked
  // rectangle (matches the style used elsewhere in the codebase).
  renderer.fillRect(barX, barY, barW, 2, true);                     // top
  renderer.fillRect(barX, barY + barH - 2, barW, 2, true);          // bottom
  renderer.fillRect(barX, barY, 2, barH, true);                     // left
  renderer.fillRect(barX + barW - 2, barY, 2, barH, true);          // right
  const int inset = 3;
  const int innerW = barW - 2 * inset;
  const int fillW = (innerW * percent_) / 100;
  if (fillW > 0) renderer.fillRect(barX + inset, barY + inset, fillW, barH - 2 * inset, true);

  char pctText[8];
  snprintf(pctText, sizeof(pctText), "%d%%", percent_);
  renderer.drawText(UI_12_FONT_ID, margin, barY + barH + 6, pctText);

  // v18.9.9.255: push the framebuffer to the panel. Without this the
  // "Building..." screen and progress bar exist only in RAM -- the
  // prompt screen stays visible until Dictionary::loadIndex returns
  // and the next activity (word-select) paints. FAST_REFRESH suits
  // this transient screen; the progress redraws every ~2 s from
  // buildIndex()'s throttled onProgress callback, and the next
  // activity's own paint handles the transition out.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
