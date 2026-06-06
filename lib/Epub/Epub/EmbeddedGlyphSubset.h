#pragma once

// CrumBLE 4.3: on-disk layout for the per-section embedded glyph subset block.
//
// This block lives inside a v39 section file at the offset recorded in the
// header's embeddedGlyphSubsetOffset field. It carries everything the renderer
// needs to draw the section's working glyph set WITHOUT consulting the source
// .cpfont at runtime — eliminating the SdCardFont miniData overhead (~14 KB
// per active style) that was blocking BT + SD-font book combos on the
// ESP32-C3 heap budget.
//
// Block layout:
//
//   Block header (16 bytes):
//     uint32_t magic              = SECTION_GLYPH_BLOCK_MAGIC ("LGSB" LE)
//     uint16_t version            = SECTION_GLYPH_BLOCK_VERSION (currently 1)
//     uint8_t  styleCount         (1..4)
//     uint8_t  reserved           = 0
//     uint32_t cpfontContentHash  (matches SdCardFont::contentHash() of the
//                                  .cpfont this section was baked against;
//                                  loader validates before installing)
//     uint32_t reserved2          = 0 (future use)
//
//   Per-style entry (24 bytes header + variable-size data, repeated styleCount
//   times in styleId order). Style-header layout:
//     uint8_t  styleId            (EpdFontFamily::Style: 0=REG, 1=BOLD, 2=ITALIC, 3=BI)
//     uint8_t  flags              (bit 0: is2Bit — 2-bit glyph bitmaps; 0 = 1-bit)
//     uint16_t reserved           = 0
//     uint32_t intervalCount
//     uint32_t glyphCount
//     uint32_t bitmapDataSize     (bytes; sum of all glyph bitmap sizes for this style)
//     uint16_t advanceY           (EpdFontData::advanceY, newline distance)
//     int16_t  ascender
//     int16_t  descender
//     uint16_t reserved2          = 0
//
//   Followed immediately by the style's variable-size data, in order:
//     intervalCount * sizeof(EpdUnicodeInterval)  = 12 bytes each
//     glyphCount    * sizeof(EpdGlyph)            = 16 bytes each
//     bitmapDataSize bytes (raw concatenated glyph bitmaps)
//
// IMPORTANT: EpdGlyph::dataOffset values inside the block are rebased to be
// relative to the START of THIS STYLE's bitmap blob (not the source
// .cpfont's bitmap blob). The prebake CLI rebases them when emitting; the
// loader doesn't need to translate further.
//
// Kerning + ligatures are NOT yet embedded in v1 of this block; they would
// add ~kernLeftEntries*3 + kernRightEntries*3 + leftClasses*rightClasses +
// ligaturePairs*8 bytes per style (~5-15 KB on typical fonts). For the
// vertical-slice scope, kerning falls back to "no kerning applied" when an
// embedded subset is active. A future v2 of this block can add them
// without disturbing v1 readers (extend Style header with optional offsets
// to kern + ligature blobs).

#include <cstdint>

#include "../../EpdFont/EpdFontData.h"  // EpdUnicodeInterval, EpdGlyph

namespace embeddedGlyphSubset {

constexpr uint32_t BLOCK_MAGIC = 0x4253474C;  // "LGSB" little-endian
constexpr uint16_t BLOCK_VERSION = 1;
constexpr uint8_t MAX_STYLES = 4;

// Per-style flag bits.
constexpr uint8_t STYLE_FLAG_IS_2BIT = 1u << 0;

// On-disk block header (16 bytes).
struct BlockHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t styleCount;
  uint8_t reserved;
  uint32_t cpfontContentHash;
  uint32_t reserved2;
} __attribute__((packed));
static_assert(sizeof(BlockHeader) == 16, "EmbeddedGlyphSubset::BlockHeader must be 16 bytes");

// On-disk per-style entry header (24 bytes). Followed by intervals, glyphs,
// and bitmaps in that order.
struct StyleHeader {
  uint8_t styleId;
  uint8_t flags;
  uint16_t reserved;
  uint32_t intervalCount;
  uint32_t glyphCount;
  uint32_t bitmapDataSize;
  uint16_t advanceY;
  int16_t ascender;
  int16_t descender;
  uint16_t reserved2;
} __attribute__((packed));
static_assert(sizeof(StyleHeader) == 24, "EmbeddedGlyphSubset::StyleHeader must be 24 bytes");

}  // namespace embeddedGlyphSubset
