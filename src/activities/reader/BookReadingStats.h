#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// v18.9.9.443 — rewritten to CrossInk's 73-byte stats_v5.bin layout so
// users migrating between CrumBLE and CrossInk keep per-book stats intact.
// Also reads CrossInk's v1/v2/v3/v4 fallback formats and CrumBLE's own
// legacy stats.bin (12 B v2). Save always emits stats_v5.bin.
struct BookReadingStats {
  uint16_t sessionCount = 0;
  uint32_t totalReadingSeconds = 0;
  uint32_t totalPagesTurned = 0;
  bool isCompleted = false;

  // v5 additions:
  // Rolling average seconds per forward page turn — updated per-page turn
  // with paceSampleCount as the divisor (capped at 1000 samples to keep
  // adjustment gradual on long sessions). Used for time-left estimate.
  uint16_t avgSecondsPerForwardPage = 0;
  uint16_t paceSampleCount = 0;

  // flags: bit0 = startDateManuallySet, bit1 = finishDateManuallySet.
  // When bits are clear, dates are auto-populated: startDate on first
  // session-open, finishDate on setBookCompleted(true).
  uint8_t flags = 0;
  static constexpr uint8_t FLAG_START_DATE_MANUAL = 0x01;
  static constexpr uint8_t FLAG_FINISH_DATE_MANUAL = 0x02;

  ReadingStatsDate startDate;   // year/month/day (all 0 when invalid)
  ReadingStatsDate finishDate;

  // Reader-type buckets (5-hour boundaries: Morning 5-12, Afternoon 12-17,
  // Evening 17-21, Night 21-5). Populated by session-end recorder.
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds = {0, 0, 0, 0};

  // Per-book DoW buckets. Monday = 0.
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds = {0, 0, 0, 0, 0, 0, 0};

  // Estimated seconds remaining. Optional overlay in status bar / stats
  // (0 = unavailable). Recomputed from avgSecondsPerForwardPage × pagesLeft.
  uint32_t estimatedTimeLeftSeconds = 0;

  static BookReadingStats load(const std::string& cachePath);
  static bool exists(const std::string& cachePath);
  void save(const std::string& cachePath) const;
  static bool remove(const std::string& cachePath);

  // v18.9.9.443: fold this segment into buckets and update pace.
  // pagesTurnedForward = pages traversed in the +direction during this
  // segment. Safe no-op when clock invalid.
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds, uint16_t pagesTurnedForward);

  // v18.9.9.443: refresh estimatedTimeLeftSeconds from pace × remaining.
  void updateEstimatedTimeLeft(uint32_t pagesRemaining);

  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
