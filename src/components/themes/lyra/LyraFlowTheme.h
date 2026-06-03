#pragma once

#include <memory>
#include <unordered_map>

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;
struct RecentBook;

// Flow ("iPod-style") carousel theme. Inherits LyraTheme wholesale and only
// overrides the home-screen recent-book carousel. Ported from the Lua-fork
// FlowTheme; date / today-clock / per-book reading-time chrome have been
// removed because their stats subsystems do not exist in this firmware.
namespace LyraFlowMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverHeight = 320;       // 25-kai book ratio (~0.7) — center cover
  v.homeCoverTileHeight = 360;   // hugs the bottom of the cover so the menu sits close
  v.homeRecentBooksCount = 5;    // matches the 5 carousel slots visible at once
                                 // (center + 2 sides each direction). Capped at 5 to
                                 // avoid first-boot OOM during sequential thumb gen on
                                 // ESP32-C3 — see HomeActivity::loadRecentCovers.
  v.homeTopPadding = 41;         // tighter than Lyra's 56: Flow's home header has no
                                 // title/subtitle, only the battery icon (rendered at
                                 // y+5 inside the rect), so the rest of the rect was
                                 // dead space. Shrinking it shifts the carousel title
                                 // and covers up ~15 px closer to the battery row.
  v.homeMenuTopOffset = 28;      // 41 + 360 + 28 = 429, ~3 px above where the
                                 // menu sat at Lyra's original 56 padding. The 28 px
                                 // gap between cover bottom and menu top houses the
                                 // per-book reading-time indicator drawn under the
                                 // center cover.
  return v;
}();
}  // namespace LyraFlowMetrics

class LyraFlowTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f) const override;
  // Flow-only override of the home menu. Two-anchor pagination with a
  // sticky bit so the second page stays in view as the cursor scrolls
  // back up through the overlap zone — switches back to page 1 only
  // when the cursor crosses page 2's top boundary.
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

  // CrumBLE Collections — bookshelf strip between the carousel and the icon
  // bar. Renders the collection name above a row of cover thumbnails. Each
  // entry in `coverPaths` is the absolute path to a BMP thumb on SD (empty
  // string => render a placeholder card for that book).
  //
  // Selection model has THREE focus states on this row (driven by the
  // caller, encoded across the two flags):
  //   • `headerFocused = true` and `selectedSpineIndex = -1`: the user is
  //     on the collection tab itself; tab is drawn bold and flanked with
  //     left/right arrows when `hasMultipleCollections` is true to hint at
  //     L/R cycling.
  //   • `headerFocused = false` and `selectedSpineIndex >= 0`: the user is
  //     on one of the books; the matching cell gets a focus ring.
  //   • `headerFocused = false` and `selectedSpineIndex = -1`: nothing on
  //     this row is focused (cursor is on the carousel or icon bar).
  //
  // `scrollOffset` is the leftmost visible book index when the collection
  // overflows the viewport — pass 0 if no scroll.
  //
  // `focusedBookTitle` is rendered centered BELOW the cell row when a
  // book is focused (`selectedSpineIndex >= 0`). Pass nullptr or empty
  // when nothing is focused or when the title shouldn't render. Pattern
  // mirrors the Lyra carousel's title affordance under the cover.
  void drawBookshelfStrip(GfxRenderer& renderer, Rect rect, const char* collectionName,
                          const std::vector<std::string>& coverPaths, int selectedSpineIndex, int scrollOffset,
                          bool headerFocused, bool hasMultipleCollections,
                          const char* focusedBookTitle = nullptr,
                          // CrumBLE series collapse: per-cell member counts. Same
                          // length as coverPaths. count == 1 means single book
                          // (no spine glyph). count >= 2 means series group —
                          // theme draws a dark spine to the left of the cover
                          // and the focused-title overlay reads "Series (N)".
                          const std::vector<int>* seriesMemberCounts = nullptr,
                          // CrumBLE: focused book's author, drawn on a smaller second
                          // line under the title. Null/empty for series cells and
                          // filename-fallback books (which have no metadata author).
                          const char* focusedBookAuthor = nullptr,
                          // CrumBLE: render layout. 1 = single row of 4 large covers
                          // (default), 2 = two rows of 6 smaller covers (12 per page).
                          // HomeActivity picks per the active collection's
                          // twoRowShelf flag.
                          int rowCount = 1,
                          // CrumBLE: per-cell titles used as placeholder content
                          // when the cover bitmap is missing or fails to load.
                          // Same length as coverPaths -- nullptr or empty entries
                          // fall back to the small cover icon. Mirrors the
                          // Bookshelf grid and carousel placeholder styles so
                          // un-thumbnailed books read as intentional cards with
                          // the title showing.
                          const std::vector<std::string>* cellTitles = nullptr) const;

  // CrumBLE #125: focus-only partial repaint for the shelf strip. When
  // the only state change between renders is the focused cell index
  // (same active collection, same scroll offset, same header focus),
  // HomeActivity calls this instead of drawBookshelfStrip to skip the
  // full per-cell repaint loop. Touches only the strokes of two focus
  // rings (erase on previously focused cell, draw on newly focused
  // cell) + repaints the shadow chunks the old ring overlapped + the
  // focused-book title strip below the cells. Cost: a handful of small
  // fill calls vs. ~4 cell blits + the title strip — observable
  // navigation feel parity with the icon bar.
  //
  // `seriesMemberCounts` is needed only to detect whether the
  // previously-focused cell was a series cell (so the dark series
  // spine that the left ring stroke overlapped is restored). Pass the
  // same vector that was passed to the prior drawBookshelfStrip call.
  void drawBookshelfStripFocusUpdate(GfxRenderer& renderer, Rect rect, int prevFocusedSpine, int newFocusedSpine,
                                     int scrollOffset, int bookCount, const char* focusedBookTitle,
                                     const std::vector<int>* seriesMemberCounts, int rowCount = 1) const;

  // CrumBLE: layout constants exposed for HomeActivity's path-window /
  // scroll math. Pass rowCount=1 for the legacy 4x1 layout, =2 for 5x2.
  struct ShelfLayout {
    int cellWidth;
    int cellHeight;
    int cellsPerRow;
    int cellGap;
    int rowGap;       // vertical space between row 1 and row 2 in 2-row mode; ignored when rowCount=1
    int rowCount;
    int stripHeight;
    // CrumBLE: page-stack shadow (the dithered strip on the right + bottom
    // edges of each cell) is dropped in 2-row mode -- the smaller covers
    // make the shadows feel like noise around the cells. The flag lets
    // both drawBookshelfStrip and the partial-repaint path agree without
    // having to re-derive from rowCount.
    bool drawShadows;
  };
  static ShelfLayout shelfLayoutFor(int rowCount);

  // CrumBLE #125: pre-bake the 4 perspective side-cover tiles for every
  // book in `recentBooks` once at home-entry. drawStackedCover then blits
  // the matching tile per book/side instead of re-walking ~70k source
  // pixels per cover. Each book contributes 2 unique tile shapes (left
  // perspective and right perspective — near and far positions on the
  // same side share the shape, only drawX differs). Caches per-book by
  // path so the table survives recentBooks reordering (e.g. just-read
  // book promotion). HomeActivity invokes this after loadRecentCovers
  // populates the in-RAM cover cache; if a source cover is not in the
  // cache, that book's tile is skipped and drawStackedCover falls back
  // to the original perspective-render path.
  void prerenderCarouselSideTiles(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks) const;

  // Drop all baked tiles (~24 KB). Called from HomeActivity::onExit so
  // the cache doesn't stay resident while the user is in the reader (the
  // reader's heap envelope is tight, especially under BLE).
  void clearCarouselSideTiles() const;

 public:
  // Set by HomeActivity right before invoking drawRecentBookCover. When
  // true, the theme bypasses the 5 BMP loads (4 side covers + 1 center
  // cover) and trusts that the framebuffer was restored from a previous
  // render where the same covers were painted at the same positions.
  // Title and footer text still render because drawHeader's fillRect
  // partially wipes the title strip on every frame. The flag is one-
  // shot: drawRecentBookCover resets it before returning, so a stale
  // value can't leak into the next paint.
  //
  // Lives on the theme rather than a function parameter because the
  // drawRecentBookCover signature is part of the BaseTheme virtual
  // contract — overloading it would touch every other theme. mutable
  // because drawRecentBookCover is const.
  mutable bool skipCarouselCoverLoads = false;

  // CrumBLE: when a book is focused on the Collections shelf (no icon is
  // active in the icon bar), HomeActivity sets this to the focused book's
  // author. drawButtonMenu then uses it as the label text shown above the
  // icons -- same physical slot the selected icon's name normally occupies,
  // since the user can never be focused on both a book AND an icon at
  // once. Drawn from drawBookshelfStrip's old author position into the
  // icon bar's label band fixed the bug where the author was being wiped
  // by drawButtonMenu's pre-render clear. One-shot: drawButtonMenu resets
  // it before returning so a stale value can't leak into the next frame.
  mutable std::string focusedBookAuthorForLabel;

 private:
  // Tracks "is page 2 currently shown" across renders. mutable because
  // drawButtonMenu is a const method (theme contract). State is purely
  // a UX hint; resets itself whenever the cursor lands unambiguously
  // inside page 1's exclusive zone.
  mutable bool stickyMenuPage2 = false;

  // CrumBLE #124: per-render center-cover header probe used to run on
  // EVERY drawRecentBookCover even on the skipCarouselCoverLoads fast
  // path -- the comment at the probe site noted it was deliberately kept
  // to keep the footer width stable across renders. But the probe is a
  // BMP file-open + header parse on SD, so every carousel L/R press paid
  // for it. Cache the parsed dimensions keyed on the cover path so the
  // SD probe only runs when the center book actually changes. Sentinel
  // emptyPath / -1 dims = "no probe done yet"; mismatch on path triggers
  // a fresh probe.
  mutable std::string lastCenterCoverPath_;
  mutable int lastCenterCoverSrcW_ = -1;
  mutable int lastCenterCoverSrcH_ = -1;

  // CrumBLE #125: track the selection-border state that was actually
  // drawn last frame so the skipCarouselCoverLoads fast path can
  // toggle JUST the border (draw or erase) when the user moves focus
  // into / out of the carousel without changing the center book.
  // Previously the border was drawn inside the `!skipCarouselCoverLoads`
  // guard, so any focus-only change had to repaint 5 covers + chrome
  // just to flip the border. -1 sentinel = "no border state recorded
  // yet" (first render after onEnter; treat as full repaint).
  mutable int lastDrawnSelectionBorder_ = -1;  // -1 = unknown, 0 = absent, 1 = present

  // CrumBLE #125: pre-baked perspective side-cover tile cache. See
  // prerenderCarouselSideTiles() / clearCarouselSideTiles() docstrings.
  // Tile pixels are 1bpp packed MSB-first, sized (sideCoverWidth + 7) / 8
  // bytes per row * sideInnerHeight rows ~= 2.4 KB. Two tiles per book
  // (left/right perspective) -> ~24 KB for the 5-book carousel.
  struct PerspectiveTile {
    std::unique_ptr<uint8_t[]> pixels;
    int width = 0;
    int height = 0;
  };
  struct BookSideTiles {
    PerspectiveTile left;   // hL = sideInnerHeight, hR = sideOuterHeight
    PerspectiveTile right;  // hL = sideOuterHeight, hR = sideInnerHeight
  };
  mutable std::unordered_map<std::string, BookSideTiles> sideTileCache_;
};
