#include "PrebakeManifestViewerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Friendly-name maps. The PrebakeManifest stores raw enum/int values from
// CrossPointSettings; here we translate them into human-readable strings
// the user can compare against the in-reader settings drawer at a glance.
// Falls back to the raw integer when the value is out of expected range
// (corrupt manifest, schema drift, etc.) so the viewer never lies about
// what's on disk.

const char* fontFamilyName(uint8_t family) {
  switch (family) {
    case CrossPointSettings::LEXENDDECA:
      return "Lexend Deca";
    case CrossPointSettings::BITTER:
      return "Bitter";
    case CrossPointSettings::CHAREINK:
      return "CharEink";
    default:
      return "Unknown";
  }
}

const char* orientationName(uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::PORTRAIT:
      return "Portrait";
    case CrossPointSettings::LANDSCAPE_CW:
      return "Landscape CW";
    case CrossPointSettings::INVERTED:
      return "Portrait inverted";
    case CrossPointSettings::LANDSCAPE_CCW:
      return "Landscape CCW";
    default:
      return "Unknown";
  }
}

const char* paragraphAlignName(uint8_t v) {
  // Mirrors CrossPointSettings::PARAGRAPH_ALIGNMENT. Order kept loose so
  // future additions don't shift the existing labels.
  switch (v) {
    case 0:
      return "Left";
    case 1:
      return "Justified";
    case 2:
      return "Right";
    case 3:
      return "Center";
    default:
      return "Unknown";
  }
}

const char* imageRenderingName(uint8_t v) {
  // Mirrors CrossPointSettings::IMAGE_RENDERING.
  switch (v) {
    case 0:
      return "B/W";
    case 1:
      return "Grayscale";
    case 2:
      return "Skip";
    default:
      return "Unknown";
  }
}

const char* onOff(bool v) { return v ? "On" : "Off"; }

struct Row {
  std::string label;
  std::string value;
  // CrumBLE 4.2: optional second line, drawn below `value` with no
  // label. Right-justified like the primary value so the eye follows
  // one continuous column of values down the page. Used for rows whose
  // value doesn't fit on a single line (notably Font when an SD family
  // name + step + range together overflow the value column budget).
  std::string valueLine2;
};

}  // namespace

PrebakeManifestViewerActivity::PrebakeManifestViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string bookTitle, PrebakeManifest manifest)
    : Activity("PrebakeManifestViewer", renderer, mappedInput),
      bookTitle_(std::move(bookTitle)),
      manifest_(std::move(manifest)) {}

void PrebakeManifestViewerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PrebakeManifestViewerActivity::loop() {
  // Only Back exits. Confirm/Up/Down intentionally do nothing -- this is
  // a static read-only viewer. The label hint at the bottom reads "Back"
  // only on the relevant button (others get empty labels via mapLabels).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void PrebakeManifestViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int sidePad = metrics.contentSidePadding;

  // Header: book title (truncated to fit, bold).
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");
  constexpr int kTitleFontId = UI_10_FONT_ID;
  const int titleY = metrics.topPadding + (metrics.headerHeight > 60 ? metrics.batteryBarHeight + 3 : 14);
  const int titleBudget = std::max(0, pageWidth - sidePad * 2 - 90);
  const std::string headerTitle = renderer.truncatedText(kTitleFontId, bookTitle_.c_str(), titleBudget);
  renderer.drawText(kTitleFontId, sidePad, titleY, headerTitle.c_str(), true, EpdFontFamily::BOLD);

  // Sub-header label so the user knows what they're looking at.
  constexpr int kSubFontId = UI_10_FONT_ID;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int y = contentTop;
  renderer.drawText(kSubFontId, sidePad, y, "Prepared layout settings", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(kSubFontId) + 4;

  // Build the row list. Order picked to mirror the reader's settings
  // drawer: typography first (what the user thinks of as the "look"),
  // then layout toggles, then geometry, then derived bookkeeping (the
  // fontId hash, viewport in pixels).
  std::vector<Row> rows;
  rows.reserve(16);

  // Font: family name on the primary value line, (step / range) on the
  // continuation line. Splitting into two values is cleaner than trying
  // to fit "SD: <long family name> (step N, range M)" into one column --
  // SD-card family names are user-supplied and can be long (a Lexend-Deca
  // re-bake hits 12+ chars before any parens). Two short lines beat one
  // truncated one.
  std::string fontLine1;
  if (manifest_.sdFontFamilyName[0] != '\0') {
    fontLine1 = std::string("SD: ") + manifest_.sdFontFamilyName;
  } else {
    fontLine1 = fontFamilyName(manifest_.fontFamily);
  }
  char fontLine2[32];
  std::snprintf(fontLine2, sizeof(fontLine2), "step %u / range %u", static_cast<unsigned>(manifest_.fontSize),
                static_cast<unsigned>(manifest_.sdFontSizeRange));
  rows.push_back({"Font", fontLine1, fontLine2});

  rows.push_back({"Orientation", orientationName(manifest_.orientation)});
  rows.push_back({"Screen margin", std::to_string(manifest_.screenMargin) + " px"});

  char lineSpBuf[16];
  std::snprintf(lineSpBuf, sizeof(lineSpBuf), "%u", static_cast<unsigned>(manifest_.lineSpacing));
  rows.push_back({"Line spacing", lineSpBuf});

  char compBuf[16];
  std::snprintf(compBuf, sizeof(compBuf), "%.2f", static_cast<double>(manifest_.lineCompression));
  rows.push_back({"Line compression", compBuf});

  rows.push_back({"Paragraph align", paragraphAlignName(manifest_.paragraphAlignment)});
  rows.push_back({"Extra para spacing", onOff(manifest_.extraParagraphSpacing)});
  rows.push_back({"Paragraph indents", onOff(manifest_.forceParagraphIndents)});
  rows.push_back({"Hyphenation", onOff(manifest_.hyphenationEnabled)});
  rows.push_back({"Embedded style", onOff(manifest_.embeddedStyle)});
  rows.push_back({"Bionic reading", onOff(manifest_.bionicReadingEnabled)});
  rows.push_back({"Guide reading", onOff(manifest_.guideReadingEnabled)});
  rows.push_back({"Image rendering", imageRenderingName(manifest_.imageRendering)});

  char viewportBuf[24];
  std::snprintf(viewportBuf, sizeof(viewportBuf), "%u x %u", static_cast<unsigned>(manifest_.viewportWidth),
                static_cast<unsigned>(manifest_.viewportHeight));
  rows.push_back({"Viewport", viewportBuf});

  char fontIdBuf[24];
  std::snprintf(fontIdBuf, sizeof(fontIdBuf), "%ld", static_cast<long>(manifest_.fontId));
  rows.push_back({"Font ID hash", fontIdBuf});

  // Render rows. Label column on the left, value column right-justified.
  // Tight line height so the whole list fits without scrolling on both
  // X4 (480 tall) and X3 (792 tall). If we ever grow past the screen,
  // add a scroll cursor; for now 15 rows × ~16 px = 240 px is comfortably
  // under either panel.
  constexpr int kRowFontId = UI_10_FONT_ID;
  const int rowHeight = renderer.getLineHeight(kRowFontId);
  const int labelX = sidePad;
  const int valueRightX = pageWidth - sidePad;
  for (const auto& row : rows) {
    const int rowTotalHeight = rowHeight * (row.valueLine2.empty() ? 1 : 2);
    if (y + rowTotalHeight > pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing) break;
    renderer.drawText(kRowFontId, labelX, y, row.label.c_str(), true, EpdFontFamily::REGULAR);
    const std::string value = renderer.truncatedText(kRowFontId, row.value.c_str(), pageWidth / 2);
    const int vw = renderer.getTextWidth(kRowFontId, value.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(kRowFontId, valueRightX - vw, y, value.c_str(), true, EpdFontFamily::REGULAR);
    if (!row.valueLine2.empty()) {
      // Continuation line: no label, right-justified value beneath the
      // primary value so the column reads cleanly top-to-bottom.
      const std::string value2 = renderer.truncatedText(kRowFontId, row.valueLine2.c_str(), pageWidth / 2);
      const int vw2 = renderer.getTextWidth(kRowFontId, value2.c_str(), EpdFontFamily::REGULAR);
      renderer.drawText(kRowFontId, valueRightX - vw2, y + rowHeight, value2.c_str(), true, EpdFontFamily::REGULAR);
    }
    y += rowTotalHeight;
  }

  // Button hints: only Back is meaningful. mapLabels gets the empty
  // strings for the other slots so the device-button mapping picks up
  // the right physical button label.
  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}
