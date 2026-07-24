#include "SleepCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
constexpr const char* kModuleTag = "SLPC";

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

bool endsWithCaseInsensitive(const std::string& s, const char* suffix) {
  const size_t slen = std::strlen(suffix);
  if (s.size() < slen) return false;
  for (size_t i = 0; i < slen; ++i) {
    char a = s[s.size() - slen + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}
}  // namespace

namespace SleepCache {

std::string cachePathFor(const std::string& sourcePath) {
  // Strip known image extensions so /.sleep/foo.png -> /.sleep/foo.slp.
  // Anything else keeps its extension and gets ".slp" appended (rare).
  std::string base = sourcePath;
  if (endsWithCaseInsensitive(base, ".png")) {
    base.resize(base.size() - 4);
  } else if (endsWithCaseInsensitive(base, ".bmp")) {
    base.resize(base.size() - 4);
  } else if (endsWithCaseInsensitive(base, ".jpg")) {
    base.resize(base.size() - 4);
  } else if (endsWithCaseInsensitive(base, ".jpeg")) {
    base.resize(base.size() - 5);
  }
  base.append(".slp");
  return base;
}

bool cacheExists(const std::string& sourcePath) {
  return Storage.exists(cachePathFor(sourcePath).c_str());
}

bool loadIntoFramebuffer(const std::string& sourcePath, uint8_t* dst, size_t dstBytes,
                         int expW, int expH) {
  if (dst == nullptr || expW <= 0 || expH <= 0) return false;
  const std::string path = cachePathFor(sourcePath);
  FsFile f;
  if (!Storage.openFileForRead(kModuleTag, path, f)) return false;

  uint8_t hdr[kHeaderSize];
  if (f.read(hdr, kHeaderSize) != kHeaderSize) {
    f.close();
    return false;
  }
  if (readU32(hdr + 0) != kMagic) { f.close(); return false; }
  if (hdr[4] != kVersion) { f.close(); return false; }
  if (hdr[5] != kFormat1bppRaw) { f.close(); return false; }
  if (readU16(hdr + 6) != static_cast<uint16_t>(expW)) { f.close(); return false; }
  if (readU16(hdr + 8) != static_cast<uint16_t>(expH)) { f.close(); return false; }
  const uint32_t payloadSize = readU32(hdr + 10);
  if (payloadSize == 0 || payloadSize > dstBytes) { f.close(); return false; }
  const size_t expectedStride = static_cast<size_t>((expW + 7) / 8);
  const size_t expectedPayload = expectedStride * static_cast<size_t>(expH);
  if (payloadSize != expectedPayload) { f.close(); return false; }

  const size_t nread = f.read(dst, payloadSize);
  f.close();
  if (nread != payloadSize) return false;
  LOG_INF(kModuleTag, "loaded %s (%u bytes)", path.c_str(), static_cast<unsigned>(payloadSize));
  return true;
}

void removeCache(const std::string& sourcePath) {
  const std::string path = cachePathFor(sourcePath);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    LOG_INF(kModuleTag, "removed stale cache: %s", path.c_str());
  }
}

bool pngHasTransparency(const std::string& sourcePath) {
  // A PNG is transparent if its color type is 3 (indexed palette that
  // may carry per-index alpha in tRNS), 4 (grayscale+alpha), or 6
  // (truecolor+alpha), OR if a tRNS chunk is present ahead of IDAT.
  // We stop scanning at the first IDAT (all metadata chunks must
  // precede it) so this touches at most ~2 KB on typical PNGs.
  FsFile f;
  if (!Storage.openFileForRead(kModuleTag, sourcePath, f)) return false;
  uint8_t sig[8];
  if (f.read(sig, 8) != 8) { f.close(); return false; }
  static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(sig, kPngSig, 8) != 0) { f.close(); return false; }

  // IHDR: 4 length + 4 type + 13 data + 4 CRC. Read the 8-byte header
  // then the 13-byte data. Byte 9 of IHDR data (offset 25 in file) is
  // color type.
  uint8_t ihdr[25];
  if (f.read(ihdr, 25) != 25) { f.close(); return false; }
  if (std::memcmp(ihdr + 4, "IHDR", 4) != 0) { f.close(); return false; }
  const uint8_t colorType = ihdr[17];  // 8 (chunk hdr) + 9
  if (colorType == 3 || colorType == 4 || colorType == 6) {
    f.close();
    return true;
  }

  // Colour type 0 (grayscale) or 2 (truecolor) -- transparent only if
  // a tRNS chunk precedes IDAT. Skip past IHDR's 4-byte CRC.
  uint8_t crc[4];
  f.read(crc, 4);
  uint8_t chunkHdr[8];
  int chunksSeen = 0;
  while (chunksSeen++ < 32 && f.read(chunkHdr, 8) == 8) {
    const uint32_t len = (static_cast<uint32_t>(chunkHdr[0]) << 24) |
                         (static_cast<uint32_t>(chunkHdr[1]) << 16) |
                         (static_cast<uint32_t>(chunkHdr[2]) << 8) |
                         static_cast<uint32_t>(chunkHdr[3]);
    if (std::memcmp(chunkHdr + 4, "tRNS", 4) == 0) { f.close(); return true; }
    if (std::memcmp(chunkHdr + 4, "IDAT", 4) == 0) { f.close(); return false; }
    if (!f.seekCur(len + 4)) break;  // skip data + CRC
  }
  f.close();
  return false;
}

bool bmpHasGreyscale(const std::string& sourcePath) {
  // BMP file layout:
  //   [0..1]   'BM'
  //   [2..13]  file header (size, reserved, pixel-data offset)
  //   [14..17] DIB header size (u32)
  //   [18..21] width  (u32)   -- BITMAPINFOHEADER onward
  //   [22..25] height (u32)
  //   [26..27] planes (u16, always 1)
  //   [28..29] bpp    (u16)   <-- what we want
  // Every DIB header variant (BITMAPINFOHEADER and later) places bpp at
  // the same offset, so a single read handles them all.
  FsFile f;
  if (!Storage.openFileForRead(kModuleTag, sourcePath, f)) return false;
  uint8_t hdr[30];
  const int n = f.read(hdr, 30);
  f.close();
  if (n != 30) return false;
  if (hdr[0] != 'B' || hdr[1] != 'M') return false;
  const uint16_t bpp = readU16(hdr + 28);
  return bpp > 1;
}

bool saveFramebuffer(const std::string& sourcePath, const uint8_t* src, size_t srcBytes,
                     int w, int h) {
  if (src == nullptr || w <= 0 || h <= 0) return false;
  const size_t stride = static_cast<size_t>((w + 7) / 8);
  const size_t payloadSize = stride * static_cast<size_t>(h);
  if (payloadSize == 0 || payloadSize > srcBytes) return false;

  const std::string path = cachePathFor(sourcePath);
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    Storage.mkdir(path.substr(0, slash).c_str());
  }

  FsFile f;
  if (!Storage.openFileForWrite(kModuleTag, path, f)) return false;

  uint8_t hdr[kHeaderSize] = {0};
  writeU32(hdr + 0, kMagic);
  hdr[4] = kVersion;
  hdr[5] = kFormat1bppRaw;
  // hdr[6..7] payload = width
  writeU16(hdr + 6, static_cast<uint16_t>(w));
  writeU16(hdr + 8, static_cast<uint16_t>(h));
  writeU32(hdr + 10, static_cast<uint32_t>(payloadSize));
  // hdr[14..15] reserved stays 0

  const size_t hdrW = f.write(hdr, kHeaderSize);
  const size_t bodyW = f.write(src, payloadSize);
  f.close();
  const bool ok = (hdrW == kHeaderSize) && (bodyW == payloadSize);
  if (ok) {
    LOG_INF(kModuleTag, "saved %s (%u bytes)", path.c_str(), static_cast<unsigned>(payloadSize));
  } else {
    LOG_ERR(kModuleTag, "save incomplete for %s (hdr=%u body=%u/%u)", path.c_str(),
            static_cast<unsigned>(hdrW), static_cast<unsigned>(bodyW),
            static_cast<unsigned>(payloadSize));
  }
  return ok;
}

}  // namespace SleepCache
