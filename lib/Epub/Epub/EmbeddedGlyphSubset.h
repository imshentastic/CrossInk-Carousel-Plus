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
// v2 (CrumBLE 4.3 second pass): kerning + ligatures are now embedded.
// Per-style header gained 8 bytes of count fields and the per-style data
// blob is followed by kernLeftEntries, kernRightEntries, kernMatrix, and
// ligaturePairs in that order. Adds ~700 bytes-2 KB per style depending
// on font density. Fixes the "text overshoots viewport / Outside range
// (480, *)" floods that happened on SD-font + BT books because the
// runtime was using glyph-only advance widths without the pair-kerning
// offsets the prebake CLI baked into wordXpos. v1 (kerning-less) sections
// are rejected by the v2 loader -- bump triggered a full re-bake of all
// section caches anyway, so callers must regenerate prebake artifacts.

#include <cstdint>

#include "../../EpdFont/EpdFontData.h"  // EpdUnicodeInterval, EpdGlyph

namespace embeddedGlyphSubset {

constexpr uint32_t BLOCK_MAGIC = 0x4253474C;  // "LGSB" little-endian
constexpr uint16_t BLOCK_VERSION = 2;
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

// On-disk per-style entry header (32 bytes). Followed by intervals, glyphs,
// bitmaps, kernLeftEntries, kernRightEntries, kernMatrix, ligaturePairs in
// that order. The four kern* and ligature* count fields drive the size of
// each blob; install code reads them and resizes the matching slot vectors.
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
  // v2 additions: kerning + ligature counts. Blobs of corresponding sizes
  // come after the bitmap, in the listed order.
  uint16_t kernLeftEntryCount;
  uint16_t kernRightEntryCount;
  uint8_t kernLeftClassCount;
  uint8_t kernRightClassCount;
  uint16_t ligaturePairCount;
} __attribute__((packed));
static_assert(sizeof(StyleHeader) == 32, "EmbeddedGlyphSubset::StyleHeader v2 must be 32 bytes");

}  // namespace embeddedGlyphSubset
