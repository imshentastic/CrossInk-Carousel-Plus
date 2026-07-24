#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Arduino.h>  // ESP.getFreeHeap/getMaxAllocHeap for OOM diagnostics
#include <FS.h>       // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>

#include <cassert>
#include <new>  // std::nothrow

#include "HalSpiBus.h"

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // CrumBLE 4.5.6 (ported CP#2135): recursive mutex so the same task can
  // re-enter StorageLock without self-deadlock. openFileForRead/Write take
  // the lock and then assign to a HalFile& out-param; if that out-param
  // already held an Impl, its dtor takes the lock again to close the prior
  // FsFile under serialization (see HalFile::Impl::~Impl below).
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  HalSpiBus::Lock spiLock;
  return SDCard.begin();
}

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() : spiLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }

 private:
  HalSpiBus::Lock spiLock;
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

// v18.9.9.311: header docstring explains the sequence. Retries once with a
// 100 ms delay because the cheap bundled SD cards intermittently reject a
// write that succeeds on retry -- one of the user-reported failure modes
// the "Cannot save progress" incidents traced back to.
bool HalStorage::writeFileWithBackup(const char* path, const String& content) {
  if (!path || !*path) return false;
  const String tmpPath = String(path) + ".tmp";
  const String bakPath = String(path) + ".bak";

  auto attemptSequence = [&]() -> bool {
    // Step 1: write fresh content to tmp. If this fails there's nothing to
    // clean up on disk (writeFile leaves partial files behind on error but
    // they get overwritten next attempt).
    if (!writeFile(tmpPath.c_str(), content)) {
      LOG_ERR("HALSTOR", "writeFileWithBackup: tmp write failed for %s", path);
      return false;
    }
    // Step 2: promote current path -> bak (only if current exists).
    if (exists(path)) {
      // Remove any stale .bak so rename can land. SdFat's rename may fail
      // if destination exists depending on card; explicit remove is safe.
      remove(bakPath.c_str());
      if (!rename(path, bakPath.c_str())) {
        LOG_ERR("HALSTOR", "writeFileWithBackup: rename %s -> %s failed", path, bakPath.c_str());
        // tmp exists but we couldn't move current out of the way. Leave
        // tmp for next attempt; primary and bak are untouched. Bail.
        return false;
      }
    }
    // Step 3: rename tmp -> path.
    if (!rename(tmpPath.c_str(), path)) {
      LOG_ERR("HALSTOR", "writeFileWithBackup: rename tmp -> %s failed", path);
      // Fatal: primary is now gone (moved to bak in step 2) and tmp
      // couldn't take its place. Try to restore bak -> primary so the
      // caller isn't left with a hole. If THAT fails too, we're in a
      // bad state but the bak file still holds the last good content.
      rename(bakPath.c_str(), path);
      return false;
    }
    return true;
  };

  if (attemptSequence()) return true;
  // One retry after a short delay.
  delay(100);
  return attemptSequence();
}

String HalStorage::readFileWithFallback(const char* path, bool& outFromBackup) {
  outFromBackup = false;
  if (!path || !*path) return String();

  String primary = readFile(path);
  if (!primary.isEmpty()) return primary;

  const String bakPath = String(path) + ".bak";
  String bak = readFile(bakPath.c_str());
  if (!bak.isEmpty()) {
    LOG_INF("HALSTOR", "readFileWithFallback: primary %s empty/missing, using .bak", path);
    outFromBackup = true;
    return bak;
  }
  return String();
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // CrumBLE 4.5.6 (ported CP#2135): SdFat is not thread-safe; FsFile::close()
  // touches SD/SPI and must run under StorageLock or it races SdSpiCard's
  // m_spiActive across tasks and trips FreeRTOS's xTaskPriorityDisinherit
  // assert. Recursive mutex (see HalStorage ctor above) lets the same task
  // re-enter safely. The FsFile member destructor (DESTRUCTOR_CLOSES_FILE=1)
  // will close() again after the lock releases, but close() on an already-
  // closed FsFile is a no-op.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;

HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

HalFile::~HalFile() = default;

HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile = SDCard.open(path, oflag);
  // v18.9.9: nothrow Impl alloc. See main.cpp:1638 terminate-handler note --
  // make_unique<Impl> throws bad_alloc under tight heap (BT + Simple
  // Rendering), which -fno-exceptions converts to std::terminate + reboot.
  auto* rawImpl = new (std::nothrow) HalFile::Impl(std::move(fsFile));
  if (!rawImpl) {
    LOG_ERR("HFS", "open(%s): Impl alloc failed free=%u maxAlloc=%u", path, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return HalFile();
  }
  return HalFile(std::unique_ptr<HalFile::Impl>(rawImpl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  auto* rawImpl = new (std::nothrow) HalFile::Impl(std::move(fsFile));
  if (!rawImpl) {
    LOG_ERR("HFS", "openFileForRead(%s,%s): Impl alloc failed free=%u maxAlloc=%u", moduleName, path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    file = HalFile();
    return false;
  }
  file = HalFile(std::unique_ptr<HalFile::Impl>(rawImpl));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  auto* rawImpl = new (std::nothrow) HalFile::Impl(std::move(fsFile));
  if (!rawImpl) {
    LOG_ERR("HFS", "openFileForWrite(%s,%s): Impl alloc failed free=%u maxAlloc=%u", moduleName, path,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    file = HalFile();
    return false;
  }
  file = HalFile(std::unique_ptr<HalFile::Impl>(rawImpl));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
uint32_t HalFile::getCreateTimeKey() {
  HalStorage::StorageLock lock;
  if (impl == nullptr) return 0;
  uint16_t date = 0, timeOfDay = 0;
  if (!impl->file.getCreateDateTime(&date, &timeOfDay)) return 0;
  // FAT packs the date in the high half (year/month/day) and time in the low
  // half, so the combined value sorts chronologically.
  return (static_cast<uint32_t>(date) << 16) | timeOfDay;
}
uint32_t HalFile::getModifyTimeKey() {
  HalStorage::StorageLock lock;
  if (impl == nullptr) return 0;
  uint16_t date = 0, timeOfDay = 0;
  if (!impl->file.getModifyDateTime(&date, &timeOfDay)) return 0;
  return (static_cast<uint32_t>(date) << 16) | timeOfDay;
}
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  FsFile next = impl->file.openNextFile();
  auto* rawImpl = new (std::nothrow) Impl(std::move(next));
  if (!rawImpl) {
    LOG_ERR("HFS", "openNextFile: Impl alloc failed free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return HalFile();
  }
  return HalFile(std::unique_ptr<Impl>(rawImpl));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
