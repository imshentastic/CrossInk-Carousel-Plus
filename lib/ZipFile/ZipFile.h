#pragma once
#include <HalStorage.h>

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  FsFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  // v18.9.9.331: promoted from private. Used by the FT upload tail-sanity
  // check to verify EOCD parses cleanly at upload time (catches
  // truncation/garbage/Zip64 that the old 32-byte tail heuristic missed).
  // Lightweight: one 4 KB buffer + streaming tail scan up to 64 KB.
  bool loadZipDetails();
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // CrumBLE: report a file's ZIP compression method (0 = STORED, 8 = DEFLATE).
  // The reader uses this to tell whether a chapter can cold-load without the
  // 32 KB inflate window (STORED) and therefore build in place under BLE.
  bool getCompressionMethod(const char* filename, uint16_t* method);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize);

  // CrumBLE: enumerate every entry's name + stats. Populated by
  // loadAllFileStatSlims(); reading before that call yields an empty map.
  // Const reference is fine to hand out -- the map's lifetime is tied to
  // this ZipFile instance and callers are expected to drain it during the
  // same scope. Used by the /api/upload-prebake-cache handler to walk the
  // optimizer's cache zip and extract each entry to SD without having to
  // know the entry names ahead of time.
  const std::unordered_map<std::string, FileStatSlim>& getEntries() const { return fileStatSlimCache; }

  // CrumBLE: streaming central-dir walk. Calls `callback(name, stat)` for
  // each entry in zip order, allocating only ONE entry's worth of memory
  // at a time (the local itemName[256] buffer + the FileStatSlim). Unlike
  // loadAllFileStatSlims this never builds the ~13 KB unordered_map, which
  // matters on ESP32 when extracting under heap pressure right after a
  // large upload completes. Callback returns true to continue, false to
  // stop walking. Returns total entries visited (or 0 on error).
  //
  // Trade-off vs. loadAllFileStatSlims: subsequent readFileToStream calls
  // re-scan the central dir for each lookup (slower), but the memory
  // ceiling drops from "size of cache zip" to a small constant. For the
  // prebake-cache extract path this is the difference between "extracts
  // nothing under pressure" and "extracts everything under pressure".
  int iterateEntries(const std::function<bool(const std::string&, const FileStatSlim&)>& callback);
};
