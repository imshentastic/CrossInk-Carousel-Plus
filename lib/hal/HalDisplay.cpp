#include <HalDisplay.h>
#include <HalGPIO.h>
#include <XteinkDetect.h>

#include "HalSpiBus.h"
#include "Logging.h"

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  HalSpiBus::Lock spiLock;

  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  // No panel-controller probe here: bit-banging the display bus at this point
  // hung X4 boot (SD shares SCLK/MOSI and is already mounted). See setup().
  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

// v18.9.9.433: framebuffer null-guards. releaseFrameBuffers() (used during
// active WS upload to reclaim ~52 KB) leaves einkDisplay.getFrameBuffer()
// == nullptr. Every buffer write and every panel op would deref that
// pointer and crash. Guards make them no-op instead; the e-ink panel keeps
// showing whatever was last painted, which is the whole point of releasing.
void HalDisplay::clearScreen(uint8_t color) const {
  if (!einkDisplay.getFrameBuffer()) return;
  einkDisplay.clearScreen(color);
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  if (!einkDisplay.getFrameBuffer()) return;
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  if (!einkDisplay.getFrameBuffer()) return;
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
    case HalDisplay::HALF_REFRESH_DEEP:
      // Both half modes share the same underlying waveform; DEEP is just a
      // hint to the X3 resync logic above, transparent to the SDK driver.
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (!einkDisplay.getFrameBuffer()) return;  // v18.9.9.433 (see clearScreen)
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3()) {
    // CrumBLE 4.4: scope the deep (2-cycle) resync to HALF_REFRESH_DEEP only.
    // Original behaviour was a single resync on every HALF_REFRESH; we
    // briefly bumped it to 2 to fix the reader->home polarity drift, but
    // that penalised every sleep refresh on X3 too (~770ms each). Now
    // sleep + most callers pay the original 1 resync; HomeActivity opts
    // into the deeper scrub via HALF_REFRESH_DEEP. X4 is unaffected
    // either way (the resync is X3-only).
    if (mode == RefreshMode::HALF_REFRESH_DEEP) {
      einkDisplay.requestResync(2);
    } else if (mode == RefreshMode::HALF_REFRESH) {
      einkDisplay.requestResync(1);
    }
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::displayBufferRegion(uint16_t x, uint16_t y, uint16_t w,
                                     uint16_t h, RefreshMode mode) {
  if (!einkDisplay.getFrameBuffer()) return;  // v18.9.9.433 (see clearScreen)
  // Clamp to display bounds. Caller may pass slightly-overshooting rects
  // (e.g. from a dirty-region union that ran past the edge); rather than
  // failing or asserting, accept and trim.
  if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
    return;  // entirely out of bounds -- no-op
  }
  if (x + w > DISPLAY_WIDTH) w = DISPLAY_WIDTH - x;
  if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
  if (w == 0 || h == 0) return;

  // Auto-fallback: a region covering >= 70% of the screen is faster as a
  // full refresh than a windowed update (the controller's setRamArea
  // sequence has fixed-cost overhead worth ~12-15ms; below the threshold
  // the partial waveform wins, above it the full waveform wins). 70%
  // chosen as the breakeven point on SSD1677; can be tuned per-panel.
  constexpr uint32_t kFullRefreshThresholdPct = 70;
  const uint32_t regionArea = static_cast<uint32_t>(w) * h;
  const uint32_t fullArea = static_cast<uint32_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT;
  if (regionArea * 100 >= fullArea * kFullRefreshThresholdPct) {
    displayBuffer(mode, /*turnOffScreen=*/false);
    return;
  }

  // 4.5.5 L1 v2.1: route to the SDK's existing displayWindow() primitive
  // (was marked EXPERIMENTAL but is complete). It uses setRamArea + windowed
  // BW write + FAST_REFRESH + windowed RED sync -- saves ~5-15 ms on SPI
  // (less data transferred) while still triggering the full 0x1C FAST
  // waveform (~417 ms refresh time). Total per-nav savings: ~10 ms.
  //
  // The dramatic 7x speedup (417 ms -> ~60 ms refresh time) requires a
  // partial-mode LUT specific to the panel waveform -- not in this build;
  // future v2.2 work once we have safe LUT data.
  //
  // Constraints inherited from displayWindow:
  //   - Only FAST_REFRESH supported as windowed. HALF / FULL fall back to
  //     full buffer (they're for big transitions anyway, partial doesn't
  //     fit the use case).
  //   - x and w must be byte-aligned (multiples of 8 pixels) -- round x
  //     down + w up to nearest 8. Worst case: window grows by ~14 pixels
  //     horizontally, still way under the 70% full-refresh threshold.
  //   - X3 panels: fall back to full (displayWindow is X4-only; X3 has
  //     its own differential-mode path already in displayBuffer).
  if (mode != RefreshMode::FAST_REFRESH) {
    displayBuffer(mode, /*turnOffScreen=*/false);
    return;
  }
  if (gpio.deviceIsX3()) {
    displayBuffer(mode, /*turnOffScreen=*/false);
    return;
  }

  // Byte-align x and w (pixels-per-byte = 8 in 1bpp framebuffer)
  const uint16_t xEnd = x + w;
  const uint16_t xAligned = x & ~uint16_t(7);
  const uint16_t xEndAligned = (xEnd + 7) & ~uint16_t(7);
  uint16_t wAligned = xEndAligned - xAligned;
  // Re-clamp after alignment in case xEndAligned overshot
  if (xAligned + wAligned > DISPLAY_WIDTH) {
    wAligned = DISPLAY_WIDTH - xAligned;
  }

  einkDisplay.displayWindow(xAligned, y, wAligned, h, /*turnOffScreen=*/false);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (!einkDisplay.getFrameBuffer()) return;  // v18.9.9.433 (see clearScreen)
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3()) {
    // CrumBLE 4.4: scope the deep (2-cycle) resync to HALF_REFRESH_DEEP only.
    // Original behaviour was a single resync on every HALF_REFRESH; we
    // briefly bumped it to 2 to fix the reader->home polarity drift, but
    // that penalised every sleep refresh on X3 too (~770ms each). Now
    // sleep + most callers pay the original 1 resync; HomeActivity opts
    // into the deeper scrub via HALF_REFRESH_DEEP. X4 is unaffected
    // either way (the resync is X3-only).
    if (mode == RefreshMode::HALF_REFRESH_DEEP) {
      einkDisplay.requestResync(2);
    } else if (mode == RefreshMode::HALF_REFRESH) {
      einkDisplay.requestResync(1);
    }
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() {
  HalSpiBus::Lock spiLock;
  einkDisplay.deepSleep();
}

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::releaseFrameBuffers() { einkDisplay.releaseBuffers(); }

bool HalDisplay::reallocFrameBuffers() { return einkDisplay.reallocBuffers(); }

// v18.9.9.432: secondary-only wrappers.
void HalDisplay::releaseSecondaryFrameBuffer() {
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  einkDisplay.releaseSecondaryBuffer();
#endif
}

bool HalDisplay::reallocSecondaryFrameBuffer() {
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  return einkDisplay.reallocSecondaryBuffer();
#else
  return true;
#endif
}

bool HalDisplay::hasSecondaryFrameBuffer() const {
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  return einkDisplay.hasSecondaryBuffer();
#else
  return false;
#endif
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  if (!einkDisplay.getFrameBuffer()) return;  // v18.9.9.433 (see clearScreen)
  HalSpiBus::Lock spiLock;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

void HalDisplay::displayBufferFastLut(bool turnOffScreen) {
  if (!einkDisplay.getFrameBuffer()) return;  // v18.9.9.433 (see clearScreen)
  HalSpiBus::Lock spiLock;
  // CrumBLE 4.5.6: freeink-sdk merged the "fast LUT" path into displayBuffer +
  // displayGrayscaleBase; no separate displayBufferFastLut method on the new
  // FreeInkDisplay API. Map to standard fast refresh -- the driver toggles
  // _customLutActive internally when displayGrayscaleBase runs.
  einkDisplay.displayBuffer(freeink::FreeInkDisplay::FAST_REFRESH, turnOffScreen);
}

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
