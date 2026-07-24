#include "StatsBackup.h"

#include <Arduino.h>
#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"

namespace {
constexpr const char* kTag = "STAT-BAK";
constexpr const char* kSourcePath = "/.crosspoint/global_stats.bin";
constexpr const char* kFilenamePrefix = "stats_";
constexpr const char* kFilenameSuffix = ".bin";

// unix epoch 2020-01-01. Below this the system clock hasn't been set.
constexpr uint32_t kValidClockThreshold = 1577836800u;

bool copyGlobalStatsToTmp(const std::string& tmpPath) {
  FsFile src;
  if (!Storage.openFileForRead(kTag, kSourcePath, src)) {
    LOG_ERR(kTag, "backup: source %s not found", kSourcePath);
    return false;
  }
  FsFile dst;
  if (!Storage.openFileForWrite(kTag, tmpPath.c_str(), dst)) {
    src.close();
    LOG_ERR(kTag, "backup: cannot open tmp %s", tmpPath.c_str());
    return false;
  }
  uint8_t buf[256];
  while (true) {
    const int n = src.read(buf, sizeof(buf));
    if (n <= 0) break;
    const size_t wrote = dst.write(buf, static_cast<size_t>(n));
    if (wrote != static_cast<size_t>(n)) {
      LOG_ERR(kTag, "backup: short write %u/%d", static_cast<unsigned>(wrote), n);
      src.close();
      dst.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }
  src.close();
  dst.flush();
  const bool syncOk = dst.sync();
  dst.close();
  if (!syncOk) {
    LOG_ERR(kTag, "backup: sync failed on tmp");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool buildFilenameFromClock(char* out, size_t outLen, bool includeHM) {
  const time_t now = time(nullptr);
  if (now < static_cast<time_t>(kValidClockThreshold)) return false;
  struct tm local;
  if (localtime_r(&now, &local) == nullptr) return false;
  if (includeHM) {
    snprintf(out, outLen, "stats_%04u-%02u-%02u_%02u%02u.bin",
             static_cast<unsigned>(1900 + local.tm_year),
             static_cast<unsigned>(local.tm_mon + 1),
             static_cast<unsigned>(local.tm_mday),
             static_cast<unsigned>(local.tm_hour),
             static_cast<unsigned>(local.tm_min));
  } else {
    snprintf(out, outLen, "stats_%04u-%02u-%02u.bin",
             static_cast<unsigned>(1900 + local.tm_year),
             static_cast<unsigned>(local.tm_mon + 1),
             static_cast<unsigned>(local.tm_mday));
  }
  return true;
}

// Fallback name when clock is invalid: stats_backup_NNN.bin, index = max+1.
bool buildFilenameFallback(char* out, size_t outLen) {
  int nextIdx = 1;
  const std::vector<String> entries = Storage.listFiles(StatsBackup::kBackupDir, 200);
  for (const auto& e : entries) {
    const char* name = e.c_str();
    if (strncmp(name, "stats_backup_", 13) != 0) continue;
    int idx = 0;
    if (sscanf(name + 13, "%d", &idx) == 1 && idx >= nextIdx) nextIdx = idx + 1;
  }
  snprintf(out, outLen, "stats_backup_%03d.bin", nextIdx);
  return true;
}

bool ensureDir() {
  if (!Storage.exists(StatsBackup::kBackupDir)) {
    if (!Storage.mkdir(StatsBackup::kBackupDir)) {
      LOG_ERR(kTag, "backup: cannot create %s", StatsBackup::kBackupDir);
      return false;
    }
  }
  return true;
}

bool writeBackupWithName(const char* filename) {
  if (!ensureDir()) return false;
  const std::string finalPath = std::string(StatsBackup::kBackupDir) + "/" + filename;
  const std::string tmpPath = finalPath + ".part";
  if (!copyGlobalStatsToTmp(tmpPath)) return false;
  // Atomic rename. If dest already exists (auto backup same day), remove first.
  if (Storage.exists(finalPath.c_str())) {
    Storage.remove(finalPath.c_str());
  }
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR(kTag, "backup: rename tmp -> %s failed", finalPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  LOG_INF(kTag, "backup written: %s", finalPath.c_str());
  StatsBackup::pruneBackups();
  return true;
}
}  // namespace

namespace StatsBackup {

bool writeManualBackup() {
  char filename[64];
  if (!buildFilenameFromClock(filename, sizeof(filename), /*includeHM=*/true)) {
    if (!buildFilenameFallback(filename, sizeof(filename))) return false;
  }
  return writeBackupWithName(filename);
}

bool maybeWriteAutoBackup() {
  if (SETTINGS.autoBackupStats == 0) return false;
  // Auto backups need a real clock so filenames sort correctly and
  // once-a-day-coalesce works. Fallback naming would produce a new file
  // every sleep entry, defeating the purpose.
  char filename[64];
  if (!buildFilenameFromClock(filename, sizeof(filename), /*includeHM=*/false)) {
    return false;  // skip silently when clock invalid
  }
  // Skip if today's backup already exists AND the source file hasn't grown
  // since. Cheap short-circuit — avoids rewriting an identical file on every
  // sleep entry.
  const std::string finalPath = std::string(kBackupDir) + "/" + filename;
  if (Storage.exists(finalPath.c_str())) {
    FsFile existing;
    FsFile source;
    if (Storage.openFileForRead(kTag, finalPath.c_str(), existing) &&
        Storage.openFileForRead(kTag, kSourcePath, source)) {
      const size_t existingSize = existing.size();
      const size_t sourceSize = source.size();
      existing.close();
      source.close();
      if (existingSize == sourceSize) {
        // Same size = likely no new session data. Skip.
        return false;
      }
    }
  }
  return writeBackupWithName(filename);
}

void pruneBackups(int keep) {
  if (keep < 1) keep = 1;
  const std::vector<String> entries = Storage.listFiles(kBackupDir, 200);
  std::vector<String> matches;
  matches.reserve(entries.size());
  for (const auto& e : entries) {
    const char* name = e.c_str();
    const size_t len = strlen(name);
    if (len < 11) continue;  // "stats_X.bin" min
    if (strncmp(name, kFilenamePrefix, 6) != 0) continue;
    if (strcmp(name + len - 4, kFilenameSuffix) != 0) continue;
    matches.push_back(e);
  }
  if (static_cast<int>(matches.size()) <= keep) return;
  std::sort(matches.begin(), matches.end());  // strcmp ascending — YYYY-MM-DD sorts chronologically
  const int toDelete = static_cast<int>(matches.size()) - keep;
  for (int i = 0; i < toDelete; ++i) {
    const std::string path = std::string(kBackupDir) + "/" + matches[i].c_str();
    if (Storage.remove(path.c_str())) {
      LOG_INF(kTag, "pruned old backup: %s", path.c_str());
    }
  }
}

}  // namespace StatsBackup
