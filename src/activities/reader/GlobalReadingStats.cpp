#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
// v18.9.9.441 — CrossInk 159-byte v3 layout (byte-identical for
// firmware-to-firmware interop):
//   [0]        version (= 3)
//   [1-4]      totalSessions          u32 LE
//   [5-8]      totalReadingSeconds    u32 LE
//   [9-12]     totalPagesTurned       u32 LE
//   [13-16]    completedBooks         u32 LE
//   [17-32]    timeOfDaySeconds[4]    u32 LE × 4 (16 B)
//   [33-60]    dayOfWeekSeconds[7]    u32 LE × 7 (28 B)
//   [61-64]    readingHistoryAnchorDay u32 LE
//   [65-156]   readingHistoryBits     u8 × 92
//   [157-158]  longestReadingStreak   u16 LE
//
// Legacy CrumBLE v3 (149 B, pre-4.5.161):
//   [0]       version (= 3)
//   [1-16]    common (unchanged)
//   [17-20]   lastReadYyyymmdd       u32 LE
//   [21-22]   currentStreak          u16 LE
//   [23-24]   longestStreak          u16 LE
//   [25-52]   dayHistogram[7]        u32 LE × 7  (Sunday=0)
//   [53-148]  hourHistogram[24]      u32 LE × 24
// Migrated on load — old ToD/DoW seconds are preserved into the new
// bucket layout; lastReadYyyymmdd seeds the bitfield anchor + bit 0.
constexpr uint8_t GLOBAL_STATS_VERSION = 3;
constexpr int GLOBAL_STATS_FILE_SIZE_V1 = 13;
constexpr int GLOBAL_STATS_FILE_SIZE_V2 = 17;
constexpr int GLOBAL_STATS_FILE_SIZE_CRUMBLE_LEGACY_V3 = 149;
constexpr int GLOBAL_STATS_FILE_SIZE = 159;
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char GLOBAL_STATS_BAK_PATH[] = "/.crosspoint/global_stats.bin.bak";

// v4.5.161: refuse-to-clobber. Set when load() sees a version byte we
// don't know or a size longer than we serialize. Persists in RAM for
// the boot session; reset() clears it (and the file).
bool s_blockDestructiveSave = false;

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

void writeLe32(uint8_t* data, const int offset, const uint32_t v) {
  data[offset] = v & 0xFF;
  data[offset + 1] = (v >> 8) & 0xFF;
  data[offset + 2] = (v >> 16) & 0xFF;
  data[offset + 3] = (v >> 24) & 0xFF;
}

void writeLe16(uint8_t* data, const int offset, const uint16_t v) {
  data[offset] = v & 0xFF;
  data[offset + 1] = (v >> 8) & 0xFF;
}

// v1/v2 shared common fields.
void loadCommonFieldsV1V2(const uint8_t* data, GlobalReadingStats& out) {
  out.totalSessions = readLe32(data, 1);
  out.totalReadingSeconds = readLe32(data, 5);
  out.totalPagesTurned = readLe32(data, 9);
}

// CrossInk 159 B v3 loader.
void loadFromCrossInkV3(const uint8_t* data, GlobalReadingStats& out) {
  loadCommonFieldsV1V2(data, out);
  out.completedBooks = readLe32(data, 13);
  for (int i = 0; i < 4; ++i) out.timeOfDaySeconds[i] = readLe32(data, 17 + i * 4);
  for (int i = 0; i < 7; ++i) out.dayOfWeekSeconds[i] = readLe32(data, 33 + i * 4);
  out.readingHistoryAnchorDay = readLe32(data, 61);
  for (int i = 0; i < static_cast<int>(READING_HISTORY_BYTES); ++i) out.readingHistoryBits[i] = data[65 + i];
  out.longestReadingStreak = readLe16(data, 157);
}

// CrumBLE legacy 149 B v3 loader. Migrates old hourHistogram[24] into
// the 4 ToD buckets (grouped by upstream's 5/12/17/21 boundaries) and
// remaps dayHistogram Sunday=0 → Monday=0. Seeds bitfield from
// lastReadYyyymmdd (if valid).
void loadFromCrumbleLegacyV3(const uint8_t* data, GlobalReadingStats& out) {
  loadCommonFieldsV1V2(data, out);
  out.completedBooks = readLe32(data, 13);

  const uint32_t lastReadYyyymmdd = readLe32(data, 17);
  // currentStreak (bytes 21-22) is not preserved — recomputed from bitfield.
  const uint16_t legacyLongest = readLe16(data, 23);

  uint32_t legacyDayHistogram[7] = {0};
  for (int i = 0; i < 7; ++i) legacyDayHistogram[i] = readLe32(data, 25 + i * 4);
  uint32_t legacyHourHistogram[24] = {0};
  for (int i = 0; i < 24; ++i) legacyHourHistogram[i] = readLe32(data, 53 + i * 4);

  // hourHistogram → timeOfDaySeconds (5-hour buckets).
  for (int h = 0; h < 24; ++h) {
    const uint32_t idx = static_cast<uint32_t>(readingTimeBucketForHour(static_cast<uint8_t>(h)));
    out.timeOfDaySeconds[idx] += legacyHourHistogram[h];
  }

  // dayHistogram Sunday=0 → Monday=0. upstream[i=Mon..Sun] = legacy[(i+1)%7].
  for (int i = 0; i < 7; ++i) out.dayOfWeekSeconds[i] = legacyDayHistogram[(i + 1) % 7];

  out.longestReadingStreak = legacyLongest;

  // Seed the bitfield from lastReadYyyymmdd (year=y, month=m, day=d).
  // Layout: YYYYMMDD as a decimal integer, e.g. 20260720.
  if (lastReadYyyymmdd >= 20000101u && lastReadYyyymmdd <= 20991231u) {
    ReadingStatsDate d;
    d.year = static_cast<uint16_t>(lastReadYyyymmdd / 10000u);
    d.month = static_cast<uint8_t>((lastReadYyyymmdd / 100u) % 100u);
    d.day = static_cast<uint8_t>(lastReadYyyymmdd % 100u);
    if (d.isValid()) {
      out.readingHistoryAnchorDay = readingStatsDayIndex(d);
      out.readingHistoryBits.fill(0);
      out.readingHistoryBits[0] = 1;  // bit 0 = anchor day
    }
  }
}

// Highest version byte we'd accept as "a future build of OUR format". Above it
// the file is something else entirely (another fork, or corruption) and is not
// worth blocking saves forever over. v4.7.3: a device in the field carried
// version=46, which is not a plausible successor to 3.
constexpr uint8_t GLOBAL_STATS_MAX_PLAUSIBLE_VERSION = 15;

enum class StatsFileVerdict : uint8_t {
  Parsed,     // known layout, loaded into `out`
  Absent,     // no file, empty, or unreadable
  NewerOurs,  // plausibly a future version of this format -- preserve, block saves
  Foreign,    // not our format at all -- safe to set aside and start fresh
};

StatsFileVerdict loadFromFile(const char* path, GlobalReadingStats& out) {
  FsFile f;
  if (!Storage.openFileForRead("GSTATS", path, f)) return StatsFileVerdict::Absent;
  // Real length, not the read cap: `n` below is clamped to GLOBAL_STATS_FILE_SIZE,
  // so the old `n > GLOBAL_STATS_FILE_SIZE` test could never fire and a longer
  // file looked exactly like a well-formed one.
  const uint32_t fileSize = static_cast<uint32_t>(f.size());
  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  const int n = f.read(data, GLOBAL_STATS_FILE_SIZE);
  f.close();
  if (n <= 0) return StatsFileVerdict::Absent;
  const uint8_t ver = data[0];

  // Something a later CrumBLE might write: a version just past ours, or one of
  // our versions with a longer record. Keep it and stop saving.
  const bool newerVersion = ver > GLOBAL_STATS_VERSION && ver <= GLOBAL_STATS_MAX_PLAUSIBLE_VERSION;
  const bool knownVersionLongerRecord = ver >= 1 && ver <= GLOBAL_STATS_VERSION && fileSize > GLOBAL_STATS_FILE_SIZE;
  if (newerVersion || knownVersionLongerRecord) {
    LOG_ERR("GSTATS", "Newer format detected (version=%u size=%lu); save blocked to preserve data",
            static_cast<unsigned>(ver), static_cast<unsigned long>(fileSize));
    return StatsFileVerdict::NewerOurs;
  }

  if (n == GLOBAL_STATS_FILE_SIZE_V1 && ver == 1) {
    loadCommonFieldsV1V2(data, out);
    return StatsFileVerdict::Parsed;
  }
  if (n == GLOBAL_STATS_FILE_SIZE_V2 && ver == 2) {
    loadCommonFieldsV1V2(data, out);
    out.completedBooks = readLe32(data, 13);
    return StatsFileVerdict::Parsed;
  }
  if (n == GLOBAL_STATS_FILE_SIZE && ver == GLOBAL_STATS_VERSION) {
    loadFromCrossInkV3(data, out);
    return StatsFileVerdict::Parsed;
  }
  if (n == GLOBAL_STATS_FILE_SIZE_CRUMBLE_LEGACY_V3 && ver == GLOBAL_STATS_VERSION) {
    loadFromCrumbleLegacyV3(data, out);
    LOG_INF("GSTATS", "Migrated CrumBLE legacy v3 (149 B) -> CrossInk v3 (159 B) -- bitfield seeded from lastRead");
    return StatsFileVerdict::Parsed;
  }
  LOG_ERR("GSTATS", "Unrecognised global_stats: version=%u size=%lu", static_cast<unsigned>(ver),
          static_cast<unsigned long>(fileSize));
  return StatsFileVerdict::Foreign;
}

// Move a file we cannot read out of the way, preserving it, so stats can start
// fresh instead of being blocked forever. Returns false if the file could not
// be moved -- in which case the caller must keep blocking rather than clobber.
bool setAsideForeign(const char* path) {
  char aside[96];
  snprintf(aside, sizeof(aside), "%s.foreign", path);
  if (Storage.exists(aside) && !Storage.remove(aside)) return false;
  if (!Storage.rename(path, aside)) return false;
  LOG_INF("GSTATS", "Set aside unrecognised stats file: %s -> %s", path, aside);
  return true;
}
}  // namespace

bool GlobalReadingStats::isSaveBlocked() { return s_blockDestructiveSave; }

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  const StatsFileVerdict primary = loadFromFile(GLOBAL_STATS_PATH, stats);
  if (primary == StatsFileVerdict::Parsed) return stats;

  GlobalReadingStats fromBak;
  const StatsFileVerdict bak = loadFromFile(GLOBAL_STATS_BAK_PATH, fromBak);
  if (bak == StatsFileVerdict::Parsed) {
    LOG_DBG("GSTATS", "Recovered global stats from backup");
    return fromBak;
  }

  // v18.9.9.208: a file that EXISTS but didn't parse must not be overwritten --
  // that turns a transient read failure into permanent all-time data loss.
  // v4.7.3 splits that into two cases, because treating them the same left
  // devices with stats silently disabled forever:
  //
  //   NewerOurs -- plausibly written by a later CrumBLE. Keep blocking; a future
  //                build can still read it.
  //   Foreign   -- not our format at all (another fork, or corruption). Move it
  //                aside once, keeping the bytes, and start fresh with saves on.
  //                Only if the move succeeds; otherwise keep blocking.
  if (primary == StatsFileVerdict::NewerOurs || bak == StatsFileVerdict::NewerOurs) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "Newer stats format present (primary=%d bak=%d); save blocked to preserve data",
            primary == StatsFileVerdict::NewerOurs ? 1 : 0, bak == StatsFileVerdict::NewerOurs ? 1 : 0);
    return stats;
  }

  if (primary == StatsFileVerdict::Foreign || bak == StatsFileVerdict::Foreign) {
    bool allMoved = true;
    if (primary == StatsFileVerdict::Foreign && !setAsideForeign(GLOBAL_STATS_PATH)) allMoved = false;
    if (bak == StatsFileVerdict::Foreign && !setAsideForeign(GLOBAL_STATS_BAK_PATH)) allMoved = false;
    if (!allMoved) {
      s_blockDestructiveSave = true;
      LOG_ERR("GSTATS", "Unreadable stats file could not be set aside; save blocked to preserve data");
    } else {
      LOG_INF("GSTATS", "Unrecognised stats file set aside; starting fresh with saves enabled");
    }
    return stats;
  }

  LOG_DBG("GSTATS", "Global stats missing, starting fresh");
  return stats;
}

void GlobalReadingStats::save() const {
  if (s_blockDestructiveSave) {
    LOG_INF("GSTATS", "save: blocked (newer file format present); skipping");
    return;
  }

  // v18.9.9.474: preserve previous file as .bak before O_TRUNC, but ONLY
  // if the existing .bak isn't a format we don't recognize. Rationale:
  // users flashing CrumBLE over VR-codex-steroids or another fork may have
  // a .bak file in a foreign layout — the audit found no explicit VR-codex
  // format handler, so we treat any .bak whose first byte (version) is
  // outside our known range (1..GLOBAL_STATS_VERSION) or whose size is
  // longer than we serialize as "foreign — don't touch". We skip the
  // rename in that case, which loses the .bak history of our OWN prior
  // save but preserves the foreign backup intact for a hypothetical roll
  // back to that firmware.
  if (Storage.exists(GLOBAL_STATS_PATH)) {
    bool bakIsForeign = false;
    if (Storage.exists(GLOBAL_STATS_BAK_PATH)) {
      FsFile bakFile;
      if (Storage.openFileForRead("GSTATS", GLOBAL_STATS_BAK_PATH, bakFile)) {
        const size_t bakSize = bakFile.size();
        uint8_t bakVersion = 0;
        if (bakSize > 0 && bakFile.read(&bakVersion, 1) == 1) {
          // Known versions map to specific sizes (see the constants at the
          // top of this file). Unknown version OR unknown size = foreign.
          const bool knownVersionAndSize =
              (bakVersion == 1 && bakSize == GLOBAL_STATS_FILE_SIZE_V1) ||
              (bakVersion == 2 && bakSize == GLOBAL_STATS_FILE_SIZE_V2) ||
              (bakVersion == 3 &&
               (bakSize == GLOBAL_STATS_FILE_SIZE || bakSize == GLOBAL_STATS_FILE_SIZE_CRUMBLE_LEGACY_V3));
          if (!knownVersionAndSize) {
            bakIsForeign = true;
            LOG_INF("GSTATS", "save: existing .bak is foreign (version=%u size=%u); preserving",
                    static_cast<unsigned>(bakVersion), static_cast<unsigned>(bakSize));
          }
        }
        bakFile.close();
      }
    }
    if (!bakIsForeign) {
      Storage.remove(GLOBAL_STATS_BAK_PATH);
      Storage.rename(GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH);
    } else {
      // Foreign .bak: don't rename over it; just clear the primary so the
      // write below starts fresh.
      Storage.remove(GLOBAL_STATS_PATH);
    }
  }

  FsFile f;
  if (!Storage.openFileForWrite("GSTATS", GLOBAL_STATS_PATH, f)) {
    LOG_ERR("GSTATS", "Could not write global_stats.bin");
    return;
  }
  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  data[0] = GLOBAL_STATS_VERSION;
  writeLe32(data, 1, totalSessions);
  writeLe32(data, 5, totalReadingSeconds);
  writeLe32(data, 9, totalPagesTurned);
  writeLe32(data, 13, completedBooks);
  for (int i = 0; i < 4; ++i) writeLe32(data, 17 + i * 4, timeOfDaySeconds[i]);
  for (int i = 0; i < 7; ++i) writeLe32(data, 33 + i * 4, dayOfWeekSeconds[i]);
  writeLe32(data, 61, readingHistoryAnchorDay);
  for (int i = 0; i < static_cast<int>(READING_HISTORY_BYTES); ++i) data[65 + i] = readingHistoryBits[i];
  writeLe16(data, 157, longestReadingStreak);

  const size_t bytesWritten = f.write(data, GLOBAL_STATS_FILE_SIZE);
  if (bytesWritten != GLOBAL_STATS_FILE_SIZE) {
    LOG_ERR("GSTATS", "Short write: %u/%u", static_cast<unsigned>(bytesWritten),
            static_cast<unsigned>(GLOBAL_STATS_FILE_SIZE));
    f.close();
    Storage.remove(GLOBAL_STATS_PATH);
    return;
  }
  f.flush();
  if (!f.sync()) {
    LOG_ERR("GSTATS", "Failed to sync global_stats.bin");
    f.close();
    Storage.remove(GLOBAL_STATS_PATH);
    return;
  }
  if (!f.close()) {
    LOG_ERR("GSTATS", "Failed to close global_stats.bin after save");
    Storage.remove(GLOBAL_STATS_PATH);
  }
}

bool GlobalReadingStats::reset() {
  bool ok = true;
  if (Storage.exists(GLOBAL_STATS_PATH) && !Storage.remove(GLOBAL_STATS_PATH)) {
    LOG_ERR("GSTATS", "Could not remove global_stats.bin");
    ok = false;
  }
  if (Storage.exists(GLOBAL_STATS_BAK_PATH) && !Storage.remove(GLOBAL_STATS_BAK_PATH)) {
    LOG_ERR("GSTATS", "Could not remove global_stats.bin.bak");
    ok = false;
  }
  s_blockDestructiveSave = false;
  return ok;
}

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);
  const uint16_t historyLongest = computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits);
  if (historyLongest > longestReadingStreak) longestReadingStreak = historyLongest;
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}
