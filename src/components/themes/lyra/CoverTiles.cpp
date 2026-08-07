#include "CoverTiles.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace CoverTiles {

namespace {

constexpr const char* kModuleTag = "TILE";

// Little-endian read helpers. All multi-byte fields in the header are
// LE regardless of host endianness -- ESP32-C3 is LE, so on the current
// device these are memcpy-fast, but we spell them explicitly so a future
// port doesn't silently break.
uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void writeU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void writeU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace

std::string tilePathFor(const std::string& coverPath, uint8_t role) {
  // Strip trailing .bmp if present so /covers/abc.bmp -> /covers/abc-N.tile
  // rather than /covers/abc.bmp-N.tile. Any other extension is left in
  // place; the -N.tile suffix disambiguates regardless.
  std::string base = coverPath;
  static constexpr const char* kBmpExt = ".bmp";
  static constexpr size_t kBmpExtLen = 4;
  if (base.size() > kBmpExtLen &&
      std::memcmp(base.data() + base.size() - kBmpExtLen, kBmpExt, kBmpExtLen) == 0) {
    base.resize(base.size() - kBmpExtLen);
  }
  base.push_back('-');
  base.push_back(static_cast<char>('0' + role));
  base.append(".tile");
  return base;
}

bool loadTile(const std::string& coverPath, uint8_t role, uint8_t expFormat,
              int expW, int expH, int expStride,
              int expSideInner, int expSideOuter, int expSideCovW,
              uint8_t* dst, size_t dstBytes) {
  if (dst == nullptr || expW <= 0 || expH <= 0 || expStride <= 0) return false;
  const std::string path = tilePathFor(coverPath, role);
  // v4.7.5: Storage.open() rather than openFileForRead(). A missing tile is
  // the normal, expected outcome -- it means "not baked yet", and the caller
  // handles it by baking one. openFileForRead routes through the SDK's
  // SDCardManager, which unconditionally Serial.printf()s "File does not
  // exist" with no log level, so it cannot be quieted by tag or filtered out.
  // That printed four lines per shelf paint before the tiles existed, and
  // would print forever for a book with no cover art (nothing to bake from).
  // open() is the same read path without the narration, and costs no extra
  // SD lookup the way an exists() pre-check would.
  FsFile f = Storage.open(path.c_str());
  if (!f) return false;

  uint8_t hdr[kHeaderSize];
  if (f.read(hdr, kHeaderSize) != kHeaderSize) {
    f.close();
    return false;
  }
  // Validate magic + version + role + format up front; a single miss
  // rejects the whole file. Phase 2 bumped format 0 -> 1 -- any leftover
  // Phase 1 tile fails here and gets re-baked as 2bpp on the fallback
  // path.
  if (readU32(hdr + 0) != kMagic) { f.close(); return false; }
  if (hdr[4] != kVersion) { f.close(); return false; }
  if (hdr[5] != role) { f.close(); return false; }
  if (hdr[6] != expFormat) { f.close(); return false; }
  // Dimensions and perspective params must match the theme's current
  // expected values -- mismatch means the tile was baked for a different
  // slot geometry (theme retune or resolution change) and would render
  // at the wrong size.
  if (readU16(hdr + 8) != static_cast<uint16_t>(expW)) { f.close(); return false; }
  if (readU16(hdr + 10) != static_cast<uint16_t>(expH)) { f.close(); return false; }
  if (readU16(hdr + 12) != static_cast<uint16_t>(expStride)) { f.close(); return false; }
  if (readU16(hdr + 14) != static_cast<uint16_t>(expSideInner)) { f.close(); return false; }
  if (readU16(hdr + 16) != static_cast<uint16_t>(expSideOuter)) { f.close(); return false; }
  if (readU16(hdr + 18) != static_cast<uint16_t>(expSideCovW)) { f.close(); return false; }
  const uint16_t dataLen = readU16(hdr + 20);
  if (dataLen == 0 || dataLen > dstBytes) { f.close(); return false; }

  const size_t nread = f.read(dst, dataLen);
  f.close();
  return nread == dataLen;
}

bool saveTile(const std::string& coverPath, uint8_t role, uint8_t format,
              int w, int h, int stride,
              int sideInner, int sideOuter, int sideCovW,
              const uint8_t* src, size_t srcBytes) {
  if (src == nullptr || w <= 0 || h <= 0 || stride <= 0) return false;
  const size_t dataLen = static_cast<size_t>(stride) * static_cast<size_t>(h);
  if (dataLen == 0 || dataLen > srcBytes || dataLen > 0xFFFF) return false;

  const std::string path = tilePathFor(coverPath, role);

  // Ensure parent directory exists. tilePathFor preserves the source
  // cover's directory, so if the cover lived at /covers/... the tile
  // lands there too. mkdir with pFlag=true creates intermediate dirs.
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    Storage.mkdir(path.substr(0, slash).c_str());
  }

  FsFile f;
  if (!Storage.openFileForWrite(kModuleTag, path, f)) return false;

  uint8_t hdr[kHeaderSize] = {0};
  writeU32(hdr + 0, kMagic);
  hdr[4] = kVersion;
  hdr[5] = role;
  hdr[6] = format;
  // hdr[7] padding stays 0
  writeU16(hdr + 8, static_cast<uint16_t>(w));
  writeU16(hdr + 10, static_cast<uint16_t>(h));
  writeU16(hdr + 12, static_cast<uint16_t>(stride));
  writeU16(hdr + 14, static_cast<uint16_t>(sideInner));
  writeU16(hdr + 16, static_cast<uint16_t>(sideOuter));
  writeU16(hdr + 18, static_cast<uint16_t>(sideCovW));
  writeU16(hdr + 20, static_cast<uint16_t>(dataLen));
  // hdr[22..23] reserved, stays 0

  bool ok = f.write(hdr, kHeaderSize) == kHeaderSize;
  if (ok) ok = f.write(src, dataLen) == dataLen;
  f.close();
  if (!ok) {
    LOG_DBG(kModuleTag, "saveTile: write failed for %s", path.c_str());
  }
  return ok;
}

}  // namespace CoverTiles
