

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra Carousel theme metrics (zero runtime cost)
namespace LyraCarouselMetrics {
// CrumBLE: header compacted to match Bookshelf -- see LyraTheme.h for
// the full rationale. Mirrors topPadding=8 / headerHeight=52 across the
// Lyra theme family.
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 8,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 52,
                                 .verticalSpacing = 16,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 35,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 28,
                                 .homeCoverHeight = 600,
                                 .homeCoverTileHeight = 660,
                                 .homeRecentBooksCount = 3,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 50,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = true,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 6};
}

class LyraCarouselTheme : public LyraTheme {
 public:
  // Max cache geometry for the carousel artwork area.
  static constexpr int kCenterCoverW = 340;
  static constexpr int kCenterCoverH = LyraCarouselMetrics::values.homeCoverHeight - 60;  // 540
  static constexpr int kCenterCoverVisualInset = 10;
  static constexpr int kBaseDisplayCenterW = (kCenterCoverW * 86) / 100;
  static constexpr int kBaseDisplayCenterH = (kCenterCoverH * 86) / 100;
  static constexpr int kDisplayCenterW =
      ((kBaseDisplayCenterW + 24) < kCenterCoverW) ? (kBaseDisplayCenterW + 24) : kCenterCoverW;
  static constexpr int kDisplayCenterH =
      ((kBaseDisplayCenterH + 24) < kCenterCoverH) ? (kBaseDisplayCenterH + 24) : kCenterCoverH;
  // Actual center image rect after the visual inset; using this for the cached
  // thumbnail avoids scaling an already-dithered BMP at draw time.
  static constexpr int kCenterThumbW = kDisplayCenterW - kCenterCoverVisualInset * 2;  // 296
  static constexpr int kCenterThumbH = kDisplayCenterH - kCenterCoverVisualInset * 2;  // 468
  static constexpr int kSideCoverW = 200;
  static constexpr int kSideCoverH = LyraCarouselMetrics::values.homeCoverHeight - 210;  // 390

  static void setPreRenderIndex(int idx);
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  // LyraTheme has no virtual overlay hook; this is a carousel-only helper.
  void drawButtonMenuSelectionOverlay(const GfxRenderer& renderer, int buttonCount, int selectedIndex,
                                      const std::function<std::string(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const;
  void drawCarouselBorder(GfxRenderer& renderer, Rect coverRect, const std::vector<RecentBook>& recentBooks,
                          int centerIdx, bool inCarouselRow) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = {},
                const std::function<UIIcon(int index)>& rowIcon = {},
                const std::function<std::string(int index)>& rowValue = {}, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = {},
                const std::function<bool(int index)>& isHeader = {}) const override;
};
