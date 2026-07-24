#include "ZipFile.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>

#include <algorithm>

struct ZipInflateCtx {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to ZipInflateCtx*
  FsFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

int zipReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<ZipInflateCtx*>(uncomp);
  if (ctx->fileRemaining == 0) return -1;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const size_t bytesRead = ctx->file->read(ctx->readBuf, toRead);
  ctx->fileRemaining -= bytesRead;

  if (bytesRead == 0) return -1;

  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}
}  // namespace

int ZipFile::iterateEntries(const std::function<bool(const std::string&, const FileStatSlim&)>& callback) {
  // Streaming variant of loadAllFileStatSlims that NEVER builds the
  // ~13 KB unordered_map. Same central-dir walk logic, but each entry
  // is yielded to the caller (typically for immediate extraction) and
  // then discarded before reading the next one. Memory ceiling: a few
  // hundred bytes max.
  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  int count = 0;

  while (file.available()) {
    if (file.read(&sig, 4) != 4) break;
    if (sig != 0x02014b50) break;  // End of central directory marker hit.

    FileStatSlim fileStat = {};
    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen >= sizeof(itemName)) {
      // Oversized name: skip past it + its extra/comment, keep walking.
      file.seekCur(nameLen + m + k);
      continue;
    }
    file.read(itemName, nameLen);
    itemName[nameLen] = '\0';
    file.seekCur(m + k);  // skip extra field + comment
    count++;

    // Save cursor BEFORE calling the callback. The callback typically
    // invokes readFileToStream / readFileToMemory, which both `file.seek`
    // to the entry's local-header offset to read the actual file data --
    // that trample the central-dir cursor we're walking. Restore it after
    // the callback returns so the next `file.read(&sig, 4)` reads the
    // NEXT central-dir record, not garbage at wherever readFileToStream
    // left the head.
    const uint32_t cursorBefore = file.position();
    const std::string nameStr(itemName, nameLen);
    const bool cont = callback(nameStr, fileStat);
    file.seek(cursorBefore);
    if (!cont) break;
  }

  return count;
}

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    file.seekCur(6);
    file.read(&fileStat->method, 2);
    file.seekCur(8);
    file.read(&fileStat->compressedSize, 4);
    file.read(&fileStat->uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat->localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        // Found it! Update cursor to next entry
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // v18.9.9.320: streaming tail scan. Fixed 4 KB window slid backwards
  // through the file up to 64 KB (ZIP spec max EOCD comment). Adjacent
  // windows overlap by 3 bytes so a 4-byte signature straddling a
  // boundary is still caught. Replaces the {65536, 16384, 4096, 1024}
  // tier-ladder malloc that failed under fragmented heap: on ESP32-C3
  // after a long session maxAlloc collapses toward the 4 KB tier, and
  // repackaged EPUBs (Anna's Archive, calibre-signed, etc.) that push
  // the EOCD 10-30 KB back from EOF then read as "EOCD signature not
  // found" -- surfaced to the user as an unresponsive reader on the
  // decline-prebake path. With this streaming approach there is no
  // fragmentation dependency: any book that has 4 KB contiguous heap
  // available (essentially always) can be opened.
  constexpr size_t kWindowSize = 4096;
  constexpr size_t kOverlap = 3;  // signature is 4 bytes; N-1 = 3-byte overlap catches straddles
  // ZIP spec max EOCD comment is 65535 bytes; +22 for the record itself.
  constexpr size_t kMaxScanBytes = 65535 + 22;

  uint8_t* buffer = static_cast<uint8_t*>(malloc(kWindowSize));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate 4KB EOCD scan buffer");
    return false;
  }

  // windowHigh is the exclusive upper file-offset of the current window.
  // First iteration reads the tail chunk ending at fileSize.
  size_t windowHigh = fileSize;
  size_t totalScanned = 0;
  int64_t foundSigFileOffset = -1;

  while (windowHigh > 0 && totalScanned < kMaxScanBytes) {
    const size_t chunkSize = windowHigh < kWindowSize ? windowHigh : kWindowSize;
    const size_t readAt = windowHigh - chunkSize;
    file.seek(readAt);
    const size_t nread = file.read(buffer, chunkSize);
    if (nread != chunkSize) {
      LOG_ERR("ZIP", "EOCD scan short read at %u (want %u got %u)",
              static_cast<unsigned>(readAt), static_cast<unsigned>(chunkSize),
              static_cast<unsigned>(nread));
      free(buffer);
      return false;
    }

    // Scan this window backwards for the 4-byte signature. Assemble via
    // byte reads so we don't rely on unaligned uint32_t deref (works on
    // ESP32-C3 but the byte-safe form is portable and no measurably
    // slower for the maybe-16 iterations we do here).
    for (int i = static_cast<int>(chunkSize) - 4; i >= 0; i--) {
      constexpr uint32_t kSignature = 0x06054b50u;
      const uint32_t v = static_cast<uint32_t>(buffer[i]) |
                         (static_cast<uint32_t>(buffer[i + 1]) << 8) |
                         (static_cast<uint32_t>(buffer[i + 2]) << 16) |
                         (static_cast<uint32_t>(buffer[i + 3]) << 24);
      if (v == kSignature) {
        foundSigFileOffset = static_cast<int64_t>(readAt) + i;
        break;
      }
    }
    if (foundSigFileOffset >= 0) break;

    totalScanned += chunkSize;
    // Slide window backwards by (kWindowSize - kOverlap). Stop when we've
    // reached the start of file (windowHigh <= chunkSize means chunk was
    // the whole remaining file and we've covered it).
    if (chunkSize < kWindowSize) break;  // ran out of file
    windowHigh -= (kWindowSize - kOverlap);
  }

  if (foundSigFileOffset < 0) {
    // v18.9.9.322: dump the tail 32 bytes so the user + field reports can
    // distinguish "file truncated" (last bytes are zeros / random garbage
    // or don't look like a ZIP tail) from "Zip64 EOCD locator present"
    // (0x50 0x4b 0x06 0x07 signature within the last 40 bytes) from
    // "genuinely no ZIP structure." Also log file size vs scanned so the
    // shape of the file is captured.
    const size_t tailLen = fileSize < 32 ? fileSize : 32;
    file.seek(fileSize - tailLen);
    uint8_t tail[32];
    const size_t tailRead = file.read(tail, tailLen);
    char tailHex[65];
    tailHex[0] = '\0';
    for (size_t i = 0; i < tailRead && i < 32; ++i) {
      snprintf(tailHex + i * 2, 3, "%02x", tail[i]);
    }
    LOG_ERR("ZIP",
            "EOCD signature not found (fileSize=%u, scanned=%u from EOF, tail32=%s)",
            static_cast<unsigned>(fileSize), static_cast<unsigned>(totalScanned), tailHex);
    // Quick Zip64 sniff: EOCD64 locator sig is 0x07064b50 (bytes 50 4b 06 07);
    // sits 20 bytes before the traditional EOCD or right at EOF for pure-Zip64.
    // If we see it in the tail, that's the diagnosis.
    for (size_t i = 0; i + 4 <= tailRead; ++i) {
      if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x06 && tail[i + 3] == 0x07) {
        LOG_ERR("ZIP", "Zip64 EOCD locator found at tail offset %u -- this file uses Zip64 (not yet supported)",
                static_cast<unsigned>(i));
        break;
      }
    }
    free(buffer);
    return false;
  }

  // Read the 22-byte EOCD record fresh via absolute seek+read. Simpler
  // than tracking whether the signature landed near the top edge of the
  // buffer where offset+22 would spill past what we captured.
  file.seek(static_cast<size_t>(foundSigFileOffset));
  const size_t eocdRead = file.read(buffer, 22);
  if (eocdRead != 22) {
    LOG_ERR("ZIP", "EOCD record short read at %d", static_cast<int>(foundSigFileOffset));
    free(buffer);
    return false;
  }
  // Relative positions within EOCD:
  //   Offset 10: total number of entries (u16 LE)
  //   Offset 16: offset of start of central directory (u32 LE)
  zipDetails.totalEntries = static_cast<uint16_t>(buffer[10]) |
                            (static_cast<uint16_t>(buffer[11]) << 8);
  zipDetails.centralDirOffset = static_cast<uint32_t>(buffer[16]) |
                                (static_cast<uint32_t>(buffer[17]) << 8) |
                                (static_cast<uint32_t>(buffer[18]) << 16) |
                                (static_cast<uint32_t>(buffer[19]) << 24);
  zipDetails.isSet = true;
  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

bool ZipFile::getCompressionMethod(const char* filename, uint16_t* method) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *method = fileStat.method;
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      free(data);
      return nullptr;
    }

    const size_t dataRead = file.read(deflatedData, deflatedDataSize);

    if (dataRead != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %d got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = false;
    {
      InflateReader r;
      r.init(false);
      r.setSource(deflatedData, deflatedDataSize);
      success = r.read(data, inflatedDataSize);
    }
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      if (out.write(buffer, dataRead) != dataRead) {
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        free(buffer);
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;

    // Size the back-reference window to this entry's uncompressed size (capped
    // at the 32 KB DEFLATE max). A modest chapter then needs only a modest
    // contiguous block, so cold loads succeed even when a BLE remote has
    // fragmented the heap and a full 32 KB block is unavailable.
    if (!ctx.reader.init(true, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to init inflate reader (free=%u, maxAlloc=%u, chunk=%zu, dict=%u)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize, static_cast<unsigned>(inflatedDataSize));
      return false;
    }

    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer (free=%u, maxAlloc=%u, chunk=%zu)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap(), chunkSize);
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize);
      free(fileReadBuffer);
      return false;
    }

    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;
    ctx.reader.setReadCallback(zipReadCallback);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStatus status = ctx.reader.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          break;
        }
      }

      if (status == InflateStatus::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStatus::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStatus::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // ctx.reader destructor frees the ring buffer
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
