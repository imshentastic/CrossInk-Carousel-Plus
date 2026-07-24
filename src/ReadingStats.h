#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// v18.9.9.290: append-only reading-time log. Records how many minutes /
// pages the user read on each device-day. Foundation for the reading
// heatmap; no UI dependency lives in this module.
//
// Storage: /.crosspoint/reading-days.bin, packed as a stream of
// ReadingDayRecord (12 bytes each). Sessions are appended as they end
// (book close, sleep entry, or once per SESSION_FLUSH_INTERVAL_MS
// while reading actively). Reader aggregates per-day on load.
//
// X3 only: requires the DS3231 RTC for real dates. On X4 (no RTC) the
// module quietly no-ops -- getStatus() returns kUnavailableNoRTC and
// nothing gets written.

namespace ReadingStats {

// One appended entry. Sessions can span midnight; a session that crosses
// day boundary flushes twice (up to midnight, then remainder). Aggregation
// on read side is a simple group-by epochDay.
struct __attribute__((packed)) ReadingDayRecord {
  uint32_t epochDay;      // days since 1970-01-01 UTC (local-tz-adjusted per user's clockUtcOffsetQ)
  uint16_t minutesRead;   // 0-1440
  uint16_t pagesTurned;   // 0-65535
  uint32_t bookHash;      // fnv1a32 of book path -- lets us list books-per-day later
};
static_assert(sizeof(ReadingDayRecord) == 12, "ReadingDayRecord must stay 12 bytes");

enum class Status : uint8_t {
  Ok,
  UnavailableNoRTC,        // legacy value, kept for API compat; not emitted since v292
  UnavailableClockNotSet,  // system clock not yet set (needs SNTP sync or manual date set)
};

// Init at boot. Call after halClock.begin() so RTC availability is known.
// No-op safe: subsequent calls after the first are ignored.
void begin();

// v18.9.9.292: re-check the system clock after an NTP sync. On X4 devices
// with no DS3231 the clock is invalid until SNTP completes; this lets the
// module flip from UnavailableClockNotSet -> Ok without a reboot.
// Cheap to call multiple times.
void reevaluateClockStatus();

// Was init successful? Callers gate their calls on this to avoid work
// when the module is dormant (X4, or RTC unset).
Status getStatus();

// Reader hooks. Cheap -- just bumps in-memory counters. The flush to SD
// happens on tick(), onBookClose(), or onSleepEntry() below.
void noteBookOpened(const char* path);
void notePageTurn();
// Called from the main loop. Every SESSION_FLUSH_INTERVAL_MS this checks
// whether an active session should be flushed to disk (crossed a day
// boundary, or accumulated enough time to be worth persisting).
void tick();
// Explicit flushers -- call from book-exit and sleep-entry paths.
void onBookClose();
void onSleepEntry();

// Read-side API. Loads all records into memory and aggregates by day.
// Returned vector is sorted ascending by epochDay. Caller frees implicit
// on scope exit; typical file size ~2-5 KB per year of moderate reading.
struct DayAggregate {
  uint32_t epochDay;
  uint16_t minutesRead;
  uint16_t pagesTurned;
  uint16_t sessionCount;  // number of ReadingDayRecord entries for this day
};
std::vector<DayAggregate> loadAggregates();
// v18.9.9.199: per-book variant — only records whose bookHash matches
// fnv1a32(bookPath) are folded in. Path must be the same SD path passed
// to noteBookOpened (reader uses the book's full path).
std::vector<DayAggregate> loadAggregatesForBook(const char* bookPath);

// Helpers exposed for the heatmap render path.
// Convert epoch-day back to Y/M/D. Uses a compact algorithm suitable for
// dates 1970-2099 (well past any device lifetime).
void epochDayToYmd(uint32_t epochDay, uint16_t& year, uint8_t& month, uint8_t& day);
// Day-of-week for an epochDay. 0=Sunday..6=Saturday.
uint8_t epochDayToDow(uint32_t epochDay);
// Today's epochDay per the RTC + user's UTC offset. Returns 0 on failure.
uint32_t todayEpochDay();

// File location (public so recovery / debug tools can find it).
constexpr const char* kStatsFilePath = "/.crosspoint/reading-days.bin";

}  // namespace ReadingStats
