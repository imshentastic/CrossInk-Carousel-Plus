#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PNGdec.h>

#include <algorithm>
#include <cstdint>
#include <new>

#include "../home/RecentBookProgress.h"
#include "../reader/BookStatsView.h"
#include "../reader/GlobalReadingStats.h"
#include "../reader/ReadingStatsUtils.h"
#include "../reader/EpubReaderActivity.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "SleepCoverAssets.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"
#include "util/SleepCache.h"

#include <InputManager.h>  // v18.9.9.264: POWER_BUTTON_PIN for bake-cancel poll
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {

constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;
constexpr int sleepBuildInfoSideMargin = 20;

// CrumBLE 4.4: cycle counter survives deep-sleep wakes (resets on power loss,
// like the other silentReboot* RTC_NOINIT_ATTR slots in main.cpp). Used to
// pick HALF every Nth cycle to scrub ghost buildup, FAST the rest of the time.
RTC_NOINIT_ATTR uint32_t sleepCycleCounter;
// v18.9.9.305: also survives across deep-sleep wakes. Set true when a cycle
// draws a grayscale bitmap; consumed on the NEXT cycle to force HALF_REFRESH
// so the panel scrubs the LSB/MSB planes before the incoming BW-only draw.
// Without this, a fast-tap onto a BW-only image ghost-overlays the previous
// grayscale image (Eiffel Tower under a new hooded-figure BMP was the field
// repro). uint8_t not bool because RTC_NOINIT_ATTR bools can't guarantee a
// clean boot-time value; we treat any non-zero as "yes".
RTC_NOINIT_ATTR uint8_t sleepCycleLastDrewGrayscale;
// CrumBLE 4.5.1: chip-specific cadence. X3's panel handles 2-in-a-row FAST
// refreshes cleanly, so HALF every 3rd cycle (FAST FAST FAST HALF) is fine.
// X4's panel leaves more residue from FAST refreshes; cycle every 2 (FAST
// FAST HALF) to scrub ghosting more often. Picked at call time via gpio
// since constexpr can't depend on runtime device detect.
constexpr uint32_t kSleepCycleHalfEveryN_X3 = 3;
constexpr uint32_t kSleepCycleHalfEveryN_X4 = 2;

// Snapshot of the last reader-rendered framebuffer, written on EpubReaderActivity::onExit
// and read by cycleScreensaverFromDeepSleep so the cold-boot cycle path can show the last
// book page behind a transparent PNG without needing fonts or the EPUB parser.
constexpr char LAST_READER_PAGE_CACHE_PATH[] = "/.crosspoint/last_reader_page.bin";

bool restoreFramebufferFromCycleCache() {
  FsFile f;
  if (!Storage.openFileForRead("SLP", LAST_READER_PAGE_CACHE_PATH, f)) {
    return false;
  }
  uint8_t* buf = display.getFrameBuffer();
  const uint32_t size = display.getBufferSize();
  const int bytesRead = f.read(buf, size);
  f.close();
  if (bytesRead != static_cast<int>(size)) {
    LOG_ERR("SLP", "Cycle cache: short read %d/%u", bytesRead, size);
    return false;
  }
  LOG_DBG("SLP", "Cycle cache: framebuffer restored");
  return true;
}

// CrumBLE #38: a full-screen sleep image cached as a raw, display-buffer-sized
// framebuffer that needs no decode to show. Written after any successful
// full-screen sleep render (Custom/Cover/Minimal), i.e. when the heap was
// healthy enough to decode the source image. Restored as a low-heap fallback
// when the PNG decoder can't get its ~60 KB working set (e.g. sleeping while a
// BLE page-turner is still connected and has fragmented the heap), so the
// user's chosen sleep picture survives instead of dropping to the default logo.
constexpr char SLEEP_FB_CACHE_PATH[] = "/.crosspoint/sleep_screen_fb.bin";

bool writeFramebufferCache(const char* path) {
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (!Storage.openFileForWrite("SLP", path, f)) {
    LOG_ERR("SLP", "FB cache: open for write failed: %s", path);
    return false;
  }
  const uint8_t* buf = display.getFrameBuffer();
  const uint32_t size = display.getBufferSize();
  const int written = f.write(buf, size);
  f.close();
  if (written != static_cast<int>(size)) {
    LOG_ERR("SLP", "FB cache: short write %d/%u to %s", written, size, path);
    return false;
  }
  return true;
}

bool readFramebufferCache(const char* path) {
  FsFile f;
  if (!Storage.openFileForRead("SLP", path, f)) {
    return false;
  }
  uint8_t* buf = display.getFrameBuffer();
  const uint32_t size = display.getBufferSize();
  const int bytesRead = f.read(buf, size);
  f.close();
  if (bytesRead != static_cast<int>(size)) {
    LOG_ERR("SLP", "FB cache: short read %d/%u from %s", bytesRead, size, path);
    return false;
  }
  return true;
}

void hideOverlayBatteryStrip(const GfxRenderer& renderer) {
  if (!SETTINGS.statusBarBattery) {
    return;
  }

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  const int textY = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - 4;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Reserve the full left-side status indicator lane used by bookmark + battery.
  // This keeps chapter/progress text readable while removing the battery glance target.
  static constexpr int bookmarkReserveWidth = 13;  // bookmark width + gap from BaseTheme::drawStatusBar()
  static constexpr int batteryPercentSpacing = 4;  // matches BaseTheme::batteryPercentSpacing
  const int clearWidth =
      bookmarkReserveWidth + metrics.batteryWidth +
      (showBatteryPercentage ? batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, "100%") : 0);
  const int clearHeight = std::max(renderer.getTextHeight(SMALL_FONT_ID), metrics.batteryHeight + 6);

  renderer.fillRect(metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, textY, clearWidth, clearHeight, false);
}

// Context passed through PNGdec's decode() user-pointer to the per-scanline draw callback.
struct PngOverlayCtx {
  const GfxRenderer* renderer;
  int screenW;
  int screenH;
  int srcWidth;
  int dstWidth;
  int dstX;
  int dstY;
  float yScale;
  int lastDstY;
  // Color-key transparency (tRNS chunk) for TRUECOLOR and GRAYSCALE images.
  // Initialized lazily on the first draw callback because tRNS is processed during decode(),
  // not during open() — so hasAlpha()/getTransparentColor() are only valid once decode() starts.
  // -2 = not yet read; -1 = no color key; >=0 = 0x00RRGGBB (TRUECOLOR) or low-byte gray.
  int32_t transparentColor;
  PNG* pngObj;  // for lazy-init of transparentColor on first callback
};

// PNGdec file I/O callbacks — mirror the pattern in PngToFramebufferConverter.cpp.
void* pngSleepOpen(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("SLP", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}
void pngSleepClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}
int32_t pngSleepRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  return f ? f->read(pBuf, len) : 0;
}
int32_t pngSleepSeek(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// Per-scanline draw callback for PNG overlay compositing.
// Transparent pixels (alpha < 128) are skipped so the reader page shows through.
// Opaque pixels are drawn in their grayscale brightness (dark → black, light → white).
int pngOverlayDraw(PNGDRAW* pDraw) {
  PngOverlayCtx* ctx = reinterpret_cast<PngOverlayCtx*>(pDraw->pUser);

  // Lazy-init: tRNS chunk is processed during decode() before any IDAT data, so by the time
  // the first draw callback fires, hasAlpha() / getTransparentColor() are already valid.
  if (ctx->transparentColor == -2) {
    const int pt = pDraw->iPixelType;
    ctx->transparentColor = (pDraw->iHasAlpha && (pt == PNG_PIXEL_TRUECOLOR || pt == PNG_PIXEL_GRAYSCALE))
                                ? static_cast<int32_t>(ctx->pngObj->getTransparentColor())
                                : -1;
  }

  const int destY = ctx->dstY + (int)(pDraw->y * ctx->yScale);
  if (destY == ctx->lastDstY) return 1;  // skip duplicate rows from Y scaling
  ctx->lastDstY = destY;
  if (destY < 0 || destY >= ctx->screenH) return 1;

  const int srcWidth = ctx->srcWidth;
  const int dstWidth = ctx->dstWidth;
  const uint8_t* pixels = pDraw->pPixels;
  const int pixelType = pDraw->iPixelType;
  const int hasAlpha = pDraw->iHasAlpha;

  int srcX = 0, error = 0;
  for (int dstX = 0; dstX < dstWidth; dstX++) {
    const int outX = ctx->dstX + dstX;
    if (outX >= 0 && outX < ctx->screenW) {
      uint8_t alpha = 255, gray = 0;
      switch (pixelType) {
        case PNG_PIXEL_TRUECOLOR_ALPHA: {
          const uint8_t* p = &pixels[srcX * 4];
          alpha = p[3];
          gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          break;
        }
        case PNG_PIXEL_GRAY_ALPHA:
          gray = pixels[srcX * 2];
          alpha = pixels[srcX * 2 + 1];
          break;
        case PNG_PIXEL_TRUECOLOR: {
          const uint8_t* p = &pixels[srcX * 3];
          gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          // tRNS color-key: if pixel matches the designated transparent color, skip it
          if (ctx->transparentColor >= 0 && p[0] == (uint8_t)((ctx->transparentColor >> 16) & 0xFF) &&
              p[1] == (uint8_t)((ctx->transparentColor >> 8) & 0xFF) &&
              p[2] == (uint8_t)(ctx->transparentColor & 0xFF)) {
            alpha = 0;
          }
          break;
        }
        case PNG_PIXEL_GRAYSCALE:
          gray = pixels[srcX];
          // tRNS color-key: transparent gray value stored in low byte
          if (ctx->transparentColor >= 0 && gray == (uint8_t)(ctx->transparentColor & 0xFF)) {
            alpha = 0;
          }
          break;
        case PNG_PIXEL_INDEXED:
          if (pDraw->pPalette) {
            const uint8_t idx = pixels[srcX];
            const uint8_t* p = &pDraw->pPalette[idx * 3];
            gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            if (hasAlpha) alpha = pDraw->pPalette[768 + idx];
          }
          break;
        default:
          gray = pixels[srcX];
          break;
      }

      if (alpha >= 128) {
        ctx->renderer->drawPixel(outX, destY, gray < 128);  // true = black, false = white
      }
      // alpha < 128: transparent — leave the reader page pixel intact
    }

    // Bresenham-style X stepping (handles downscaling; 1:1 when srcWidth == dstWidth)
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }
  return 1;
}

// Decode a PNG into the current framebuffer using the standard sleep-screen pipeline:
// scale-to-fit, centered, transparency preserved by skipping pixels with alpha < 128.
// Does NOT clear the framebuffer or call displayBuffer — callers prepare the background
// (white for full-screen, reader page for overlay) and present after.
bool decodeSleepPngToBuffer(GfxRenderer& renderer, const std::string& filename, int pageWidth, int pageHeight) {
  if (!Storage.exists(filename.c_str())) {
    return false;
  }

  // v18.9.9.294: was 60 KB, lowered to 48 KB. The old value was set
  // conservatively when NimBLE ate 40+ KB resident and the decoder had
  // to squeeze into whatever remained. v285 deinit(false) reclaims ~40 KB
  // before sleep entry via the disable() call in main.cpp's enterDeepSleep,
  // so actual PNG decoder usage (~42 KB per the comment above) leaves
  // ~6 KB safety margin at 48 KB gate. Second-test symptom: gate refused
  // at 60844 free vs 61440 threshold -- 596 bytes short of a value that
  // had 18 KB of unused headroom above real usage.
  constexpr size_t MIN_FREE_HEAP = 48 * 1024;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("SLP", "Not enough heap for PNG decoder: %u free, need %u for %s", ESP.getFreeHeap(),
            static_cast<unsigned>(MIN_FREE_HEAP), filename.c_str());
    return false;
  }
  PNG* png = new (std::nothrow) PNG();
  if (!png) {
    LOG_ERR("SLP", "Failed to allocate PNG decoder for %s", filename.c_str());
    return false;
  }

  int rc = png->open(filename.c_str(), pngSleepOpen, pngSleepClose, pngSleepRead, pngSleepSeek, pngOverlayDraw);
  if (rc != PNG_SUCCESS) {
    delete png;
    LOG_ERR("SLP", "PNG open failed for %s: %d", filename.c_str(), rc);
    return false;
  }

  const int srcW = png->getWidth();
  const int srcH = png->getHeight();
  float yScale = 1.0f;
  int dstW = srcW, dstH = srcH;
  if (srcW > pageWidth || srcH > pageHeight) {
    const float scaleX = (float)pageWidth / srcW;
    const float scaleY = (float)pageHeight / srcH;
    const float scale = (scaleX < scaleY) ? scaleX : scaleY;
    dstW = (int)(srcW * scale);
    dstH = (int)(srcH * scale);
    yScale = (float)dstH / srcH;
  }

  PngOverlayCtx ctx;
  ctx.renderer = &renderer;
  ctx.screenW = pageWidth;
  ctx.screenH = pageHeight;
  ctx.srcWidth = srcW;
  ctx.dstWidth = dstW;
  ctx.dstX = (pageWidth - dstW) / 2;
  ctx.dstY = (pageHeight - dstH) / 2;
  ctx.yScale = yScale;
  ctx.lastDstY = -1;
  ctx.transparentColor = -2;  // resolved on first draw callback after tRNS is parsed
  ctx.pngObj = png;

  rc = png->decode(&ctx, 0);
  png->close();
  delete png;
  if (rc != PNG_SUCCESS) {
    LOG_ERR("SLP", "PNG decode failed for %s: %d", filename.c_str(), rc);
    return false;
  }
  return true;
}

// Draws a PNG as a full-screen sleep image with a white background. Transparent
// pixels remain white. Caller is responsible for nothing — clears, decodes, and
// presents in one shot.
bool renderPngToSleepScreen(GfxRenderer& renderer, const std::string& filename) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  if (!decodeSleepPngToBuffer(renderer, filename, pageWidth, pageHeight)) {
    return false;
  }
  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  return true;
}

void renderBitmapToSleepScreen(GfxRenderer& renderer, const Bitmap& bitmap, bool skipGreyscalePass = false,
                               HalDisplay::RefreshMode bwRefresh = HalDisplay::HALF_REFRESH) {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  // v18.9.9.294: diagnostic log for the "BMP no greys on first entry"
  // mystery -- capture the state going in so a repro attaches log
  // context we can act on next iteration.
  LOG_INF("SLP", "renderBitmap: hasGrey=%d bmpGrey=%d filter=%d skipGrey=%d",
          hasGreyscale ? 1 : 0, bitmap.hasGreyscale() ? 1 : 0,
          static_cast<int>(SETTINGS.sleepScreenCoverFilter), skipGreyscalePass ? 1 : 0);

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // v18.9.9.305: only turn off the panel after the BW pass when NO grayscale
  // pass follows. Prior code always passed TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH,
  // powering the panel down between the BW and LSB/MSB passes; the grayscale
  // display then either fired at a dark panel (no grays visible) or produced
  // ghost-on-ghost overlays on X4 tap-cycles. composePngOverReaderPage got this
  // right (line 1348 pattern); this path matches it now.
  const bool willRunGrayscale = hasGreyscale && !skipGreyscalePass;
  renderer.displayBuffer(bwRefresh, !willRunGrayscale && TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);

  // Cache the composed B/W full-screen sleep image so a later heap-starved sleep
  // can restore it without a decode (see SLEEP_FB_CACHE_PATH). Snapshot the B/W
  // buffer here, before the optional grayscale passes overwrite it below.
  writeFramebufferCache(SLEEP_FB_CACHE_PATH);

  if (willRunGrayscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    renderer.setRenderMode(GfxRenderer::BW);
    // v18.9.9.305: signal to the NEXT cycleScreensaverFromDeepSleep tap
    // that it must use HALF_REFRESH, since FAST_REFRESH will leave this
    // grayscale RAM on the panel and ghost the incoming BW image.
    sleepCycleLastDrewGrayscale = 1;
  }
}

std::string filenameFromPath(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
}

std::string recentTitleForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto book = std::find_if(books.begin(), books.end(), [&path](const RecentBook& candidate) {
    return candidate.path == path && !candidate.title.empty();
  });
  return book == books.end() ? std::string{} : book->title;
}

RecentBook recentBookForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto book =
      std::find_if(books.begin(), books.end(), [&path](const RecentBook& candidate) { return candidate.path == path; });
  if (book != books.end()) {
    return *book;
  }

  RecentBook loadedBook = RECENT_BOOKS.getDataFromBook(path);
  if (loadedBook.title.empty()) {
    loadedBook.title = filenameFromPath(path);
  }
  return loadedBook;
}

std::string epubCachePathFor(const std::string& path) { return Epub::cachePathForFilePath(path, "/.crosspoint"); }

BookReadingStats loadBookStatsForPath(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) {
    return BookReadingStats{};
  }
  return BookReadingStats::load(epubCachePathFor(path));
}

enum class OverlayDrawResult : uint8_t { NotFound, Drawn, Failed };

enum class SleepImageMode : uint8_t { Custom, Overlay };

struct SleepImageSelection {
  std::string path;
  bool isPng = false;
  // v18.9.9.257: deferred-commit fields. When a select function advances a
  // persistent cursor (cycle: sidecar+SETTINGS; random: recentSleepImages),
  // do it AFTER the caller has decoded the image successfully -- else a
  // tight-heap decode failure silently skips the image visually but the
  // cursor moved past it, so the user "loses" one sleep frame per failed
  // decode. Callers set the fields via the select fn and then must call
  // commitSleepSelectionAdvance() on decode success.
  enum class AdvanceKind : uint8_t { None, Cycle, Random };
  AdvanceKind advanceKind = AdvanceKind::None;
  uint16_t pendingCycleNextIndex = 0;   // Cycle: value to write to sidecar + SETTINGS.sleepScreenCycleIndex
  uint16_t pendingRandomIndex = 0;      // Random: index to pushRecentSleep()
};

bool isBmpSleepImagePath(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool isPngSleepImagePath(const std::string& path) { return FsHelpers::hasPngExtension(path); }

bool openPreferredSleepDirectory(FsFile& dir, const char*& sleepDir) {
  sleepDir = nullptr;
  dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
    return true;
  }

  if (dir) dir.close();
  dir = Storage.open("/sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/sleep";
    return true;
  }

  if (dir) dir.close();
  return false;
}

bool selectPinnedSleepImage(SleepImageMode /*mode*/, SleepImageSelection& selection) {
  const std::string& favorite = APP_STATE.favoriteSleepImagePath;
  if (favorite.empty()) {
    return false;
  }

  if (!Storage.exists(favorite.c_str())) {
    LOG_INF("SLP", "Pinned sleep image missing, falling back: %s", favorite.c_str());
    return false;
  }

  if (isBmpSleepImagePath(favorite)) {
    selection.path = favorite;
    selection.isPng = false;
    return true;
  }

  if (isPngSleepImagePath(favorite)) {
    selection.path = favorite;
    selection.isPng = true;
    return true;
  }

  LOG_ERR("SLP", "Pinned sleep image has unsupported extension: %s", favorite.c_str());
  return false;
}

bool selectRandomSleepImage(SleepImageMode /*mode*/, SleepImageSelection& selection) {
  FsFile dir;
  const char* sleepDir = nullptr;
  if (!openPreferredSleepDirectory(dir, sleepDir)) {
    return false;
  }

  const bool allowPng = true;
  std::vector<std::string> files;
  files.reserve(16);
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    const bool isBmp = FsHelpers::hasBmpExtension(filename);
    const bool isPng = allowPng && FsHelpers::hasPngExtension(filename);
    if (!isBmp && !isPng) {
      file.close();
      continue;
    }

    if (isBmp) {
      Bitmap bitmap(file);
      const BmpReaderError parseResult = bitmap.parseHeaders();
      if (parseResult != BmpReaderError::Ok) {
        LOG_ERR("SLP", "Skipping invalid BMP sleep image %s/%s: %s", sleepDir, filename.c_str(),
                Bitmap::errorToString(parseResult));
        file.close();
        continue;
      }
    }

    files.emplace_back(std::move(filename));
    file.close();
  }
  dir.close();

  if (files.empty()) {
    return false;
  }

  const uint16_t fileCount = static_cast<uint16_t>(std::min(files.size(), static_cast<size_t>(UINT16_MAX)));
  const uint8_t window =
      static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), files.size() - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  // v18.9.9.257: defer cursor state advance to post-decode. See
  // SleepImageSelection.AdvanceKind for why -- tight-heap PNG decode can
  // fail after we've marked an image as "recently shown", making that
  // image invisible next round even though the user never saw it.
  selection.path = std::string(sleepDir) + "/" + files[randomFileIndex];
  selection.isPng = FsHelpers::hasPngExtension(selection.path);
  selection.advanceKind = SleepImageSelection::AdvanceKind::Random;
  selection.pendingRandomIndex = randomFileIndex;
  return true;
}

// v18.9.9.203: tiny sidecar for the cycle cursor. The settings-JSON save
// (SETTINGS.saveToFile) can defer under low heap (needs 16 KB free / 4 KB
// maxAlloc); if sleep entry follows the defer before the main-loop retry
// fires, the advance is lost and next boot restarts at index 0 (the field
// report). A 2-byte file with no heap gate always writes and always reads
// back. Loaded here on cycle-pick so it beats a stale SETTINGS value.
constexpr const char* kSleepCycleSidecar = "/.crosspoint/sleep_cycle.bin";
uint16_t readCycleIndexSidecar() {
  FsFile f;
  if (!Storage.openFileForRead("SLP", kSleepCycleSidecar, f)) return UINT16_MAX;
  uint16_t v = 0;
  const size_t n = f.read(reinterpret_cast<uint8_t*>(&v), sizeof(v));
  f.close();
  return n == sizeof(v) ? v : UINT16_MAX;
}
void writeCycleIndexSidecar(uint16_t v) {
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (!Storage.openFileForWrite("SLP", kSleepCycleSidecar, f)) return;
  f.write(reinterpret_cast<const uint8_t*>(&v), sizeof(v));
  f.close();
}

// Cycle through /.sleep/ in alphabetical order, picking by the persisted cursor
// in SETTINGS.sleepScreenCycleIndex and advancing it. Returns false if the
// directory is absent or empty so the caller falls through to pinned/random.
// 4.7.2: YYYYMMDD key for Daily Mode. Compared for equality only, so no
// epoch math is needed. Returns 0 when the clock isn't trustworthy, which
// the caller treats as "Daily Mode inactive" (falls back to per-sleep
// cycling rather than freezing on one image forever).
uint32_t currentDayKeyOrZero() {
  if (!halClock.hasValidTime()) return 0;
  uint16_t year = 0;
  uint8_t dayOfWeek = 0, day = 0, month = 0;
  if (!halClock.getDate(dayOfWeek, day, month, year)) return 0;
  if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
  return static_cast<uint32_t>(year) * 10000u + static_cast<uint32_t>(month) * 100u + day;
}

// manual: a user tap (always advances, and re-stamps the Daily Mode day).
// backward: step to the previous image instead of the next.
bool selectCycleSleepImage(SleepImageSelection& selection, bool manual, bool backward) {
  FsFile dir;
  const char* sleepDir = nullptr;
  if (!openPreferredSleepDirectory(dir, sleepDir)) {
    LOG_INF("SLP", "Cycle: no sleep directory found");
    return false;
  }

  std::vector<std::string> files;
  files.reserve(16);
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    const bool isBmp = FsHelpers::hasBmpExtension(filename);
    const bool isPng = FsHelpers::hasPngExtension(filename);
    if (!isBmp && !isPng) {
      file.close();
      continue;
    }

    if (isBmp) {
      Bitmap bitmap(file);
      const BmpReaderError parseResult = bitmap.parseHeaders();
      if (parseResult != BmpReaderError::Ok) {
        LOG_ERR("SLP", "Skipping invalid BMP sleep image %s/%s: %s", sleepDir, filename.c_str(),
                Bitmap::errorToString(parseResult));
        file.close();
        continue;
      }
    }

    files.emplace_back(std::move(filename));
    file.close();
  }
  dir.close();

  if (files.empty()) {
    LOG_INF("SLP", "Cycle: %s is empty (after BMP/PNG filter)", sleepDir);
    return false;
  }

  std::sort(files.begin(), files.end());

  const uint16_t fileCount = static_cast<uint16_t>(std::min(files.size(), static_cast<size_t>(UINT16_MAX)));
  // v18.9.9.203: sidecar wins over SETTINGS if present -- it's the source of
  // truth for the cursor across deep-sleep boundaries where the JSON save
  // may have deferred. Falls back to SETTINGS on cold boot (no sidecar yet).
  const uint16_t sidecarIndex = readCycleIndexSidecar();
  const uint16_t storedIndex =
      (sidecarIndex != UINT16_MAX) ? sidecarIndex : SETTINGS.sleepScreenCycleIndex;
  // The cursor stores the NEXT image to show, so a forward step displays it
  // as-is. Stepping back must display the one BEFORE the last-shown image,
  // i.e. cursor-2, so that back-after-forward returns to the previous
  // picture rather than repeating the current one.
  const uint16_t idx = backward
                           ? static_cast<uint16_t>((storedIndex + fileCount - 2) % fileCount)
                           : static_cast<uint16_t>(storedIndex % fileCount);
  selection.path = std::string(sleepDir) + "/" + files[idx];
  selection.isPng = FsHelpers::hasPngExtension(selection.path);

  // 4.7.2 Daily Mode: on an AUTOMATIC sleep, hold the current image until
  // the calendar day rolls over. Manual taps ignore this and always move.
  // dayKey == 0 means no trustworthy clock -> behave exactly as before.
  const uint32_t dayKey = SETTINGS.sleepCycleDailyMode ? currentDayKeyOrZero() : 0u;
  if (!manual && dayKey != 0u && SETTINGS.sleepCycleLastChangeDay == dayKey) {
    selection.advanceKind = SleepImageSelection::AdvanceKind::None;
    LOG_INF("SLP", "Cycle: daily mode holding index=%u for day=%lu", idx,
            static_cast<unsigned long>(dayKey));
    return true;
  }

  // Cursor invariant holds in both directions: always one past what we show.
  const uint16_t nextIndex = static_cast<uint16_t>((idx + 1) % fileCount);
  if (dayKey != 0u) {
    SETTINGS.sleepCycleLastChangeDay = dayKey;
  }
  // v18.9.9.257: defer cursor advance to post-decode (see AdvanceKind
  // comment on SleepImageSelection). Under tight heap, PNG decode of the
  // picked image can fail after we've already stamped the sidecar +
  // SETTINGS with nextIndex, so the failed image is "skipped" visually
  // and the user never sees it again until the cursor wraps around.
  // Now the caller commits after a successful decode.
  selection.advanceKind = SleepImageSelection::AdvanceKind::Cycle;
  selection.pendingCycleNextIndex = nextIndex;
  LOG_INF("SLP", "Cycle: count=%u stored=%u (sidecar=%s) picked=%u pending-next=%u file=%s", fileCount,
          storedIndex, sidecarIndex == UINT16_MAX ? "none" : "present",
          idx, nextIndex, selection.path.c_str());
  return true;
}

// v18.9.9.257: commit the pending cursor/recent advance after successful
// decode. No-op for AdvanceKind::None (pinned or /sleep.bmp fallback --
// nothing to advance). Called from renderCustomSleepScreen just after
// the image renders without falling through to the error path.
void commitSleepSelectionAdvance(const SleepImageSelection& selection) {
  switch (selection.advanceKind) {
    case SleepImageSelection::AdvanceKind::None:
      return;
    case SleepImageSelection::AdvanceKind::Cycle:
      SETTINGS.sleepScreenCycleIndex = selection.pendingCycleNextIndex;
      writeCycleIndexSidecar(selection.pendingCycleNextIndex);
      SETTINGS.saveToFile();
      return;
    case SleepImageSelection::AdvanceKind::Random:
      APP_STATE.pushRecentSleep(selection.pendingRandomIndex);
      APP_STATE.saveToFile();
      return;
  }
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  overlayPageBufferStored = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY &&
                            APP_STATE.lastSleepFromReader && renderer.storeBwBuffer();
  overlayPageBufferTrusted = overlayPageBufferStored && canSnapshotOverlayBackground;

  // Cache the clean reader page for the deep-sleep screensaver-cycle path BEFORE
  // the "Entering sleep" popup is drawn over it. Capturing later (the old
  // reader-onExit snapshot) baked the popup into the cached background that
  // transparent sleep PNGs show through when the user cycles screensavers.
  if (APP_STATE.lastSleepFromReader) {
    snapshotFramebufferForCycle();
  }

  // Show the popup in the reader's orientation when sleep starts from an open book.
  // Reset to portrait afterwards so the sleep screen renderer keeps its existing layout.
  // v18.9.9.348: skip the popup for OVERLAY sleep mode -- overlay renders
  // the book page + sleep image over it in ONE additional flash; the
  // popup added a redundant FAST_REFRESH before that. Field-observed
  // triple-flash on highlighted book pages (popup + BW overlay + optional
  // grayscale). Overlay is quick enough (~2 s) that no reassurance popup
  // is needed. Other sleep modes (Custom/Cover/Cover_Custom) still show
  // it since their render can take longer.
  const bool isOverlayMode = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY;
  if (!isOverlayMode) {
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
      GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    } else {
      GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    }
  }

  // v18.9.9.462 (P3b): when Dashboard is the UI theme, always route sleep to
  // the stats overlay renderer regardless of sleepScreen mode. Dashboard is
  // a "stats-forward" identity — the theme choice itself implies the user
  // wants stats visible on sleep. Users who explicitly want a non-stats
  // sleep can switch UI theme.
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::DASHBOARD) {
    return renderMinimalStatsSleepScreen();
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY):
      return renderOverlaySleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::READING_STATS_SLEEP):
      return renderReadingStatsSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP):
      return renderMinimalSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP):
      return renderMinimalStatsSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  SleepImageSelection selection;
  const bool alphabetical =
      SETTINGS.sleepScreenOrder == CrossPointSettings::SLEEP_ORDER_ALPHABETICAL;
  auto trySelectFallback = [&]() -> bool {
    return alphabetical ? selectCycleSleepImage(selection, /*manual=*/false, /*backward=*/false)
                        : selectRandomSleepImage(SleepImageMode::Custom, selection);
  };
  if (selectPinnedSleepImage(SleepImageMode::Custom, selection) || trySelectFallback()) {
    LOG_INF("SLP", "Loading custom sleep image: %s", selection.path.c_str());
    delay(100);
    // v18.9.9.258: try the pre-baked .slp cache first. Loads the
    // ready-to-blit 1bpp framebuffer bytes straight from SD, skipping
    // PNG/BMP decode + the transient buffers that path allocates. On
    // tight post-BT / post-book-open heap, that's the difference
    // between "sleep frame shows" and "no image, falls to default".
    // Cache mismatch (bad magic, panel resolution changed, no .slp
    // file) returns false and we fall through to the source decoder
    // as before.
    // v18.9.9.279: skip .slp entirely for transparent PNGs. Baked .slp
    // freezes whatever pixels were behind the alpha regions at bake
    // time (typically white), so an old bake shows white instead of
    // the reader page. Runtime composePngOverReaderPage restores the
    // cycle cache first and composites on top -- the whole reason we
    // support transparent sleep PNGs. One extra header sniff per
    // sleep entry (~a few hundred bytes off SD) is cheap.
    //
    // v18.9.9.281: same treatment for grayscale BMPs (bpp > 1). The
    // grayscale second pass (LSB + MSB layers) can't be represented in
    // a single 1bpp .slp -- a baked .slp for these shows the 1-bit
    // render only, no greys. Skip the cache so the fallthrough hits
    // renderBitmapSleepScreen which runs both passes.
    const bool transparentPng = selection.isPng && SleepCache::pngHasTransparency(selection.path);
    const bool grayscaleBmp = !selection.isPng && SleepCache::bmpHasGreyscale(selection.path);
    if (!transparentPng && !grayscaleBmp && renderer.hasFrameBuffer() &&
        SleepCache::loadIntoFramebuffer(selection.path, renderer.getFrameBuffer(),
                                         renderer.getBufferSize(),
                                         renderer.getScreenWidth(),
                                         renderer.getScreenHeight())) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
      commitSleepSelectionAdvance(selection);
      return;
    }
    if (selection.isPng) {
      // v18.9.9.260: composePngOverReaderPage now returns success. Only
      // commit the cursor advance if the PNG source actually decoded --
      // fallback to SLEEP_FB_CACHE_PATH (last successful image) means
      // the user is seeing a DIFFERENT image than the one at the current
      // cursor, so advancing would silently skip the picked image.
      if (composePngOverReaderPage(selection.path)) {
        commitSleepSelectionAdvance(selection);
      }
      return;
    } else {
      FsFile file;
      if (Storage.openFileForRead("SLP", selection.path, file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          commitSleepSelectionAdvance(selection);
          return;
        }
        LOG_ERR("SLP", "Failed to parse custom sleep BMP: %s", selection.path.c_str());
      } else {
        LOG_ERR("SLP", "Failed to open custom sleep image: %s", selection.path.c_str());
      }
    }
  }

  // Look for sleep.bmp or sleep.png on the root of the SD card as a fallback
  // before giving up on custom mode.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }
  if (Storage.exists("/sleep.png")) {
    LOG_DBG("SLP", "Loading: /sleep.png");
    composePngOverReaderPage("/sleep.png");
    return;
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  // Match the boot screen's rebrand: CrumBLE, not the upstream name.
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CRUMBLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  const bool lightSleepScreen = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT;
  if (!lightSleepScreen) {
    renderer.invertScreen();
  }

#ifdef CROSSINK_SHOW_SLEEP_BUILD_INFO
  const std::string buildInfo = std::string(CROSSINK_BUILD_ENV) + " " + CROSSINK_VERSION;
  const std::string visibleBuildInfo =
      renderer.truncatedText(SMALL_FONT_ID, buildInfo.c_str(), pageWidth - sleepBuildInfoSideMargin * 2);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 118, visibleBuildInfo.c_str(), lightSleepScreen);
#endif

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const { renderBitmapToSleepScreen(renderer, bitmap); }

void SleepActivity::snapshotFramebufferForCycle() {
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (!Storage.openFileForWrite("SLP", LAST_READER_PAGE_CACHE_PATH, f)) {
    LOG_ERR("SLP", "Cycle cache: open for write failed");
    return;
  }
  const uint8_t* buf = display.getFrameBuffer();
  const uint32_t size = display.getBufferSize();
  const int written = f.write(buf, size);
  f.close();
  if (written != static_cast<int>(size)) {
    LOG_ERR("SLP", "Cycle cache: short write %d/%u", written, size);
  } else {
    LOG_DBG("SLP", "Cycle cache: snapshot saved (%u bytes)", size);
  }
}

void SleepActivity::cycleScreensaverFromDeepSleep(GfxRenderer& renderer, bool backward) {
  SleepImageSelection selection;
  const bool alphabetical =
      SETTINGS.sleepScreenOrder == CrossPointSettings::SLEEP_ORDER_ALPHABETICAL;
  // This entry point is only ever reached from a user tap, so manual=true:
  // Daily Mode never suppresses a deliberate cycle.
  const bool selected = alphabetical ? selectCycleSleepImage(selection, /*manual=*/true, backward)
                                     : selectRandomSleepImage(SleepImageMode::Custom, selection);
  if (!selected) {
    LOG_INF("SLP", "Cycle skipped: no sleep image available");
    return;
  }

  LOG_INF("SLP", "Cycling sleep image to: %s", selection.path.c_str());

  // CrumBLE 4.4: pick FAST_REFRESH for most cycles; HALF every Nth.
  // FAST is ~470 ms vs HALF's ~770 ms (plus the X3 resync) -- ~half the
  // wall-clock cost on every cycle that hits this branch. Periodic HALF
  // sweeps panel ghost buildup that FAST diffs leave behind. 4.5.1: N
  // tuned per chip -- X4 panel needs more frequent HALF sweeps.
  const uint32_t halfEveryN = gpio.deviceIsX3() ? kSleepCycleHalfEveryN_X3 : kSleepCycleHalfEveryN_X4;
  ++sleepCycleCounter;
  // v18.9.9.305: scrub the panel whenever the previous cycle painted
  // grayscale (LSB+MSB) planes. FAST_REFRESH after grayscale only diffs
  // the BW plane and leaves the grayscale RAM lit, ghost-overlaying the
  // new image (Eiffel-Tower + hooded-figure field bug). Consumed here so
  // the scrub fires exactly once, on the cycle immediately after
  // grayscale.
  //
  // v18.9.9.305 attempt 2: HALF_REFRESH is not sufficient. On X4 the
  // HALF path is a no-op resync (the extra X3 resync in HalDisplay is
  // gpio.deviceIsX3()-gated), so we get plain HALF which does not clear
  // the panel's LSB/MSB waveform state. Ghost persisted after a HALF
  // scrub. Bump to FULL_REFRESH -- costs an extra ~500 ms wave cycle on
  // this one cycle, but reliably clears the grayscale RAM on both
  // panels. Only fires the cycle after grayscale, so the amortised cost
  // is one FULL per pair of grayscale-adjacent taps.
  const bool scrubAfterGray = sleepCycleLastDrewGrayscale != 0;
  sleepCycleLastDrewGrayscale = 0;
  const bool useHalf = (sleepCycleCounter % halfEveryN) == 0;
  // v18.9.9.346: transparent PNGs demand HALF_REFRESH. FAST_REFRESH's DU
  // waveform under-drives black->white transitions on pixels the new
  // PNG's transparent regions leave uncovered -- symptom being the
  // previous PNG's opaque parts ghosting under the current one.
  //
  // v18.9.9.350: for PNGs specifically, HALF is enough even after a
  // grayscale predecessor -- we're compositing on top of the book-page
  // background whose grayscale panel state is EXACTLY what we want to
  // keep visible under the transparent PNG regions. Scrub-after-gray
  // FULL only helps opaque BW frames (BMPs). For PNGs the FULL wave
  // was showing up as an extra visible flash without a benefit --
  // user-reported "triple flash before some PNGs on highlight
  // background". BMP with grays -> BMP still gets the FULL scrub.
  const bool needsPngScrub = selection.isPng;
  const HalDisplay::RefreshMode cycleRefresh =
      needsPngScrub                   ? HalDisplay::HALF_REFRESH
      : scrubAfterGray                ? HalDisplay::FULL_REFRESH
      : useHalf                       ? HalDisplay::HALF_REFRESH
                                      : HalDisplay::FAST_REFRESH;
  LOG_DBG("SLP", "Cycle refresh: %s (count=%u, postGray=%d, isPng=%d)",
          needsPngScrub ? "HALF-png" : (scrubAfterGray ? "FULL" : (useHalf ? "HALF" : "FAST")),
          static_cast<unsigned>(sleepCycleCounter), scrubAfterGray ? 1 : 0, needsPngScrub ? 1 : 0);

  // v18.9.9.281: always advance the cycle cursor before we return,
  // regardless of decode/render outcome. This function was the only
  // sleep-image path that never called commitSleepSelectionAdvance
  // (v257 added the commit-after-decode discipline to
  // renderCustomSleepScreen but missed the deep-sleep cycle path),
  // so alphabetical-mode users tap-cycling from deep sleep saw the
  // SAME image every tap forever. On failure we still advance -- if
  // the current image can't decode this cycle (typically post-BT
  // heap starvation), staying on it just means the next tap fails
  // the same way. Better to move on.
  struct AdvanceGuard {
    const SleepImageSelection& sel;
    ~AdvanceGuard() { commitSleepSelectionAdvance(sel); }
  } _advance{selection};

  if (selection.isPng) {
    // Try to use the cached last reader page as the background so transparent
    // regions of the PNG show book text underneath. Falls back to a clean
    // white background if the cache is missing or unreadable.
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    if (!restoreFramebufferFromCycleCache()) {
      renderer.clearScreen();
    }
    if (!decodeSleepPngToBuffer(renderer, selection.path, pageWidth, pageHeight)) {
      LOG_ERR("SLP", "Cycle: PNG decode failed for %s", selection.path.c_str());
      return;
    }
    if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(cycleRefresh, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("SLP", selection.path, file)) {
    LOG_ERR("SLP", "Cycle: failed to open %s", selection.path.c_str());
    return;
  }

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Cycle: failed to parse %s", selection.path.c_str());
    return;
  }

  // sleepCycleSkipGrayscale: optional opt-in for users who tap-cycle fast
  // and would rather see a snappy BW frame than wait for the grayscale
  // LSB/MSB double-pass. Default off so behaviour matches v3.7.3 unless
  // the user explicitly turns it on in Display settings.
  const bool skipGrayscale = SETTINGS.sleepCycleSkipGrayscale != 0;
  renderBitmapToSleepScreen(renderer, bitmap, skipGrayscale, cycleRefresh);
}

// v18.9.9.258: bake .slp caches for all sleep source images.
// Iterates /.sleep/ (or /sleep/), skips images with a valid existing
// .slp companion, decodes the rest via the same helpers used by the
// runtime sleep path, and snapshots the 1bpp framebuffer bytes via
// SleepCache::saveFramebuffer. Blocking; caller draws a progress popup.
SleepActivity::BakeResult SleepActivity::bakeAllSleepImages(GfxRenderer& renderer, BakeProgressFn onProgress) {
  BakeResult result;
  FsFile dir;
  const char* sleepDir = nullptr;
  if (!openPreferredSleepDirectory(dir, sleepDir)) {
    LOG_INF("SLP", "Bake: no sleep directory found");
    return result;
  }

  // Enumerate PNG/BMP candidates first so we know `total` for progress.
  std::vector<std::string> files;
  files.reserve(16);
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) { file.close(); continue; }
    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.') { file.close(); continue; }
    const bool isBmp = FsHelpers::hasBmpExtension(filename);
    const bool isPng = FsHelpers::hasPngExtension(filename);
    if (!isBmp && !isPng) { file.close(); continue; }
    files.emplace_back(std::move(filename));
    file.close();
  }
  dir.close();

  result.total = static_cast<int>(files.size());
  if (result.total == 0) {
    LOG_INF("SLP", "Bake: sleep dir is empty (after PNG/BMP filter)");
    return result;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // v18.9.9.264: raw-GPIO Back/cancel via power button between images.
  // The bake runs in main.cpp setup() before MappedInputManager exists,
  // so we poll the pin directly. INPUT_PULLUP -> LOW when pressed.
  // Cancels leave already-baked images untouched -- resuming later just
  // skips them (cacheExists check). Fast (~1 microsecond digitalRead).
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);

  for (int i = 0; i < result.total; ++i) {
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      LOG_INF("SLP", "Bake: cancelled by user after %d/%d", i, result.total);
      break;
    }
    const std::string fullPath = std::string(sleepDir) + "/" + files[i];
    // v18.9.9.279: transparent PNGs must NOT be baked. .slp is a fixed
    // 1bpp framebuffer -- baking freezes whatever background pixels
    // were behind the PNG at bake time (typically clearScreen white),
    // which defeats the whole point of a transparent overlay. Runtime
    // path falls through to composePngOverReaderPage, which restores
    // the last reader page from the cycle cache first. Also purge any
    // .slp left over from an older build that baked these wrong.
    if (FsHelpers::hasPngExtension(files[i]) && SleepCache::pngHasTransparency(fullPath)) {
      SleepCache::removeCache(fullPath);
      result.skipped++;
      // v18.9.9.299: don't call onProgress on skip. Skips are ~10 ms
      // (file open + 30-byte header read + close) but the progress
      // popup redraw is a ~500 ms display refresh. 26 skips = 13 s of
      // wasted screen fade with no visible progress. Skip the callback
      // so the popup stays static while we blaze through.
      continue;
    }
    if (SleepCache::cacheExists(fullPath)) {
      // Already baked. Cheap skip -- no header validation here; a later
      // load with header mismatch will just fall back to the source
      // decoder as always.
      result.skipped++;
      // v18.9.9.299: don't call onProgress on skip. Skips are ~10 ms
      // (file open + 30-byte header read + close) but the progress
      // popup redraw is a ~500 ms display refresh. 26 skips = 13 s of
      // wasted screen fade with no visible progress. Skip the callback
      // so the popup stays static while we blaze through.
      continue;
    }

    // Fresh decode into framebuffer. Use the same helpers the runtime
    // sleep path uses so the baked pixels are what the user would see
    // otherwise. clearScreen first so transparent PNG regions don't
    // pick up whatever the previous iteration left behind.
    renderer.clearScreen();
    bool decoded = false;
    if (FsHelpers::hasPngExtension(files[i])) {
      decoded = decodeSleepPngToBuffer(renderer, fullPath, pageWidth, pageHeight);
    } else {
      FsFile bmpFile;
      if (Storage.openFileForRead("SLP", fullPath, bmpFile)) {
        Bitmap bitmap(bmpFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // v18.9.9.281: BMPs with bpp > 1 rely on a grayscale second
          // pass that renderBitmapToSleepScreen only runs when
          // skipGreyscalePass=false. .slp is a fixed B/W framebuffer
          // that can't carry the LSB+MSB grayscale layers, so baking
          // one for a grayscale BMP produces a 1-bit-only sleep image
          // (visible symptom: the specific BMP that "loses its greys"
          // has a .slp; the others fall through to the full render).
          // Skip the bake + purge stale .slp so the runtime path takes
          // over.
          if (bitmap.hasGreyscale()) {
            SleepCache::removeCache(fullPath);
            result.skipped++;
            // v18.9.9.299: don't call onProgress on skip (see comment in
            // the earlier skip branches).
            bmpFile.close();
            continue;
          }
          // renderBitmapToSleepScreen draws row-by-row + calls
          // displayBuffer(HALF_REFRESH). During bake we accept the
          // brief flash of each image as visual progress feedback
          // -- and the framebuffer is left holding the decoded
          // bytes we need to snapshot into .slp.
          renderBitmapToSleepScreen(renderer, bitmap, /*skipGreyscalePass=*/true);
          decoded = true;
        }
      }
    }
    if (!decoded) {
      LOG_ERR("SLP", "Bake: decode failed for %s", fullPath.c_str());
      result.failed++;
      if (onProgress) onProgress(i + 1, result.total);
      continue;
    }

    if (SleepCache::saveFramebuffer(fullPath, renderer.getFrameBuffer(), renderer.getBufferSize(),
                                     pageWidth, pageHeight)) {
      result.baked++;
    } else {
      LOG_ERR("SLP", "Bake: save failed for %s", fullPath.c_str());
      result.failed++;
    }
    if (onProgress) onProgress(i + 1, result.total);
  }

  LOG_INF("SLP", "Bake done: baked=%d skipped=%d failed=%d total=%d",
          result.baked, result.skipped, result.failed, result.total);
  return result;
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  const std::string coverBmpPath = SleepCoverAssets::cachedCoverPathFor(path, cropped);
  if (coverBmpPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderReadingStatsSleepScreen() const {
  BookReadingStats bookStats;
  GlobalReadingStats globalStats = GlobalReadingStats::load();
  std::string bookTitle = tr(STR_READING_STATS);

  const std::string& path = APP_STATE.openEpubPath;
  if (!path.empty()) {
    const std::string recentTitle = recentTitleForPath(path);
    bookTitle = recentTitle.empty() ? filenameFromPath(path) : recentTitle;

    bookStats = loadBookStatsForPath(path);
  }

  renderBookStatsView(renderer, nullptr, bookTitle, bookStats, globalStats, false);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderMinimalSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  RecentBook book = recentBookForPath(path);
  book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const float progressPercent = RecentBookProgress::loadPercent(book);
  MinimalTheme theme;
  theme.drawSleepScreen(renderer, book, &bookStats, progressPercent);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  // Cache the rendered Minimal sleep screen for the low-heap restore (#38).
  writeFramebufferCache(SLEEP_FB_CACHE_PATH);
}

// v18.9.9.445 (CrossInk parity): Minimal Stats sleep screen. Falls back to
// plain Minimal when no book open. Reader-type + streak overlay only shows
// meaningful values when clock is valid (X3 always, X4 after SNTP).
void SleepActivity::renderMinimalStatsSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    LOG_INF("SLP", "MinimalStatsSleep: no book path, fallback to default sleep screen");
    return renderDefaultSleepScreen();
  }
  RecentBook book = recentBookForPath(path);
  book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);
  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const float progressPercent = RecentBookProgress::loadPercent(book);
  const GlobalReadingStats globalStats = GlobalReadingStats::load();
  ReadingStatsDateTime nowLocal;
  const bool clockValid = getCurrentLocalReadingStatsDateTime(nowLocal);
  LOG_INF("SLP", "MinimalStatsSleep: clockValid=%d ToD=%u/%u/%u/%u streakAnchor=%u",
          clockValid ? 1 : 0,
          static_cast<unsigned>(globalStats.timeOfDaySeconds[0]),
          static_cast<unsigned>(globalStats.timeOfDaySeconds[1]),
          static_cast<unsigned>(globalStats.timeOfDaySeconds[2]),
          static_cast<unsigned>(globalStats.timeOfDaySeconds[3]),
          static_cast<unsigned>(globalStats.readingHistoryAnchorDay));
  MinimalTheme theme;
  // v18.9.9.466: renamed drawSleepScreenWithStats → drawStatsSleepScreen
  // (1:1 with CrossInk). Passes globalStats as pointer; inverted=false
  // = BLACK background (default sleep style).
  (void)clockValid;  // gate now lives inside drawStatsOverlay
  theme.drawStatsSleepScreen(renderer, book, &bookStats, &globalStats, progressPercent, /*inverted=*/false);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  writeFramebufferCache(SLEEP_FB_CACHE_PATH);
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

bool SleepActivity::composePngOverReaderPage(const std::string& pngPath) const {
  // Mirror the overlay mode flow but with a caller-supplied PNG path: restore
  // (or rebuild) the last reader page as background, then decode the PNG with
  // transparency preserved so the reader page shows through transparent
  // regions. Used by Custom mode for PNG selections so the user gets a
  // "transparent overlay over book page" sleep look without changing the
  // sleep screen mode.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& path = APP_STATE.openEpubPath;

  auto renderSavedReaderPage = [&]() -> bool {
    if (path.empty()) return false;
    if (FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch")) {
      return XtcReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".txt")) {
      return TxtReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".epub")) {
      return EpubReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    return false;
  };

  const bool backgroundSupportsGrayscale =
      FsHelpers::checkFileExtension(path, ".txt") || FsHelpers::checkFileExtension(path, ".epub");
  bool backgroundWasRebuilt = false;

  if (overlayPageBufferTrusted) {
    renderer.restoreBwBuffer();
  } else if (restoreFramebufferFromCycleCache()) {
    // Reuse the persistent reader-page snapshot from
    // /.crosspoint/last_reader_page.bin. The file is written both when we
    // sleep from a reader (SleepActivity::onEnter) and when we exit a reader
    // to Home (Activity::exitToHomeWithPopup), so a transparent PNG sleep
    // image composes over the user's last book page even when they sleep
    // from Home/Settings — not just when they sleep directly from the reader.
    //
    // History: 9f970dec gated this on canSnapshotOverlayBackground to avoid a
    // "zoomed-in home screen" effect from the old renderSavedReaderPage()
    // re-render path, which could pull stale content via APP_STATE.openEpubPath.
    // The cache-based approach (introduced in f43693d6 alongside that gating
    // "out of caution") has no such risk: the snapshot file is *only* written
    // from reader contexts, never from Home/Settings, so its bytes are always
    // a book page. Dropping the gate lets non-reader sleeps benefit from the
    // last-known reader page as a backdrop.
    //
    // Heap-light: avoids the section-cache re-render that "SCT: parameters do
    // not match" warned about on long reading sessions, which could leave no
    // room for the PNG decoder and freeze the "Going to sleep" popup.
  } else {
    // No snapshot ever written (e.g. fresh boot, no reader opened yet):
    // blank background.
    renderer.clearScreen();
  }

  hideOverlayBatteryStrip(renderer);

  if (!decodeSleepPngToBuffer(renderer, pngPath, pageWidth, pageHeight)) {
    LOG_ERR("SLP", "Failed to compose PNG over reader page: %s", pngPath.c_str());
    renderer.setOrientation(GfxRenderer::Portrait);
    // Low-heap fallback (#38): restore the last successfully-rendered full-screen
    // sleep image from the framebuffer cache -- no decode needed -- so the user's
    // sleep picture survives a heap-starved sleep (e.g. BLE page-turner still
    // connected). Only if the cache is unavailable do we drop to the heap-light
    // default sleep screen. Either way the "Going to sleep" popup never stays
    // frozen on screen.
    if (readFramebufferCache(SLEEP_FB_CACHE_PATH)) {
      LOG_INF("SLP", "Restored cached sleep image (PNG decode unavailable, low heap)");
      renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    } else {
      renderDefaultSleepScreen();
    }
    renderer.setOrientation(savedOrientation);
    // v18.9.9.260: signal decode failure so caller doesn't advance the
    // sleep cycle cursor -- the user never actually saw THIS image.
    return false;
  }

  // Cache the composed B/W sleep image for the low-heap restore above. Snapshot
  // before the grayscale passes (which overwrite the buffer) and before the
  // orientation is restored, mirroring the B/W displayBuffer below.
  writeFramebufferCache(SLEEP_FB_CACHE_PATH);

  renderer.setOrientation(savedOrientation);

  const bool shouldRunGrayscalePass =
      backgroundSupportsGrayscale && (backgroundWasRebuilt || (overlayPageBufferTrusted && !path.empty()));
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, !shouldRunGrayscalePass && TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);

  // Beyond this point the PNG source itself decoded successfully -- the
  // grayscale-pass branches are enhancement steps for the reader-page
  // background, not the PNG itself. Any failure there still leaves the
  // BW PNG on screen, so we return true.
  if (!shouldRunGrayscalePass) return true;

  if (!renderer.storeBwBuffer()) {
    LOG_ERR("SLP", "Compose PNG: failed to store BW buffer for grayscale pass");
    return true;
  }

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Compose PNG: failed to rebuild page for grayscale LSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Compose PNG: failed to rebuild page for grayscale MSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  // v18.9.9.305: see sleepCycleLastDrewGrayscale doc block. Composed PNG
  // over reader page also leaves LSB/MSB planes lit; the next tap-cycle
  // must scrub via HALF_REFRESH.
  sleepCycleLastDrewGrayscale = 1;
  return true;
}

void SleepActivity::renderOverlaySleepScreen() const {
  // Overlay pictures always use portrait orientation regardless of the reader's orientation preference.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& path = APP_STATE.openEpubPath;

  auto renderSavedReaderPage = [&]() -> bool {
    if (path.empty()) {
      return false;
    }

    if (FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch")) {
      return XtcReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".txt")) {
      return TxtReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".epub")) {
      return EpubReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    return false;
  };
  const bool backgroundSupportsGrayscale =
      FsHelpers::checkFileExtension(path, ".txt") || FsHelpers::checkFileExtension(path, ".epub");
  bool backgroundWasRebuilt = false;

  // Step 1: Ensure the frame buffer contains only the reader page.
  // When sleeping from the reader, restore the page snapshot taken before the
  // popup was drawn. Otherwise, rebuild from the saved position.
  if (overlayPageBufferTrusted) {
    renderer.restoreBwBuffer();
  } else if (canSnapshotOverlayBackground && !path.empty()) {
    // Only rebuild the reader page as the overlay background when we slept from
    // a book. From the home carousel (or anywhere else) the last-opened book's
    // page must not be pulled in — it reads as a "zoomed-in home screen". Fall
    // through to a clean white background below instead.
    backgroundWasRebuilt = renderSavedReaderPage();

    if (!backgroundWasRebuilt) {
      if (overlayPageBufferStored) {
        LOG_DBG("SLP", "Page re-render failed, using captured screen as overlay fallback");
        renderer.restoreBwBuffer();
      } else {
        LOG_DBG("SLP", "Page re-render failed, using white background");
        renderer.clearScreen();
      }
    }
  } else {
    // Not sleeping from a book: blank background behind the overlay image.
    renderer.clearScreen();
  }

  // Remove the live battery strip from the preserved/reconstructed reader page so the
  // overlay sleep screen still shows chapter/progress details without the battery glance target.
  hideOverlayBatteryStrip(renderer);

  // Step 2: Load the overlay image using the same selection logic as renderCustomSleepScreen.
  // BMP: white pixels are skipped (transparent via drawBitmap), black pixels composited on top.
  // PNG: pixels with alpha < 128 are skipped; opaque pixels are drawn with their grayscale value.
  auto tryDrawOverlay = [&](const std::string& filename) -> OverlayDrawResult {
    FsFile file;
    if (!Storage.openFileForRead("SLP", filename, file)) {
      if (Storage.exists(filename.c_str())) {
        LOG_ERR("SLP", "BMP overlay exists but could not be opened: %s", filename.c_str());
        return OverlayDrawResult::Failed;
      }
      LOG_DBG("SLP", "BMP overlay not found: %s", filename.c_str());
      return OverlayDrawResult::NotFound;
    }
    Bitmap bitmap(file, true);
    const BmpReaderError parseResult = bitmap.parseHeaders();
    if (parseResult != BmpReaderError::Ok) {
      LOG_ERR("SLP", "BMP overlay header parse failed for %s: %s", filename.c_str(),
              Bitmap::errorToString(parseResult));
      file.close();
      return OverlayDrawResult::Failed;
    }

    int x, y;
    float cropX = 0, cropY = 0;
    if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
      float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
      if (ratio > screenRatio) {
        x = 0;
        y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      } else {
        x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        y = 0;
      }
    } else {
      x = (pageWidth - bitmap.getWidth()) / 2;
      y = (pageHeight - bitmap.getHeight()) / 2;
    }

    // Draw without clearScreen so the reader page remains in the frame buffer beneath
    LOG_INF("SLP", "Drawing BMP overlay: %s", filename.c_str());
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    file.close();
    return OverlayDrawResult::Drawn;
  };

  auto tryDrawPngOverlay = [&](const std::string& filename) -> OverlayDrawResult {
    if (!Storage.exists(filename.c_str())) {
      LOG_DBG("SLP", "PNG overlay not found: %s", filename.c_str());
      return OverlayDrawResult::NotFound;
    }
    LOG_INF("SLP", "Drawing PNG overlay: %s", filename.c_str());
    // Reuse the shared sleep-screen PNG decoder. For overlay we deliberately
    // do not clear the framebuffer beforehand — the reader page stays under
    // transparent regions of the PNG.
    return decodeSleepPngToBuffer(renderer, filename, pageWidth, pageHeight) ? OverlayDrawResult::Drawn
                                                                              : OverlayDrawResult::Failed;
  };

  bool overlayDrawn = false;
  bool overlayCandidateFailed = false;
  SleepImageSelection selection;
  auto trySelectedOverlay = [&](const SleepImageSelection& image) {
    LOG_INF("SLP", "Selected overlay image: %s", image.path.c_str());
    const OverlayDrawResult result = image.isPng ? tryDrawPngOverlay(image.path) : tryDrawOverlay(image.path);
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  };

  if (selectPinnedSleepImage(SleepImageMode::Overlay, selection)) {
    trySelectedOverlay(selection);
  }
  if (!overlayDrawn && selectRandomSleepImage(SleepImageMode::Overlay, selection)) {
    trySelectedOverlay(selection);
  }

  if (!overlayDrawn) {
    const OverlayDrawResult result = tryDrawOverlay("/sleep.bmp");
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  }
  if (!overlayDrawn) {
    const OverlayDrawResult result = tryDrawPngOverlay("/sleep.png");
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  }

  if (!overlayDrawn) {
    if (overlayCandidateFailed) {
      LOG_ERR("SLP", "Overlay image was found but could not be drawn; falling back to default sleep screen");
      renderer.setOrientation(savedOrientation);
      return renderDefaultSleepScreen();
    }
    LOG_DBG("SLP", "No overlay image found, displaying page without overlay");
  }

  renderer.setOrientation(savedOrientation);
  // v18.9.9.346: gate grayscale pass on whether the reader's last render
  // actually needed it. Previously we ran it whenever the underlying
  // page was rebuilt OR the BW backup was trusted, but for a text-only
  // (or highlight-only) page the extra LSB/MSB rebuild + gray display
  // is two additional full-screen refreshes with no visible benefit --
  // the triple-flash bug on highlighted pages. On a fresh boot (no
  // reader render this session, e.g. sleep-cycle from cold boot) the
  // flag defaults to false; we still run the pass when backgroundWasRebuilt
  // to preserve the safe default for un-flagged rebuild paths.
  const bool readerFlaggedGrayscale = APP_STATE.lastReaderPageNeededGrayscale;
  const bool shouldRunGrayscalePass =
      backgroundSupportsGrayscale && readerFlaggedGrayscale &&
      (backgroundWasRebuilt || (overlayPageBufferTrusted && !path.empty()));
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, !shouldRunGrayscalePass && TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);

  if (!shouldRunGrayscalePass) {
    return;
  }

  if (!renderer.storeBwBuffer()) {
    LOG_ERR("SLP", "Overlay: failed to store BW buffer for grayscale pass");
    return;
  }

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Overlay: failed to rebuild page for grayscale LSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Overlay: failed to rebuild page for grayscale MSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  // v18.9.9.305: overlay path also lit the LSB/MSB planes.
  sleepCycleLastDrewGrayscale = 1;
}
