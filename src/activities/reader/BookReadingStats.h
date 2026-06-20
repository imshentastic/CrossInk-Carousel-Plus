#pragma once
#include <cstdint>
#include <string>

// Per-book reading statistics, persisted to cachePath/stats.bin.
struct BookReadingStats {
  uint16_t sessionCount = 0;         // Total times this book was opened
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time in seconds
  uint32_t totalPagesTurned = 0;     // Total page-turn actions (forward + backward)
  bool isCompleted = false;          // Whether the user manually marked this book as finished

  // Loads stats from cachePath/stats.bin. Returns default-constructed stats if
  // the file is missing or the version byte does not match.
  static BookReadingStats load(const std::string& cachePath);

  // CrumBLE: cheap existence check used by the "Unopened" virtual collection
  // to gate book membership. A book whose reader was never entered has no
  // stats.bin -- the reader writes one on first onEnter (see EpubReaderActivity)
  // so this flips immediately on open, even if the user backs out before any
  // reading time is logged.
  static bool exists(const std::string& cachePath);

  // Saves stats to cachePath/stats.bin.
  void save(const std::string& cachePath) const;

  // CrumBLE 4.4 (ported from CrossInk v1.3.3): deletes cachePath/stats.bin.
  // Missing files are treated as success. Used by the new "Delete Book's
  // Reading Stats" action in the reader menu + file-browser context menu.
  static bool remove(const std::string& cachePath);

  // Formats a duration in seconds into a human-readable string.
  // Output examples: "< 1 min", "45 min", "2h 30 min"
  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
