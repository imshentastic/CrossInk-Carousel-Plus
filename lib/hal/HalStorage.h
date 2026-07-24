#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <vector>

class HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);

  // v18.9.9.311: atomic write + rolling backup. Sequence:
  //   1. Write to <path>.tmp (fresh file).
  //   2. On success, rename existing <path> to <path>.bak (previous good).
  //   3. Rename <path>.tmp to <path>.
  //   4. Retries the whole sequence once after 100 ms on failure -- helps
  //      with the "cheap SD card transient write reject" pattern users
  //      report on the bundled cards.
  // Post-condition guarantee: EITHER <path> is the newly-written content,
  // OR the previous <path> and <path>.bak are both intact. A crash / power
  // loss mid-sequence can leave <path>.tmp behind but never corrupts the
  // main file. Callers can use readFileWithFallback to auto-recover from
  // .bak when a subsequent read finds <path> unreadable.
  bool writeFileWithBackup(const char* path, const String& content);

  // v18.9.9.311: paired reader. Tries <path> first; on empty/failure tries
  // <path>.bak. Sets `outFromBackup=true` iff .bak served the content, so
  // the caller can log a warning or (later) prompt the user. Returns the
  // content as a String -- empty String means both primary and .bak failed.
  String readFileWithFallback(const char* path, bool& outFromBackup);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

 private:
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool sync();
  bool rename(const char* newPath);
  bool isDirectory() const;
  // CrumBLE: file timestamps as chronologically-sortable keys (FAT date<<16 |
  // FAT time), 0 when unavailable. CREATE time is set by the OS when the file is
  // written to *this* SD card (copy time) -- the right signal for "recently
  // added". MODIFY time is preserved from the source on copy (authoring date),
  // so it's the fallback only.
  uint32_t getCreateTimeKey();
  uint32_t getModifyTimeKey();
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Only do renaming FsFile to HalFile if this header is included by downstream code
// The renaming is to allow using the thread-safe HalFile instead of the raw FsFile, without needing to change the
// downstream code
#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
