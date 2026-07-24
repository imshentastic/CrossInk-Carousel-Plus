#include "ReadingStats.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <time.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"

// v18.9.9.292: unix epoch for 2020-01-01 UTC. Anything before this is
// treated as "clock not set yet" -- ESP32 boots with time() near 0.
// Same threshold vcodex-steroids uses, keeps X3 (DS3231) + X4 (SNTP-only)
// on one code path via the system clock instead of gating on DS3231
// availability.
static constexpr uint32_t VALID_CLOCK_THRESHOLD = 1577836800u;

namespace {
constexpr const char* kTag = "STAT";
constexpr unsigned long SESSION_FLUSH_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 min
constexpr unsigned long TICK_MIN_INTERVAL_MS = 30UL * 1000UL;             // 30 s cadence

ReadingStats::Status gStatus = ReadingStats::Status::UnavailableNoRTC;
bool gBegan = false;

// In-flight session state.
uint32_t gSessionEpochDay = 0;
uint32_t gSessionBookHash = 0;
uint32_t gSessionStartMs = 0;
uint32_t gSessionLastTickMs = 0;
uint16_t gSessionAccumulatedMinutes = 0;  // committed minutes NOT yet flushed
uint16_t gSessionPagesTurned = 0;
bool gSessionActive = false;

unsigned long gLastTickMs = 0;

// fnv1a32 for book-hash. Same hash used elsewhere in the codebase so
// records line up if we ever cross-reference.
uint32_t fnv1a32(const char* s) {
  uint32_t h = 0x811C9DC5u;
  for (; *s; ++s) {
    h ^= static_cast<uint8_t>(*s);
    h *= 0x01000193u;
  }
  return h;
}

// Convert local Y/M/D to epoch-day. Uses Howard Hinnant's civil_from_days
// inverse (days_from_civil). Public-domain formula, handles all valid
// Gregorian dates fine.
uint32_t ymdToEpochDay(uint16_t y, uint8_t m, uint8_t d) {
  const int yi = (m <= 2) ? y - 1 : y;
  const int era = (yi >= 0 ? yi : yi - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(yi - era * 400);
  const unsigned mp = (m + 9) % 12;
  const unsigned doy = (153 * mp + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<uint32_t>(era * 146097 + static_cast<int>(doe) - 719468);
}

// Pull today's epoch-day from the ESP32 system clock. Works on both X3
// (DS3231-backed system time) and X4 (SNTP-anchored system time that
// survives deep sleep via the SoC's internal RTC counter, until power
// loss). Honours the process TZ (set by TimeUtils::configureTimezone
// upstream) so the "day" bucket matches the user's local wall calendar.
uint32_t nowEpochDay() {
  const time_t now = time(nullptr);
  if (now < static_cast<time_t>(VALID_CLOCK_THRESHOLD)) return 0;
  struct tm local;
  if (localtime_r(&now, &local) == nullptr) return 0;
  return ymdToEpochDay(static_cast<uint16_t>(1900 + local.tm_year),
                       static_cast<uint8_t>(local.tm_mon + 1),
                       static_cast<uint8_t>(local.tm_mday));
}

bool appendRecord(const ReadingStats::ReadingDayRecord& rec) {
  // HalStorage has no O_APPEND mode; openFileForWrite always truncates.
  // Read existing bytes into a small buffer, then rewrite with the new
  // record appended. File is ~2-5 KB per year of moderate reading, so
  // this RMW stays cheap on heap and SD-write bandwidth. Bulk-buffered
  // instead of per-record streaming so a single power loss during
  // write can only lose one flush cycle, not a partial record.
  Storage.mkdir("/.crosspoint");
  std::vector<uint8_t> buf;
  {
    FsFile fr;
    if (Storage.openFileForRead(kTag, ReadingStats::kStatsFilePath, fr)) {
      const size_t existingSize = fr.size();
      // Cap at 128 KB so a runaway file (bug or corruption) doesn't OOM us.
      const size_t readSize = existingSize > 128u * 1024u ? 128u * 1024u : existingSize;
      buf.resize(readSize);
      if (readSize > 0) {
        const int n = fr.read(buf.data(), readSize);
        if (n != static_cast<int>(readSize)) {
          LOG_ERR(kTag, "append: short read %d/%zu", n, readSize);
          fr.close();
          return false;
        }
      }
      fr.close();
    }
  }
  buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&rec),
             reinterpret_cast<const uint8_t*>(&rec) + sizeof(rec));

  FsFile fw;
  if (!Storage.openFileForWrite(kTag, ReadingStats::kStatsFilePath, fw)) {
    LOG_ERR(kTag, "append: open for write failed");
    return false;
  }
  const size_t n = fw.write(buf.data(), buf.size());
  fw.close();
  if (n != buf.size()) {
    LOG_ERR(kTag, "append: short write %zu/%zu", n, buf.size());
    return false;
  }
  LOG_DBG(kTag, "append: day=%u minutes=%u pages=%u book=0x%08x (file now %zu bytes)",
          rec.epochDay, rec.minutesRead, rec.pagesTurned, rec.bookHash, buf.size());
  return true;
}

// Commit the current session's counters to disk and reset them so a
// new session can accumulate. If nothing to commit, no-op.
void flushCurrentSession() {
  if (!gSessionActive) return;
  if (gSessionAccumulatedMinutes == 0 && gSessionPagesTurned == 0) return;

  ReadingStats::ReadingDayRecord rec;
  rec.epochDay = gSessionEpochDay;
  rec.minutesRead = gSessionAccumulatedMinutes;
  rec.pagesTurned = gSessionPagesTurned;
  rec.bookHash = gSessionBookHash;
  appendRecord(rec);

  gSessionAccumulatedMinutes = 0;
  gSessionPagesTurned = 0;
  gSessionStartMs = millis();  // reset the accumulation window
  gSessionLastTickMs = gSessionStartMs;
}

}  // namespace

namespace ReadingStats {

void begin() {
  if (gBegan) return;
  gBegan = true;
  // v18.9.9.292: gate on system-clock validity rather than DS3231
  // availability. X4 devices (no DS3231) that have completed at least
  // one SNTP sync this session now log stats normally.
  if (nowEpochDay() == 0) {
    gStatus = Status::UnavailableClockNotSet;
    LOG_INF(kTag, "system clock not set yet; stats will start once time syncs");
    return;
  }
  gStatus = Status::Ok;
  LOG_INF(kTag, "reading stats initialised (today=epochDay %u)", nowEpochDay());
}

// v18.9.9.292: re-evaluate clock validity. Called on WiFi connect / NTP
// sync so an X4 device that finally learns the wall-clock time flips
// from UnavailableClockNotSet -> Ok without needing a reboot.
void reevaluateClockStatus() {
  if (!gBegan) {
    begin();
    return;
  }
  if (gStatus == Status::Ok) return;
  if (nowEpochDay() != 0) {
    gStatus = Status::Ok;
    LOG_INF(kTag, "reading stats: clock became valid (today=epochDay %u)", nowEpochDay());
  }
}

Status getStatus() { return gStatus; }

void noteBookOpened(const char* path) {
  if (gStatus != Status::Ok) return;
  if (path == nullptr) return;
  // If we had a prior session active on a different book, flush it first.
  if (gSessionActive) {
    flushCurrentSession();
  }
  gSessionEpochDay = nowEpochDay();
  gSessionBookHash = fnv1a32(path);
  gSessionStartMs = millis();
  gSessionLastTickMs = gSessionStartMs;
  gSessionAccumulatedMinutes = 0;
  gSessionPagesTurned = 0;
  gSessionActive = true;
}

void notePageTurn() {
  if (gStatus != Status::Ok || !gSessionActive) return;
  if (gSessionPagesTurned < 65535) gSessionPagesTurned++;
}

void tick() {
  if (gStatus != Status::Ok || !gSessionActive) return;
  const unsigned long now = millis();
  if (now - gLastTickMs < TICK_MIN_INTERVAL_MS) return;
  gLastTickMs = now;

  // Advance accumulated minutes from wall-clock delta since last tick.
  // Assumes the user is actively reading; page-turn is a stronger signal
  // but many users read a page for minutes at a time. Using elapsed time
  // captures reading-in-progress but overcounts if the device is on but
  // the user walked away. Trade-off accepted for MVP; can add
  // inactivity-idle detection later.
  //
  // v18.9.9.201 FIX: only consume WHOLE minutes from the delta window and
  // leave the remainder for the next tick. The old code advanced
  // gSessionLastTickMs on every tick, so with ticks every ~30 s the
  // delta was always < 60000 ms → addMinutes was always 0 and the 30 s
  // was silently discarded. Net effect: minutesRead stayed 0 for any
  // session that didn't stall the main loop > 60 s, which left heatmap
  // cells blank even after real reading.
  const unsigned long deltaMs = now - gSessionLastTickMs;
  const uint32_t addMinutes = deltaMs / 60000UL;
  if (addMinutes > 0) {
    gSessionLastTickMs += addMinutes * 60000UL;  // keep sub-minute remainder
    uint32_t sum = static_cast<uint32_t>(gSessionAccumulatedMinutes) + addMinutes;
    if (sum > 65535) sum = 65535;
    gSessionAccumulatedMinutes = static_cast<uint16_t>(sum);
  }

  // Did we cross a day boundary since session start? If yes, flush the
  // pre-midnight portion under yesterday's epochDay and roll to today's.
  const uint32_t today = nowEpochDay();
  if (today != 0 && today != gSessionEpochDay) {
    flushCurrentSession();
    gSessionEpochDay = today;
  }

  // Flush every SESSION_FLUSH_INTERVAL_MS to survive power loss.
  if (now - gSessionStartMs >= SESSION_FLUSH_INTERVAL_MS) {
    flushCurrentSession();
  }
}

void onBookClose() {
  if (gStatus != Status::Ok || !gSessionActive) return;
  tick();  // account for any elapsed time since last tick
  flushCurrentSession();
  gSessionActive = false;
}

void onSleepEntry() {
  if (gStatus != Status::Ok || !gSessionActive) return;
  tick();
  flushCurrentSession();
  // Keep session active -- user often wakes and continues the same book.
  // On wake, tick() will pick up where we left off.
}

// Shared reader for the full and per-book variants. When filterByBook is
// set, only records whose bookHash matches are folded in.
static std::vector<DayAggregate> loadAggregatesImpl(bool filterByBook, uint32_t bookHash) {
  std::vector<DayAggregate> out;
  FsFile f;
  if (!Storage.openFileForRead(kTag, kStatsFilePath, f)) return out;

  // v18.9.9.208: sanity bounds. A corrupt record (truncated RMW rewrite,
  // FAT hiccup) can carry garbage — minutesRead=0xFFFF skews the heatmap's
  // adaptive buckets into nonsense thresholds, and a garbage epochDay
  // drags the 20-week window into phantom months. Drop anything outside
  // plausible range instead of letting one bad record poison the view.
  constexpr uint32_t kMinPlausibleEpochDay = 18262;  // 2020-01-01
  constexpr uint32_t kMaxPlausibleEpochDay = 47481;  // 2099-12-31
  constexpr uint16_t kMaxPlausibleMinutes = 1440;    // 24h/day

  ReadingDayRecord rec;
  while (f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) == sizeof(rec)) {
    if (rec.epochDay < kMinPlausibleEpochDay || rec.epochDay > kMaxPlausibleEpochDay ||
        rec.minutesRead > kMaxPlausibleMinutes) {
      continue;
    }
    if (filterByBook && rec.bookHash != bookHash) continue;
    // Insertion-sort into out by epochDay ascending. Cheap because the
    // file is written in near-chronological order; most inserts land at
    // the tail with zero shifts.
    auto it = std::lower_bound(out.begin(), out.end(), rec.epochDay,
                               [](const DayAggregate& a, uint32_t d) { return a.epochDay < d; });
    if (it != out.end() && it->epochDay == rec.epochDay) {
      uint32_t m = static_cast<uint32_t>(it->minutesRead) + rec.minutesRead;
      it->minutesRead = static_cast<uint16_t>(m > 65535 ? 65535 : m);
      uint32_t p = static_cast<uint32_t>(it->pagesTurned) + rec.pagesTurned;
      it->pagesTurned = static_cast<uint16_t>(p > 65535 ? 65535 : p);
      if (it->sessionCount < 65535) it->sessionCount++;
    } else {
      DayAggregate agg;
      agg.epochDay = rec.epochDay;
      agg.minutesRead = rec.minutesRead;
      agg.pagesTurned = rec.pagesTurned;
      agg.sessionCount = 1;
      out.insert(it, agg);
    }
  }
  f.close();
  return out;
}

std::vector<DayAggregate> loadAggregates() { return loadAggregatesImpl(false, 0); }

std::vector<DayAggregate> loadAggregatesForBook(const char* bookPath) {
  return loadAggregatesImpl(true, fnv1a32(bookPath));
}

void epochDayToYmd(uint32_t epochDay, uint16_t& year, uint8_t& month, uint8_t& day) {
  const int z = static_cast<int>(epochDay) + 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int yi = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<uint16_t>(yi + (month <= 2 ? 1 : 0));
}

uint8_t epochDayToDow(uint32_t epochDay) {
  // 1970-01-01 was a Thursday (dow=4 if 0=Sun).
  return static_cast<uint8_t>((epochDay + 4) % 7);
}

uint32_t todayEpochDay() { return nowEpochDay(); }

}  // namespace ReadingStats
