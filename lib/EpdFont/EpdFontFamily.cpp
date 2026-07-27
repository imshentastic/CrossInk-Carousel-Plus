#include "EpdFontFamily.h"

#include <Logging.h>
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

  // CrumBLE 4.5.4 UI font fallback. If a global fallback family is
  // registered (typically the user's loaded SD-card family), try it
  // before resorting to REPLACEMENT_GLYPH. Skip when we ARE the
  // fallback family (avoid infinite recursion) and when the codepoint
  // is the replacement itself (would loop on REPLACEMENT_GLYPH miss).
  if (cp != REPLACEMENT_GLYPH) {
#ifdef CJK_VARIANT
    // tiny-cjk: flash-resident CJK face, consulted before the SD-card UI
    // fallback. findGlyphData on the baked family is a pure interval
    // binary search into .rodata -- no heap, no SD, safe under BLE. The
    // face ships REGULAR only; the style argument is deliberately not
    // forwarded so bold/italic hanzi resolve too (visually regular, same
    // trade-off as the SD regular-only fallback below).
    const EpdFontFamily* cjkFb = cjkFallbackFamily();
    if (cjkFb != nullptr && cjkFb != this) {
      if (const GlyphData cjkData = cjkFb->findGlyphData(cp, REGULAR); cjkData.glyph) {
        return cjkData;
      }
    }
#endif
    const EpdFontFamily* fb = uiFallbackFamily();
    if (fb != nullptr && fb != this) {
      // Two-step lookup: cheap in-memory findGlyphData first, then if
      // null fall through to the EpdFont-level miss handler so on-SD
      // CJK glyphs (NOT prewarmed) get lazy-loaded. Calling the
      // family-level getGlyphData on the fallback would re-enter the
      // typography-substitution + recursive-fallback path and could
      // ping-pong back into THIS family via REPLACEMENT_GLYPH lookup.
      auto tryFallbackStyle = [fb, cp](Style fbStyle) -> GlyphData {
        if (const GlyphData fbData = fb->findGlyphData(cp, fbStyle); fbData.glyph) {
          return fbData;
        }
        const EpdFont* fbFont = fb->getFont(fbStyle);
        if (fbFont == nullptr) return {nullptr, nullptr};
        if (const EpdGlyph* g = fbFont->getGlyph(cp)) {
          return {fbFont->data, g};
        }
        return {nullptr, nullptr};
      };
      if (const GlyphData fbData = tryFallbackStyle(style); fbData.glyph) {
        return fbData;
      }
      // Some SD fonts only have REGULAR. Retry with REGULAR if we asked
      // for a styled variant.
      if (style != REGULAR) {
        if (const GlyphData fbReg = tryFallbackStyle(REGULAR); fbReg.glyph) {
          return fbReg;
        }
      }
    }
    // CrumBLE 4.5.4 task #5C+: built-in fallback. If the primary AND UI
    // fallback both miss, try the built-in family (typically Bitter UI_10
    // for its broad Latin coverage). Catches the case where the user picks
    // an SD CJK font that ships CJK-only -- ASCII / digits / punctuation
    // in book text fall through to Bitter and render correctly instead of
    // showing '?'.
    const EpdFontFamily* bi = builtInFallbackFamily();
    if (bi != nullptr && bi != this) {
      if (const GlyphData biData = bi->findGlyphData(cp, style); biData.glyph) {
        // CrumBLE 4.5.4 diag: confirm the built-in fallback is actually
        // being consulted at render time. Field report: SD CJK font as
        // primary + English book = '?' even though Bitter has the
        // codepoint. If this line never fires we know the fallback chain
        // isn't being entered; if it fires we know it IS hit but the
        // glyph isn't reaching the renderer. One-shot per style so the
        // log doesn't drown in per-glyph traffic.
        static bool dumpedBi[4] = {false, false, false, false};
        const uint8_t styleIdx = static_cast<uint8_t>(style) & 0x03;
        if (!dumpedBi[styleIdx]) {
          dumpedBi[styleIdx] = true;
          LOG_INF("BIFB", "Built-in fallback HIT style=%u cp=U+%04lX this=%p bi=%p",
                  styleIdx, static_cast<unsigned long>(cp),
                  static_cast<const void*>(this), static_cast<const void*>(bi));
        }
        return biData;
      }
      if (style != REGULAR) {
        if (const GlyphData biReg = bi->findGlyphData(cp, REGULAR); biReg.glyph) {
          static bool dumpedBiR = false;
          if (!dumpedBiR) {
            dumpedBiR = true;
            LOG_INF("BIFB", "Built-in fallback REGULAR HIT (style asked=%u) cp=U+%04lX",
                    static_cast<uint8_t>(style) & 0x03, static_cast<unsigned long>(cp));
          }
          return biReg;
        }
      }
    } else {
      // CrumBLE 4.5.4 diag: one-shot log when the built-in fallback
      // path is REACHED but skipped. Catches (a) bi is null (not
      // registered at boot), (b) bi == this (the renderer's primary
      // is somehow the same instance as the registered built-in).
      static bool dumpedBiSkip = false;
      if (!dumpedBiSkip) {
        dumpedBiSkip = true;
        LOG_INF("BIFB", "Built-in fallback SKIPPED cp=U+%04lX this=%p bi=%p (bi==this:%d bi==null:%d)",
                static_cast<unsigned long>(cp),
                static_cast<const void*>(this), static_cast<const void*>(bi),
                (bi == this) ? 1 : 0, (bi == nullptr) ? 1 : 0);
      }
    }
  }

  if (cp != REPLACEMENT_GLYPH) {
    // CrumBLE 4.5.4 diag: one-shot log when we're about to bottom out
    // on REPLACEMENT_GLYPH. Confirms the fallback chain (UI + built-in)
    // both missed for this cp.
    static bool dumpedDead = false;
    if (!dumpedDead) {
      dumpedDead = true;
      LOG_INF("BIFB", "Fallback chain BOTTOM-OUT cp=U+%04lX style=%u this=%p ui=%p bi=%p -- returning REPLACEMENT",
              static_cast<unsigned long>(cp), static_cast<uint8_t>(style) & 0x03,
              static_cast<const void*>(this),
              static_cast<const void*>(uiFallbackFamily()),
              static_cast<const void*>(builtInFallbackFamily()));
    }
    return getGlyphData(REPLACEMENT_GLYPH, style);
  }
  return {nullptr, nullptr};
}

namespace {
// File-scope so the static-init order is well-defined relative to any
// translation unit that calls setUiFallbackFamily / uiFallbackFamily.
const EpdFontFamily* gUiFallback = nullptr;
EpdFontFamily::LazyFallbackLoader gLazyLoader = nullptr;
}  // namespace

void EpdFontFamily::setUiFallbackFamily(const EpdFontFamily* family) {
  gUiFallback = family;
  // CrumBLE 4.5.4 diag: field reports of CJK book titles still rendering
  // as '?' even with an SD font selected. Log every set/clear so we can
  // confirm from serial whether loadFamily actually wired the fallback.
  LOG_INF("UIFB", "UI font fallback %s", family ? "REGISTERED" : "CLEARED");
}

void EpdFontFamily::setLazyFallbackLoader(LazyFallbackLoader loader) {
  gLazyLoader = loader;
  LOG_INF("UIFB", "UI fallback lazy loader %s", loader ? "ARMED" : "DISARMED");
}

namespace {
const EpdFontFamily* gBuiltInFallback = nullptr;
}

void EpdFontFamily::setBuiltInFallbackFamily(const EpdFontFamily* family) {
  gBuiltInFallback = family;
  LOG_INF("UIFB", "Built-in fallback %s", family ? "REGISTERED" : "CLEARED");
}

const EpdFontFamily* EpdFontFamily::builtInFallbackFamily() { return gBuiltInFallback; }

#ifdef CJK_VARIANT
namespace {
const EpdFontFamily* gCjkFallback = nullptr;
}

void EpdFontFamily::setCjkFallbackFamily(const EpdFontFamily* family) {
  gCjkFallback = family;
  LOG_INF("UIFB", "CJK flash fallback %s", family ? "REGISTERED" : "CLEARED");
}

const EpdFontFamily* EpdFontFamily::cjkFallbackFamily() { return gCjkFallback; }
#endif

const EpdFontFamily* EpdFontFamily::uiFallbackFamily() {
  // CrumBLE 4.5.4 task #5C: first-miss lazy load. If a loader has been
  // armed (SdCardFontSystem::registerLazyFallback was called at boot
  // with a non-empty SETTINGS.uiFontFallbackFamily) AND we don't have
  // a resident fallback yet, invoke the loader. It runs synchronously,
  // loads the SD font, calls setUiFallbackFamily(...) which populates
  // gUiFallback. Then null gLazyLoader so the next miss is a fast
  // pointer-read with no allocation cost. If the loader fails (font
  // not on SD, OOM, etc.) gUiFallback stays null and gLazyLoader still
  // gets cleared -- no point retrying every miss.
  if (gUiFallback == nullptr && gLazyLoader != nullptr) {
    LazyFallbackLoader once = gLazyLoader;
    gLazyLoader = nullptr;  // one-shot; clear before invocation in case loader recurses
    LOG_INF("UIFB", "UI fallback lazy load triggered by first glyph miss");
    once();
  }
  return gUiFallback;
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getGlyphData(cp, style).glyph;
}

uint32_t EpdFontFamily::getFallbackCodepoint(const uint32_t cp, const Style style) const {
  if (findGlyphData(cp, style).glyph) return cp;
  const uint32_t aliasCp = syntheticGlyph::aliasCodepoint(cp);
  if (aliasCp != cp) {
    if (findGlyphData(aliasCp, style).glyph) return aliasCp;
    // CrumBLE 4.4 task #28 follow-up: when findGlyphData misses on both
    // cp and aliasCp, give the font's own miss handler a chance to load
    // the alias target. The atlas / SD-font miniData only contain
    // codepoints the chapter actually uses, so a chapter that has '◆'
    // but no '*' anywhere will see findGlyphData('*') miss even though
    // the underlying .cpfont ships an asterisk glyph. font->getGlyph
    // walks the font's intervals AND (for SD-card fonts) calls the
    // onGlyphMiss handler which lazy-loads from the .cpfont's full
    // glyph table into the overflow ring buffer -- so a successful
    // load here means the renderer will draw a real glyph instead of
    // the synthetic replacement box. Zero cost when the font's intervals
    // already excluded the alias target (binary-search miss = ~150ns).
    const EpdFont* f = getFont(style);
    if (f && f->getGlyph(aliasCp)) return aliasCp;
    // CrumBLE 4.4 task #26: when bold/italic miss-handler also fails
    // (font lacks that style entirely), check the regular-style chain.
    // Mirrors getGlyphData's regular-fallback so drawText's pre-flight
    // decision matches the actual draw-time outcome.
    if (style != REGULAR && getGlyphData(aliasCp, REGULAR).glyph) return aliasCp;
    return REPLACEMENT_GLYPH;
  }
  if (syntheticGlyph::isSpaceFallback(cp)) return cp;
  if (syntheticGlyph::isSolid(cp) || syntheticGlyph::isGreekFallback(cp)) return cp;
  // CrumBLE 4.4 task #26: chapter titles + emphasis use BOLD / ITALIC.
  // When the atlas only carries the REGULAR style (the prebake's prewarm
  // only loads REGULAR by default) AND the SD font lacks the requested
  // style's glyph (miss handler returns null because the .cpfont ships
  // regular-only), getGlyphData's existing regular fallback (line ~276)
  // would salvage the draw -- but only IF the renderer reaches the
  // renderCharImpl path. Returning REPLACEMENT_GLYPH here short-circuits
  // that path and forces the synthetic '?' box instead. Probe regular's
  // chain for `cp`; if it has the glyph, return cp so drawText proceeds
  // to renderCharImpl which calls getGlyphData and lets the regular
  // fallback cascade run. The bold/italic visual style is lost but text
  // stays readable -- preferable to "??? Two" for chapter headers.
  if (style != REGULAR && getGlyphData(cp, REGULAR).glyph) return cp;
  // CrumBLE 4.5.4: before declaring this codepoint replaceable, consult
  // the UI fallback family. drawText short-circuits to the synthetic '?'
  // when this function returns REPLACEMENT_GLYPH, so without this check
  // CJK book titles / collection names rendered as '?' even though the
  // fallback hook in getGlyphData would have lazy-loaded the glyph.
  // Probe via the EpdFont-level getGlyph (invokes the SD miss handler)
  // -- NOT via family-level getGlyphData, which would recurse through
  // typography substitutions back into the same REPLACEMENT_GLYPH path.
  if (cp != REPLACEMENT_GLYPH) {
#ifdef CJK_VARIANT
    // tiny-cjk: keep the width-probe in lockstep with getGlyphData's flash
    // CJK fallback, mirroring the UI/built-in probes below. Without this,
    // drawText would short-circuit hanzi to the synthetic '?' box.
    const EpdFontFamily* cjkFb = cjkFallbackFamily();
    if (cjkFb != nullptr && cjkFb != this) {
      if (cjkFb->findGlyphData(cp, REGULAR).glyph) return cp;
    }
#endif
    const EpdFontFamily* fb = uiFallbackFamily();
    if (fb != nullptr && fb != this) {
      if (const EpdFont* fbFont = fb->getFont(style); fbFont && fbFont->getGlyph(cp)) return cp;
      if (style != REGULAR) {
        if (const EpdFont* fbReg = fb->getFont(REGULAR); fbReg && fbReg->getGlyph(cp)) return cp;
      }
    }
    // CrumBLE 4.5.4 task #5C+: built-in fallback probe (Bitter for Latin).
    // Mirrors the UI-fallback path above so getFallbackCodepoint stays in
    // lockstep with getGlyphData -- without this, drawText's width-probe
    // would return REPLACEMENT_GLYPH for ASCII codepoints that getGlyph
    // Data CAN resolve via Bitter, and the actual draw would render '?'
    // anyway because the synthetic-replacement short-circuit fires first.
    const EpdFontFamily* bi = builtInFallbackFamily();
    if (bi != nullptr && bi != this) {
      if (const EpdFont* biFont = bi->getFont(style); biFont && biFont->findGlyph(cp)) return cp;
      if (style != REGULAR) {
        if (const EpdFont* biReg = bi->getFont(REGULAR); biReg && biReg->findGlyph(cp)) return cp;
      }
    }
  }
  return REPLACEMENT_GLYPH;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
