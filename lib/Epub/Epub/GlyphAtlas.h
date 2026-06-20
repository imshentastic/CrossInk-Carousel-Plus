#pragma once

#include <cstdint>

// CrumBLE 4.4 pre-rendered glyph atlas (v4.4 task #35).
//
// On-disk and in-RAM format for a per-section glyph atlas: every codepoint
// the chapter uses, rendered to a 1-bit packed bitmap at chapter prebake
// time. Replaces the runtime decompression / SD-card read paths
// (FontDecompressor, SdCardFont miss handler, v39 embedded subset). At
// render time the renderer looks up the glyph in a flat table and blits
// the 1-bit bitmap directly to the framebuffer -- no decompression, no
// per-render allocations.
//
// See notes/v4.4-glyph-atlas-design.md for design rationale and memory
// budget analysis.

namespace glyphatlas {

// File-format magic; written as little-endian 4-byte word at the start of
// every AtlasBlock. 'GATL' = 0x4C544147.
constexpr uint32_t MAGIC = 0x4C544147;

constexpr uint8_t FORMAT_VERSION = 1;

// Bit-depth options for the packed bitmap payload. Phase 1 ships 1-bit only;
// 2-bit can be added later for higher-quality rendering when SD storage
// budget allows.
constexpr uint8_t BIT_DEPTH_1 = 1;
constexpr uint8_t BIT_DEPTH_2 = 2;

// Style ids (matches EpdFontFamily::Style enum). At most one slot per id is
// emitted per section; the styleMask in BlockHeader records which slots
// are present.
constexpr uint8_t STYLE_REGULAR = 0;
constexpr uint8_t STYLE_BOLD = 1;
constexpr uint8_t STYLE_ITALIC = 2;
constexpr uint8_t STYLE_BOLDITALIC = 3;

// On-disk header for the atlas block. Followed immediately by one
// StyleHeader + glyphCount * GlyphEntry per style bit set in styleMask,
// then the raw bitmap payload of bitmapBytes bytes (1-bit or 2-bit packed
// per format).
struct BlockHeader {
  uint32_t magic;         // MAGIC ('GATL')
  uint8_t version;        // FORMAT_VERSION
  uint8_t bitDepth;       // BIT_DEPTH_1 or BIT_DEPTH_2
  uint8_t styleMask;      // bit 0 = REGULAR, 1 = BOLD, 2 = ITALIC, 3 = BOLDITALIC
  uint8_t reserved;       // 0; reserved for future flags
  uint16_t totalGlyphs;   // sum of glyphCount across all styles
  uint16_t bitmapBytes;   // size of raw bitmap data trailing the per-style
                          // tables. Limit 64 KB per section, reasonable for
                          // 600-glyph 1-bit atlases.
} __attribute__((packed));

// Per-style metadata header. Written once per style bit set in styleMask,
// in numerical order (regular, bold, italic, boldItalic).
struct StyleHeader {
  uint8_t styleId;        // STYLE_REGULAR etc.
  uint8_t reserved;       // 0
  uint16_t glyphCount;    // number of GlyphEntry records that follow

  // Font metrics shared by every glyph in this style. Pulled from the
  // source EpdFontData so the renderer doesn't need a separate metrics
  // path for atlas-baked vs heap-allocated fonts.
  uint16_t ascender;      // in pixels
  uint16_t descender;     // in pixels (positive)
  uint16_t lineHeight;    // in pixels
  uint16_t spaceWidth;    // in pixels (12.4 fixed-point would be more
                          // accurate but the device's drawText currently
                          // works in whole pixels here)
} __attribute__((packed));

// Per-glyph entry. Sorted by codepoint ascending so the renderer can
// binary-search; for the typical ~600-glyph atlas a linear scan would also
// be fine but binary search costs nothing extra.
struct GlyphEntry {
  uint32_t codepoint;     // UTF-32 (single code point per entry; ligatures
                          // are emitted as separate entries with codepoints
                          // from the LIG_FIRST range -- see ligatures.h)
  uint16_t bitmapOffset;  // byte offset into the BlockHeader's bitmap
                          // payload. 0 is a valid offset (first glyph).
  uint8_t width;          // glyph bitmap width in pixels
  uint8_t height;         // glyph bitmap height in pixels
  int8_t left;            // x offset from cursor to bitmap's left edge
                          // (positive = right of cursor)
  int8_t top;             // y offset from baseline to bitmap's top edge
                          // (positive = above baseline)
  uint16_t advanceX;      // 12.4 fixed-point pixels (matches EpdGlyph)
} __attribute__((packed));

static_assert(sizeof(BlockHeader) == 12, "BlockHeader must be 12 bytes on the wire");
static_assert(sizeof(StyleHeader) == 12, "StyleHeader must be 12 bytes on the wire");
static_assert(sizeof(GlyphEntry) == 12, "GlyphEntry must be 12 bytes on the wire");

// Helpers for packed bitmap addressing. The bitmap payload is a CONTINUOUS
// bitstream (no per-row alignment) so a single (y*width + x) pixel index
// addresses straight into it. This matches GfxRenderer::renderCharImpl's
// blit loop (see GfxRenderer.cpp, the "pixelPosition" variable: pixel index
// runs 0..(width*height-1) across rows, and the byte address is
// pixelPosition >> 3 for 1-bit / >> 2 for 2-bit). The FontDecompressor's
// compactSingleGlyph likewise produces continuous bitstream output -- so
// builtin fonts feed the same renderer with the same packing convention.
// Byte-aligning rows would silently misalign every row past the first for
// any glyph whose width isn't a multiple of 8 (for 1-bit) or 4 (for 2-bit).
//
// rowBytes() is retained only as a per-row helper for callers that walk
// individual rows; the canonical glyph size is glyphBytes(), which uses
// continuous packing.
constexpr uint16_t rowBytes(uint8_t widthPx, uint8_t bitDepth) {
  return static_cast<uint16_t>((widthPx * bitDepth + 7) / 8);
}

constexpr uint16_t glyphBytes(uint8_t widthPx, uint8_t heightPx, uint8_t bitDepth) {
  // Continuous packing: total bits = width * height * bitDepth, rounded up
  // to the next whole byte for the glyph as a whole (not per-row).
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(widthPx) * static_cast<uint32_t>(heightPx) * bitDepth + 7) / 8);
}

}  // namespace glyphatlas
