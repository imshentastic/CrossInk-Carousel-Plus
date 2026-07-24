#include "PngOverlayRenderer.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PNGdec.h>

#include <cstdint>
#include <new>

// PNG overlay decode pipeline. Mirrors the heap-tuned sleep-screen PNG path in
// SleepActivity (decodeSleepPngToBuffer) so the file-browser image preview can
// compose a PNG (with transparency) over an already-prepared framebuffer using
// the same scaling/transparency rules. Kept as its own module rather than shared
// with SleepActivity so the sleep path stays untouched; if the decode logic
// changes, update both.

namespace {

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
  // not during open() -- so hasAlpha()/getTransparentColor() are only valid once decode() starts.
  // -2 = not yet read; -1 = no color key; >=0 = 0x00RRGGBB (TRUECOLOR) or low-byte gray.
  int32_t transparentColor;
  PNG* pngObj;  // for lazy-init of transparentColor on first callback
};

// PNGdec file I/O callbacks -- mirror the pattern in PngToFramebufferConverter.cpp.
void* pngOverlayOpen(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}
void pngOverlayClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}
int32_t pngOverlayRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  return f ? f->read(pBuf, len) : 0;
}
int32_t pngOverlaySeek(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// Per-scanline draw callback for PNG overlay compositing.
// Transparent pixels (alpha < 128) are skipped so the background shows through.
// Opaque pixels are drawn in their grayscale brightness (dark -> black, light -> white).
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
      // alpha < 128: transparent -- leave the background pixel intact
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

}  // namespace

namespace PngOverlay {

bool decodeToBuffer(GfxRenderer& renderer, const std::string& filename, int pageWidth, int pageHeight) {
  if (!Storage.exists(filename.c_str())) {
    return false;
  }

  // v18.9.9.294: same rationale as SleepActivity::decodeSleepPngToBuffer -- old
  // 60 KB gate was conservative pre-v285 shrink; actual usage is ~42 KB.
  constexpr size_t MIN_FREE_HEAP = 48 * 1024;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder: %u free, need %u for %s", ESP.getFreeHeap(),
            static_cast<unsigned>(MIN_FREE_HEAP), filename.c_str());
    return false;
  }
  PNG* png = new (std::nothrow) PNG();
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder for %s", filename.c_str());
    return false;
  }

  int rc = png->open(filename.c_str(), pngOverlayOpen, pngOverlayClose, pngOverlayRead, pngOverlaySeek, pngOverlayDraw);
  if (rc != PNG_SUCCESS) {
    delete png;
    LOG_ERR("PNG", "PNG open failed for %s: %d", filename.c_str(), rc);
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
    LOG_ERR("PNG", "PNG decode failed for %s: %d", filename.c_str(), rc);
    return false;
  }
  return true;
}

}  // namespace PngOverlay
