#pragma once

#include <array>
#include <cstdint>

#include "ReadingStatsUtils.h"

// v18.9.9.441 — rewritten to be BYTE-COMPATIBLE with CrossInk's 159-byte
// global_stats.bin v3. Enables migration between CrumBLE and CrossInk
// firmwares without stats loss.
//
// Persisted to /.crosspoint/global_stats.bin. Backup at .bin.bak.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;        // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;     // Total page-turn actions across all books
  uint32_t completedBooks = 0;       // Books manually marked as finished

  // Time-of-day buckets (5-hour boundaries: Morning 5-12, Afternoon 12-17,
  // Evening 17-21, Night 21-5). Each session's seconds are split across
  // buckets at the boundaries. Reader-type on Dashboard / Minimal Stats
  // sleep screen is argmax over this array.
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds = {0, 0, 0, 0};

  // Day-of-week buckets. Monday = 0 (matches CrossInk).
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds = {0, 0, 0, 0, 0, 0, 0};

  // 730-day rolling reading-history bitfield: bit N is set iff the user
  // read on day (anchorDay - N). Anchor is the most recently recorded
  // day-index (days since 2000-01-01). Used for streak calculation.
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits = {};

  // Monotonically tracked longest streak. Effective longest is
  // max(this, computeReadingHistoryLongestStreak) — the bitfield can
  // only see the last 730 days, so this preserves older records.
  uint16_t longestReadingStreak = 0;

  static GlobalReadingStats load();
  void save() const;

  // Wipes primary + backup. Missing files count as success.
  static bool reset();

  // CrumBLE 4.5.161: refuse-to-save flag. Set when load() encounters a
  // NEWER file format than we understand (e.g. v4 CrossInk written to
  // an SD card that then boots CrumBLE v3-only firmware). save() no-ops
  // in that state so we don't clobber the newer file with our older
  // schema. Reset by GlobalReadingStats::reset() and process restart.
  static bool isSaveBlocked();

  // Fold a completed reading segment into ToD/DoW/history buckets.
  // `seconds` is total elapsed seconds for the segment; `localStart` is
  // the wall-clock start of the segment in local time. Safe no-op when
  // clock is invalid (localStart.isValid() == false).
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);

  // Convenience: derive current streak from bitfield relative to `today`.
  // Pass nullptr to skip the "gap since anchor" check.
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
};
