#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "ReadingStatsUtils.h"
#include "util/CacheWriteRecovery.h"

namespace {
// v18.9.9.443 — CrossInk stats_v5.bin, 73 bytes. Byte-identical for
// firmware-to-firmware interop.
//
//   [0]      version (= 5)
//   [1-2]    sessionCount              u16 LE
//   [3-6]    totalReadingSeconds       u32 LE
//   [7-10]   totalPagesTurned          u32 LE
//   [11]     isCompleted               u8  (0/1)
//   [12-13]  avgSecondsPerForwardPage  u16 LE
//   [14-15]  paceSampleCount           u16 LE  (capped 1000)
//   [16]     flags                     u8   bit0=startManual bit1=finishManual
//   [17-18]  startDate.year            u16 LE
//   [19]     startDate.month           u8
//   [20]     startDate.day             u8
//   [21-22]  finishDate.year           u16 LE
//   [23]     finishDate.month          u8
//   [24]     finishDate.day            u8
//   [25-40]  timeOfDaySeconds[4]       u32 LE × 4 (16 B)
//   [41-68]  dayOfWeekSeconds[7]       u32 LE × 7 (28 B)  Monday=0
//   [69-72]  estimatedTimeLeftSeconds  u32 LE  (0 = unavailable)
constexpr uint8_t STATS_V5_VERSION = 5;
constexpr int STATS_V5_SIZE = 73;
constexpr int STATS_V4_SIZE = 69;  // CrossInk v4 (adds pre-date fields, no est-time)
constexpr int STATS_V3_SIZE = 16;  // CrossInk v3 (adds flags + dates only)
constexpr int STATS_V2_SIZE = 12;  // CrumBLE and CrossInk v2 (isCompleted appended)
constexpr int STATS_V1_SIZE = 11;  // pre-completed

constexpr uint16_t PACE_SAMPLE_CAP = 1000;

// File-name candidates in decreasing preference order.
struct StatsFileVariant {
  const char* suffix;
  int size;
  uint8_t version;
};
constexpr StatsFileVariant STATS_VARIANTS[] = {
    {"/stats_v5.bin", STATS_V5_SIZE, 5},  // preferred
    {"/stats_v4.bin", STATS_V4_SIZE, 4},
    {"/stats_v3.bin", STATS_V3_SIZE, 3},
    {"/stats_v2.bin", STATS_V2_SIZE, 2},
    {"/stats_v1.bin", STATS_V1_SIZE, 1},
    {"/stats.bin", STATS_V2_SIZE, 2},  // CrumBLE legacy (12 B, treated as v2)
};

constexpr const char* PREFERRED_STATS_FILENAME = "/stats_v5.bin";
constexpr const char* LEGACY_STATS_FILENAME = "/stats.bin";

uint16_t readLe16(const uint8_t* d, int off) {
  return static_cast<uint16_t>(d[off]) | (static_cast<uint16_t>(d[off + 1]) << 8);
}

uint32_t readLe32(const uint8_t* d, int off) {
  return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
         (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

void writeLe16(uint8_t* d, int off, uint16_t v) {
  d[off] = v & 0xFF;
  d[off + 1] = (v >> 8) & 0xFF;
}

void writeLe32(uint8_t* d, int off, uint32_t v) {
  d[off] = v & 0xFF;
  d[off + 1] = (v >> 8) & 0xFF;
  d[off + 2] = (v >> 16) & 0xFF;
  d[off + 3] = (v >> 24) & 0xFF;
}

bool loadFromBuffer(const uint8_t* data, int size, BookReadingStats& out) {
  if (size <= 0) return false;

  // v1 (11 B): sessionCount, totalReadingSeconds, totalPagesTurned.
  if (size == STATS_V1_SIZE && data[0] == 1) {
    out.sessionCount = readLe16(data, 1);
    out.totalReadingSeconds = readLe32(data, 3);
    out.totalPagesTurned = readLe32(data, 7);
    return true;
  }
  // v2 (12 B): + isCompleted. Same layout on CrumBLE legacy stats.bin and CrossInk v2.
  if (size == STATS_V2_SIZE && data[0] == 2) {
    out.sessionCount = readLe16(data, 1);
    out.totalReadingSeconds = readLe32(data, 3);
    out.totalPagesTurned = readLe32(data, 7);
    out.isCompleted = data[11] != 0;
    return true;
  }
  // v3 (16 B): + flags + dates. Speculative — read best-effort; if
  // CrossInk's v3 differs in layout we still get sessionCount/etc.
  if (size == STATS_V3_SIZE && data[0] == 3) {
    out.sessionCount = readLe16(data, 1);
    out.totalReadingSeconds = readLe32(data, 3);
    out.totalPagesTurned = readLe32(data, 7);
    out.isCompleted = data[11] != 0;
    out.flags = data[12];
    // 3 more bytes — leave to defaults; format uncertain without upstream.
    return true;
  }
  // v4 (69 B): all fields except estimatedTimeLeftSeconds.
  if (size == STATS_V4_SIZE && data[0] == 4) {
    out.sessionCount = readLe16(data, 1);
    out.totalReadingSeconds = readLe32(data, 3);
    out.totalPagesTurned = readLe32(data, 7);
    out.isCompleted = data[11] != 0;
    out.avgSecondsPerForwardPage = readLe16(data, 12);
    out.paceSampleCount = readLe16(data, 14);
    out.flags = data[16];
    out.startDate.year = readLe16(data, 17);
    out.startDate.month = data[19];
    out.startDate.day = data[20];
    out.finishDate.year = readLe16(data, 21);
    out.finishDate.month = data[23];
    out.finishDate.day = data[24];
    for (int i = 0; i < 4; ++i) out.timeOfDaySeconds[i] = readLe32(data, 25 + i * 4);
    for (int i = 0; i < 7; ++i) out.dayOfWeekSeconds[i] = readLe32(data, 41 + i * 4);
    // estimatedTimeLeftSeconds stays 0 (unavailable).
    return true;
  }
  // v5 (73 B): full CrossInk-compat layout.
  if (size == STATS_V5_SIZE && data[0] == 5) {
    out.sessionCount = readLe16(data, 1);
    out.totalReadingSeconds = readLe32(data, 3);
    out.totalPagesTurned = readLe32(data, 7);
    out.isCompleted = data[11] != 0;
    out.avgSecondsPerForwardPage = readLe16(data, 12);
    out.paceSampleCount = readLe16(data, 14);
    out.flags = data[16];
    out.startDate.year = readLe16(data, 17);
    out.startDate.month = data[19];
    out.startDate.day = data[20];
    out.finishDate.year = readLe16(data, 21);
    out.finishDate.month = data[23];
    out.finishDate.day = data[24];
    for (int i = 0; i < 4; ++i) out.timeOfDaySeconds[i] = readLe32(data, 25 + i * 4);
    for (int i = 0; i < 7; ++i) out.dayOfWeekSeconds[i] = readLe32(data, 41 + i * 4);
    out.estimatedTimeLeftSeconds = readLe32(data, 69);
    return true;
  }
  return false;
}

// Try each variant filename in preference order; return first that loads.
bool loadFirstAvailable(const std::string& cachePath, BookReadingStats& out) {
  for (const auto& v : STATS_VARIANTS) {
    const std::string p = cachePath + v.suffix;
    if (!Storage.exists(p.c_str())) continue;
    FsFile f;
    if (!Storage.openFileForRead("STATS", p, f)) continue;
    uint8_t buf[STATS_V5_SIZE] = {};
    const int n = f.read(buf, STATS_V5_SIZE);
    f.close();
    if (loadFromBuffer(buf, n, out)) {
      return true;
    }
    LOG_DBG("STATS", "Rejected %s (size=%d version=%u)", p.c_str(), n, static_cast<unsigned>(buf[0]));
  }
  return false;
}
}  // namespace

bool BookReadingStats::exists(const std::string& cachePath) {
  // Any variant counts as "exists" for the Unopened-collection gate.
  for (const auto& v : STATS_VARIANTS) {
    if (Storage.exists((cachePath + v.suffix).c_str())) return true;
  }
  return false;
}

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  if (loadFirstAvailable(cachePath, stats)) return stats;
  return stats;
}

void BookReadingStats::save(const std::string& cachePath) const {
  const std::string statsPath = cachePath + PREFERRED_STATS_FILENAME;
  FsFile f;
  if (!CacheWriteRecovery::openForWriteOrRecover("STATS", statsPath, f)) {
    LOG_ERR("STATS", "Could not write stats_v5.bin");
    return;
  }
  uint8_t data[STATS_V5_SIZE] = {};
  data[0] = STATS_V5_VERSION;
  writeLe16(data, 1, sessionCount);
  writeLe32(data, 3, totalReadingSeconds);
  writeLe32(data, 7, totalPagesTurned);
  data[11] = isCompleted ? 1 : 0;
  writeLe16(data, 12, avgSecondsPerForwardPage);
  writeLe16(data, 14, paceSampleCount);
  data[16] = flags;
  writeLe16(data, 17, startDate.year);
  data[19] = startDate.month;
  data[20] = startDate.day;
  writeLe16(data, 21, finishDate.year);
  data[23] = finishDate.month;
  data[24] = finishDate.day;
  for (int i = 0; i < 4; ++i) writeLe32(data, 25 + i * 4, timeOfDaySeconds[i]);
  for (int i = 0; i < 7; ++i) writeLe32(data, 41 + i * 4, dayOfWeekSeconds[i]);
  writeLe32(data, 69, estimatedTimeLeftSeconds);
  f.write(data, STATS_V5_SIZE);
  f.close();

  // Migration cleanup: delete the CrumBLE-legacy stats.bin so we're not
  // maintaining two files. CrossInk's own v4/v3/v2/v1 files are left in
  // place — they never coexist with stats_v5.bin on the same book after
  // one save cycle from either firmware.
  const std::string legacyPath = cachePath + LEGACY_STATS_FILENAME;
  if (Storage.exists(legacyPath.c_str())) {
    Storage.remove(legacyPath.c_str());
    LOG_INF("STATS", "Migrated %s → stats_v5.bin; legacy file removed", legacyPath.c_str());
  }
}

bool BookReadingStats::remove(const std::string& cachePath) {
  bool anyRemoved = false;
  bool allOk = true;
  for (const auto& v : STATS_VARIANTS) {
    const std::string p = cachePath + v.suffix;
    if (Storage.exists(p.c_str())) {
      if (!Storage.remove(p.c_str())) {
        LOG_ERR("STATS", "Could not delete %s", p.c_str());
        allOk = false;
      } else {
        anyRemoved = true;
      }
    }
  }
  (void)anyRemoved;
  return allOk;
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds,
                                         uint16_t pagesTurnedForward) {
  if (localStart.isValid() && seconds > 0) {
    recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  }
  // Update running pace regardless of clock validity (uses only page-turn
  // count + elapsed seconds, both of which are known even pre-SNTP).
  if (pagesTurnedForward > 0 && seconds > 0) {
    // Rolling avg: new avg = old avg + (sample - old avg) / n, capped n.
    for (uint16_t i = 0; i < pagesTurnedForward; ++i) {
      const uint32_t perPage = seconds / pagesTurnedForward;
      const uint16_t n = paceSampleCount < PACE_SAMPLE_CAP ? paceSampleCount + 1 : PACE_SAMPLE_CAP;
      const int32_t delta = static_cast<int32_t>(perPage) - static_cast<int32_t>(avgSecondsPerForwardPage);
      const int32_t adjusted = static_cast<int32_t>(avgSecondsPerForwardPage) + delta / static_cast<int32_t>(n);
      avgSecondsPerForwardPage = static_cast<uint16_t>(adjusted < 0 ? 0 : adjusted);
      paceSampleCount = n;
    }
  }
}

void BookReadingStats::updateEstimatedTimeLeft(uint32_t pagesRemaining) {
  if (avgSecondsPerForwardPage == 0 || pagesRemaining == 0) {
    estimatedTimeLeftSeconds = 0;
    return;
  }
  const uint64_t est = static_cast<uint64_t>(pagesRemaining) * static_cast<uint64_t>(avgSecondsPerForwardPage);
  estimatedTimeLeftSeconds = est > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(est);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}
