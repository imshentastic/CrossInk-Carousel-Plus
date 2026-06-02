#pragma once

// Host-side stand-in for the firmware's HalStorage (lib/hal/HalStorage.h).
// Same class names, same public method signatures the EPUB pipeline calls,
// implemented over std::filesystem + std::fstream so the parsers /
// BookMetadataCache / ZipFile compile-share between firmware and the
// crumble-prebake CLI.
//
// Anything HalStorage exposes on device that we don't use yet (directory
// iteration, FAT timestamps, etc.) is either stubbed to a sensible
// default or left compiling-but-unimplemented to provoke a clean error
// if the prebake tool grows into a path that needs it.

#include <Print.h>
#include <common/FsApiConstants.h>
#include <freertos/semphr.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

class HalFile;

class HalStorage {
 public:
  HalStorage() = default;

  // begin()/ready() are no-ops on host -- there's no SD card to mount.
  bool begin() { return true; }
  bool ready() const { return true; }

  // Listing -- not used in the prebake phase 1 surface, but stubbed so
  // accidental call sites still link. Returns empty vector.
  std::vector<std::string> listFiles(const char* path = "/", int maxFiles = 200);

  std::string readFile(const char* path);
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  bool writeFile(const char* path, const std::string& content);
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
 public:
  HalFile();
  ~HalFile() override;
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t size();
  size_t fileSize() { return size(); }
  uint64_t fileSize64() { return size(); }
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // single byte; -1 on EOF
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  // Explicitly override the bulk virtual. Print's version is pure since
  // making it pure-virtual surfaced an earlier subclass-override bug;
  // HalFile gets a direct implementation here so it can be instantiated.
  size_t write(const uint8_t* buf, size_t count) override;
  bool sync();
  bool isOpen() const;
  bool close();
  explicit operator bool() const { return isOpen(); }

  // Pieces the firmware EPUB pipeline doesn't actually call on this
  // surface but that exist in the device-side HalFile. Stubbed so any
  // accidental call site links and behaves benignly.
  size_t getName(char*, size_t) { return 0; }
  bool rename(const char*) { return false; }
  bool isDirectory() const { return false; }
  uint32_t getCreateTimeKey() { return 0; }
  uint32_t getModifyTimeKey() { return 0; }
  void rewindDirectory() {}
  HalFile openNextFile();

 private:
  friend class HalStorage;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Same aliasing trick the firmware uses so downstream code can refer to
// FsFile and we substitute HalFile.
#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif
