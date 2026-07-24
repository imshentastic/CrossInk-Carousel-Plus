// From
// https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h

#pragma once
#include <cstdint>

/// Font metrics use "fixed-point 4" (4 fractional bits, i.e. 1/16-pixel
/// resolution).  Both the 12.4 glyph advances (uint16_t) and the 4.4 kern
/// values (int8_t) share the same 4 fractional bits, so they can be freely
/// added before snapping to whole pixels.
///
/// Rendering and measurement use "differential rounding": each glyph step
/// (previous advance + current kern) is combined in fixed-point and snapped
/// to a pixel as one unit.  This guarantees identical character pairs always
/// produce the same pixel spacing, regardless of position on the line.
///
/// The helpers below eliminate the raw bit-shifts that would otherwise be
/// scattered across every layout / measurement call site.
namespace fp4 {
constexpr int FRAC_BITS = 4;
constexpr int32_t HALF = 1 << (FRAC_BITS - 1);  // 8, added before shift for round-to-nearest

/// Convert an integer pixel value to 12.4 fixed-point.
constexpr int32_t fromPixel(int px) { return static_cast<int32_t>(px) << FRAC_BITS; }

/// Snap a fixed-point value to the nearest integer pixel.
constexpr int toPixel(int32_t fp) { return static_cast<int>((fp + HALF) >> FRAC_BITS); }

/// Convert a fixed-point value to float (mainly useful for debug logging).
constexpr float toFloat(int32_t fp) { return fp / static_cast<float>(1 << FRAC_BITS); }
}  // namespace fp4

/// Helpers for positioning Unicode combining marks (U+0300 ff.) over a
/// preceding base glyph without GPOS anchor tables.
namespace combiningMark {

constexpr int MIN_GAP_PX = 1;

/// Compute the cursor-X at which to render a combining mark so its bitmap
/// is visually centered over the base glyph's bitmap.
constexpr int centerOver(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos + baseLeft + baseWidth / 2 - markWidth / 2 - markLeft;
}

/// Rotated-90CW variant of centerOver.  In the rotated coordinate system
/// renderCharImpl uses (cursorY - left) instead of (cursorX + left), so
/// every left/width term inverts sign.
constexpr int centerOverRotated90CW(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos - baseLeft - baseWidth / 2 + markWidth / 2 + markLeft;
}

/// For combining marks that sit entirely above the baseline, compute how many
/// pixels to raise the mark so there is at least MIN_GAP_PX between its bottom
/// edge and the top of the base glyph.  Returns 0 for marks that extend to or
/// below the baseline (e.g. cedilla, dot-below, ogonek).
constexpr int raiseAboveBase(int markTop, int markHeight, int baseTop) {
  if (markTop - markHeight <= 0) return 0;
  const int gap = markTop - markHeight - baseTop;
  return (gap < MIN_GAP_PX) ? (MIN_GAP_PX - gap) : 0;
}

}  // namespace combiningMark

/// Fixed-point conventions used by EpdGlyph and EpdFontData:
///   advanceX:   12.4 unsigned fixed-point in uint16_t  (use fp4::toPixel)
///   kernMatrix:  4.4 signed fixed-point in int8_t      (use fp4::toPixel)
/// Both share 4 fractional bits so they combine directly in an accumulator.

/// Font data stored PER GLYPH
typedef struct {
  uint8_t width;        ///< Bitmap dimensions in pixels
  uint8_t height;       ///< Bitmap dimensions in pixels
  uint16_t advanceX;    ///< Distance to advance cursor (x axis), 12.4 fixed-point in pixels
  int16_t left;         ///< X dist from cursor pos to UL corner
  int16_t top;          ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint32_t dataOffset;  ///< Pointer into EpdFont->bitmap (or within-group offset for compressed fonts)
} EpdGlyph;

/// Compressed font group: a DEFLATE-compressed block of glyph bitmaps
typedef struct {
  uint32_t compressedOffset;  ///< Byte offset into compressed data array
  uint32_t compressedSize;    ///< Compressed DEFLATE stream size
  uint32_t uncompressedSize;  ///< Decompressed size
  uint16_t glyphCount;        ///< Number of glyphs in this group
  uint32_t firstGlyphIndex;   ///< First glyph index in the global glyph array
} EpdFontGroup;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

/// Maps a codepoint to a kerning class ID, sorted by codepoint for binary search.
/// Class IDs are 1-based; codepoints not in the table have implicit class 0 (no kerning).
typedef struct {
  uint16_t codepoint;  ///< Unicode codepoint
  uint8_t classId;     ///< 1-based kerning class ID
} __attribute__((packed)) EpdKernClassEntry;

/// Ligature substitution for a specific glyph pair, sorted by `pair` for binary search.
/// `pair` encodes (leftCodepoint << 16 | rightCodepoint) for single-key lookup.
typedef struct {
  uint32_t pair;        ///< Packed codepoint pair (left << 16 | right)
  uint32_t ligatureCp;  ///< Codepoint of the replacement ligature glyph
} __attribute__((packed)) EpdLigaturePair;

/// Data stored for FONT AS A WHOLE
typedef struct {
  const uint8_t* bitmap;                ///< Glyph bitmaps, concatenated
  const EpdGlyph* glyph;                ///< Glyph array
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  bool is2Bit;
  const EpdFontGroup* groups;                 ///< NULL for uncompressed fonts
  uint16_t groupCount;                        ///< 0 for uncompressed fonts
  const uint16_t* glyphToGroup;               ///< Per-glyph group ID (nullptr for contiguous-group fonts)
  const EpdKernClassEntry* kernLeftClasses;   ///< Sorted left-side class map (nullptr if none)
  const EpdKernClassEntry* kernRightClasses;  ///< Sorted right-side class map (nullptr if none)
  const int8_t* kernMatrix;              ///< Flat leftClassCount x rightClassCount matrix, 4.4 fixed-point in pixels
  uint16_t kernLeftEntryCount;           ///< Entries in kernLeftClasses
  uint16_t kernRightEntryCount;          ///< Entries in kernRightClasses
  uint8_t kernLeftClassCount;            ///< Number of distinct left classes (matrix rows)
  uint8_t kernRightClassCount;           ///< Number of distinct right classes (matrix cols)
  const EpdLigaturePair* ligaturePairs;  ///< Sorted ligature pair table (nullptr if none)
  uint32_t ligaturePairCount;            ///< Number of entries in ligaturePairs

  /// On-demand glyph loading for fonts that don't keep all glyphs in RAM (e.g. SD card fonts).
  /// Called by getGlyph() when a codepoint is not found in the interval table.
  /// Returns a valid EpdGlyph* with correct metadata, or nullptr to fall back to the
  /// replacement glyph.  The returned pointer is valid until the next glyphMissHandler
  /// call that causes a ring-buffer eviction — callers must consume it (measure or draw)
  /// before requesting another missed glyph.
  const EpdGlyph* (*glyphMissHandler)(void* ctx, uint32_t codepoint);

  /// Context pointer for glyphMissHandler (typically SdCardFont*).  Also used by
  /// GfxRenderer::getGlyphBitmap() to retrieve overflow bitmaps via SdCardFont.
  void* glyphMissCtx;

  /// Streaming glyph-bitmap fetch (CrumBLE 4.5.5: hybrid streaming atlas).
  /// When non-null, GfxRenderer::getGlyphBitmap() routes through this callback
  /// instead of dereferencing `bitmap[dataOffset]`.  The glyph is present in
  /// the interval table (findGlyph hits), but the bitmap bytes are not
  /// resident -- the callback reads them on demand into a small scratch
  /// owned by the ctx (typically Section*).  Same valid-until-next-fetch
  /// contract as glyphMissHandler: caller must consume (draw/measure) the
  /// returned pointer before requesting another glyph.
  const uint8_t* (*glyphBitmapFetch)(void* ctx, const EpdGlyph* glyph);

  /// Context for glyphBitmapFetch (typically Section* in streaming-atlas mode).
  void* glyphBitmapCtx;
} EpdFontData;

namespace syntheticGlyph {

constexpr uint32_t FULL_BLOCK = 0x2588;
constexpr uint32_t BLACK_SQUARE = 0x25A0;
constexpr uint32_t GREEK_CAPITAL_GAMMA = 0x0393;
constexpr uint32_t GREEK_SMALL_EPSILON = 0x03B5;
constexpr uint32_t GREEK_SMALL_OMEGA = 0x03C9;
constexpr uint32_t MODIFIER_LETTER_TURNED_COMMA = 0x02BB;
constexpr uint32_t LEFT_SINGLE_QUOTATION_MARK = 0x2018;

constexpr bool isSolid(uint32_t cp) { return cp == FULL_BLOCK || cp == BLACK_SQUARE; }
constexpr bool isGreekFallback(uint32_t cp) {
  return cp == GREEK_CAPITAL_GAMMA || cp == GREEK_SMALL_EPSILON || cp == GREEK_SMALL_OMEGA;
}
constexpr bool isReplacementFallback(uint32_t cp) { return cp == 0xFFFD; }
constexpr bool isSpaceFallback(uint32_t cp) {
  return cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}
// Typography fallback table (v4.3 task #28). When a font lacks a fancy
// punctuation codepoint, alias it to the nearest ASCII equivalent so the
// renderer's glyph lookup finds a real glyph instead of producing '?'.
// EpdFontFamily::getFallbackCodepoint runs this on every miss, AND a
// successful alias still has to be present in the font itself for the
// renderer to draw it -- but for the SD-font atlas / embedded-subset
// path the ASCII targets ('. - " ' , * `) appear in almost every section
// (they are the bread-and-butter glyphs of any prose chapter), so the
// alias closes the visible '?' gap in practice. Tried-and-failed
// substitutions still bottom out at REPLACEMENT_GLYPH per the existing
// getFallbackCodepoint flow.
constexpr uint32_t aliasCodepoint(uint32_t cp) {
  switch (cp) {
    // Pre-existing alias: the Hawaiian okina / Latin extended turned comma
    // both look like a left single quote; keep mapping it to the typographic
    // single quote so any font shipping the typographic glyph wins. The
    // smart-quote branches below alias that target back to ASCII when the
    // font lacks it.
    case MODIFIER_LETTER_TURNED_COMMA:
      return LEFT_SINGLE_QUOTATION_MARK;

    // Smart single quotes / similar -> ASCII apostrophe (U+0027)
    case 0x2018:  // LEFT SINGLE QUOTATION MARK
    case 0x2019:  // RIGHT SINGLE QUOTATION MARK
    case 0x201A:  // SINGLE LOW-9 QUOTATION MARK
    case 0x201B:  // SINGLE HIGH-REVERSED-9 QUOTATION MARK
    case 0x2032:  // PRIME
      return 0x27;

    // Smart double quotes / similar -> ASCII quotation mark (U+0022)
    case 0x201C:  // LEFT DOUBLE QUOTATION MARK
    case 0x201D:  // RIGHT DOUBLE QUOTATION MARK
    case 0x201E:  // DOUBLE LOW-9 QUOTATION MARK
    case 0x201F:  // DOUBLE HIGH-REVERSED-9 QUOTATION MARK
    case 0x2033:  // DOUBLE PRIME
      return 0x22;

    // Dashes (incl. non-breaking hyphen) -> ASCII hyphen-minus (U+002D).
    // Loses semantic length distinction (em vs en) -- acceptable trade for
    // not showing '?' in dialogue dashes and parentheticals.
    case 0x2010:  // HYPHEN
    case 0x2011:  // NON-BREAKING HYPHEN
    case 0x2012:  // FIGURE DASH
    case 0x2013:  // EN DASH
    case 0x2014:  // EM DASH
    case 0x2015:  // HORIZONTAL BAR
    case 0x2212:  // MINUS SIGN (math)
      return 0x2D;

    // Bullets, geometric shapes, decoration glyphs -> ASCII asterisk
    // (U+002A). Covers the scene-break decorator chars publishers use
    // to separate scenes within a chapter (diamonds, stars, hollow
    // shapes) -- ChareInk and similarly-trimmed SD fonts often ship
    // without these specific geometric blocks. The renderer-level
    // getFallbackCodepoint miss-handler path will try to load the '*'
    // glyph on demand from the .cpfont's full intervals if the prewarmed
    // mini set didn't include it.
    case 0x2022:  // BULLET
    case 0x2023:  // TRIANGULAR BULLET
    case 0x25CF:  // BLACK CIRCLE
    case 0x25E6:  // WHITE BULLET
    case 0x25C6:  // BLACK DIAMOND
    case 0x25C7:  // WHITE DIAMOND
    case 0x25C8:  // WHITE DIAMOND CONTAINING BLACK SMALL DIAMOND
    case 0x25CA:  // LOZENGE
    case 0x2666:  // BLACK DIAMOND SUIT
    case 0x2662:  // WHITE DIAMOND SUIT
    case 0x2605:  // BLACK STAR
    case 0x2606:  // WHITE STAR
    case 0x2731:  // HEAVY ASTERISK
    case 0x2732:  // OPEN CENTRE ASTERISK
    case 0x2735:  // EIGHT POINTED PINWHEEL STAR
      return 0x2A;

    // Horizontal ellipsis -> ASCII period (U+002E). 1:1 alias loses the
    // three-dot appearance; ASCII '.' is in every chapter so this beats '?'.
    case 0x2026:
      return 0x2E;

    default:
      return cp;
  }
}

inline uint16_t solidAdvanceX(const EpdFontData* data, const EpdGlyph* emGlyph) {
  if (emGlyph && emGlyph->advanceX > 0) {
    return emGlyph->advanceX;
  }
  int px = data && data->ascender > 0 ? (data->ascender * 3 + 3) / 4 : 8;
  if (px < 1) px = 1;
  return static_cast<uint16_t>(fp4::fromPixel(px));
}

inline int solidHeight(const EpdFontData* data, uint32_t cp) {
  int ascender = data && data->ascender > 0 ? data->ascender : 8;
  if (cp == BLACK_SQUARE) {
    int height = (ascender * 2 + 2) / 3;
    return height > 1 ? height : 1;
  }
  return ascender > 1 ? ascender : 1;
}

inline int solidWidth(uint32_t cp, uint16_t advanceX, int height) {
  const int advancePx = fp4::toPixel(advanceX);
  if (cp == BLACK_SQUARE) {
    return height < advancePx ? height : advancePx;
  }
  return advancePx > 1 ? advancePx : 1;
}

inline int solidLeft(uint32_t cp, uint16_t advanceX, int width) {
  if (cp != BLACK_SQUARE) return 0;
  const int advancePx = fp4::toPixel(advanceX);
  return advancePx > width ? (advancePx - width) / 2 : 0;
}

inline int solidTop(const EpdFontData* data, uint32_t cp, int height) {
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  if (cp != BLACK_SQUARE || ascender <= height) return height;
  return height + (ascender - height) / 2;
}

inline uint16_t replacementAdvanceX(const EpdFontData* data, const EpdGlyph* emGlyph) {
  if (emGlyph && emGlyph->advanceX > 0) {
    return emGlyph->advanceX;
  }
  int px = data && data->ascender > 0 ? (data->ascender * 3 + 3) / 4 : 8;
  if (px < 3) px = 3;
  return static_cast<uint16_t>(fp4::fromPixel(px));
}

inline int replacementHeight(const EpdFontData* data) {
  const int ascender = data && data->ascender > 0 ? data->ascender : 8;
  return ascender > 3 ? ascender : 3;
}

inline int replacementWidth(uint16_t advanceX, int height) {
  const int advancePx = fp4::toPixel(advanceX);
  int width = advancePx < height ? advancePx : height;
  if (width < 3) width = 3;
  return width;
}

inline int replacementLeft(uint16_t advanceX, int width) {
  const int advancePx = fp4::toPixel(advanceX);
  return advancePx > width ? (advancePx - width) / 2 : 0;
}

inline int replacementTop(const EpdFontData* data, int height) {
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return ascender > height ? height + (ascender - height) / 2 : height;
}

inline uint16_t greekAdvanceX(const EpdFontData* data, const EpdGlyph* emGlyph, uint32_t cp) {
  if (cp == GREEK_SMALL_OMEGA && emGlyph && emGlyph->advanceX > 0) {
    return emGlyph->advanceX;
  }
  const int ascender = data && data->ascender > 0 ? data->ascender : 8;
  int px = cp == GREEK_CAPITAL_GAMMA ? (ascender * 7 + 5) / 10 : (ascender * 3 + 3) / 4;
  if (px < 1) px = 1;
  return static_cast<uint16_t>(fp4::fromPixel(px));
}

inline int greekHeight(const EpdFontData* data, uint32_t cp) {
  int ascender = data && data->ascender > 0 ? data->ascender : 8;
  if (cp != GREEK_CAPITAL_GAMMA) {
    ascender = (ascender * 3 + 3) / 4;
  }
  return ascender > 1 ? ascender : 1;
}

inline int greekWidth(uint32_t cp, uint16_t advanceX, int height) {
  const int advancePx = fp4::toPixel(advanceX);
  if (cp == GREEK_CAPITAL_GAMMA) {
    const int width = height > 2 ? (height * 6 + 5) / 10 : height;
    return width < advancePx ? width : advancePx;
  }
  return advancePx > 1 ? advancePx : 1;
}

inline int greekLeft(uint32_t cp, uint16_t advanceX, int width) {
  const int advancePx = fp4::toPixel(advanceX);
  if (cp == GREEK_CAPITAL_GAMMA) return 0;
  return advancePx > width ? (advancePx - width) / 2 : 0;
}

inline int greekTop(const EpdFontData* data, uint32_t cp, int height) {
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  if (cp == GREEK_CAPITAL_GAMMA || ascender <= height) return height;
  return height + (ascender - height) / 2;
}

}  // namespace syntheticGlyph
