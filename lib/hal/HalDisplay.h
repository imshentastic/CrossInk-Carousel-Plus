#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // v4.7.2: boot-time X3 panel-controller verdict. setup() probes before SD
  // mount; begin() applies it after, where the profile switch is safe.
  void setX3IsUc8279(bool isUc8279) { _x3IsUc8279 = isUc8279; }

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,       // Full refresh with complete waveform
    HALF_REFRESH,       // Half refresh (1720ms) - balanced quality and speed
    HALF_REFRESH_DEEP,  // Same waveform as HALF_REFRESH but with an extra X3
                        // resync cycle. Use ONLY for transitions where
                        // accumulated panel polarity drift is visible (e.g.
                        // reader -> home after a long dark-mode session).
                        // Costs ~770ms more on X3; no-op vs HALF on X4.
    FAST_REFRESH,       // Fast refresh using custom LUT
    // CrumBLE 4.5.6: draw-only sentinel. Callers of GUI.drawPopup that just
    // want the popup pixels IN the framebuffer without a display refresh
    // (e.g. silent-restart paths that snapshot the framebuffer for boot
    // restore -- one HALF on wake instead of FAST here + HALF on wake).
    NO_REFRESH
  };

  // Pass seamless=true on any path where the panel already shows the
  // content it should after begin() returns (silent reboot's popup,
  // sleep-wake with a restored buffer). Skips the wakeup-gated
  // requestResync() and defuses the SDK's X3 _x3InitialFullSyncsRemaining
  // counter; otherwise the first two paints get promoted to FULL
  // (~770ms each on X3).
  void begin(bool seamless = false);

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // 4.5.5+: Partial-refresh API (Layer 1). Push only a rectangular region
  // of the framebuffer to the panel, using the SSD1677's window-update
  // primitive (setRamArea + windowed BW write + windowed RED sync). Use
  // cases: cover swap during shelf navigation, focus ring update, single-
  // line text change -- the changed area is small, full FAST refresh
  // would waste ~80% of the SPI bandwidth on pixels that didn't change.
  //
  // v2.1 status (this build): routes to EInkDisplay::displayWindow() for
  // FAST_REFRESH mode on X4 panels. Saves ~5-15 ms on SPI write (windowed
  // data vs full 48 KB buffer) but still uses the standard FAST_REFRESH
  // waveform (~417 ms refresh wait). Net ~10 ms savings per nav.
  //
  // v2.2 future: load a panel-specific partial-update LUT to drop refresh
  // wait from 417 ms to ~60-150 ms (7x speedup). Requires safe waveform
  // data (datasheet or vendor reference) before implementing.
  //
  // Behavior:
  //   - Bounds clamped to display dimensions
  //   - Rect >= 70% of screen -> falls back to full displayBuffer
  //   - mode != FAST_REFRESH -> falls back to full displayBuffer
  //   - X3 panels -> falls back to full displayBuffer (X3 uses its own
  //     differential mode path internally)
  //   - x/w byte-aligned automatically (rounded to multiples of 8 pixels)
  void displayBufferRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           RefreshMode mode = RefreshMode::FAST_REFRESH);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // v18.9.9.70 (ported from crosspoint 05c1e9aa): lend the framebuffer's RAM
  // to a memory-hungry phase (section cold-build). No display calls may run
  // between release and a successful realloc; buffers come back white, so
  // callers must redraw the full screen.
  void releaseFrameBuffers();
  bool reallocFrameBuffers();

  // v18.9.9.432: release ONLY the secondary (previous-frame) buffer, ~52 KB
  // on X3. Unlike releaseFrameBuffers, single-buffer rendering keeps working
  // via full refresh; only grayscale AA / fast differential refresh are lost
  // until reallocSecondaryFrameBuffer restores it. Safe to call/no-op when
  // already released. Used by File Transfer to reclaim heap during upload.
  void releaseSecondaryFrameBuffer();
  bool reallocSecondaryFrameBuffer();
  bool hasSecondaryFrameBuffer() const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);
  // CrumBLE 4.5.5+: short LUT-driven refresh (~61 ms) for home-nav-style
  // updates where the panel was recently refreshed. Forwards to
  // EInkDisplay::displayBufferFastLut; see header there for prerequisites.
  void displayBufferFastLut(bool turnOffScreen = false);

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  EInkDisplay einkDisplay;
  bool _x3IsUc8279 = false;
};

extern HalDisplay display;
