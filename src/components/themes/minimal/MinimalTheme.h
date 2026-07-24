#pragma once

#include <cstdint>

#include "components/themes/lyra/LyraTheme.h"

namespace MinimalMetrics {
constexpr int coverWidthForHeight(const int coverHeight) {
  return static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
}

constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = LyraMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 583;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverWidth = coverWidthForHeight(values.homeCoverHeight);
constexpr int homeCoverImageWidth = homeCoverWidth;
constexpr int homeCoverImageHeight = 525;
}  // namespace MinimalMetrics

class MinimalTheme : public LyraTheme {
 public:
  static void setHomeButtonHintSelection(int selectedIndex);

  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<bool(int index)>& isHeader = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                       bool allowInvertedText = false, bool darkMode = false) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f) const override;
  // v18.9.9.466: 1:1 signatures with CrossInk v1.4.0 MinimalTheme.
  // drawSleepScreen — inverted flag: false = BLACK background (default),
  //                                  true = WHITE background.
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats = nullptr,
                       float progressPercent = -1.0f, bool inverted = false) const;
  // drawStatsSleepScreen — base sleep screen + reader-type/streak overlay.
  // Renamed from CrumBLE's drawSleepScreenWithStats. Overlay is a no-op
  // when clock invalid (CrumBLE gate — upstream is X3-only).
  void drawStatsSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                            const struct GlobalReadingStats* globalStats, float progressPercent = -1.0f,
                            bool inverted = false) const;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  bool usesCompactFileBrowserRows() const override { return true; }
  int compactFileBrowserRowHeight(const GfxRenderer& renderer) const override;
};
