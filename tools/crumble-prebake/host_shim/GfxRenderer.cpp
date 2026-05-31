// Host-side GfxRenderer measurement implementations.
//
// The drawing surface of GfxRenderer stays as inline no-ops in the header.
// The measurement surface lives here, forwarding into EpdFontFamily so we
// reuse the same integer fixed-point math the device uses. By construction
// (no float pivots in the layout path) host and device produce identical
// glyph advances, kerning, line heights, and ascenders for the same font
// data.
//
// This is the minimum measurement surface ChapterHtmlSlimParser +
// TextBlock + ImageBlock + Page actually call. Synthetic glyph fallbacks
// (replacement / Greek / solid) that lib/GfxRenderer/GfxRenderer.cpp
// implements around lines 2486-2508 are NOT mirrored here yet -- text
// containing characters that hit those fallbacks (missing glyphs in the
// active font) will measure slightly differently than device. For the
// first byte-match milestone (text-only English chapter, default LexendDeca
// Medium font), the test EPUB shouldn't exercise those paths.

#include "GfxRenderer.h"

#include <EpdFont.h>
#include <EpdFontData.h>  // brings in fp4::toPixel (already provided by the firmware tree)
#include <Utf8.h>

int GfxRenderer::getTextWidth(int fontId, const char* text,
                              EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  int w = 0, h = 0;
  fontIt->second.getTextDimensions(text, &w, &h, style);
  return w;
}

int GfxRenderer::getTextAdvanceX(int fontId, const char* text,
                                 EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;

  // Mirrors lib/GfxRenderer/GfxRenderer.cpp:2446-2516. We omit the
  // synthetic-glyph fallback branches (solid/Greek/replacement) since
  // those only trigger on missing-glyph paths that the default font +
  // simple English text shouldn't hit. Add those branches here when we
  // start byte-matching against EPUBs that use exotic glyphs.

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) continue;
    cp = font.applyLigatures(cp, text, style);
    cp = font.getFallbackCodepoint(cp, style);

    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);
  return widthPx;
}

int GfxRenderer::getSpaceWidth(int fontId, EpdFontFamily::Style style) const {
  // Device path (lib/GfxRenderer/GfxRenderer.cpp:2405-2412): get space
  // glyph advance and snap via fp4. Reproducing here.
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const EpdGlyph* space = fontIt->second.getGlyph(' ', style);
  return space ? fp4::toPixel(space->advanceX) : 0;
}

int GfxRenderer::getSpaceAdvance(int fontId, uint32_t /*leftCp*/, uint32_t /*rightCp*/,
                                 EpdFontFamily::Style style) const {
  // Device comment (line 2418): "Kern data is not loaded during layout
  // (consistent with previous metadataOnly behavior), so we return just
  // the space advance without kerning." Returning getSpaceWidth here
  // matches the no-kern device behavior verbatim.
  return getSpaceWidth(fontId, style);
}

int GfxRenderer::getKerning(int fontId, uint32_t leftCp, uint32_t rightCp,
                            EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);
  return fp4::toPixel(kernFP);
}

int GfxRenderer::getFontAscenderSize(int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}
