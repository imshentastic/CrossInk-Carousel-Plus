#pragma once
#include <string>
#include <utility>

#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         std::string currentBookPath = {}, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        currentBookPath(std::move(currentBookPath)),
        fromTimeout(fromTimeout) {}
  void onEnter() override;

  // Pick a fresh random image from /.sleep (or /sleep) and draw it without any popup or text.
  // Used by the deep-sleep tap-to-cycle path: APP_STATE must already be loaded; the renderer
  // and display must already be initialized; fonts are not required because only a BMP is drawn.
  // No-op if no usable image is found — the existing on-screen image stays visible.
  static void cycleScreensaverFromDeepSleep(GfxRenderer& renderer);

  // Snapshot the current framebuffer to SD so the cycle path can re-use it as
  // the background behind a transparent sleep PNG without needing fonts or the
  // EPUB parser. EpubReaderActivity::onExit calls this so the cache reflects
  // the last book page the user saw.
  static void snapshotFramebufferForCycle();

  // v18.9.9.258: bake .slp caches for every source image in /.sleep/ (or
  // /sleep/). For each *.png / *.bmp with no existing .slp companion,
  // decodes the source once and snapshots the 1bpp framebuffer bytes to
  // SD. Runtime sleep entry then does a straight fread into the
  // framebuffer -- no PNG/BMP decoder, no transient buffers. Returns
  // {baked, skipped} counts. Called from Settings > Display > "Bake
  // sleep images". Idempotent: images with a valid existing .slp are
  // skipped so running twice is cheap.
  struct BakeResult {
    int baked = 0;
    int skipped = 0;
    int failed = 0;
    int total = 0;
  };
  using BakeProgressFn = void (*)(int done, int total);
  static BakeResult bakeAllSleepImages(GfxRenderer& renderer, BakeProgressFn onProgress = nullptr);

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderMinimalSleepScreen() const;
  void renderMinimalStatsSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  // Compose a (possibly transparent) PNG over the last reader page, with the
  // same background-rebuild + grayscale-pass flow used by overlay mode. Used
  // by Custom mode when a PNG is picked so the reader page shows through
  // transparent regions of the image.
  // v18.9.9.260: returns true iff decodeSleepPngToBuffer succeeded and
  // the composited PNG was displayed. False means decode failed and we
  // fell back to SLEEP_FB_CACHE_PATH (or the default sleep screen).
  // Used by renderCustomSleepScreen to decide whether the sleep-image
  // cursor should advance -- pre-v260 the cursor advanced regardless,
  // so a run of failed decodes silently skipped through the cycle.
  bool composePngOverReaderPage(const std::string& pngPath) const;
  bool canSnapshotOverlayBackground = false;
  bool overlayPageBufferStored = false;
  bool overlayPageBufferTrusted = false;
  std::string currentBookPath;
  bool fromTimeout = false;
};
