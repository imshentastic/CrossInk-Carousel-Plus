#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// v18.9.9.209 Phase 1: pre-baked perspective cover tiles for the Flow
// carousel. Each side cover in Flow is rendered by walking ~70k source
// BMP pixels through a trapezoidal projection at ~140 ms per cover per
// L/R press. The .tile file caches the 1bpp packed output of that walk
// on SD so a subsequent nav is a single ~10 ms read + drawPacked1bpp
// blit.
//
// Roles map 1-to-1 to the 4 side-cover slots in drawStackedCover:
//   1 = Left-far   (hL=inner, hR=outer)
//   2 = Left-near  (hL=inner, hR=outer)  -- same shape as L-far, only drawX differs
//   3 = Right-near (hL=outer, hR=inner)
//   4 = Right-far  (hL=outer, hR=inner)
// Since near/far on the same side share the tile shape, we only actually
// need two distinct tile files per book (role=1 for left, role=3 for
// right) -- but keeping four roles gives us a future path to per-slot
// variation without a format bump.
//
// Header carries perspective params so a theme retune (e.g. someone bumps
// sideInnerHeight) invalidates stale tiles without silently rendering a
// wrong-sized blit. Mismatch -> load fails -> LyraFlowTheme falls back
// to the streaming path -> tile gets re-baked with the new params.

namespace CoverTiles {

// File format (v1, header = 24 bytes little-endian):
//   [0..3]  magic         "TILE"
//   [4]     version       0x01
//   [5]     role          1..4
//   [6]     format        0 = 1bpp packed MSB-first (Phase 1 only)
//   [7]     padding
//   [8..9]  width         tile width in pixels
//   [10..11] height       tile height in pixels
//   [12..13] stride       bytes per row = (width + 7) / 8
//   [14..15] sideInner    perspective param -- must match theme constant
//   [16..17] sideOuter    perspective param
//   [18..19] sideCovW     side-cover width the tile was baked for
//   [20..21] dataLen      stride * height (u16; ~2.4 KB typical)
//   [22..23] reserved
//   [24..]  data          dataLen bytes of raw 1bpp packed pixels
constexpr uint32_t kMagic = 0x454C4954u;  // "TILE" little-endian
constexpr uint8_t kVersion = 1;
constexpr uint8_t kFormat1bpp = 0;  // Phase 1 only, kept for header ID
constexpr uint8_t kFormat2bpp = 1;  // Phase 2: 4-gray packed MSB-first per byte
constexpr size_t kHeaderSize = 24;

constexpr uint8_t kRoleLeftFar = 1;
constexpr uint8_t kRoleLeftNear = 2;
constexpr uint8_t kRoleRightNear = 3;
constexpr uint8_t kRoleRightFar = 4;
// v18.9.9.211 Phase 3: aspect-fit scaled center cover. Perspective params
// (sideInner/Outer/CovW) are unused for this role -- caller should pass 0
// on save and 0 on load. The w/h in the header carry the actual aspect-
// fit dimensions per book; if the source cover changes, the fit dims
// change -> load rejects -> re-bake.
constexpr uint8_t kRoleCenterThumb = 5;
// v4.7.5: Flow bookshelf cell. Aspect-FILL with crop (the shelf crops to a
// 2:3 cell; the center thumb above aspect-FITS), so it needs its own role
// even though the header shape is identical -- a fit-baked tile and a
// fill-baked tile can share dimensions but differ in pixels. Perspective
// params unused: pass 0 on both save and load.
//
// This role exists to take cover decode off the shelf's first paint. The
// cell BMPs are already stored at exactly cell size, so the old cost was
// not scaling -- it was the SD open plus a Bitmap decode through the
// renderer's LRU image cache, which refuses to allocate when maxAlloc is
// low. On a cold boot the library walk fragments the heap just before the
// shelf paints, so the cache refused and every cell fell back to a direct
// streamed decode. A .tile read needs no decode and no cache budget, which
// removes that heap coupling rather than just making it cheaper.
constexpr uint8_t kRoleShelfCell = 6;

// Derive the tile file path from the resolved cover-thumb path. E.g.
// "/covers/abcd-320.bmp" + role=1 -> "/covers/abcd-320-1.tile". Handles
// paths with or without a .bmp extension.
std::string tilePathFor(const std::string& coverPath, uint8_t role);

// Attempt to load a tile. Returns true on full success (magic, version,
// format, role, dimensions AND perspective params all match). On any
// mismatch or IO error, returns false and dst is untouched -- caller
// falls through to the streaming render + save path.
//
// dstBytes must be >= (expW * expH * bitsPerPixel + 7) / 8. expW/expH/
// expStride are the target dimensions the theme wants right now; the
// header must match these exactly. expSideInner/Outer/CovW likewise.
// expFormat is kFormat1bpp or kFormat2bpp -- a Phase 1 (1bpp) tile
// present alongside a Phase 2 (2bpp) reader gets rejected here, and
// the caller re-bakes in the new format on the next render.
bool loadTile(const std::string& coverPath, uint8_t role, uint8_t expFormat,
              int expW, int expH, int expStride,
              int expSideInner, int expSideOuter, int expSideCovW,
              uint8_t* dst, size_t dstBytes);

// Save a freshly-walked tile to SD. Fire-and-forget: returns true on
// success, false on any IO error (which is silently swallowed by the
// caller -- the render already succeeded, just no cache next time).
// Creates parent directory if needed.
bool saveTile(const std::string& coverPath, uint8_t role, uint8_t format,
              int w, int h, int stride,
              int sideInner, int sideOuter, int sideCovW,
              const uint8_t* src, size_t srcBytes);

}  // namespace CoverTiles
