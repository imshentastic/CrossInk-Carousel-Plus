#include "BookStatsActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Mirror of the cache-path convention used elsewhere in the firmware
// (HomeActivity, Epub.h:42, Xtc.h:52, Txt.cpp:94). Stats files live at
// <cachePath>/stats.bin.
std::string statsCachePathFor(const std::string& bookPath) {
  const std::size_t h = std::hash<std::string>{}(bookPath);
  if (FsHelpers::hasEpubExtension(bookPath)) return "/.crosspoint/epub_" + std::to_string(h);
  if (FsHelpers::hasXtcExtension(bookPath)) return "/.crosspoint/xtc_" + std::to_string(h);
  return "/.crosspoint/txt_" + std::to_string(h);
}

}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::string& bookPath, const std::string& title,
                                     const std::string& coverBmpPath, const BookReadingStats& stats,
                                     const GlobalReadingStats& globalStats, bool backToHome)
    : Activity("BookStats", renderer, mappedInput),
      initialBookPath(bookPath),
      initialBookTitle(title),
      initialCoverBmpPath(coverBmpPath),
      initialStats(stats),
      globalStats(globalStats),
      backToHome(backToHome) {}

void BookStatsActivity::buildNavList() {
  nav.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  for (const auto& b : books) {
    const auto bookStats = BookReadingStats::load(statsCachePathFor(b.path));
    if (bookStats.sessionCount > 0) {
      // CrumBLE #125: store the freshly-loaded stats on the NavEntry so
      // loadCurrent on every L/R press can read from RAM instead of
      // re-opening stats.bin from SD. The filter already paid the SD
      // cost; throwing the value away meant every press paid again.
      nav.push_back({b.path, b.title, b.author, b.coverBmpPath, bookStats});
    }
  }
  bool found = false;
  for (size_t i = 0; i < nav.size(); ++i) {
    if (!initialBookPath.empty() && nav[i].path == initialBookPath) {
      found = true;
      currentIndex = static_cast<int>(i);
      break;
    }
  }
  if (!found) {
    // Author isn't passed via the constructor; if the initial book isn't in
    // RECENT_BOOKS we just leave it blank rather than widening scope.
    // CrumBLE #125: seed the cached stats slot from initialStats so the
    // first-render path reads from RAM uniformly with the recent-list
    // entries above. useInitialStats then governs the override.
    nav.insert(nav.begin(),
               {initialBookPath, initialBookTitle, std::string{}, initialCoverBmpPath, initialStats});
    currentIndex = 0;
  }
}

void BookStatsActivity::loadCurrent(int index) {
  if (nav.empty()) return;
  const int n = static_cast<int>(nav.size());
  while (index < 0) index += n;
  while (index >= n) index -= n;
  currentIndex = index;

  const auto& e = nav[currentIndex];
  currentBookPath = e.path;
  currentTitle = e.title;
  currentAuthor = e.author;
  currentCoverBmpPath = e.coverBmpPath;

  if (useInitialStats && !initialBookPath.empty() && e.path == initialBookPath) {
    currentStats = initialStats;
  } else {
    // CrumBLE #125: read from the NavEntry's pre-loaded copy instead of
    // re-hitting stats.bin on every press. buildNavList already loaded
    // them once when filtering.
    currentStats = e.stats;
    useInitialStats = false;
  }
}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  buildNavList();
  loadCurrent(currentIndex);
  requestUpdate();
}

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // Confirm opens the book currently in view. onSelectBook → goToReader →
  // replaceActivity clears the whole stack (including the parent that
  // opened us via startActivityForResult). The reader exits to a fresh
  // HomeActivity, and because opening a book bumps it to RECENT_BOOKS[0],
  // the next time the user opens Reading Stats from Home it'll show the
  // just-opened book first — same "current book" behavior the carousel
  // already gives.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!currentBookPath.empty() && Storage.exists(currentBookPath.c_str())) {
      LOG_DBG("BSA", "Opening book from stats: %s", currentBookPath.c_str());
      onSelectBook(currentBookPath);
      return;
    }
  }
  // Right + Down → next book; Left + Up → previous book. Side buttons
  // mirror the bottom rocker so either pair cycles books.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (nav.size() > 1) {
      loadCurrent(currentIndex + 1);
      requestUpdate();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (nav.size() > 1) {
      loadCurrent(currentIndex - 1);
      requestUpdate();
    }
  }
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();

  // ─── Page-level layout constants ─────────────────────────────────────────
  // Print-style: typography only. No boxes, outlines, dividers.
  constexpr int margin = 16;
  const int contentX = margin;
  const int contentRight = screenWidth - margin;
  const int contentW = screenWidth - 2 * margin;

  // ─── Header ("Reading Stats" + battery) ─────────────────────────────────
  // CrumBLE: pass the title directly to drawHeader so it picks up the
  // compacted layout (matches Bookshelf's header geometry). Previously
  // drew our own title with batteryBarHeight-based positioning that was
  // tuned for the legacy 84-px-tall header and would now collide with
  // the divider in the new 52-px-tall layout.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  // Cursor that walks down the page; each section advances it.
  int y = metrics.topPadding + metrics.headerHeight + 18;  // 18px below the screen title

  // ─── Section 1: Cover (no border, no rounded corners, raw bitmap) ────────
  // Aspect-fit within (448 × 280) and center horizontally. If no cached
  // cover thumb is available we draw nothing and let the rest of the page
  // shift up (no placeholder).
  // Bitmap fits inside (444 × 236); a 3px black border is drawn just
  // outside it (matches the selected-book border thickness in the Recent
  // Books grid), so the visual footprint (bitmap + border) is (450 × 242).
  // CrumBLE #125: align Stats main-cover dimensions with the Flow
  // carousel's centerCoverWidth × centerCoverHeight (220 × 320). The
  // home carousel already pre-builds a 1bpp scaledPixels buffer at
  // that target size for every recent book; making Stats request the
  // same target lets drawCachedBitmap reuse the same scaled buffer
  // (no buildScaledBitmap rebuild per press). Previously kCoverMaxW
  // = 444, kCoverMaxH = 236 -- a landscape slot that rendered the
  // portrait cover at 162×236, which didn't align with any home-cache
  // entry and forced a ~70k source-pixel re-scale per L/R press.
  // Visual change: cover now renders at full 220×320 (or aspect-fit
  // close to it for non-standard sources), centered horizontally, in
  // a portrait box. Stats sections below shift down by ~84 px.
  constexpr int kCoverMaxW = 220;
  constexpr int kCoverMaxH = 320;
  constexpr int kCoverGapBottom = 14;

  if (!currentCoverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(currentCoverBmpPath, metrics.homeCoverHeight);

    // CrumBLE: shared constants between the cover-rendering lambda and
    // the cache-vs-SD blit closures below. Layered selection rings (3 px
    // inner + 1 px outer with a 2 px gap, matching RecentBooksGridActivity)
    // give the same "layered 3D" look. bookCornerRadius=6 matches the
    // LyraFlow carousel center cover so the silhouette is identical
    // across home and stats.
    constexpr int kBookCornerRadius = 6;
    constexpr int kSelectionPadding = 4;
    constexpr int kSelectionGap = 2;
    constexpr int kSelectionOuterInset = kSelectionPadding + kSelectionGap;

    // CrumBLE #125: cover-render block now tries the in-RAM bitmap
    // cache before falling back to a fresh SD open + BMP parse. The
    // common case (entering stats from the home carousel, where the
    // same thumb size is already cached) eliminates 1 SD open + 1 BMP
    // header parse + a full SD-streamed pixel read per L/R press. The
    // peek block below applies the same fast path for both prev/next
    // covers (2 more SD reads × press eliminated when cached).
    auto renderMain = [&](int srcW, int srcH, std::function<void(int, int, int, int)> blit) {
      const float fitScale = std::min(static_cast<float>(kCoverMaxW) / static_cast<float>(srcW),
                                      static_cast<float>(kCoverMaxH) / static_cast<float>(srcH));
      // Box dimensions to pass to the blit callback (its scale = min of
      // these ratios). drawBitmap / drawCachedBitmap render into
      // floor((srcDim - 1) * scale) + 1 pixels per dimension — which can
      // be 1 px shorter than the box on either axis depending on
      // rounding of srcW/srcH. We compute the actual rendered dimensions
      // and tight-wrap the border to those, so books with slightly
      // different aspect ratios get a snug border.
      const int awBox = std::min(kCoverMaxW, static_cast<int>(std::round(srcW * fitScale)));
      const int ahBox = std::min(kCoverMaxH, static_cast<int>(std::round(srcH * fitScale)));
      const float actualScale = std::min(static_cast<float>(awBox) / static_cast<float>(srcW),
                                         static_cast<float>(ahBox) / static_cast<float>(srcH));
      const int aw = static_cast<int>(std::floor((srcW - 1) * actualScale)) + 1;
      const int ah = static_cast<int>(std::floor((srcH - 1) * actualScale)) + 1;
      // Cover sits below the outer ring; rings constants defined at the
      // outer cover-section scope so the blit lambdas (below) can also
      // see kBookCornerRadius.
      const int bx = (screenWidth - aw) / 2;
      const int by = y + kSelectionOuterInset;
      // White-fill the cover area so drawCachedBitmap's corner-skip
      // leaves white substrate at the rounded corners (not the
      // clearScreen's white from earlier, which is fine — defensive).
      renderer.fillRoundedRect(bx, by, aw, ah, kBookCornerRadius, Color::White);
      blit(bx, by, awBox, ahBox);
      // Inner ring: 3 px stroke just outside the cover.
      renderer.drawRoundedRect(bx - kSelectionPadding, by - kSelectionPadding, aw + 2 * kSelectionPadding,
                               ah + 2 * kSelectionPadding, 3, kBookCornerRadius + kSelectionPadding, true);
      // Outer ring: 1 px stroke, 2 px further out. Layered look.
      renderer.drawRoundedRect(bx - kSelectionOuterInset, by - kSelectionOuterInset,
                               aw + 2 * kSelectionOuterInset, ah + 2 * kSelectionOuterInset, 1,
                               kBookCornerRadius + kSelectionOuterInset, true);

      // ─── Prev / next book peeks (cyclic; only when nav has ≥ 2 books) ──
      // Peeks are flush against the screen's left and right edges. Peek
      // bitmap is scaled to 85% of the current cover's height (so its
      // aspect-correct width is 85% too), vertically centered against
      // the current cover. Each peek gets a 2 px black border just
      // outside its bitmap — the inside-facing edge plus top/bottom are
      // visible; the outside edge falls off-screen and harmlessly clips.
      // A bold white chevron with a 1 px black halo is layered on top.
      if (nav.size() >= 2) {
        constexpr int peekW = 64;
        constexpr int peekBorder = 1;  // thinner border on edge peeks (main cover keeps 2 px)
        // CrumBLE #125: peek height matches MAIN cover height (was 85%).
        // Reason: drawCachedBitmap's scaledPixels buffer is per-source and
        // sized to the most-recently-requested target. With peeks at 85%
        // the scaled buffer for an adjacent book had to be rebuilt every
        // press (peek-size != main-size). With them aligned, each book's
        // scaled buffer is reused across main/peek/main as the user
        // navigates -- zero re-scale per press. Visual: peeks now show
        // as full-height vertical slivers (think Apple Music queue peek)
        // rather than shorter shrunken thumbnails; the chevron + 64 px
        // visible width keep them reading as "peek next/prev book".
        const int ph = ah;
        const int peekY = by;

        auto drawChevron = [&](int cx, int cy, bool pointLeft) {
          // CrumBLE: bumped chevron size + outline thickness for readability
          // against both light and dark cover backgrounds. Was 7x12 with
          // 5/3 stroke (1 px black halo each side); now 9x14 with 8/4
          // stroke (2 px black halo each side of a 4 px white core). The
          // thicker black halo makes the arrow visible against pure-white
          // peek areas where the white core alone would blend in; the
          // wider white core keeps it readable against dark covers.
          constexpr int chHalfW = 9;
          constexpr int chHalfH = 14;
          const int x1 = pointLeft ? cx + chHalfW : cx - chHalfW;
          const int x2 = pointLeft ? cx - chHalfW : cx + chHalfW;
          renderer.drawLine(x1, cy - chHalfH, x2, cy, 8, true);
          renderer.drawLine(x2, cy, x1, cy + chHalfH, 8, true);
          renderer.drawLine(x1, cy - chHalfH, x2, cy, 4, false);
          renderer.drawLine(x2, cy, x1, cy + chHalfH, 4, false);
        };

        auto drawPeekBorder = [&](int peekX, int pY, int pH) {
          renderer.fillRect(peekX - peekBorder, pY - peekBorder, peekW + 2 * peekBorder, peekBorder, true);  // top
          renderer.fillRect(peekX - peekBorder, pY + pH, peekW + 2 * peekBorder, peekBorder, true);          // bottom
          renderer.fillRect(peekX - peekBorder, pY, peekBorder, pH, true);                                   // left
          renderer.fillRect(peekX + peekW, pY, peekBorder, pH, true);                                        // right
        };

        auto drawPeek = [&](bool isLeft, const std::string& adjCoverPath) {
          const int peekX = isLeft ? 0 : (screenWidth - peekW);

          bool drawnFromBitmap = false;
          if (!adjCoverPath.empty()) {
            const std::string adjThumb = UITheme::getCoverThumbPath(adjCoverPath, metrics.homeCoverHeight);
            // Cache fast-path first.
            GfxRenderer::CachedBitmap* adjCached = renderer.lookupCachedBitmap(adjThumb);
            int aSrcW = 0, aSrcH = 0;
            if (adjCached && renderer.getCachedBitmapDimensions(adjCached, &aSrcW, &aSrcH) && aSrcW > 0 &&
                aSrcH > 0) {
              const int aScaledW = (ph * aSrcW) / aSrcH;
              const int adjX = isLeft ? (peekX + peekW - aScaledW) : peekX;
              renderer.drawCachedBitmap(adjCached, adjX, peekY, aScaledW, ph);
              drawnFromBitmap = true;
              drawPeekBorder(peekX, peekY, ph);
            } else if (Storage.exists(adjThumb.c_str())) {
              FsFile adjFile;
              if (Storage.openFileForRead("STATS", adjThumb, adjFile)) {
                Bitmap adjBmp(adjFile);
                if (adjBmp.parseHeaders() == BmpReaderError::Ok && adjBmp.getWidth() > 0 &&
                    adjBmp.getHeight() > 0) {
                  aSrcW = adjBmp.getWidth();
                  aSrcH = adjBmp.getHeight();
                  const int aScaledW = (ph * aSrcW) / aSrcH;
                  // Position so the inside-facing edge of the adjacent
                  // cover lands on the inside-facing edge of the peek.
                  // Bleed past the screen edge is clipped by drawBitmap.
                  const int adjX = isLeft ? (peekX + peekW - aScaledW) : peekX;
                  renderer.drawBitmap(adjBmp, adjX, peekY, aScaledW, ph);
                  drawnFromBitmap = true;
                  drawPeekBorder(peekX, peekY, ph);
                }
                adjFile.close();
              }
            }
          }
          if (!drawnFromBitmap) {
            renderer.fillRect(peekX, peekY, peekW, ph, true);  // solid dark fallback
          }

          drawChevron(peekX + peekW / 2, peekY + ph / 2, isLeft);
        };

        const int n = static_cast<int>(nav.size());
        const int prevIdx = (currentIndex - 1 + n) % n;
        const int nextIdx = (currentIndex + 1) % n;
        drawPeek(true, nav[prevIdx].coverBmpPath);
        drawPeek(false, nav[nextIdx].coverBmpPath);
      }

      // Advance y by the FULL footprint of the cover + both rings + the
      // bottom gap. by = original y + kSelectionOuterInset, then cover
      // is ah tall, then another kSelectionOuterInset of ring on the
      // bottom side, then the gap before the next section.
      y += 2 * kSelectionOuterInset + ah + kCoverGapBottom;
    };

    // Main cover: try cache, fall back to SD. The Opaque<true> +
    // cornerRadius args on drawCachedBitmap match Bookshelf grid's
    // pattern -- the blit writes both inks AND skips the four corner
    // triangles so the rounded silhouette is rendered directly without
    // a follow-up maskRoundedRectOutsideCorners pass.
    int srcW = 0, srcH = 0;
    GfxRenderer::CachedBitmap* cached = renderer.lookupCachedBitmap(thumbPath);
    if (cached && renderer.getCachedBitmapDimensions(cached, &srcW, &srcH) && srcW > 0 && srcH > 0) {
      renderMain(srcW, srcH, [&](int x, int y, int w, int h) {
        renderer.drawCachedBitmap<true>(cached, x, y, w, h, 0.0f, 0.0f, kBookCornerRadius);
      });
    } else if (Storage.exists(thumbPath.c_str())) {
      FsFile file;
      if (Storage.openFileForRead("STATS", thumbPath, file)) {
        Bitmap bmp(file);
        if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
          renderMain(bmp.getWidth(), bmp.getHeight(), [&](int x, int y, int w, int h) {
            renderer.drawBitmap(bmp, x, y, w, h);
            // drawBitmap doesn't have the cornerRadius skip arg yet --
            // mask the four corner triangles back to white so the SD
            // fallback path matches the cached-path silhouette.
            renderer.maskRoundedRectOutsideCorners(x, y, w, h, kBookCornerRadius, Color::White);
          });
        }
        file.close();
      }
    }
  }

  // ─── Typography ──────────────────────────────────────────────────────────
  //   book title       = UI_12 BOLD       (largest body text — primary landmark)
  //   author           = SMALL regular    (visually secondary under the title)
  //   "All Books"      = UI_12 BOLD       (clear section heading)
  //   stat value       = UI_10 BOLD       (centered in cell — slightly smaller than v3 to fit the page)
  //   stat label       = SMALL regular    (centered, sits below value)
  const int ui12Lh = renderer.getLineHeight(UI_12_FONT_ID);
  const int valueLh = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallLh = renderer.getLineHeight(SMALL_FONT_ID);

  // ─── Section 2: book title (centered) ────────────────────────────────────
  {
    const std::string truncTitle =
        renderer.truncatedText(UI_12_FONT_ID, currentTitle.c_str(), contentW, EpdFontFamily::BOLD);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, (screenWidth - tw) / 2, y, truncTitle.c_str(), true, EpdFontFamily::BOLD);
    y += ui12Lh;
  }
  // CrumBLE #125: gaps halved across the text/stats sections so the
  // taller 220x320 cover (vs the previous 162x236) doesn't push the
  // global stats grid off the bottom of the screen.
  y += 5;  // gap before per-book stats grid (matched with all-books heading→grid)

  // ─── Stat grid helper ────────────────────────────────────────────────────
  // 3 equal-width columns spanning contentW (~149 px each on a 480 px screen).
  // Each cell stacks: bold value, 6 px gap, small regular label, both
  // horizontally centered. 10 px padding above and below the value/label
  // pair keeps the grid tight.
  // If the last row contains fewer than gridColumns stats, that row's cells
  // widen to evenly divide contentW so the partial row reads centered (the
  // 5-stat per-book grid → row 2 has 2 cells × 224 px instead of 2 cells ×
  // 149 px stuck to the left).
  constexpr int gridColumns = 3;
  const int gridFullColW = contentW / gridColumns;
  // CrumBLE #125: halved from 6/10/10 so the per-book grid + global
  // grid both fit under the taller 220x320 cover. Tighter spacing
  // also reads as more data-dense, which suits a stats screen.
  constexpr int gridValueLabelGap = 3;
  constexpr int gridRowPadTop = 5;
  constexpr int gridRowPadBottom = 5;
  const int gridRowH = gridRowPadTop + valueLh + gridValueLabelGap + smallLh + gridRowPadBottom;

  auto drawStatGrid = [&](int gridY, std::initializer_list<std::pair<const char*, const char*>> stats) -> int {
    const int total = static_cast<int>(stats.size());
    const int totalRows = (total + gridColumns - 1) / gridColumns;
    int i = 0;
    for (const auto& kv : stats) {
      const char* label = kv.first;
      const char* value = kv.second;
      const int col = i % gridColumns;
      const int row = i / gridColumns;

      // Cells keep the full-row width even when the last row is partial; we
      // just center the partial row by offsetting its left edge so the cells
      // sit close together rather than spread to the screen edges.
      int cellsInRow = gridColumns;
      if (row == totalRows - 1) cellsInRow = total - row * gridColumns;
      const int rowOffsetX = (contentW - cellsInRow * gridFullColW) / 2;
      const int cellW = gridFullColW;

      const int cellX = contentX + rowOffsetX + col * cellW;
      const int cellY = gridY + row * gridRowH;
      const int valueY = cellY + gridRowPadTop;
      const int labelY = valueY + valueLh + gridValueLabelGap;

      const int vw = renderer.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, cellX + (cellW - vw) / 2, valueY, value, true, EpdFontFamily::BOLD);

      const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, cellX + (cellW - lw) / 2, labelY, label, true);
      ++i;
    }
    return gridY + totalRows * gridRowH;
  };

  // ─── Section 2 (continued): per-book grid ────────────────────────────────
  // 5 stats — Sessions, Reading Time, Pages Turned, Avg Session, Pages/Min.
  // 3-col grid: row 1 fully populated, row 2's last cell stays empty.
  char sessionsBuf[16];
  char timeBuf[24];
  char pagesBuf[16];
  char avgBuf[24];
  char ppmBuf[8];
  snprintf(sessionsBuf, sizeof(sessionsBuf), "%u", static_cast<unsigned>(currentStats.sessionCount));
  BookReadingStats::formatDuration(currentStats.totalReadingSeconds, timeBuf, sizeof(timeBuf));
  snprintf(pagesBuf, sizeof(pagesBuf), "%lu", static_cast<unsigned long>(currentStats.totalPagesTurned));
  {
    const uint32_t avgSecs =
        currentStats.sessionCount > 0 ? currentStats.totalReadingSeconds / currentStats.sessionCount : 0;
    BookReadingStats::formatDuration(avgSecs, avgBuf, sizeof(avgBuf));
  }
  if (currentStats.totalReadingSeconds > 60) {
    const float ppm = static_cast<float>(currentStats.totalPagesTurned) * 60.0f /
                      static_cast<float>(currentStats.totalReadingSeconds);
    snprintf(ppmBuf, sizeof(ppmBuf), "%.1f", ppm);
  } else {
    snprintf(ppmBuf, sizeof(ppmBuf), "0.0");
  }

  y = drawStatGrid(y, {
                          {tr(STR_STATS_SESSIONS_LBL), sessionsBuf},
                          {tr(STR_STATS_TIME_LBL), timeBuf},
                          {tr(STR_STATS_PAGES_LBL), pagesBuf},
                          {tr(STR_STATS_AVG_SESSION_LBL), avgBuf},
                          {tr(STR_STATS_PAGES_PER_MIN), ppmBuf},
                      });

  // ─── Section 3: All Books (centered heading) ─────────────────────────────
  // CrumBLE #125: gaps halved (18→9, 10→5) to fit under the taller cover.
  y += 9;
  {
    const int hw = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_STATS_ALL_TIME), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, (screenWidth - hw) / 2, y, tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);
  }
  y += ui12Lh + 5;  // heading sits 5 px above the grid that follows (matches title→grid)

  // 6 stats — Sessions, Reading Time, Pages Turned, Avg Session, Pages/Min,
  // and Books Read (existing STR_STATS_COMPLETED_LBL backing the same data).
  char gSessionsBuf[16];
  char gTimeBuf[24];
  char gPagesBuf[16];
  char gAvgBuf[24];
  char gPpmBuf[8];
  char gCompletedBuf[16];
  snprintf(gSessionsBuf, sizeof(gSessionsBuf), "%lu", static_cast<unsigned long>(globalStats.totalSessions));
  BookReadingStats::formatDuration(globalStats.totalReadingSeconds, gTimeBuf, sizeof(gTimeBuf));
  snprintf(gPagesBuf, sizeof(gPagesBuf), "%lu", static_cast<unsigned long>(globalStats.totalPagesTurned));
  {
    const uint32_t globalAvgSecs =
        globalStats.totalSessions > 0 ? globalStats.totalReadingSeconds / globalStats.totalSessions : 0;
    BookReadingStats::formatDuration(globalAvgSecs, gAvgBuf, sizeof(gAvgBuf));
  }
  if (globalStats.totalReadingSeconds > 60) {
    const float gppm = static_cast<float>(globalStats.totalPagesTurned) * 60.0f /
                       static_cast<float>(globalStats.totalReadingSeconds);
    snprintf(gPpmBuf, sizeof(gPpmBuf), "%.1f", gppm);
  } else {
    snprintf(gPpmBuf, sizeof(gPpmBuf), "0.0");
  }
  snprintf(gCompletedBuf, sizeof(gCompletedBuf), "%lu", static_cast<unsigned long>(globalStats.completedBooks));

  drawStatGrid(y, {
                      {tr(STR_STATS_SESSIONS_LBL), gSessionsBuf},
                      {tr(STR_STATS_TIME_LBL), gTimeBuf},
                      {tr(STR_STATS_PAGES_LBL), gPagesBuf},
                      {tr(STR_STATS_AVG_SESSION_LBL), gAvgBuf},
                      {tr(STR_STATS_PAGES_PER_MIN), gPpmBuf},
                      {tr(STR_STATS_COMPLETED_LBL), gCompletedBuf},
                  });

  // ─── Button hints ────────────────────────────────────────────────────────
  const char* prevLbl = nav.size() > 1 ? tr(STR_DIR_PREV) : "";
  const char* nextLbl = nav.size() > 1 ? tr(STR_DIR_NEXT) : "";
  const auto labels = mappedInput.mapLabels(backToHome ? tr(STR_HOME) : tr(STR_BACK), tr(STR_OPEN), prevLbl, nextLbl);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
