#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  FsFile f;
  if (!Storage.openFileForWrite("ERS", progressPath, f)) {
    // Defensive recovery: a chip-tracked bug surfaced after the chapter
    // prebake feature shipped (and was reported even on v3.7.2) makes
    // progress.bin's open-for-write fail repeatedly on every page turn
    // for some books after a close+reopen+back-navigate sequence. Root
    // cause TBD (likely a leaked file handle or a stale FAT entry).
    // Mitigation here: if the open failed, try removing the file and
    // re-opening fresh. Costs one extra Storage.remove on the failure
    // path but unblocks the user who'd otherwise see "Could not save
    // progress" on every page turn for the rest of their session.
    LOG_ERR("ERS", "Open failed for %s; retrying after remove", progressPath.c_str());
    Storage.remove(progressPath.c_str());
    if (!Storage.openFileForWrite("ERS", progressPath, f)) {
      LOG_ERR("ERS", "Could not open progress file for write!");
      return false;
    }
    LOG_INF("ERS", "Recovered progress.bin write after remove+reopen");
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
