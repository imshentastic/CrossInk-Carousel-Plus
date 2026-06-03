#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace CacheWriteRecovery {

// Defensive open-for-write that survives the FAT-state class of bugs
// where a per-book cache directory ends up with a directory entry that
// can be opened for read but can't be re-opened for write (the SD
// library returns false from sd.open(path, O_RDWR|O_CREAT|O_TRUNC)
// repeatedly until the entry is removed and re-created from scratch).
//
// Caller usage stays identical to the bare openFileForWrite -- one
// boolean return -- so writers don't need to know about the recovery
// path. On a successful first-try open we add zero overhead; on the
// failure path we log the recovery so the rate of these events is
// visible in serial / chip-tracked.
//
// This was originally introduced for EpubReader's progress.bin after
// a user hit a state where every page turn raised "Could not save
// progress" and only Disk Utility's First Aid (which repairs FAT
// cluster chains) cleared it. Pulling the pattern out into one place
// so the other per-book writers (XTC progress, TXT progress + index,
// reading stats, ...) all get the same recovery without duplicating
// the inline retry block at each site.
inline bool openForWriteOrRecover(const char* tag, const std::string& path, FsFile& f) {
  if (Storage.openFileForWrite(tag, path, f)) {
    return true;
  }
  LOG_ERR(tag, "Open failed for %s; retrying after remove", path.c_str());
  Storage.remove(path.c_str());
  if (!Storage.openFileForWrite(tag, path, f)) {
    return false;
  }
  LOG_INF(tag, "Recovered %s write after remove+reopen", path.c_str());
  return true;
}

}  // namespace CacheWriteRecovery
