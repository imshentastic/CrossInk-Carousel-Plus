#include "ClockOffsetActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint8_t MAX_POS_HOURS = 14;
constexpr uint8_t MAX_NEG_HOURS = 12;
constexpr uint8_t MINUTE_STEPS = 4;  // 0, 15, 30, 45
constexpr uint8_t MINUTES_PER_QUARTER = 15;
constexpr uint8_t BIAS_QUARTER_HOURS = 48;  // 0 stored = UTC-12, 48 stored = UTC+0

// Convert a (sign, hours, quarter) triple into the biased storage value.
// Returns a value in [0, 104].
uint8_t encodeOffset(uint8_t sign, uint8_t hours, uint8_t quarter) {
  int signedQuarter = static_cast<int>(hours) * 4 + static_cast<int>(quarter);
  if (sign == 1) signedQuarter = -signedQuarter;
  int biased = signedQuarter + BIAS_QUARTER_HOURS;
  if (biased < 0) biased = 0;
  if (biased > 104) biased = 104;
  return static_cast<uint8_t>(biased);
}

// Decompose the biased storage value into (sign, hours, quarter).
void decodeOffset(uint8_t biased, uint8_t& sign, uint8_t& hours, uint8_t& quarter) {
  if (biased > 104) biased = BIAS_QUARTER_HOURS;
  int signedQuarter = static_cast<int>(biased) - BIAS_QUARTER_HOURS;
  if (signedQuarter < 0) {
    sign = 1;
    signedQuarter = -signedQuarter;
  } else {
    sign = 0;
  }
  hours = static_cast<uint8_t>(signedQuarter / 4);
  quarter = static_cast<uint8_t>(signedQuarter % 4);
}
}  // namespace

void ClockOffsetActivity::onEnter() {
  Activity::onEnter();
  loadFromSettings();
  // CrumBLE 4.4: start at the sign field so western-hemisphere users see
  // the +/- caret immediately and can flip to negative without first
  // discovering that "Next Field" cycles all three positions. Previously
  // started at FIELD_HOURS, which made it look like only positive
  // offsets (0..+14) were available unless you pressed Next Field twice.
  activeField = FIELD_SIGN;
  requestUpdate();
}

void ClockOffsetActivity::onExit() {
  saveToSettings();
  Activity::onExit();
}

void ClockOffsetActivity::loadFromSettings() {
  decodeOffset(SETTINGS.clockUtcOffsetQ, sign, hours, minutesQuarter);
  clampForSign();
}

void ClockOffsetActivity::saveToSettings() const {
  const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
  if (encoded == SETTINGS.clockUtcOffsetQ) return;
  SETTINGS.clockUtcOffsetQ = encoded;
  SETTINGS.saveToFile();
}

void ClockOffsetActivity::clampForSign() {
  const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
  if (hours > maxHours) hours = maxHours;
  // At the absolute boundary (-12:00 or +14:00) only :00 is valid.
  if (hours == maxHours && minutesQuarter != 0) {
    minutesQuarter = 0;
  }
}

void ClockOffsetActivity::adjustActiveField(int delta) {
  switch (activeField) {
    case FIELD_SIGN: {
      sign = static_cast<uint8_t>((sign + 1) % 2);
      clampForSign();
      break;
    }
    case FIELD_HOURS: {
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      const int next = (static_cast<int>(hours) + delta + (maxHours + 1)) % (maxHours + 1);
      hours = static_cast<uint8_t>(next);
      clampForSign();
      break;
    }
    case FIELD_MINUTES: {
      // At the boundary hour, lock minutes to :00.
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      if (hours == maxHours) {
        minutesQuarter = 0;
        break;
      }
      const int next = (static_cast<int>(minutesQuarter) + delta + MINUTE_STEPS) % MINUTE_STEPS;
      minutesQuarter = static_cast<uint8_t>(next);
      break;
    }
    default:
      break;
  }
}

void ClockOffsetActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activeField = static_cast<Field>((activeField + 1) % FIELD_COUNT);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
}

void ClockOffsetActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_UTC_OFFSET));

  // CrumBLE 4.5: render each segment with its own drawText call, tracking
  // the running X. The focus-box X for any field MUST equal the X where
  // that segment was drawn -- measuring prefix substrings of a single
  // composite string can drift by a pixel or two due to kerning with the
  // following character, which shifted the sign/hours frames so the
  // character appeared right-justified inside them. With per-segment
  // drawText + per-segment X tracking, the frame and the glyph use the
  // identical coordinate so the character always sits centred.
  auto widthOf = [&](const char* s) {
    return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD);
  };

  const char* signStr = sign == 1 ? "-" : "+";
  char hoursStr[8];
  snprintf(hoursStr, sizeof(hoursStr), "%d", hours);
  char minutesStr[8];
  snprintf(minutesStr, sizeof(minutesStr), "%02d", minutesQuarter * MINUTES_PER_QUARTER);

  const int utcW = widthOf("UTC ");
  const int signW = widthOf(signStr);
  const int gapW = widthOf(" ");
  const int hoursW = widthOf(hoursStr);
  const int colonW = widthOf(":");
  const int minutesW = widthOf(minutesStr);
  const int totalWidth = utcW + signW + gapW + hoursW + colonW + minutesW;

  const int centreY = pageHeight / 2 - 40;
  int x = (pageWidth - totalWidth) / 2;

  renderer.drawText(UI_12_FONT_ID, x, centreY, "UTC ", true, EpdFontFamily::BOLD);
  x += utcW;
  const int signX = x;
  renderer.drawText(UI_12_FONT_ID, x, centreY, signStr, true, EpdFontFamily::BOLD);
  x += signW;
  renderer.drawText(UI_12_FONT_ID, x, centreY, " ", true, EpdFontFamily::BOLD);
  x += gapW;
  const int hoursX = x;
  renderer.drawText(UI_12_FONT_ID, x, centreY, hoursStr, true, EpdFontFamily::BOLD);
  x += hoursW;
  renderer.drawText(UI_12_FONT_ID, x, centreY, ":", true, EpdFontFamily::BOLD);
  x += colonW;
  const int minutesX = x;
  renderer.drawText(UI_12_FONT_ID, x, centreY, minutesStr, true, EpdFontFamily::BOLD);

  int caretX = 0;
  int caretW = 0;
  switch (activeField) {
    case FIELD_SIGN:
      caretX = signX;
      caretW = signW;
      break;
    case FIELD_HOURS:
      caretX = hoursX;
      caretW = hoursW;
      break;
    case FIELD_MINUTES:
      caretX = minutesX;
      caretW = minutesW;
      break;
    default:
      break;
  }
  // CrumBLE 4.4: dotted box around the active field. The previous design
  // was a short underline at centreY+10 which (with this font) landed
  // partway up the glyph rather than below it, making it look like a
  // mid-character strikethrough. A dotted rectangle wraps the whole
  // character unambiguously regardless of glyph metrics.
  const int boxLineH = renderer.getLineHeight(UI_12_FONT_ID);
  constexpr int kBoxPadX = 4;
  constexpr int kBoxPadY = 3;
  constexpr int kDashThickness = 2;  // 2px-thick edges so the box reads from arm's length
  const int boxLeft = caretX - kBoxPadX;
  const int boxRight = caretX + caretW + kBoxPadX;
  const int boxTop = centreY - kBoxPadY;
  const int boxBottom = centreY + boxLineH + kBoxPadY;
  // 3-on / 2-off dashed pattern, 2px thick. Slightly longer dashes than
  // before so the box reads as a frame from arm's length on e-ink.
  constexpr int kDashOn = 3;
  constexpr int kDashStep = 5;  // 3 on + 2 off
  for (int x = boxLeft; x <= boxRight; x += kDashStep) {
    const int x2 = std::min(x + kDashOn - 1, boxRight);
    renderer.fillRect(x, boxTop, x2 - x + 1, kDashThickness, true);
    renderer.fillRect(x, boxBottom - (kDashThickness - 1), x2 - x + 1, kDashThickness, true);
  }
  for (int y = boxTop; y <= boxBottom; y += kDashStep) {
    const int y2 = std::min(y + kDashOn - 1, boxBottom);
    renderer.fillRect(boxLeft, y, kDashThickness, y2 - y + 1, true);
    renderer.fillRect(boxRight - (kDashThickness - 1), y, kDashThickness, y2 - y + 1, true);
  }

  // Live preview of the resulting wall-clock time, so users can verify against a watch.
  if (halClock.isAvailable()) {
    char timeBuf[9];
    const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), encoded, SETTINGS.clockFormat == 1)) {
      char preview[24];
      snprintf(preview, sizeof(preview), "%s %s", tr(STR_CURRENT_TIME), timeBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, centreY + 60, preview);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_FIELD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
