#pragma once

#include <cstdint>
#include <string>

// v18.9.9.258: pre-baked sleep image cache. PNG / BMP decode of a
// full-screen sleep image is a big transient allocation (zlib inflate
// buffer + row buffer + intermediate BGR/greyscale rows) that fails
// or barely fits on tight post-BT / post-book-open heap. This module
// caches the ALREADY-decoded 1bpp framebuffer bytes on SD so sleep
// entry is a straight fread into the display framebuffer -- zero
// decoder allocations, zero transient buffers, one panel refresh.
//
// File format (v1, header = 16 bytes little-endian):
//   [0..3]   magic "SLPX"
//   [4]      version 0x01
//   [5]      format  0 = raw 1bpp packed MSB-first (matches framebuffer)
//   [6..7]   width  (u16, panel width in pixels)
//   [8..9]   height (u16, panel height in pixels)
//   [10..13] payloadSize (u32, raw byte length = stride * height)
//   [14..15] reserved
//   [16..]   payload (payloadSize bytes)
//
// stride = (width + 7) / 8. For a 512x760 X3 panel that's 64 * 760 =
// 48640 bytes of payload + 16 header = ~48 KB per .slp file. Fine on
// SD (~half a MB for 10 sleep images).
//
// Rebake triggers:
//   - Mismatched width/height (panel replaced, orientation flip locked in)
//   - Version bump
//   - Explicit "Rebake sleep images" settings action
//
// The bake step decodes each source PNG/BMP once via the same code path
// SleepActivity uses at runtime, then snapshots the framebuffer bytes
// to the .slp companion file. Runs from the Settings > Display "Bake
// sleep images" action; heap-heavy, but one-time.

namespace SleepCache {

constexpr uint32_t kMagic = 0x58504C53u;   // 'SLPX' little-endian
constexpr uint8_t  kVersion = 1;
constexpr uint8_t  kFormat1bppRaw = 0;
constexpr size_t   kHeaderSize = 16;

// Derive the .slp companion path for a sleep source image. E.g.
// "/.sleep/foo.png" -> "/.sleep/foo.slp". Handles paths with or
// without .png/.bmp extension.
std::string cachePathFor(const std::string& sourcePath);

// Attempt to load a .slp cache into the framebuffer. Returns true if
// the header matches (magic, version, format, panel dimensions AND
// payload size) AND the payload was read cleanly. On any mismatch or
// IO error, returns false and the framebuffer is untouched -- caller
// falls through to the source decoder as before.
//
// `dst` is the display framebuffer; `dstBytes` is its capacity; expW/H
// are the current panel dimensions. Panel dims embedded in the header
// must match exactly -- a rotated/replaced panel invalidates the cache.
bool loadIntoFramebuffer(const std::string& sourcePath, uint8_t* dst, size_t dstBytes,
                          int expW, int expH);

// Write the current framebuffer contents to the .slp companion. Called
// from the Bake action after decoding the source into the framebuffer.
// Fire-and-forget: returns true on success. Creates the parent directory
// if needed. Overwrites any existing .slp for this source.
bool saveFramebuffer(const std::string& sourcePath, const uint8_t* src, size_t srcBytes,
                      int w, int h);

// Convenience: does the .slp exist? Used by the Bake action to decide
// whether to skip an already-baked source. Doesn't validate contents.
bool cacheExists(const std::string& sourcePath);

// Delete the .slp companion if present. No-op if missing. Used by the
// bake action to purge stale caches for transparent PNGs -- those must
// go through the runtime composite path (reader-page background), so
// a baked-with-white-bg .slp is worse than no cache at all.
void removeCache(const std::string& sourcePath);

// Quick PNG-header sniff: is this file a PNG whose pixels may be
// transparent? Reads only the first few KB and does no decoding. Returns
// true for color types 3/4/6 (indexed with palette alpha / grayscale+alpha /
// truecolor+alpha) or when a tRNS chunk is found before IDAT. Returns
// false for non-PNGs and opaque PNGs (types 0/2 with no tRNS).
bool pngHasTransparency(const std::string& sourcePath);

// Quick BMP DIB-header sniff: does this file store more than 1 bit per
// pixel (i.e. Bitmap::hasGreyscale() would return true after full parse)?
// Reads ~30 bytes off SD. Grayscale BMPs need the runtime path so the
// second grayscale pass can render -- .slp only holds a 1bpp framebuffer.
bool bmpHasGreyscale(const std::string& sourcePath);

}  // namespace SleepCache
