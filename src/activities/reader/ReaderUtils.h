#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "../../SilentRestart.h"  // CrumBLE 4.4: skip HALF refresh on first paint post-silent-reboot
#include "../../components/UITheme.h"
#include "../../fontIds.h"
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

// v18.9.9.167: status-bar ghost mitigation (ported from crosspoint 204d7d5).
// When the status bar carries dynamic content (page counter, progress %, bar),
// a plain FAST_REFRESH cycle leaves visible ghosting of the previous values.
// These helpers let renderContents actively drive the status-bar band white
// before the fast refresh, then repaint it fresh.
inline bool hasDynamicStatusBarContent() {
  // v18.9.9.205: clock + time-left added — both change across page turns
  // and ghost under FAST refresh exactly like the page counter.
  return SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
         SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS ||
         SETTINGS.statusBarClock || SETTINGS.statusBarTimeLeft;
}

inline bool shouldPreclearStatusBarBeforeFastRefresh(int pagesUntilFullRefresh) {
  return hasDynamicStatusBarContent() && pagesUntilFullRefresh > 1;
}

inline void clearStatusBarBand(const GfxRenderer& renderer, int orientedMarginBottom, int paddingBottom = 0) {
  (void)orientedMarginBottom;
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }
  // v18.9.9.207: compute the band top from the same BASE margins that
  // BaseTheme::drawStatusBar positions with (getOrientedViewableTRBL).
  // Callers used to pass the reader's INFLATED orientedMarginBottom —
  // which already includes the max(screenMargin, statusBar+3) text
  // reserve — so the clear started a full reserve ABOVE the actual
  // status row and white-flashed the last line of body text on every
  // preclear. drawStatusBar draws its text at
  //   screenH - statusBarHeight - baseMarginBottom - padding - 4
  // so that exact y is the band's top edge; everything the status bar
  // paints sits at or below it.
  int baseTop, baseRight, baseBottom, baseLeft;
  renderer.getOrientedViewableTRBL(&baseTop, &baseRight, &baseBottom, &baseLeft);
  int clearY = renderer.getScreenHeight() - baseBottom - paddingBottom - statusBarHeight - 4;
  if (clearY < 0) {
    clearY = 0;
  }
  // v18.9.9.209: clear only the RIGHT-HAND zone, not the full width. The
  // per-page-turn dynamic text (page counter, percent, time-left, clock) is
  // right aligned; the title and progress bar don't ghost the way small
  // glyphs do, and whiting the whole strip read as a full-width flash.
  // 4.7.1: zone width now mirrors drawStatusBar's own layout math instead
  // of a flat 40% of screen -- the guess cut through the middle of the
  // dynamic block when the clock was on, so the clock and leading page
  // digits sat outside the zone and stayed ghosted (blurry) between HALF
  // refreshes. Worst-case progress ref matches BaseTheme's preclear; the
  // clock reserve matches its 10 px gap.
  const int screenW = renderer.getScreenWidth();
  const char* progressRef =
      SETTINGS.statusBarTimeLeft ? "9999/9999  100%  99h 59m" : "9999/9999  100%";
  int zoneW = UITheme::getInstance().getMetrics().statusBarHorizontalMargin + baseRight +
              renderer.getTextWidth(SMALL_FONT_ID, progressRef);
  if (SETTINGS.statusBarClock) {
    zoneW += 10 + renderer.getTextWidth(SMALL_FONT_ID, "12:59 PM");
  }
  zoneW += 8;  // ghost bleed slack
  if (zoneW < screenW * 2 / 5) zoneW = screenW * 2 / 5;
  if (zoneW > screenW * 7 / 10) zoneW = screenW * 7 / 10;
  const int zoneX = screenW - zoneW;
  renderer.fillRect(zoneX, clearY, zoneW, renderer.getScreenHeight() - clearY, false);
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
