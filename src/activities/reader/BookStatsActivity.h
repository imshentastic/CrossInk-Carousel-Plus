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
  // v18.9.9.189: reading progress for the currently-viewed book (0-100,
  // -1 = unknown). Loaded on each L/R nav so per-book "Progress %" /
  // "Time Left" / "Est Finish" don't hit SD on every render.
  float currentProgressPercent_ = -1.0f;
  // v18.9.9.199 redesign: 2 pages × 2 data sources.
  //   Page 0 = stats view (cover + stat grid; All Books swaps the cover
  //            slot for the aggregate heatmap)
  //   Page 1 = charts view (heatmap + Time of Day + Day of Week bars)
  // Up/Down flips the page. Confirm toggles showAllBooks_ — same layouts,
  // aggregate data. L/R cycles books only when showing a single book.
  int currentPage_ = 0;
  bool showAllBooks_ = false;

  // v18.9.9.202 (P2c): date editor. Long-press Toggle on the book stats
  // page edits Started / Finished dates: 6 fields (start M/D/Y, finish
  // M/D/Y), Up/Down/L/R adjust, Confirm advances, long-press Confirm
  // clears the selected date, Back commits + exits. Manually-set dates
  // get the manual flag so the reader stops auto-populating them, and a
  // valid finish date marks the book completed (clearing it un-completes).
  bool editingDates_ = false;
  int editField_ = 0;  // 0..5
  bool confirmLongHandled_ = false;

  void adjustEditedDateField(int delta);
  void clearEditedDateGroup();
  void commitEditedDates();

  void buildNavList();
  void loadCurrent(int index);

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& bookPath,
                    const std::string& title, const std::string& coverBmpPath, const BookReadingStats& stats,
                    const GlobalReadingStats& globalStats, bool backToHome = false,
                    bool startOnAllBooksPage = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
