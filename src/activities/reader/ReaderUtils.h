#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "../../SilentRestart.h"  // CrumBLE 4.4: skip HALF refresh on first paint post-silent-reboot
#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;

// CrumBLE 4.4: reader dark mode (selective inversion). When enabled, the
// reader page is painted on a black background with white text/UI; EPUB
// content images render right-side up (no negative-photo effect). These
// helpers centralise the SETTINGS lookup so callers don't sprinkle
// SETTINGS.readerDarkMode reads through the draw pipeline.
inline bool readerDarkModeEnabled() { return SETTINGS.readerDarkMode != 0; }
inline uint8_t readerBackgroundColor() { return readerDarkModeEnabled() ? 0x00 : 0xFF; }
inline bool readerForegroundBlack() { return !readerDarkModeEnabled(); }

inline GfxRenderer::Orientation toRendererOrientation(const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      return GfxRenderer::Orientation::Portrait;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case CrossPointSettings::ORIENTATION::INVERTED:
      return GfxRenderer::Orientation::PortraitInverted;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  renderer.setOrientation(toRendererOrientation(orientation));
}

inline uint8_t rotatedOrientation(const uint8_t orientation, const bool clockwise) {
  return clockwise ? (orientation + 1) % CrossPointSettings::ORIENTATION_COUNT
                   : (orientation + CrossPointSettings::ORIENTATION_COUNT - 1) % CrossPointSettings::ORIENTATION_COUNT;
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromSideBtn;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  // Side buttons fire on press only when long-press action is OFF (nothing to detect).
  const bool sideUsePress = SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_OFF;

  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool sidePrev = sideUsePress ? input.wasPressed(MappedInputManager::Button::PageBack)
                                     : input.wasReleased(MappedInputManager::Button::PageBack);
  const bool sideNext = sideUsePress ? input.wasPressed(MappedInputManager::Button::PageForward)
                                     : input.wasReleased(MappedInputManager::Button::PageForward);

  const bool frontPrev = input.wasReleased(MappedInputManager::Button::Left);
  const bool powerReleased = input.wasReleased(MappedInputManager::Button::Power);
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              input.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             input.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  const bool powerTurn = shortPowerTurn || longPowerTurn;
  const bool frontNext = input.wasReleased(MappedInputManager::Button::Right) || powerTurn;

  // fromSideBtn is true when only side buttons contributed to this page turn.
  const bool fromSide = (sidePrev || sideNext) && !(frontPrev || frontNext);
  return {tiltPrev || sidePrev || frontPrev, tiltNext || sideNext || frontNext, fromSide, tiltPrev || tiltNext};
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  // CrumBLE 4.4 post-bisect: on the first paint after a silent restart,
  // skip the HALF refresh's panel-cycling flash. The panel is still
  // holding the user's pre-restart frame (because we didn't paint
  // anything before ESP.restart), so a FAST update transitions smoothly
  // without a visible black/white cycle. Don't touch pagesUntilFullRefresh
  // here so the normal HALF cadence resumes once the activity's
  // pre-flight has cleared the continuation flag.
  if (isContinuingFromSilentReboot()) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
