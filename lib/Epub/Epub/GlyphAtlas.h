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

// CrumBLE 4.5.5: v2 bumps bitmapBytes (BlockHeader) and bitmapOffset
// (GlyphEntry) from uint16_t to uint32_t. The 64 KB cap that v1 imposed
// truncated CJK 2-bit atlases (~500+ glyphs at 16 pt = 60+ KB and
// climbing). v2 lifts that to 4 GB, which is meaningless on hardware
// but trivially fits any realistic atlas. Reader (Section.cpp:tryInstall
// GlyphAtlas) accepts both versions -- v1 atlases on device stay valid;
// only re-baked sections upgrade to v2 with full glyph coverage. Writer
// always emits v2 from this point.
constexpr uint8_t FORMAT_VERSION_V1 = 1;
constexpr uint8_t FORMAT_VERSION_V2 = 2;
constexpr uint8_t FORMAT_VERSION = FORMAT_VERSION_V2;  // what the writer emits

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
  uint32_t bitmapBytes;   // size of raw bitmap data trailing the per-style
                          // tables. v2 widened from uint16_t; 4 GB cap is
                          // moot on this hardware but lifts the 64 KB
                          // wire-format cap that was truncating CJK 2-bit
                          // atlases (~500 glyphs at 16 pt = 60+ KB).
} __attribute__((packed));

// CrumBLE 4.5.5: legacy v1 wire layout, retained ONLY so Section.cpp's
// reader can deserialise already-baked v41 prebakes on device. New
// bakes never use this -- BlockHeader (above) is what the writer emits.
struct BlockHeaderV1 {
  uint32_t magic;
  uint8_t version;
  uint8_t bitDepth;
  uint8_t styleMask;
  uint8_t reserved;
  uint16_t totalGlyphs;
  uint16_t bitmapBytes;   // 64 KB cap, the reason for v2
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
  uint32_t bitmapOffset;  // byte offset into the BlockHeader's bitmap
                          // payload. 0 is a valid offset (first glyph).
                          // v2 widened from uint16_t -- same reason as
                          // BlockHeader::bitmapBytes above. EpdGlyph::
                          // dataOffset already uint32_t so the runtime
                          // path was always wide enough; only the wire
                          // layout was constrained.
  uint8_t width;          // glyph bitmap width in pixels
  uint8_t height;         // glyph bitmap height in pixels
  int8_t left;            // x offset from cursor to bitmap's left edge
                          // (positive = right of cursor)
  int8_t top;             // y offset from baseline to bitmap's top edge
                          // (positive = above baseline)
  uint16_t advanceX;      // 12.4 fixed-point pixels (matches EpdGlyph)
} __attribute__((packed));

// CrumBLE 4.5.5: legacy v1 wire layout, reader-only (see BlockHeaderV1).
struct GlyphEntryV1 {
  uint32_t codepoint;
  uint16_t bitmapOffset;  // 64 KB-limited; widened in v2
  uint8_t width;
  uint8_t height;
  int8_t left;
  int8_t top;
  uint16_t advanceX;
} __attribute__((packed));

static_assert(sizeof(BlockHeader) == 14, "BlockHeader (v2) must be 14 bytes on the wire");
static_assert(sizeof(BlockHeaderV1) == 12, "BlockHeaderV1 must be 12 bytes on the wire");
static_assert(sizeof(StyleHeader) == 12, "StyleHeader must be 12 bytes on the wire");
static_assert(sizeof(GlyphEntry) == 14, "GlyphEntry (v2) must be 14 bytes on the wire");
static_assert(sizeof(GlyphEntryV1) == 12, "GlyphEntryV1 must be 12 bytes on the wire");

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
