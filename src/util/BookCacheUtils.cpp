#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>
#include <iterator>

namespace {

// CrumBLE 4.4 (ported from upstream CrossInk v1.3.3 reading-stats split):
// minimum preservation infrastructure needed for "Clear Reading Cache" to
// keep per-book stats. The full upstream BookCacheUtils preserves several
// other state files across uploads/etc.; CrumBLE only needs the stats path.
struct PreservedCacheFile {
  const char* name;
  const char* tmpName;
};

constexpr PreservedCacheFile BOOK_STATS_FILES[] = {
    {"stats.bin", "clear_preserve_stats.bin"},
};

bool restorePreservedFiles(const std::string& cachePath, const PreservedCacheFile* files, const size_t count,
                           const bool* movedFiles) {
  bool ok = true;
  bool restoredAny = false;
  for (size_t i = 0; i < count; i++) {
    if (movedFiles && !movedFiles[i]) continue;
    const std::string tmpPath = cachePath + "." + files[i].tmpName;
    if (!Storage.exists(tmpPath.c_str())) continue;

    Storage.mkdir(cachePath.c_str());
    const std::string finalPath = cachePath + "/" + files[i].name;
    if (Storage.exists(finalPath.c_str())) {
      Storage.remove(finalPath.c_str());
    }
    if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
      LOG_ERR("BookCache", "Failed to restore preserved cache state: %s", finalPath.c_str());
      ok = false;
    } else {
      restoredAny = true;
    }
  }
  if (restoredAny) {
    LOG_DBG("BookCache", "Restored preserved user cache state: %s", cachePath.c_str());
  }
  return ok;
}

bool preserveStateFiles(const std::string& cachePath, const PreservedCacheFile* files, const size_t count,
                        bool* movedFiles) {
  bool ok = true;
  for (size_t i = 0; i < count; i++) {
    if (movedFiles) movedFiles[i] = false;
    const std::string sourcePath = cachePath + "/" + files[i].name;
    const std::string tmpPath = cachePath + "." + files[i].tmpName;

    if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to remove stale preserved state temp: %s", tmpPath.c_str());
      ok = false;
      continue;
    }
    if (!Storage.exists(sourcePath.c_str())) continue;
    if (!Storage.rename(sourcePath.c_str(), tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to preserve cache state: %s", sourcePath.c_str());
      ok = false;
    } else if (movedFiles) {
      movedFiles[i] = true;
    }
  }
  return ok;
}

}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath) {
  if (cachePath.empty()) return false;
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("BookCache", "Cache does not exist, no action needed: %s", cachePath.c_str());
    return true;
  }

  constexpr size_t kCount = std::size(BOOK_STATS_FILES);
  bool movedFiles[kCount] = {};
  if (!preserveStateFiles(cachePath, BOOK_STATS_FILES, kCount, movedFiles)) {
    restorePreservedFiles(cachePath, BOOK_STATS_FILES, kCount, movedFiles);
    LOG_ERR("BookCache", "Aborted cache clear because preserved state could not be moved: %s", cachePath.c_str());
    return false;
  }

  const bool clearOk = Storage.removeDir(cachePath.c_str());
  const bool restoreOk = restorePreservedFiles(cachePath, BOOK_STATS_FILES, kCount, movedFiles);
  if (!clearOk) {
    LOG_ERR("BookCache", "Failed to clear cache directory: %s", cachePath.c_str());
  }
  return clearOk && restoreOk;
}
