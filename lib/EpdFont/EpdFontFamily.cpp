#include "EpdFontFamily.h"

#include <Utf8.h>

#include <algorithm>

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  if (*string == '\0') {
    *w = 0;
    *h = 0;
    return;
  }

  int lastBaseX = 0;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const bool isCombining = utf8IsCombiningMark(cp);

    if (!isCombining) {
      cp = applyLigatures(cp, string, style);
      cp = getFallbackCodepoint(cp, style);
    }

    const bool hasRealGlyph = findGlyphData(cp, style).glyph != nullptr;

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      lastBaseX += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      continue;
    }

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      const EpdFontData* data = getData(style);
      const uint16_t advanceX = syntheticGlyph::solidAdvanceX(data, getGlyph('M', style));
      const int glyphHeight = syntheticGlyph::solidHeight(data, cp);
      const int glyphWidth = syntheticGlyph::solidWidth(cp, advanceX, glyphHeight);
      const int glyphLeft = syntheticGlyph::solidLeft(cp, advanceX, glyphWidth);
      const int glyphTop = syntheticGlyph::solidTop(data, cp, glyphHeight);

      if (prevCp != 0) {
        const auto kernFP = getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }

      minX = std::min(minX, lastBaseX + glyphLeft);
      maxX = std::max(maxX, lastBaseX + glyphLeft + glyphWidth);
      minY = std::min(minY, glyphTop - glyphHeight);
      maxY = std::max(maxY, glyphTop);

      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseTop = glyphTop;
      prevAdvanceFP = advanceX;
      prevCp = cp;
      continue;
    }

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      const EpdFontData* data = getData(style);
      const uint16_t advanceX = syntheticGlyph::greekAdvanceX(data, getGlyph('M', style), cp);
      const int glyphHeight = syntheticGlyph::greekHeight(data, cp);
      const int glyphWidth = syntheticGlyph::greekWidth(cp, advanceX, glyphHeight);
      const int glyphLeft = syntheticGlyph::greekLeft(cp, advanceX, glyphWidth);
      const int glyphTop = syntheticGlyph::greekTop(data, cp, glyphHeight);

      if (prevCp != 0) {
        const auto kernFP = getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }

      minX = std::min(minX, lastBaseX + glyphLeft);
      maxX = std::max(maxX, lastBaseX + glyphLeft + glyphWidth);
      minY = std::min(minY, glyphTop - glyphHeight);
      maxY = std::max(maxY, glyphTop);

      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseTop = glyphTop;
      prevAdvanceFP = advanceX;
      prevCp = cp;
      continue;
    }

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      const EpdFontData* data = getData(style);
      const uint16_t advanceX = syntheticGlyph::replacementAdvanceX(data, getGlyph('M', style));
      const int glyphHeight = syntheticGlyph::replacementHeight(data);
      const int glyphWidth = syntheticGlyph::replacementWidth(advanceX, glyphHeight);
      const int glyphLeft = syntheticGlyph::replacementLeft(advanceX, glyphWidth);
      const int glyphTop = syntheticGlyph::replacementTop(data, glyphHeight);

      if (prevCp != 0) {
        const auto kernFP = getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }

      minX = std::min(minX, lastBaseX + glyphLeft);
      maxX = std::max(maxX, lastBaseX + glyphLeft + glyphWidth);
      minY = std::min(minY, glyphTop - glyphHeight);
      maxY = std::max(maxY, glyphTop);

      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseTop = glyphTop;
      prevAdvanceFP = advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = getGlyph(cp, style);
    if (!glyph) {
      if (!isCombining) {
        lastBaseX += fp4::toPixel(prevAdvanceFP);
        prevCp = 0;
        prevAdvanceFP = 0;
        lastBaseLeft = 0;
        lastBaseWidth = 0;
        lastBaseTop = 0;
      }
      continue;
    }

    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(glyph->top, glyph->height, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      const auto kernFP = getKerning(prevCp, cp, style);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX =
        isCombining ? combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, glyph->left, glyph->width)
                    : lastBaseX;
    const int glyphBaseY = -raiseBy;

    minX = std::min(minX, glyphBaseX + glyph->left);
    maxX = std::max(maxX, glyphBaseX + glyph->left + glyph->width);
    minY = std::min(minY, glyphBaseY + glyph->top - glyph->height);
    maxY = std::max(maxY, glyphBaseY + glyph->top);

    if (!isCombining) {
      lastBaseLeft = glyph->left;
      lastBaseWidth = glyph->width;
      lastBaseTop = glyph->top;
      prevAdvanceFP = glyph->advanceX;
      prevCp = cp;
    }
  }

  *w = maxX - minX;
  *h = maxY - minY;
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

EpdFontFamily::GlyphData EpdFontFamily::findGlyphData(const uint32_t cp, const Style style) const {
  // CrumBLE 4.3: per-section embedded glyph subset takes precedence over
  // the per-font interval search. When the active Section has installed a
  // glyph subset block for this style, search the embedded intervals for
  // cp; if found, return its glyph directly from the in-RAM block (zero SD
  // reads, zero SdCardFont miniData allocation). When NOT found in the
  // embedded subset OR no embedded data installed, fall through to the
  // existing per-font lookup (and its REGULAR-style + miss-handler
  // fallbacks). Style mask preserves UNDERLINE / STRIKETHROUGH bits via
  // the lower 2 bits, matching the rest of the family selection logic.
  const uint8_t styleIdx = static_cast<uint8_t>(style) & 0x03;
  if (const EpdFontData* embeddedData = embeddedDataByStyle_[styleIdx]) {
    const auto* intervals = embeddedData->intervals;
    const uint32_t intervalCount = embeddedData->intervalCount;
    if (intervals && intervalCount > 0) {
      // upper_bound: find the first interval with first > cp; the one
      // immediately before it is the only candidate that could contain cp.
      const EpdUnicodeInterval* end = intervals + intervalCount;
      const auto it = std::upper_bound(intervals, end, cp, [](uint32_t v, const EpdUnicodeInterval& iv) {
        return v < iv.first;
      });
      if (it != intervals) {
        const auto& iv = *(it - 1);
        if (cp <= iv.last) {
          return {embeddedData, &embeddedData->glyph[iv.offset + (cp - iv.first)]};
        }
      }
    }
    // Not in this style's embedded subset -- fall through to per-font path
    // BUT skip the REGULAR-style fallback below for this case (the embedded
    // subset deliberately did NOT include cp for this style, so falling
    // back to regular would visually misrepresent it just like the SD-font
    // miss-handler case does). The miss handler the SD font sets up will
    // still load the bitmap on demand if cp is in the .cpfont at all.
  }

  const EpdFont* font = getFont(style);
  if (const EpdGlyph* glyph = font->findGlyph(cp)) {
    return {font->data, glyph};
  }

  if (font != regular) {
    // CrumBLE 4.2 Option 2: skip the REGULAR-style fallback for SD-card
    // fonts. SD fonts set `data->glyphMissHandler` on their stubData; the
    // outer getGlyphData fallback (via font->getGlyph) will invoke that
    // handler to lazy-load the bold / italic / bolditalic glyph from SD
    // through the overflow ring buffer. Falling back to REGULAR here
    // would silently render bold/italic text in the regular style, which
    // is the wrong visual result. Built-in compressed fonts (no
    // glyphMissHandler) keep the original REGULAR fallback so existing
    // builtin behaviour is unchanged.
    if (font->data->glyphMissHandler == nullptr) {
      if (const EpdGlyph* glyph = regular->findGlyph(cp)) {
        return {regular->data, glyph};
      }
    }
  }

  return {nullptr, nullptr};
}

EpdFontFamily::GlyphData EpdFontFamily::getGlyphData(const uint32_t cp, const Style style) const {
  if (const GlyphData glyphData = findGlyphData(cp, style); glyphData.glyph) {
    return glyphData;
  }

  // CrumBLE 4.2 Option 2: SD-card lazy-load fallback. findGlyphData
  // failed for `style` -- for SD-card fonts that's the expected case for
  // BOLD / ITALIC / BOLDITALIC under lazy prewarm (only REGULAR is
  // eagerly prewarmed; non-REGULAR styles' miniData stays empty). Invoke
  // EpdFont::getGlyph, which: (1) tries findGlyph (still null for the
  // SD-font miss case), then (2) calls data->glyphMissHandler ->
  // SdCardFont::onGlyphMiss, which reads the glyph from the .cpfont file
  // on SD and caches it in the per-style overflow ring buffer for
  // subsequent draws on the same page. For built-in compressed fonts
  // glyphMissHandler is nullptr so this is a no-op fall through.
  const EpdFont* font = getFont(style);
  if (const EpdGlyph* glyph = font->getGlyph(cp)) {
    return {font->data, glyph};
  }

  // CrumBLE 4.3: regular-style fallback for SD fonts with missing styles.
  // When this is a non-regular style and the lookup fully failed (no
  // embedded subset entry + miss handler returned null -- typically
  // because the SD font doesn't have this style at all, e.g. Readerly_12
  // ships only REGULAR), fall back to the regular glyph instead of
  // REPLACEMENT_GLYPH. The result is italic/bold text rendered in the
  // regular style -- visually a small downgrade but readable. Previously
  // these codepoints became '?' which broke ~30% of text in books with
  // mixed-style italic phrases. We DON'T retry through the regular style's
  // miss handler -- recurse via getGlyphData(cp, REGULAR) which does the
  // full embedded + miss-handler lookup on the regular style.
  if (style != REGULAR) {
    if (const GlyphData regularData = getGlyphData(cp, REGULAR); regularData.glyph) {
      return regularData;
    }
  }

  // CrumBLE 4.3: typography substitutions. Some SD fonts (Readerly_12,
  // ChareInk_*) don't ship Unicode punctuation glyphs -- curly quotes,
  // dashes, ellipsis etc. EPUBs use these heavily (smart-quoted dialogue,
  // em-dashed asides). Without substitution they render as REPLACEMENT
  // GLYPH ('?'), which dominates the page on dialogue-heavy chapters.
  // ASCII equivalents are guaranteed to be in any font (a-z + ASCII
  // punctuation), so we map the smart character to its straight equivalent
  // and retry the lookup. Trade-off: typography downgrade (curly quotes
  // become straight, em dashes become hyphens) but the text is readable.
  uint32_t substitute = 0;
  switch (cp) {
    case 0x2018:  // LEFT SINGLE QUOTATION MARK
    case 0x2019:  // RIGHT SINGLE QUOTATION MARK
    case 0x201A:  // SINGLE LOW-9 QUOTATION MARK
    case 0x201B:  // SINGLE HIGH-REVERSED-9 QUOTATION MARK
      substitute = 0x0027;  // APOSTROPHE
      break;
    case 0x201C:  // LEFT DOUBLE QUOTATION MARK
    case 0x201D:  // RIGHT DOUBLE QUOTATION MARK
    case 0x201E:  // DOUBLE LOW-9 QUOTATION MARK
    case 0x201F:  // DOUBLE HIGH-REVERSED-9 QUOTATION MARK
      substitute = 0x0022;  // QUOTATION MARK
      break;
    case 0x2013:  // EN DASH
    case 0x2014:  // EM DASH
    case 0x2015:  // HORIZONTAL BAR
    case 0x2212:  // MINUS SIGN
      substitute = 0x002D;  // HYPHEN-MINUS
      break;
    case 0x2026:  // HORIZONTAL ELLIPSIS
      substitute = 0x002E;  // FULL STOP (one dot -- not "..." since the
                            // page-DOM wordXpos accounts for a single glyph)
      break;
    case 0x00A0:  // NO-BREAK SPACE
    case 0x2009:  // THIN SPACE
    case 0x200A:  // HAIR SPACE
    case 0x202F:  // NARROW NO-BREAK SPACE
      substitute = 0x0020;  // SPACE
      break;
    default:
      break;
  }
  if (substitute != 0 && substitute != cp) {
    if (const GlyphData subData = getGlyphData(substitute, style); subData.glyph) {
      return subData;
    }
  }

  if (cp != REPLACEMENT_GLYPH) {
    return getGlyphData(REPLACEMENT_GLYPH, style);
  }
  return {nullptr, nullptr};
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getGlyphData(cp, style).glyph;
}

uint32_t EpdFontFamily::getFallbackCodepoint(const uint32_t cp, const Style style) const {
  if (findGlyphData(cp, style).glyph) return cp;
  const uint32_t aliasCp = syntheticGlyph::aliasCodepoint(cp);
  if (aliasCp != cp) {
    return findGlyphData(aliasCp, style).glyph ? aliasCp : REPLACEMENT_GLYPH;
  }
  if (syntheticGlyph::isSpaceFallback(cp)) return cp;
  if (syntheticGlyph::isSolid(cp) || syntheticGlyph::isGreekFallback(cp)) return cp;
  return REPLACEMENT_GLYPH;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
