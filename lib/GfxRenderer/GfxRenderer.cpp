#include "GfxRenderer.h"

#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <esp_system.h>  // esp_get_free_heap_size (cached-bitmap budget)
#include <freertos/task.h>

#include <algorithm>
#include <new>

#include "FontCacheManager.h"

namespace {

const char* resolveVisualText(const char* text, std::string& visualBuffer, int paragraphLevel);

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    const uint8_t* bitmap = fd->getBitmap(fontData, glyph, glyphIndex);
    // Distinguish an OOM-skipped glyph (page can't render -> drop BLE) from a
    // benignly-missing glyph.
    if (bitmap == nullptr && fd->consumeOom()) {
      markRenderStarved();
    }
    return bitmap;
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

GfxRenderer::BitmapScratchLock::BitmapScratchLock(const GfxRenderer& renderer) : renderer_(renderer) {
  if (renderer_.bitmapScratchMutex_ == nullptr) {
    LOG_ERR("GFX", "!! Bitmap scratch mutex is not initialized");
    assert(false);
    return;
  }

  const auto takeResult = xSemaphoreTake(renderer_.bitmapScratchMutex_, portMAX_DELAY);
  if (takeResult != pdTRUE) {
    LOG_ERR("GFX", "!! Failed to acquire bitmap scratch mutex");
    assert(false);
    return;
  }

  locked_ = true;
}

GfxRenderer::BitmapScratchLock::~BitmapScratchLock() {
  if (!locked_) return;
  xSemaphoreGive(renderer_.bitmapScratchMutex_);
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    int missed = it->second->buildAdvanceTable(words, includeHyphen, styleMask);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

bool GfxRenderer::releaseSdCardFontForLowMemory(int fontId) const {
  auto it = sdCardFonts_.find(fontId);
  if (it == sdCardFonts_.end()) return false;

  it->second->releaseForLowMemory();
  return true;
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
}

void GfxRenderer::freeBitmapScratchBuffers() {
  free(bitmapScratchOutputRow_);
  bitmapScratchOutputRow_ = nullptr;
  bitmapScratchOutputRowSize_ = 0;

  free(bitmapScratchRowBytes_);
  bitmapScratchRowBytes_ = nullptr;
  bitmapScratchRowBytesSize_ = 0;
}

bool GfxRenderer::bitmapScratchLockHeldByCurrentTask() const {
  if (bitmapScratchMutex_ == nullptr) return false;
  return xSemaphoreGetMutexHolder(bitmapScratchMutex_) == xTaskGetCurrentTaskHandle();
}

bool GfxRenderer::ensureBitmapScratchBuffers(const size_t outputRowSize, const size_t rowBytesSize) const {
  if (!bitmapScratchLockHeldByCurrentTask()) {
    LOG_ERR("GFX", "!! Bitmap scratch buffers used without holding scratch mutex");
    assert(false);
    return false;
  }

  if (outputRowSize > bitmapScratchOutputRowSize_) {
    auto* grownOutput = static_cast<uint8_t*>(realloc(bitmapScratchOutputRow_, outputRowSize));
    if (!grownOutput) {
      LOG_ERR("GFX", "!! Failed to grow BMP output row scratch buffer to %zu bytes", outputRowSize);
      return false;
    }
    bitmapScratchOutputRow_ = grownOutput;
    bitmapScratchOutputRowSize_ = outputRowSize;
  }

  if (rowBytesSize > bitmapScratchRowBytesSize_) {
    auto* grownRowBytes = static_cast<uint8_t*>(realloc(bitmapScratchRowBytes_, rowBytesSize));
    if (!grownRowBytes) {
      LOG_ERR("GFX", "!! Failed to grow BMP row-bytes scratch buffer to %zu bytes", rowBytesSize);
      return false;
    }
    bitmapScratchRowBytes_ = grownRowBytes;
    bitmapScratchRowBytesSize_ = rowBytesSize;
  }

  return true;
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

enum class TextRotation { None, Rotated90CW };

struct SyntheticSolidGlyphMetrics {
  uint16_t advanceX;
  int ascender;
  int left;
  int top;
  int width;
  int height;
};

static void fillRectClipped(const GfxRenderer& renderer, int x, int y, int width, int height, const bool pixelState) {
  if (width <= 0 || height <= 0) return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int x2 = x + width;
  const int y2 = y + height;
  if (x >= screenWidth || y >= screenHeight || x2 <= 0 || y2 <= 0) return;

  const int clippedX = std::max(0, x);
  const int clippedY = std::max(0, y);
  const int clippedX2 = std::min(screenWidth, x2);
  const int clippedY2 = std::min(screenHeight, y2);
  renderer.fillRect(clippedX, clippedY, clippedX2 - clippedX, clippedY2 - clippedY, pixelState);
}

static void drawPixelClipped(const GfxRenderer& renderer, const int x, const int y, const bool pixelState) {
  if (x < 0 || x >= renderer.getScreenWidth() || y < 0 || y >= renderer.getScreenHeight()) return;
  renderer.drawPixel(x, y, pixelState);
}

static SyntheticSolidGlyphMetrics getSyntheticSolidGlyphMetrics(const EpdFontFamily& font,
                                                                const EpdFontFamily::Style style, const uint32_t cp) {
  const EpdFontData* data = font.getData(style);
  const uint16_t advanceX = syntheticGlyph::solidAdvanceX(data, font.getGlyph('M', style));
  const int height = syntheticGlyph::solidHeight(data, cp);
  const int width = syntheticGlyph::solidWidth(cp, advanceX, height);
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return {
      advanceX, ascender, syntheticGlyph::solidLeft(cp, advanceX, width), syntheticGlyph::solidTop(data, cp, height),
      width,    height};
}

static SyntheticSolidGlyphMetrics getSyntheticGreekGlyphMetrics(const EpdFontFamily& font,
                                                                const EpdFontFamily::Style style, const uint32_t cp) {
  const EpdFontData* data = font.getData(style);
  const uint16_t advanceX = syntheticGlyph::greekAdvanceX(data, font.getGlyph('M', style), cp);
  const int height = syntheticGlyph::greekHeight(data, cp);
  const int width = syntheticGlyph::greekWidth(cp, advanceX, height);
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return {
      advanceX, ascender, syntheticGlyph::greekLeft(cp, advanceX, width), syntheticGlyph::greekTop(data, cp, height),
      width,    height};
}

static SyntheticSolidGlyphMetrics getSyntheticReplacementGlyphMetrics(const EpdFontFamily& font,
                                                                      const EpdFontFamily::Style style) {
  const EpdFontData* data = font.getData(style);
  const uint16_t advanceX = syntheticGlyph::replacementAdvanceX(data, font.getGlyph('M', style));
  const int height = syntheticGlyph::replacementHeight(data);
  const int width = syntheticGlyph::replacementWidth(advanceX, height);
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return {advanceX,
          ascender,
          syntheticGlyph::replacementLeft(advanceX, width),
          syntheticGlyph::replacementTop(data, height),
          width,
          height};
}

static void fillSyntheticSolidGlyph(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                    const int cursorX, const int baselineY, const bool pixelState) {
  fillRectClipped(renderer, cursorX + metrics.left, baselineY - metrics.top, metrics.width, metrics.height, pixelState);
}

static void fillSyntheticSolidGlyphRotated90CW(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                               const int cursorX, const int cursorY, const bool pixelState) {
  fillRectClipped(renderer, cursorX + metrics.ascender - metrics.top, cursorY - metrics.left - metrics.width + 1,
                  metrics.height, metrics.width, pixelState);
}

static int syntheticStroke(const SyntheticSolidGlyphMetrics& metrics) {
  const int minDim = metrics.width < metrics.height ? metrics.width : metrics.height;
  return minDim >= 14 ? 2 : 1;
}

static bool epsilonTemplatePixel(const int srcX, const int srcY) {
  static constexpr uint8_t EPSILON_7X7[] = {
      0b0111110, 0b1100000, 0b1000000, 0b1111100, 0b1000000, 0b1100000, 0b0111110,
  };
  return (EPSILON_7X7[srcY] & (1 << (6 - srcX))) != 0;
}

static bool questionTemplatePixel(const int srcX, const int srcY) {
  static constexpr uint8_t QUESTION_7X9[] = {
      0b0011100, 0b0100010, 0b0000010, 0b0000100, 0b0001000, 0b0001000, 0b0000000, 0b0001000, 0b0001000,
  };
  return (QUESTION_7X9[srcY] & (1 << (6 - srcX))) != 0;
}

static void drawSyntheticReplacementGlyph(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                          const int cursorX, const int baselineY, const bool pixelState) {
  const int x = cursorX + metrics.left;
  const int y = baselineY - metrics.top;
  const int w = metrics.width;
  const int h = metrics.height;
  if (w <= 0 || h <= 0) return;

  const int limit = w < h ? w : h;
  for (int gy = 0; gy < h; gy++) {
    for (int gx = 0; gx < w; gx++) {
      const int diamondX = abs((2 * gx + 1) - w);
      const int diamondY = abs((2 * gy + 1) - h);
      if (diamondX + diamondY <= limit) {
        drawPixelClipped(renderer, x + gx, y + gy, pixelState);
      }
    }
  }

  if (w < 7 || h < 9) return;
  const int qW = w > 10 ? 7 : 5;
  const int qH = h > 12 ? 9 : 7;
  const int qX = x + (w - qW) / 2;
  const int qY = y + (h - qH) / 2;
  for (int gy = 0; gy < qH; gy++) {
    for (int gx = 0; gx < qW; gx++) {
      if (questionTemplatePixel(gx * 7 / qW, gy * 9 / qH)) {
        drawPixelClipped(renderer, qX + gx, qY + gy, !pixelState);
      }
    }
  }
}

static void drawSyntheticReplacementGlyphRotated90CW(const GfxRenderer& renderer,
                                                     const SyntheticSolidGlyphMetrics& metrics, const int cursorX,
                                                     const int cursorY, const bool pixelState) {
  const int baseX = cursorX + metrics.ascender - metrics.top;
  const int baseY = cursorY - metrics.left;
  const int w = metrics.width;
  const int h = metrics.height;
  if (w <= 0 || h <= 0) return;

  const int limit = w < h ? w : h;
  for (int gy = 0; gy < h; gy++) {
    for (int gx = 0; gx < w; gx++) {
      const int diamondX = abs((2 * gx + 1) - w);
      const int diamondY = abs((2 * gy + 1) - h);
      if (diamondX + diamondY <= limit) {
        drawPixelClipped(renderer, baseX + gy, baseY - gx, pixelState);
      }
    }
  }

  if (w < 7 || h < 9) return;
  const int qW = w > 10 ? 7 : 5;
  const int qH = h > 12 ? 9 : 7;
  const int qLeft = (w - qW) / 2;
  const int qTop = (h - qH) / 2;
  for (int gy = 0; gy < qH; gy++) {
    for (int gx = 0; gx < qW; gx++) {
      if (questionTemplatePixel(gx * 7 / qW, gy * 9 / qH)) {
        drawPixelClipped(renderer, baseX + qTop + gy, baseY - (qLeft + gx), !pixelState);
      }
    }
  }
}

static void drawSyntheticGreekGlyph(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                    const uint32_t cp, const int cursorX, const int baselineY, const bool pixelState) {
  const int x = cursorX + metrics.left;
  const int y = baselineY - metrics.top;
  const int w = metrics.width;
  const int h = metrics.height;
  const int s = syntheticStroke(metrics);
  if (w <= 0 || h <= 0) return;

  if (cp == syntheticGlyph::GREEK_CAPITAL_GAMMA) {
    fillRectClipped(renderer, x, y, s, h, pixelState);
    fillRectClipped(renderer, x, y, w, s, pixelState);
  } else if (cp == syntheticGlyph::GREEK_SMALL_EPSILON) {
    for (int gy = 0; gy < h; gy++) {
      const int srcY = gy * 7 / h;
      for (int gx = 0; gx < w; gx++) {
        if (epsilonTemplatePixel(gx * 7 / w, srcY)) drawPixelClipped(renderer, x + gx, y + gy, pixelState);
      }
    }
  } else if (cp == syntheticGlyph::GREEK_SMALL_OMEGA) {
    const int mid = w / 2;
    fillRectClipped(renderer, x, y + s, s, h - 2 * s, pixelState);
    fillRectClipped(renderer, x + w - s, y + s, s, h - 2 * s, pixelState);
    fillRectClipped(renderer, x + s, y + h - s, mid - s, s, pixelState);
    fillRectClipped(renderer, x + mid, y + h - s, w - mid - s, s, pixelState);
    fillRectClipped(renderer, x + mid - s / 2, y + h / 2, s, h / 2, pixelState);
  }
}

static void drawSyntheticGreekGlyphRotated90CW(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                               const uint32_t cp, const int cursorX, const int cursorY,
                                               const bool pixelState) {
  const int baseX = cursorX + metrics.ascender - metrics.top;
  const int baseY = cursorY - metrics.left;
  const int s = syntheticStroke(metrics);
  for (int gy = 0; gy < metrics.height; gy++) {
    for (int gx = 0; gx < metrics.width; gx++) {
      bool draw = false;
      if (cp == syntheticGlyph::GREEK_CAPITAL_GAMMA) {
        draw = gx < s || gy < s;
      } else if (cp == syntheticGlyph::GREEK_SMALL_EPSILON) {
        draw = epsilonTemplatePixel(gx * 7 / metrics.width, gy * 7 / metrics.height);
      } else if (cp == syntheticGlyph::GREEK_SMALL_OMEGA) {
        draw = (gx < s && gy >= s && gy < metrics.height - s) ||
               (gx >= metrics.width - s && gy >= s && gy < metrics.height - s) ||
               (gy >= metrics.height - s && gx >= s && gx < metrics.width - s) ||
               (gx >= metrics.width / 2 - s / 2 && gx < metrics.width / 2 - s / 2 + s && gy >= metrics.height / 2);
      }
      if (draw) drawPixelClipped(renderer, baseX + gy, baseY - gx, pixelState);
    }
  }
}

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const auto glyphData = fontFamily.getGlyphData(cp, style);
  const EpdGlyph* glyph = glyphData.glyph;
  const EpdFontData* fontData = glyphData.fontData;
  if (!glyph || !fontData) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // Dedicated X3 gray LUTs now provide proper 4-level gray on both devices
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            renderer.drawPixel(screenX, screenY, false);
          } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            renderer.drawPixel(screenX, screenY, false);
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  // SD-card fonts can measure from their persistent advance table during layout.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      widthFP += sdIt->second->getAdvance(cp, styleIdx);
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, combiningGlyph->left,
                                                       combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);
    cp = font.getFallbackCodepoint(cp, style);
    const bool hasRealGlyph = font.findGlyphData(cp, style).glyph != nullptr;

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    if (!hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      const auto metrics = getSyntheticSolidGlyphMetrics(font, style, cp);
      fillSyntheticSolidGlyph(*this, metrics, lastBaseX, yPos, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      const auto metrics = getSyntheticGreekGlyphMetrics(font, style, cp);
      drawSyntheticGreekGlyph(*this, metrics, cp, lastBaseX, yPos, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      const auto metrics = getSyntheticReplacementGlyphMetrics(font, style);
      drawSyntheticReplacementGlyph(*this, metrics, lastBaseX, yPos, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    if (!glyph) {
      // Advance was already flushed into lastBaseX above; clear base metrics so the
      // next character does not kern or attach to stale state.
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point

    renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    prevCp = cp;
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  const int sw = getScreenWidth();
  const int sh = getScreenHeight();
  if (x1 == x2) {
    if (x1 < 0 || x1 >= sw) return;
    if (y2 < y1) std::swap(y1, y2);
    y1 = std::max(y1, 0);
    y2 = std::min(y2, sh - 1);
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (y1 < 0 || y1 >= sh) return;
    if (x2 < x1) std::swap(x1, x2);
    x1 = std::max(x1, 0);
    x2 = std::min(x2, sw - 1);
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle.
// rhythmerc fix: each edge needs `width - 1 - i` (not `width - i`) so the
// lineWidth==1 case matches the single-pixel drawRect overload above and
// every pixel of the border lands strictly inside the (x,y,width,height)
// box. The old form drew one row/column outside the box on the right and
// bottom edges, producing an asymmetric 1px overhang.
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - 1 - i, y + i, state);
    drawLine(x + width - 1 - i, y + i, x + width - 1 - i, y + height - 1 - i, state);
    drawLine(x + width - 1 - i, y + height - 1 - i, x + i, y + height - 1 - i, state);
    drawLine(x + i, y + height - 1 - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    if (width > 0) {
      fillRect(left, py, width, 1, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

// CrumBLE Phase 2: dispatch into the byte-aligned template. `state=true`
// means "draw black" (matches drawPixel's convention). The implementation
// lives in fillRectImpl<Color> below.
void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  if (state) {
    fillRectImpl<Color::Black>(x, y, width, height);
  } else {
    fillRectImpl<Color::White>(x, y, width, height);
  }
}

template <Color C>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  if constexpr (C == Color::Clear) return;
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max(0, x);
  const int ly0 = std::max(0, y);
  const int lx1 = std::min(screenW, x + width);
  const int ly1 = std::min(screenH, y + height);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // Rotate the two opposing logical corners into physical-framebuffer space.
  // Rotation is rigid (no shear/stretch) so the bbox of the two corners IS
  // the physical rect we need to fill.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  const int phyY0 = std::min(paY, pbY);
  const int phyY1 = std::max(paY, pbY);

  // Bit/byte layout: MSB-first within a byte, so phyX -> bit (7 - (phyX & 7)).
  // Head/tail masks cover only the in-rect bits of the first/last byte.
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  if constexpr (C == Color::Black || C == Color::White) {
    // Solid fill. Framebuffer convention: bit 0 = black, bit 1 = white.
    const uint8_t fillByte = (C == Color::Black) ? 0x00u : 0xFFu;
    for (int py = phyY0; py <= phyY1; ++py) {
      uint8_t* row = frameBuffer + static_cast<int32_t>(py) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t mask = headMask & tailMask;
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~mask);
        } else {
          row[byteStart] |= mask;
        }
      } else {
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~headMask);
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] &= static_cast<uint8_t>(~tailMask);
        } else {
          row[byteStart] |= headMask;
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] |= tailMask;
        }
      }
    }
  } else {
    // Dither (LightGray / DarkGray). Pattern has period 2 in logical (x, y).
    // Within one physical row the dither byte is constant -- precompute it
    // once and memset it across the row's full bytes. dlxPerPhyX /
    // dlyPerPhyX encode how logical (x, y) change as phyX increments
    // (derived from inverting rotateCoordinates).
    int dlxPerPhyX = 0, dlyPerPhyX = 0;
    switch (orientation) {
      case Portrait:                  dlxPerPhyX =  0; dlyPerPhyX =  1; break;
      case PortraitInverted:          dlxPerPhyX =  0; dlyPerPhyX = -1; break;
      case LandscapeClockwise:        dlxPerPhyX = -1; dlyPerPhyX =  0; break;
      case LandscapeCounterClockwise: dlxPerPhyX =  1; dlyPerPhyX =  0; break;
    }

    for (int py = phyY0; py <= phyY1; ++py) {
      // Logical (x, y) at bit-7 of the row's starting byte (phyX = byteStart*8).
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - py;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = py;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - py;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = py;
          break;
      }

      // Build the row's "black bitmask" by walking 8 bit positions.
      uint8_t blackMask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else if constexpr (C == Color::DarkGray) {
          isBlack = (((lx + ly) & 1) == 0);
        } else {  // VeryDarkGray -- complement of LightGray (3-of-4 coverage)
          isBlack = !(((lx & 1) == 0) && ((ly & 1) == 0));
        }
        if (isBlack) blackMask |= static_cast<uint8_t>(1u << (7 - b));
      }
      const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // The legacy dither path called drawPixel for every pixel, setting
      // both inks. We match that: inside the row's rect mask, write
      // whiteMask (1 = white, 0 = black). Outside the mask, untouched.
      uint8_t* row = frameBuffer + static_cast<int32_t>(py) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t rectMask = headMask & tailMask;
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~rectMask) | (rectMask & whiteMask));
      } else {
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (headMask & whiteMask));
        if (byteEnd > byteStart + 1) {
          memset(row + byteStart + 1, whiteMask, byteEnd - byteStart - 1);
        }
        row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (tailMask & whiteMask));
      }
    }
  }
}

// Explicit instantiations -- callers in other TUs can dispatch by Color at
// runtime through fillRect / fillRectDither.
template void GfxRenderer::fillRectImpl<Color::Black>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::White>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::LightGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::DarkGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::VeryDarkGray>(int, int, int, int) const;

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

template <>
void GfxRenderer::drawPixelDither<Color::VeryDarkGray>(const int x, const int y) const {
  // Inverse of LightGray's 2x2 mask -- everywhere EXCEPT (x even AND y
  // even) is black, giving ~75% coverage.
  drawPixel(x, y, !(x % 2 == 0 && y % 2 == 0));
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  // CrumBLE Phase 2: dispatch into fillRectImpl<Color> for every color
  // (including the solid Black/White cases, which previously delegated to
  // fillRect anyway). Clear short-circuits.
  switch (color) {
    case Color::Clear:                                                            break;
    case Color::Black:        fillRectImpl<Color::Black>(x, y, width, height);        break;
    case Color::White:        fillRectImpl<Color::White>(x, y, width, height);        break;
    case Color::LightGray:    fillRectImpl<Color::LightGray>(x, y, width, height);    break;
    case Color::DarkGray:     fillRectImpl<Color::DarkGray>(x, y, width, height);     break;
    case Color::VeryDarkGray: fillRectImpl<Color::VeryDarkGray>(x, y, width, height); break;
  }
}

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::VeryDarkGray) {
          drawPixelDither<Color::VeryDarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::VeryDarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::VeryDarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::VeryDarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // rhythmerc fast path: cornerRadius == 0 bypasses the roundedSides / maxRadius
  // math entirely and degenerates to a flat fill. The maxRadius<=0 fallback
  // below still catches cases where w/h are tiny enough to clamp the radius
  // to zero, but the explicit-0 hot path is what most flat-rect callers hit.
  if (cornerRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::VeryDarkGray:
        fillArc<Color::VeryDarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Portrait-mode coordinate transform (x<->y swap). Draw black source pixels
  // transparently while preserving sub-byte x offsets; EInkDisplay's transparent
  // blit is byte-aligned, which can visually shift icons when y is not /8.
  const int physX = y;
  const int physY = getScreenWidth() - width - x;
  const int imgW = height;
  const int imgH = width;
  const int srcStride = (imgW + 7) / 8;

  if (physX + imgW <= 0 || physX >= panelWidth) return;
  if (physY + imgH <= 0 || physY >= panelHeight) return;

  const int baseByte = (physX >= 0) ? (physX >> 3) : -(((-physX) + 7) >> 3);
  const int bitShift = ((physX % 8) + 8) % 8;
  const int trail = srcStride * 8 - imgW;
  const uint8_t trailMask = static_cast<uint8_t>(0xFF << trail);
  const int lastCol = srcStride - 1;

  for (int row = 0; row < imgH; ++row) {
    const int destY = physY + row;
    if (destY < 0 || destY >= panelHeight) continue;
    const int rowBase = destY * panelWidthBytes;
    const int srcOffset = row * srcStride;

    if (bitShift == 0) {
      for (int col = 0; col < srcStride; ++col) {
        const int dst = baseByte + col;
        if (dst < 0) continue;
        if (dst >= panelWidthBytes) break;
        uint8_t blackPixels = ~bitmap[srcOffset + col];
        if (col == lastCol && trail > 0) blackPixels &= trailMask;
        frameBuffer[rowBase + dst] &= static_cast<uint8_t>(~blackPixels);
      }
    } else {
      const int rsh = bitShift;
      const int lsh = 8 - bitShift;
      for (int col = 0; col < srcStride; ++col) {
        uint8_t blackPixels = ~bitmap[srcOffset + col];
        if (col == lastCol && trail > 0) blackPixels &= trailMask;
        const int dstHi = baseByte + col;
        const int dstLo = dstHi + 1;
        if (dstHi >= 0 && dstHi < panelWidthBytes) {
          frameBuffer[rowBase + dstHi] &= static_cast<uint8_t>(~static_cast<uint8_t>(blackPixels >> rsh));
        }
        if (dstLo >= 0 && dstLo < panelWidthBytes) {
          frameBuffer[rowBase + dstLo] &= static_cast<uint8_t>(~static_cast<uint8_t>(blackPixels << lsh));
        }
      }
    }
  }
}

void GfxRenderer::drawIconInverted(const uint8_t bitmap[], const int x, const int y, const int width,
                                   const int height) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Portrait-mode coordinate transform (x↔y swap), matching drawIcon.
  // OR with ~srcByte sets framebuffer bits to 1 (white) wherever the icon
  // bitmap is 0 (black) — produces a white icon on a black background.
  const int physX = y;
  const int physY = getScreenWidth() - width - x;
  const int imgW = height;  // dimensions swapped by portrait transform
  const int imgH = width;
  const int srcStride = (imgW + 7) / 8;  // round up so non-byte-aligned widths copy fully

  // Trivial off-screen rejection.
  if (physX + imgW <= 0 || physX >= panelWidth) return;
  if (physY + imgH <= 0 || physY >= panelHeight) return;

  // Floor-divide so a negative physX produces the correct (negative) base byte;
  // C++ integer division truncates toward zero, which would round up for negatives.
  const int baseByte = (physX >= 0) ? (physX >> 3) : -(((-physX) + 7) >> 3);
  const int bitShift = ((physX % 8) + 8) % 8;  // 0..7

  // Strip spurious low bits in the trailing source byte when imgW is not a
  // multiple of 8 — without this, ~bitmap would set them to 1 and bleed extra
  // white pixels past the icon's right edge.
  const int trail = srcStride * 8 - imgW;
  const uint8_t trailMask = static_cast<uint8_t>(0xFF << trail);
  const int lastCol = srcStride - 1;

  for (int row = 0; row < imgH; ++row) {
    const int destY = physY + row;
    if (destY < 0 || destY >= panelHeight) continue;
    const int rowBase = destY * panelWidthBytes;
    const int srcOffset = row * srcStride;

    if (bitShift == 0) {
      for (int col = 0; col < srcStride; ++col) {
        const int dst = baseByte + col;
        if (dst < 0) continue;
        if (dst >= panelWidthBytes) break;
        uint8_t inv = ~bitmap[srcOffset + col];
        if (col == lastCol && trail > 0) inv &= trailMask;
        frameBuffer[rowBase + dst] |= inv;
      }
    } else {
      const int rsh = bitShift;
      const int lsh = 8 - bitShift;
      for (int col = 0; col < srcStride; ++col) {
        uint8_t inv = ~bitmap[srcOffset + col];
        if (col == lastCol && trail > 0) inv &= trailMask;
        const int dstHi = baseByte + col;
        const int dstLo = dstHi + 1;
        if (dstHi >= 0 && dstHi < panelWidthBytes) {
          frameBuffer[rowBase + dstHi] |= static_cast<uint8_t>(inv >> rsh);
        }
        if (dstLo >= 0 && dstLo < panelWidthBytes) {
          frameBuffer[rowBase + dstLo] |= static_cast<uint8_t>(inv << lsh);
        }
      }
    }
  }
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  BitmapScratchLock scratchLock(*this);
  if (!scratchLock.isLocked()) return;

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  if (!ensureBitmapScratchBuffers(outputRowSize, bitmap.getRowBytes())) {
    return;
  }
  auto* outputRow = bitmapScratchOutputRow_;
  auto* rowBytes = bitmapScratchRowBytes_;

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  BitmapScratchLock scratchLock(*this);
  if (!scratchLock.isLocked()) return;

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  if (!ensureBitmapScratchBuffers(outputRowSize, bitmap.getRowBytes())) {
    return;
  }
  auto* outputRow = bitmapScratchOutputRow_;
  auto* rowBytes = bitmapScratchRowBytes_;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }
}

// ─── Cached-bitmap path (CrumBLE Phase 1) ─────────────────────────────────
// Ported from rhythmerc/crosspoint-reader (commits 2df82e2, 70f7c43, d386fab,
// a77ebeb). Caches decoded BMPs in RAM keyed by path; subsequent paints at
// the same target size blit straight from a pre-scaled 1bpp buffer. Heap
// budget adjusts dynamically based on free heap so BLE coexistence stays
// safe (reconcileImageCacheBudget()).

void GfxRenderer::reconcileImageCacheBudget() const {
  // Tiered budget: full cache when memory is fat, shrunk when NimBLE eats
  // ~58 KB, off entirely when the chapter / book also demands big buffers.
  // Numbers are based on the standing CrumBLE heap envelope (~197 KB free
  // fresh, ~140 KB with BLE on). See the design discussion in #90.
  const uint32_t freeHeap = esp_get_free_heap_size();
  size_t newBudget;
  if (freeHeap >= 120u * 1024u) {
    newBudget = 64u * 1024u;
  } else if (freeHeap >= 80u * 1024u) {
    newBudget = 16u * 1024u;
  } else {
    newBudget = 0u;
  }
  if (newBudget != imageCacheBudget_) {
    imageCacheBudget_ = newBudget;
    evictImageCacheToBudget();
  }
}

void GfxRenderer::evictImageCacheToBudget() const {
  while (imageCacheBytes_ > imageCacheBudget_ && !imageCache_.empty()) {
    auto victim = imageCache_.begin();
    for (auto it = imageCache_.begin(); it != imageCache_.end(); ++it) {
      if (it->second.lastUsedTick < victim->second.lastUsedTick) victim = it;
    }
    imageCacheBytes_ -= victim->second.pixelsBytes + victim->second.scaledPixelsBytes;
    imageCache_.erase(victim);
  }
}

void GfxRenderer::clearImageCache() const {
  imageCache_.clear();
  imageCacheBytes_ = 0;
}

GfxRenderer::CachedBitmap* GfxRenderer::lookupCachedBitmap(const char* path) const {
  if (path == nullptr || path[0] == '\0') return nullptr;

  // Hit: bump LRU tick and return the existing entry.
  auto it = imageCache_.find(path);
  if (it != imageCache_.end()) {
    it->second.lastUsedTick = ++imageCacheTick_;
    return &it->second;
  }

  // Miss: open + decode + insert. Pre-flight the heap budget so the new
  // decode doesn't push us past what BLE / the reader need.
  reconcileImageCacheBudget();
  if (imageCacheBudget_ == 0) return nullptr;

  FsFile file;
  if (!Storage.openFileForRead("GFXC", path, file)) {
    LOG_DBG("GFX", "Cached bitmap miss: file open failed: %s", path);
    return nullptr;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    LOG_DBG("GFX", "Cached bitmap: parse failed: %s", path);
    file.close();
    return nullptr;
  }

  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  const int srcStride = (width + 3) / 4;  // 2bpp packed
  const size_t bufBytes = static_cast<size_t>(srcStride) * static_cast<size_t>(height);

  // Refuse to insert anything that would blow the budget even after a full
  // eviction. Spares us a big alloc + free dance for a doomed insert.
  if (bufBytes > imageCacheBudget_) {
    LOG_DBG("GFX", "Cached bitmap: %u bytes exceeds budget %u; not caching %s",
            static_cast<unsigned>(bufBytes), static_cast<unsigned>(imageCacheBudget_), path);
    file.close();
    return nullptr;
  }
  // Evict LRU until the new entry fits.
  while (imageCacheBytes_ + bufBytes > imageCacheBudget_ && !imageCache_.empty()) {
    auto victim = imageCache_.begin();
    for (auto candidate = imageCache_.begin(); candidate != imageCache_.end(); ++candidate) {
      if (candidate->second.lastUsedTick < victim->second.lastUsedTick) victim = candidate;
    }
    imageCacheBytes_ -= victim->second.pixelsBytes + victim->second.scaledPixelsBytes;
    imageCache_.erase(victim);
  }

  std::unique_ptr<uint8_t[]> pixels(new (std::nothrow) uint8_t[bufBytes]);
  if (!pixels) {
    LOG_ERR("GFX", "Cached bitmap: pixels OOM (%u bytes) for %s", static_cast<unsigned>(bufBytes), path);
    file.close();
    return nullptr;
  }

  // Walk every row through readNextRow into the packed 2bpp buffer.
  // BitmapScratchLock guards the shared rowBytes / outputRow temporaries.
  BitmapScratchLock lock(*this);
  if (!lock.isLocked()) {
    LOG_ERR("GFX", "Cached bitmap: scratch lock contention for %s", path);
    file.close();
    return nullptr;
  }
  if (!ensureBitmapScratchBuffers(srcStride, bitmap.getRowBytes())) {
    LOG_ERR("GFX", "Cached bitmap: scratch alloc failed for %s", path);
    file.close();
    return nullptr;
  }
  for (int row = 0; row < height; ++row) {
    if (bitmap.readNextRow(bitmapScratchOutputRow_, bitmapScratchRowBytes_) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Cached bitmap: row %d read failed for %s", row, path);
      file.close();
      return nullptr;
    }
    memcpy(pixels.get() + row * srcStride, bitmapScratchOutputRow_, srcStride);
  }
  file.close();

  CachedBitmap entry;
  entry.pixels = std::move(pixels);
  entry.pixelsBytes = bufBytes;
  entry.width = width;
  entry.height = height;
  entry.topDown = bitmap.isTopDown();
  entry.lastUsedTick = ++imageCacheTick_;

  auto [inserted, ok] = imageCache_.emplace(path, std::move(entry));
  if (!ok) return nullptr;
  imageCacheBytes_ += bufBytes;
  return &inserted->second;
}

bool GfxRenderer::getCachedBitmapDimensions(CachedBitmap* handle, int* outWidth, int* outHeight) const {
  if (handle == nullptr || !handle->pixels) return false;
  if (outWidth) *outWidth = handle->width;
  if (outHeight) *outHeight = handle->height;
  return true;
}

void GfxRenderer::buildScaledBitmap(CachedBitmap* entry, const int targetW, const int targetH,
                                    const float cropX, const float cropY) const {
  if (entry == nullptr) return;
  if (entry->scaledPixels) {
    imageCacheBytes_ -= entry->scaledPixelsBytes;
    entry->scaledPixels.reset();
    entry->scaledPixelsBytes = 0;
  }

  const int scaledStride = (targetW + 7) / 8;
  const size_t scaledBytes = static_cast<size_t>(scaledStride) * static_cast<size_t>(targetH);

  std::unique_ptr<uint8_t[]> scaled(new (std::nothrow) uint8_t[scaledBytes]);
  if (!scaled) {
    LOG_ERR("GFX", "Scaled bitmap OOM: %u bytes", static_cast<unsigned>(scaledBytes));
    entry->scaledWidth = 0;
    entry->scaledHeight = 0;
    entry->scaledCropX = 0.0f;
    entry->scaledCropY = 0.0f;
    return;
  }
  // 0xFF = "all white" in our 1bpp scheme (bit 1 = white). We OR in zeros
  // for source-black pixels.
  memset(scaled.get(), 0xFF, scaledBytes);

  const int srcStride = (entry->width + 3) / 4;
  // CrumBLE: aspect-fill crop support. cropX/cropY (0.0-1.0) trim the
  // source's long axis evenly from both sides before scaling, so the
  // scaled output exactly fills (targetW x targetH) without
  // letterboxing. cropX=0, cropY=0 (default) reproduces the legacy
  // aspect-fit behaviour: full source mapped 1:1 into target.
  const float clampedCropX = std::clamp(cropX, 0.0f, 0.99f);
  const float clampedCropY = std::clamp(cropY, 0.0f, 0.99f);
  const float visibleSrcW = static_cast<float>(entry->width) * (1.0f - clampedCropX);
  const float visibleSrcH = static_cast<float>(entry->height) * (1.0f - clampedCropY);
  const float srcXOffset = static_cast<float>(entry->width) * clampedCropX * 0.5f;
  const float srcYOffset = static_cast<float>(entry->height) * clampedCropY * 0.5f;
  const float xRatio = visibleSrcW / static_cast<float>(targetW);
  const float yRatio = visibleSrcH / static_cast<float>(targetH);

  for (int ty = 0; ty < targetH; ++ty) {
    const int srcRenderY = static_cast<int>(srcYOffset + ty * yRatio);
    const int srcRow = entry->topDown ? srcRenderY : (entry->height - 1 - srcRenderY);
    const int clampedRow = std::clamp(srcRow, 0, entry->height - 1);
    const uint8_t* srcRowPtr = entry->pixels.get() + clampedRow * srcStride;
    uint8_t* dstRowPtr = scaled.get() + ty * scaledStride;
    for (int tx = 0; tx < targetW; ++tx) {
      const int srcX = std::clamp(static_cast<int>(srcXOffset + tx * xRatio), 0, entry->width - 1);
      // 2bpp packed pixel: 4 pixels per source byte, MSB first.
      const uint8_t val = (srcRowPtr[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;
      if (val < 3) {  // 0-2 = darker than full white; render as black
        dstRowPtr[tx / 8] &= static_cast<uint8_t>(~(1u << (7 - (tx % 8))));
      }
    }
  }

  entry->scaledPixels = std::move(scaled);
  entry->scaledPixelsBytes = scaledBytes;
  entry->scaledWidth = targetW;
  entry->scaledHeight = targetH;
  entry->scaledCropX = clampedCropX;
  entry->scaledCropY = clampedCropY;
  imageCacheBytes_ += scaledBytes;
}

template <bool Opaque>
bool GfxRenderer::drawCachedBitmap(const char* path, const int x, const int y, const int maxWidth,
                                   const int maxHeight, const float cropX, const float cropY,
                                   const int cornerRadius) const {
  return drawCachedBitmap<Opaque>(lookupCachedBitmap(path), x, y, maxWidth, maxHeight, cropX, cropY, cornerRadius);
}

template <bool Opaque>
bool GfxRenderer::drawCachedBitmap(CachedBitmap* entry, const int x, const int y, const int maxWidth,
                                   const int maxHeight, const float cropX, const float cropY,
                                   const int cornerRadius) const {
  if (entry == nullptr) return false;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return true;  // text-scan pass, no painting

  // (maxWidth, maxHeight) is treated as the EXACT target rect. Caller is
  // responsible for choosing dims that match the source aspect (for
  // letterbox-free aspect-fit) or for passing cropX/cropY to trim the
  // source's long axis (aspect-fill). Skipping the cache's own aspect
  // math keeps the contract simple: scaled output is always exactly
  // (maxWidth, maxHeight).
  const int targetW = maxWidth;
  const int targetH = maxHeight;
  if (targetW <= 0 || targetH <= 0) return false;

  // Cache invalidation also keys on cropX/cropY: a re-render with a
  // different crop at the same target size must rebuild.
  constexpr float kCropEpsilon = 1e-4f;
  const bool cropChanged =
      std::abs(entry->scaledCropX - cropX) > kCropEpsilon || std::abs(entry->scaledCropY - cropY) > kCropEpsilon;
  if (!entry->scaledPixels || entry->scaledWidth != targetW || entry->scaledHeight != targetH || cropChanged) {
    buildScaledBitmap(entry, targetW, targetH, cropX, cropY);
  }
  if (!entry->scaledPixels) return false;

  // Rounded-corner skip table: pixels inside the four [0, r) × [0, r) corner
  // boxes that fall outside the circle (dx² + dy² > rr²) are left untouched.
  // Same maths as maskRoundedRectOutsideCorners, but applied during the
  // blit so the framebuffer underneath (typically a drop shadow) stays
  // visible through the corner.
  constexpr int kMaxCornerR = 32;
  int8_t skipPerRow[kMaxCornerR];
  const int rMax = std::min(kMaxCornerR, std::min(targetW / 2, targetH / 2));
  const int r = std::max(0, std::min(cornerRadius, rMax));
  const bool rounded = r > 0;
  if (rounded) {
    const int rr = r - 1;
    const int rr2 = rr * rr;
    for (int dy = 0; dy < r; ++dy) {
      const int ty = rr - dy;
      const int ty2 = ty * ty;
      int skip = 0;
      while (skip < r) {
        const int tx = rr - skip;
        if (tx * tx + ty2 > rr2) ++skip;
        else break;
      }
      skipPerRow[dy] = static_cast<int8_t>(skip);
    }
  }

  const int scaledStride = (targetW + 7) / 8;
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int sx0 = std::max(0, -x);
  const int sx1 = std::min(targetW, screenW - x);
  const int sy0 = std::max(0, -y);
  const int sy1 = std::min(targetH, screenH - y);
  if (sx0 >= sx1 || sy0 >= sy1) return true;

  int phyX0, phyY0;
  rotateCoordinates(orientation, x + sx0, y + sy0, &phyX0, &phyY0, panelWidth, panelHeight);

  // Per-orientation inner loops. Portrait variants hold phyX constant per
  // source row → hoist byte column + bitMask out of the inner pixel loop;
  // landscape variants hold phyY constant → hoist row base. Each pixel
  // costs an AND + branch instead of recomputing 0x80 >> n.
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  switch (orientation) {
    case Portrait:
    case PortraitInverted: {
      const int dyPerSx = (orientation == Portrait) ? -1 : 1;
      const int dxPerSy = (orientation == Portrait) ? 1 : -1;
      const int32_t byteStep = static_cast<int32_t>(dyPerSx) * panelStride;
      for (int sy = sy0; sy < sy1; ++sy) {
        int rowSx0 = sx0;
        int rowSx1 = sx1;
        if (rounded) {
          int skip = 0;
          if (sy < r) skip = skipPerRow[sy];
          else if (sy >= targetH - r) skip = skipPerRow[targetH - 1 - sy];
          if (skip > 0) {
            rowSx0 = std::max(sx0, skip);
            rowSx1 = std::min(sx1, targetW - skip);
            if (rowSx0 >= rowSx1) continue;
          }
        }
        const int phyX = phyX0 + (sy - sy0) * dxPerSy;
        const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (phyX & 7));
        int32_t byteIndex = static_cast<int32_t>(phyY0) * panelStride + (phyX >> 3) +
                            static_cast<int32_t>(rowSx0 - sx0) * byteStep;
        const uint8_t* srcRow = entry->scaledPixels.get() + sy * scaledStride;
        int sx = rowSx0;
        while (sx < rowSx1) {
          const int bitOffset = sx & 7;
          const int run = std::min(8 - bitOffset, rowSx1 - sx);
          uint8_t srcByte = srcRow[sx >> 3];
          uint8_t srcMask = static_cast<uint8_t>(0x80u >> bitOffset);
          for (int b = 0; b < run; ++b) {
            const bool srcSet = (srcByte & srcMask) != 0;  // bit set = WHITE in our 1bpp
            if constexpr (Opaque) {
              if (srcSet) frameBuffer[byteIndex] |= bitMask;
              else        frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            } else {
              if (!srcSet) frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            }
            byteIndex += byteStep;
            srcMask >>= 1;
          }
          sx += run;
        }
      }
      break;
    }
    case LandscapeClockwise:
    case LandscapeCounterClockwise: {
      const int dxPerSx = (orientation == LandscapeCounterClockwise) ? 1 : -1;
      const int dyPerSy = (orientation == LandscapeCounterClockwise) ? 1 : -1;
      for (int sy = sy0; sy < sy1; ++sy) {
        int rowSx0 = sx0;
        int rowSx1 = sx1;
        if (rounded) {
          int skip = 0;
          if (sy < r) skip = skipPerRow[sy];
          else if (sy >= targetH - r) skip = skipPerRow[targetH - 1 - sy];
          if (skip > 0) {
            rowSx0 = std::max(sx0, skip);
            rowSx1 = std::min(sx1, targetW - skip);
            if (rowSx0 >= rowSx1) continue;
          }
        }
        const int phyY = phyY0 + (sy - sy0) * dyPerSy;
        const int32_t rowBase = static_cast<int32_t>(phyY) * panelStride;
        int phyX = phyX0 + (rowSx0 - sx0) * dxPerSx;
        const uint8_t* srcRow = entry->scaledPixels.get() + sy * scaledStride;
        int sx = rowSx0;
        while (sx < rowSx1) {
          const int bitOffset = sx & 7;
          const int run = std::min(8 - bitOffset, rowSx1 - sx);
          uint8_t srcByte = srcRow[sx >> 3];
          uint8_t srcMask = static_cast<uint8_t>(0x80u >> bitOffset);
          for (int b = 0; b < run; ++b) {
            const bool srcSet = (srcByte & srcMask) != 0;
            const int32_t byteIndex = rowBase + (phyX >> 3);
            const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (phyX & 7));
            if constexpr (Opaque) {
              if (srcSet) frameBuffer[byteIndex] |= bitMask;
              else        frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            } else {
              if (!srcSet) frameBuffer[byteIndex] &= static_cast<uint8_t>(~bitMask);
            }
            phyX += dxPerSx;
            srcMask >>= 1;
          }
          sx += run;
        }
      }
      break;
    }
  }
  return true;
}

// Explicit template instantiations -- the blit specializations live here so
// callers in other translation units can call drawCachedBitmap without
// pulling the full body into their includes.
template bool GfxRenderer::drawCachedBitmap<false>(const char*, int, int, int, int, float, float, int) const;
template bool GfxRenderer::drawCachedBitmap<true>(const char*, int, int, int, int, float, float, int) const;
template bool GfxRenderer::drawCachedBitmap<false>(CachedBitmap*, int, int, int, int, float, float, int) const;
template bool GfxRenderer::drawCachedBitmap<true>(CachedBitmap*, int, int, int, int, float, float, int) const;

void GfxRenderer::drawPerspectiveBitmap(const Bitmap& bitmap, const int x, const int y, const int w, const int hL,
                                        const int hR) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (w <= 0 || hL <= 0 || hR <= 0) return;

  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  if (srcW <= 0 || srcH <= 0) return;

  const int hMax = std::max(hL, hR);
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const bool topDown = bitmap.isTopDown();

  BitmapScratchLock scratchLock(*this);
  if (!scratchLock.isLocked()) return;

  const int outputRowSize = (srcW + 3) / 4;
  if (!ensureBitmapScratchBuffers(outputRowSize, bitmap.getRowBytes())) {
    return;
  }
  auto* outputRow = bitmapScratchOutputRow_;
  auto* rowBytes = bitmapScratchRowBytes_;

  for (int srcY = 0; srcY < srcH; srcY++) {
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap (perspective)", srcY);
      return;
    }
    const int srcRowIndex = topDown ? srcY : (srcH - 1 - srcY);

    for (int dx = 0; dx < w; dx++) {
      const int colH = (w == 1) ? hL : (hL + (hR - hL) * dx / (w - 1));
      if (colH <= 0) continue;
      const int colTop = (hMax - colH) / 2;
      const int screenX = x + dx;
      if (screenX < 0 || screenX >= screenW) continue;

      const int srcX = (dx * srcW) / w;
      const uint8_t val = (outputRow[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;

      const int dstYStart = (srcRowIndex * colH) / srcH;
      const int dstYEnd = ((srcRowIndex + 1) * colH) / srcH;
      for (int dy = dstYStart; dy < dstYEnd; ++dy) {
        const int screenY = y + colTop + dy;
        if (screenY < 0 || screenY >= screenH) continue;

        if (renderMode == BW && val < 3) {
          drawPixel(screenX, screenY);
        } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
          drawPixel(screenX, screenY, false);
        } else if (renderMode == GRAYSCALE_LSB && val == 1) {
          drawPixel(screenX, screenY, false);
        }
      }
    }
  }
}

void GfxRenderer::drawPerspectiveBitmap(CachedBitmap* handle, const int x, const int y, const int w, const int hL,
                                        const int hR) const {
  if (handle == nullptr || !handle->pixels) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (w <= 0 || hL <= 0 || hR <= 0) return;

  const int srcW = handle->width;
  const int srcH = handle->height;
  if (srcW <= 0 || srcH <= 0) return;

  const int hMax = std::max(hL, hR);
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const bool topDown = handle->topDown;

  // Source pixels are already in 2bpp packed format in RAM; no scratch
  // buffer or row read needed. Each source row is srcStride bytes.
  const int srcStride = (srcW + 3) / 4;

  for (int srcY = 0; srcY < srcH; srcY++) {
    const int srcRowIndex = topDown ? srcY : (srcH - 1 - srcY);
    const uint8_t* srcRow = handle->pixels.get() + srcY * srcStride;

    for (int dx = 0; dx < w; dx++) {
      const int colH = (w == 1) ? hL : (hL + (hR - hL) * dx / (w - 1));
      if (colH <= 0) continue;
      const int colTop = (hMax - colH) / 2;
      const int screenX = x + dx;
      if (screenX < 0 || screenX >= screenW) continue;

      const int srcX = (dx * srcW) / w;
      const uint8_t val = (srcRow[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;

      const int dstYStart = (srcRowIndex * colH) / srcH;
      const int dstYEnd = ((srcRowIndex + 1) * colH) / srcH;
      for (int dy = dstYStart; dy < dstYEnd; ++dy) {
        const int screenY = y + colTop + dy;
        if (screenY < 0 || screenY >= screenH) continue;

        if (renderMode == BW && val < 3) {
          drawPixel(screenX, screenY);
        } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
          drawPixel(screenX, screenY, false);
        } else if (renderMode == GRAYSCALE_LSB && val == 1) {
          drawPixel(screenX, screenY, false);
        }
      }
    }
  }
}

void GfxRenderer::renderPerspectiveBitmapToPacked1bpp(CachedBitmap* handle, const int w, const int hL, const int hR,
                                                      uint8_t* dst) const {
  if (handle == nullptr || !handle->pixels || dst == nullptr) return;
  if (w <= 0 || hL <= 0 || hR <= 0) return;

  const int srcW = handle->width;
  const int srcH = handle->height;
  if (srcW <= 0 || srcH <= 0) return;

  const int hMax = std::max(hL, hR);
  const bool topDown = handle->topDown;

  // Source 2bpp packed; dest 1bpp packed MSB-first per byte. Mirrors the
  // cached drawPerspectiveBitmap geometry pixel-for-pixel so a tile baked
  // here renders identically when blitted at the same (w, hL, hR).
  const int srcStride = (srcW + 3) / 4;
  const int dstStride = (w + 7) / 8;

  for (int srcY = 0; srcY < srcH; srcY++) {
    const int srcRowIndex = topDown ? srcY : (srcH - 1 - srcY);
    const uint8_t* srcRow = handle->pixels.get() + srcY * srcStride;

    for (int dx = 0; dx < w; dx++) {
      const int colH = (w == 1) ? hL : (hL + (hR - hL) * dx / (w - 1));
      if (colH <= 0) continue;
      const int colTop = (hMax - colH) / 2;

      const int srcX = (dx * srcW) / w;
      const uint8_t val = (srcRow[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;
      // BW threshold: 3 = pure white, skip. Lower values become a set bit
      // in the tile. Independent of renderMode -- tiles are always BW.
      if (val >= 3) continue;

      const int dstYStart = (srcRowIndex * colH) / srcH;
      const int dstYEnd = ((srcRowIndex + 1) * colH) / srcH;
      for (int dy = dstYStart; dy < dstYEnd; ++dy) {
        const int outY = colTop + dy;
        if (outY < 0 || outY >= hMax) continue;
        dst[outY * dstStride + (dx >> 3)] |= static_cast<uint8_t>(0x80u >> (dx & 7));
      }
    }
  }
}

void GfxRenderer::drawPacked1bpp(const uint8_t* src, const int srcStride, const int x, const int y, const int w,
                                 const int h, const bool state) const {
  if (src == nullptr || w <= 0 || h <= 0 || srcStride <= 0) return;
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  for (int row = 0; row < h; ++row) {
    const int dstY = y + row;
    if (dstY < 0 || dstY >= screenH) continue;
    const uint8_t* srcRow = src + row * srcStride;
    // Two inner loops merged into one: iterate columns, test the bit, draw
    // if set. Hot-path optimization (e.g. byte-aligned runs) could be
    // added later; for the 5 carousel side covers @ 66 px wide this is
    // already orders of magnitude faster than the perspective walk it
    // replaces.
    for (int col = 0; col < w; ++col) {
      const int dstX = x + col;
      if (dstX < 0 || dstX >= screenW) continue;
      const uint8_t bit = srcRow[col >> 3] & static_cast<uint8_t>(0x80u >> (col & 7));
      if (!bit) continue;
      if (state) {
        drawPixel(dstX, dstY);
      } else {
        drawPixel(dstX, dstY, false);
      }
    }
  }
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX, nodeX + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  display.clearScreen(color);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(refreshMode, fadingFix || turnOffScreen);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't.
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);                                           // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      widthFP += sdIt->second->getAdvance(cp, styleIdx);
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);
    cp = font.getFallbackCodepoint(cp, style);
    const bool hasRealGlyph = font.findGlyphData(cp, style).glyph != nullptr;

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);         // snap 12.4 fixed-point to nearest pixel
    }

    if (!hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      prevAdvanceFP = 0;
      prevCp = 0;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      prevAdvanceFP = getSyntheticSolidGlyphMetrics(font, style, cp).advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      prevAdvanceFP = getSyntheticGreekGlyphMetrics(font, style, cp).advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      prevAdvanceFP = getSyntheticReplacementGlyphMetrics(font, style).advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }

  const auto& font = fontIt->second;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::centerOverRotated90CW(lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);
    cp = font.getFallbackCodepoint(cp, style);
    const bool hasRealGlyph = font.findGlyphData(cp, style).glyph != nullptr;

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    if (!hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      const auto metrics = getSyntheticSolidGlyphMetrics(font, style, cp);
      fillSyntheticSolidGlyphRotated90CW(*this, metrics, x, lastBaseY, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      const auto metrics = getSyntheticGreekGlyphMetrics(font, style, cp);
      drawSyntheticGreekGlyphRotated90CW(*this, metrics, cp, x, lastBaseY, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      const auto metrics = getSyntheticReplacementGlyphMetrics(font, style);
      drawSyntheticReplacementGlyphRotated90CW(*this, metrics, x, lastBaseY, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    if (!glyph) {
      // Advance was already flushed into lastBaseY above; clear base metrics so the
      // next character does not kern or attach to stale state.
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer(const bool turnOffScreen) const {
  display.displayGrayBuffer(fadingFix || turnOffScreen);
}

void GfxRenderer::freeBwCompressedBackup() {
  if (bwCompressedBackup) {
    free(bwCompressedBackup);
    bwCompressedBackup = nullptr;
  }
  bwCompressedBackupSize = 0;
}

/**
 * Encode the framebuffer into a PackBits-style RLE backup.
 *
 * The output stream is a sequence of (header, payload) packets:
 *   - header [0x00, 0x7F]: literal run; copy the next (header + 1) bytes
 *     verbatim from the input. The header itself is stored as count-1, so a
 *     header byte of 0x00 means "copy 1 byte".
 *   - header [0x80, 0xFF]: byte run; repeat the next byte ((header & 0x7F) + 1)
 *     times. A header byte of 0x80 means "repeat the next byte once" — i.e.
 *     a run of 1 — which is wasteful, so the encoder never emits runs <3 here
 *     and prefers literal runs for short sequences.
 *
 * Worst-case expansion (purely random input) is +1 byte per 128 literals,
 * but reader pages are >95% 0xFF runs and typically encode to <5 KB. If the
 * compressed size exceeds the (heap-aware) ceiling in storeBwBuffer the call
 * fails and the caller falls back to the re-render path for grayscale — same
 * UX as the old chunked alloc failure, just one extra render pass.
 *
 * A `restoreBwBuffer` call should always follow the grayscale render if this
 * method was called.
 *
 * Returns true if the framebuffer was successfully stored, false if the
 * compressed buffer couldn't be allocated or the data overflowed.
 */
// PackBits-style RLE compression of the framebuffer. When `dst` is non-null
// the compressed bytes are written to it (the caller must have allocated at
// least the size returned by a prior count pass); when `dst` is null nothing
// is written and the function only computes the output size. The byte counts
// are a pure function of the source contents and identical in both modes, so
// a count pass and a write pass always agree.
//
// Encoding: a control byte then payload.
//   0x80 | (n-1)  -> run: the next single byte repeated n times (3..128)
//   (n-1)         -> literal: the next n bytes copied verbatim (1..128)
static size_t packbitsCompressBw(const uint8_t* src, size_t srcSize, uint8_t* dst) {
  const uint8_t* const srcEnd = src + srcSize;
  size_t outSize = 0;
  while (src < srcEnd) {
    // Look ahead for a run of >=3 identical bytes — the break-even point
    // versus encoding them as a literal sequence.
    size_t runLen = 1;
    const size_t maxRun = std::min<size_t>(128, srcEnd - src);
    while (runLen < maxRun && src[runLen] == src[0]) {
      runLen++;
    }

    if (runLen >= 3) {
      if (dst) {
        *dst++ = 0x80 | static_cast<uint8_t>(runLen - 1);
        *dst++ = *src;
      }
      outSize += 2;
      src += runLen;
    } else {
      // Literal run — read up to 128 bytes, stopping early if we spot a
      // worthwhile (>=3 byte) run upcoming so we don't bury it inside a
      // literal packet.
      size_t litLen = 1;
      const size_t maxLit = std::min<size_t>(128, srcEnd - src);
      while (litLen < maxLit) {
        if (litLen + 2 < static_cast<size_t>(srcEnd - src) && src[litLen] == src[litLen + 1] &&
            src[litLen + 1] == src[litLen + 2]) {
          break;
        }
        litLen++;
      }
      if (dst) {
        *dst++ = static_cast<uint8_t>(litLen - 1);
        memcpy(dst, src, litLen);
        dst += litLen;
      }
      outSize += 1 + litLen;
      src += litLen;
    }
  }
  return outSize;
}

bool GfxRenderer::storeBwBuffer() {
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! storeBwBuffer: no framebuffer");
    return false;
  }

  // Two-pass exact-size allocation. Pass 1 measures the compressed size
  // without writing or allocating; we then allocate exactly that many bytes
  // and pass 2 fills it. This is the key to reliable grayscale AA while a
  // Bluetooth remote is connected: a normal text page compresses to only
  // 2-5 KB, and allocating *that* (rather than the worst-case 32 KB block
  // the old single-buffer path grabbed up front) succeeds even when NimBLE
  // (~58 KB) has fragmented the heap and there's no large contiguous span
  // free. Dense/image pages that exceed the cap fall back to the reader's
  // re-render path for AA (no backup needed), so AA still applies.
  const size_t compressedSize = packbitsCompressBw(frameBuffer, frameBufferSize, nullptr);

  // Size ceiling for keeping a backup, chosen from current heap headroom.
  //
  // The 32 KB floor (MAX_BW_COMPRESSED_SIZE) is always safe and is the right
  // limit when a BLE remote is connected: NimBLE holds ~58 KB and fragments
  // the heap, so only light text pages (2-5 KB) keep a backup and dense pages
  // fall back to the reader's re-render path for grayscale.
  //
  // With BLE off there's a large contiguous span free (~80 KB) — more than
  // enough to also hold a dense-page backup (~35 KB) AND still render the two
  // grayscale passes that run while it's held. Raising the ceiling there lets
  // dense/image pages use the fast restore path instead of an extra full
  // re-render, which is the visible page-turn "delay" on those pages. (It also
  // lets the BookSettings drawer snapshot dense pages instead of falling back.)
  //
  // The gate sits well above the BLE-on maxAlloc (~40 KB) so it only engages
  // with the remote disconnected, and we always leave kGrayPassHeadroom in the
  // largest block for the glyph/image rendering that follows.
  size_t sizeCeiling = MAX_BW_COMPRESSED_SIZE;
  constexpr size_t kHeapAwareGate = 64U * 1024U;     // BLE-off heap looks like this
  constexpr size_t kGrayPassHeadroom = 24U * 1024U;  // keep this contiguous for the grayscale passes
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  if (maxAlloc >= kHeapAwareGate) {
    sizeCeiling = std::min<size_t>(frameBufferSize, maxAlloc - kGrayPassHeadroom);
  }
  if (compressedSize == 0 || compressedSize > sizeCeiling) {
    // Empty, or too large to keep without risking the render that follows. Not
    // an error: the reader re-renders the page for grayscale instead of
    // restoring this backup. Leave any existing backup intact so the
    // BookSettings drawer's fallback (restore the previous snapshot) still works.
    LOG_DBG("GFX", "BW backup %zu over ceiling %zu (maxAlloc=%u); not stored (re-render path)", compressedSize,
            sizeCeiling, static_cast<unsigned>(maxAlloc));
    return false;
  }

  // Allocate the exact-size buffer BEFORE freeing the existing backup, so a
  // malloc failure preserves the old snapshot for the drawer fallback rather
  // than clobbering it (which used to leave the drawer painting on white).
  uint8_t* const newBackup = static_cast<uint8_t*>(malloc(compressedSize));
  if (!newBackup) {
    LOG_DBG("GFX", "Could not allocate %zu-byte BW backup; not stored (reader re-renders; existing backup preserved)",
            compressedSize);
    return false;
  }

  // Pass 2: compress for real into the right-sized buffer.
  packbitsCompressBw(frameBuffer, frameBufferSize, newBackup);

  if (bwCompressedBackup) {
    freeBwCompressedBackup();
  }
  bwCompressedBackup = newBackup;
  bwCompressedBackupSize = compressedSize;
  LOG_DBG("GFX", "Stored BW buffer compressed: %lu -> %zu bytes (%.1f%%)", static_cast<unsigned long>(frameBufferSize),
          bwCompressedBackupSize, (100.0f * bwCompressedBackupSize) / frameBufferSize);
  return true;
}

/**
 * Decompress the backup back into the framebuffer. Pairs with storeBwBuffer.
 * If the backup is missing or malformed, the framebuffer is left as-is and
 * the call is a no-op — the grayscale layer is already on the e-ink, so
 * leaving the framebuffer mid-grayscale only matters for subsequent partial
 * refreshes (status bar etc.), which will re-render their region anyway.
 */
void GfxRenderer::restoreBwBuffer() {
  if (!bwCompressedBackup || bwCompressedBackupSize == 0) {
    return;
  }

  const uint8_t* src = bwCompressedBackup;
  const uint8_t* srcEnd = bwCompressedBackup + bwCompressedBackupSize;
  uint8_t* dst = frameBuffer;
  uint8_t* dstEnd = frameBuffer + frameBufferSize;

  while (src < srcEnd && dst < dstEnd) {
    const uint8_t hdr = *src++;
    if (hdr & 0x80) {
      // Byte-run packet: (header & 0x7F) + 1 copies of the next byte.
      const size_t runLen = (hdr & 0x7F) + 1u;
      if (src >= srcEnd || dst + runLen > dstEnd) {
        LOG_ERR("GFX", "!! BW restore truncated at run (decoded=%zu)", static_cast<size_t>(dst - frameBuffer));
        break;
      }
      memset(dst, *src++, runLen);
      dst += runLen;
    } else {
      // Literal packet: header + 1 verbatim bytes.
      const size_t litLen = static_cast<size_t>(hdr) + 1u;
      if (src + litLen > srcEnd || dst + litLen > dstEnd) {
        LOG_ERR("GFX", "!! BW restore truncated at literal (decoded=%zu)", static_cast<size_t>(dst - frameBuffer));
        break;
      }
      memcpy(dst, src, litLen);
      dst += litLen;
      src += litLen;
    }
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwCompressedBackup();
  LOG_DBG("GFX", "Restored BW buffer from compressed backup");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
