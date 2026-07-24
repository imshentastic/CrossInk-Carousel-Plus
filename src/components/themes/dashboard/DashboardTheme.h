#pragma once

// v18.9.9.465 (P3d): Dashboard theme — 1:1 port of CrossInk v1.4.0's
// DashboardTheme. Two-column Home: cover on left, stats grid on right,
// book title below cover, streak/reader-type footer.
//
// Differences from upstream (all pragmatic):
//   - drawRecentBookCover signature matches MinimalTheme's 9-arg form
//     (upstream added globalStats + currentChapterTitle params which would
//     require widening BaseTheme + updating HomeActivity call sites). We
//     load GlobalReadingStats inline and skip chapter title for now.
//   - Reader-type / streak icons use Book24Icon as placeholder until proper
//     morning/afternoon/evening/night/streak icon headers are ported.
//   - X3-only stats gate relaxed to isClockValid() (X4 users with SNTP get
//     the full grid — matches earlier v445 Minimal Stats decision).

#include <cstdint>

#include "components/themes/minimal/MinimalTheme.h"

namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 445;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverImageWidth = 296;
constexpr int homeCoverImageHeight = 444;
}  // namespace DashboardMetrics

class DashboardTheme : public MinimalTheme {
 public:
  // Home rendering: cover on left, stats grid on right, book title below,
  // streak/reader-type footer. Signature matches MinimalTheme's; the
  // extra CrossInk params (globalStats, currentChapterTitle) are loaded
  // inline / skipped in this port.
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f) const override;

  // Sleep-screen: same dashboard composition inverted (black background,
  // white text). Called from SleepActivity when uiTheme == DASHBOARD.
  void drawDashboardSleepScreen(const GfxRenderer& renderer, const RecentBook& book,
                                const BookReadingStats* stats, float progressPercent,
                                const struct GlobalReadingStats& globalStats, bool clockValid) const;
};
