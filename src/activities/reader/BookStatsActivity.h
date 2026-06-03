#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Reading-stats screen with two-section layout:
//   Top: cover image (left) + per-book stats (right)
//   Bottom: all-time aggregate stats (full width)
// Left/Right buttons cycle through recent books that have stats; on the
// initial book the caller's `stats` are shown (so the reader's in-memory
// in-progress session time survives), other books reload from disk.
class BookStatsActivity final : public Activity {
  struct NavEntry {
    std::string path;
    std::string title;
    std::string author;
    std::string coverBmpPath;
    // CrumBLE #125: stats are loaded once by buildNavList() to filter
    // entries to books with sessionCount > 0. Cache them here so
    // loadCurrent doesn't re-read stats.bin on every L/R press --
    // 1 SD open per press eliminated. Initial-book override (which
    // carries the reader's live in-progress session time) still wins
    // when useInitialStats is true.
    BookReadingStats stats;
  };

  // Constructor inputs.
  std::string initialBookPath;
  std::string initialBookTitle;
  std::string initialCoverBmpPath;
  BookReadingStats initialStats;
  GlobalReadingStats globalStats;
  // True when launched via replaceActivity from Home (back lands on home,
  // so the leftmost button hint should say "Home"). False when launched
  // via startActivityForResult from the reader (back returns to the open
  // book, so the hint stays "Back"). Default preserves the reader path.
  bool backToHome = false;

  // Navigation state populated in onEnter().
  std::vector<NavEntry> nav;
  int currentIndex = 0;
  // True until the user navigates away from the initial book; lets the
  // reader's live session time survive on first display.
  bool useInitialStats = true;

  // What the active render is showing.
  BookReadingStats currentStats;
  std::string currentTitle;
  std::string currentAuthor;
  std::string currentCoverBmpPath;
  std::string currentBookPath;

  void buildNavList();
  void loadCurrent(int index);

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& bookPath,
                    const std::string& title, const std::string& coverBmpPath, const BookReadingStats& stats,
                    const GlobalReadingStats& globalStats, bool backToHome = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
