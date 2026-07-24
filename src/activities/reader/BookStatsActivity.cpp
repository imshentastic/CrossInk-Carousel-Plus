#include "BookStatsActivity.h"

#include <Bitmap.h>
#include <Epub.h>
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
#include "ReadingStats.h"
#include "ReadingStatsUtils.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/home/RecentBookProgress.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Mirror of the cache-path convention used elsewhere in the firmware
// (HomeActivity, Epub.h:42, Xtc.h:52, Txt.cpp:94). Stats files live at
// <cachePath>/stats.bin.
//
// CrumBLE: EPUB cache paths hash with ZipFile::fnvHash64 (see
// Epub::cachePathForFilePath at Epub.cpp:213), NOT std::hash. The
// earlier impl here used std::hash for all three formats, so EPUB
// stats were loaded from a non-existent directory and the carousel
// reported zero sessions / zero pages for every book the user
// navigated to. Until the sessionCount>0 filter was dropped, the
// bug was masked because only the initial book (whose stats are
// passed in by value, not loaded) survived the filter. Txt and Xtc
// still use std::hash; switching them would orphan existing caches.
std::string statsCachePathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    return Epub::cachePathForFilePath(bookPath, "/.crosspoint");
  }
  const std::size_t h = std::hash<std::string>{}(bookPath);
  if (FsHelpers::hasXtcExtension(bookPath)) return "/.crosspoint/xtc_" + std::to_string(h);
  return "/.crosspoint/txt_" + std::to_string(h);
}

// v18.9.9.189 — small duplicates of DashboardTheme's time-left / est-finish
// helpers so the per-book stats page can show the same derived stats
// Dashboard shows on Home. Duplicated (not shared) to keep this change
// scoped to BookStatsActivity and avoid header churn in DashboardTheme.
bool fallbackEstimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || stats.totalReadingSeconds < 120) return false;
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(stats.totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) return false;
  seconds = static_cast<uint32_t>(estimate + 0.5f);
  return seconds > 0;
}

bool estimatedTimeLeftFor(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  return fallbackEstimatedTimeLeft(stats, progressPercent, seconds);
}

bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  outDate = {};
  if (!today.isValid() || !stats.startDate.isValid() || estimatedReadingSeconds == 0 ||
      stats.totalReadingSeconds == 0) {
    return false;
  }
  const uint16_t elapsedDays = readingSpanDaysElapsed(stats.startDate, today.date);
  const uint16_t readingDays = std::max<uint16_t>(1, elapsedDays);
  const uint64_t estimatedCalendarSeconds =
      (static_cast<uint64_t>(estimatedReadingSeconds) * static_cast<uint64_t>(readingDays) * 86400ULL +
       static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
      static_cast<uint64_t>(stats.totalReadingSeconds);
  if (estimatedCalendarSeconds == 0) return false;
  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::string& bookPath, const std::string& title,
                                     const std::string& coverBmpPath, const BookReadingStats& stats,
                                     const GlobalReadingStats& globalStats, bool backToHome,
                                     bool startOnAllBooksPage)
    : Activity("BookStats", renderer, mappedInput),
      initialBookPath(bookPath),
      initialBookTitle(title),
      initialCoverBmpPath(coverBmpPath),
      initialStats(stats),
      globalStats(globalStats),
      backToHome(backToHome) {
  // v18.9.9.199: "All Books" is a data-source toggle now, not a page.
  if (startOnAllBooksPage) showAllBooks_ = true;
}

void BookStatsActivity::buildNavList() {
  nav.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  for (const auto& b : books) {
    // CrumBLE: previously filtered to only books with sessionCount > 0,
    // which meant any SD-state event that wiped stats (eg. a Disk
    // Utility First Aid pass, a manual cache delete, a fresh upload of
    // a previously-cached book) collapsed the carousel to a single
    // entry and stripped the user's ability to scroll. Show every
    // recent book instead -- books without stats yet render their
    // numeric fields as zeroes, which is honest and matches the
    // user's mental model of "show me my recent books." Still caches
    // the loaded stats on the NavEntry so L/R presses don't re-open
    // stats.bin from SD.
    const auto bookStats = BookReadingStats::load(statsCachePathFor(b.path));
    nav.push_back({b.path, b.title, b.author, b.coverBmpPath, bookStats});
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

  // v18.9.9.189: pull reading progress for THIS book (one SD read per
  // book-nav, cached in currentProgressPercent_ until next L/R).
  RecentBook rb;
  rb.path = e.path;
  rb.title = e.title;
  rb.author = e.author;
  rb.coverBmpPath = e.coverBmpPath;
  currentProgressPercent_ = RecentBookProgress::loadPercent(rb);
}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  // v18.9.9.474: arm terminate-recovery to Home so a mid-buildNavList /
  // per-book stats-load OOM (typically hit on cold-boot with a very large
  // recents list) lands on a clean Home instead of whatever activity was
  // previously armed.
  armSilentRestartTarget(/*SILENT_REBOOT_TARGET_HOME=*/0);
  buildNavList();
  loadCurrent(currentIndex);
  requestUpdate();
}

void BookStatsActivity::onExit() {
  Activity::onExit();
  // v18.9.9.474: clear our terminate-recovery arming so a later terminate
  // uses whatever the next activity arms.
  clearArmedSilentRestartTarget();
}

void BookStatsActivity::loop() {
  constexpr unsigned long kEditHoldMs = 800;

  // v18.9.9.202: date-editor mode intercepts everything.
  if (editingDates_) {
    if (!confirmLongHandled_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= kEditHoldMs) {
      confirmLongHandled_ = true;
      clearEditedDateGroup();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmLongHandled_) {
        confirmLongHandled_ = false;
        return;
      }
      editField_ = (editField_ + 1) % 6;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      commitEditedDates();
      editingDates_ = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      adjustEditedDateField(+1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      adjustEditedDateField(-1);
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // v18.9.9.202: long-press Toggle on the book stats page opens the date
  // editor. Short-press stays the This Book ↔ All Books toggle.
  if (!confirmLongHandled_ && !showAllBooks_ && currentPage_ == 0 &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= kEditHoldMs) {
    confirmLongHandled_ = true;
    editingDates_ = true;
    editField_ = 0;
    requestUpdate();
    return;
  }
  // v18.9.9.199: Confirm = Toggle. Swaps the data source between the
  // current book and the All Books aggregate; both pages keep their
  // layout, just re-render with the other source. (The old "Open book
  // from stats" behavior is gone — Back → Home → open covers that.)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmLongHandled_) {
      confirmLongHandled_ = false;
      return;
    }
    showAllBooks_ = !showAllBooks_;
    requestUpdate();
    return;
  }
  // Up / Down flip between the stats page (0) and the charts page (1).
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    currentPage_ ^= 1;
    requestUpdate();
    return;
  }
  // L/R cycle books — only meaningful when showing a single book.
  if (!showAllBooks_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (nav.size() > 1) {
        loadCurrent(currentIndex + 1);
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (nav.size() > 1) {
        loadCurrent(currentIndex - 1);
        requestUpdate();
      }
    }
  }
}

void BookStatsActivity::adjustEditedDateField(int delta) {
  const bool finishGroup = editField_ >= 3;
  ReadingStatsDate& date = finishGroup ? currentStats.finishDate : currentStats.startDate;
  if (!date.isValid()) {
    // Seed a cleared date: sibling date, else today, else a sane default.
    const ReadingStatsDate& other = finishGroup ? currentStats.startDate : currentStats.finishDate;
    ReadingStatsDateTime now;
    if (other.isValid()) {
      date = other;
    } else if (getCurrentLocalReadingStatsDateTime(now)) {
      date = now.date;
    } else {
      date = {2026, 1, 1};
    }
  }
  switch (editField_ % 3) {
    case 0: {  // month, wraps
      int m = static_cast<int>(date.month) + delta;
      if (m < 1) m = 12;
      if (m > 12) m = 1;
      date.month = static_cast<uint8_t>(m);
      break;
    }
    case 1: {  // day, wraps within the month
      const int monthDays = daysInMonth(date.year, date.month);
      int d = static_cast<int>(date.day) + delta;
      if (d < 1) d = monthDays;
      if (d > monthDays) d = 1;
      date.day = static_cast<uint8_t>(d);
      break;
    }
    default: {  // year, clamps to the format's range
      int yr = static_cast<int>(date.year) + delta;
      if (yr < 2000) yr = 2000;
      if (yr > 2099) yr = 2099;
      date.year = static_cast<uint16_t>(yr);
      break;
    }
  }
  // Month/year changes can strand the day past the month's end.
  const int monthDays = daysInMonth(date.year, date.month);
  if (date.day > monthDays) date.day = static_cast<uint8_t>(monthDays);
  currentStats.flags |=
      finishGroup ? BookReadingStats::FLAG_FINISH_DATE_MANUAL : BookReadingStats::FLAG_START_DATE_MANUAL;
}

void BookStatsActivity::clearEditedDateGroup() {
  if (editField_ >= 3) {
    currentStats.finishDate.clear();
    currentStats.flags &= static_cast<uint8_t>(~BookReadingStats::FLAG_FINISH_DATE_MANUAL);
    if (currentStats.isCompleted) {
      currentStats.isCompleted = false;
      if (globalStats.completedBooks > 0) globalStats.completedBooks--;
    }
  } else {
    currentStats.startDate.clear();
    currentStats.flags &= static_cast<uint8_t>(~BookReadingStats::FLAG_START_DATE_MANUAL);
  }
}

void BookStatsActivity::commitEditedDates() {
  // Keep finish >= start when both are set.
  if (currentStats.startDate.isValid() && currentStats.finishDate.isValid() &&
      compareReadingStatsDate(currentStats.finishDate, currentStats.startDate) < 0) {
    currentStats.finishDate = currentStats.startDate;
  }
  // A manually-set finish date means the book is done.
  if (currentStats.finishDate.isValid() && !currentStats.isCompleted) {
    currentStats.isCompleted = true;
    globalStats.completedBooks++;
  }
  if (!currentBookPath.empty()) {
    currentStats.save(statsCachePathFor(currentBookPath));
  }
  globalStats.save();
  // Keep the L/R nav cache + initial-book override in sync with the edit.
  if (currentIndex >= 0 && currentIndex < static_cast<int>(nav.size())) {
    nav[currentIndex].stats = currentStats;
  }
  useInitialStats = false;
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

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

  // ─── Date editor (v18.9.9.202, P2c) ──────────────────────────────────────
  if (editingDates_) {
    const int edUi12Lh = renderer.getLineHeight(UI_12_FONT_ID);
    const int edSmallLh = renderer.getLineHeight(SMALL_FONT_ID);
    y += 12;
    // Book title for context.
    {
      const std::string t =
          renderer.truncatedText(UI_10_FONT_ID, currentTitle.c_str(), screenWidth - 32, EpdFontFamily::BOLD);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, t.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, (screenWidth - tw) / 2, y, t.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(UI_10_FONT_ID) + 24;
    }

    constexpr int kBoxH = 44;
    constexpr int kBoxGap = 10;
    constexpr int kMonthW = 90;
    constexpr int kDayW = 70;
    constexpr int kYearW = 110;
    const int rowW = kMonthW + kDayW + kYearW + kBoxGap * 2;
    const int rowX = (screenWidth - rowW) / 2;

    auto drawDateRow = [&](int rowY, const char* label, const ReadingStatsDate& date, int firstFieldIdx) -> int {
      const int lw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, (screenWidth - lw) / 2, rowY, label, true, EpdFontFamily::BOLD);
      rowY += edUi12Lh + 8;

      char vals[3][12];
      if (date.isValid()) {
        formatReadingStatsMonthToken(date, vals[0], sizeof(vals[0]));
        snprintf(vals[1], sizeof(vals[1]), "%u", static_cast<unsigned>(date.day));
        snprintf(vals[2], sizeof(vals[2]), "%u", static_cast<unsigned>(date.year));
      } else {
        for (auto& v : vals) snprintf(v, sizeof(v), "-");
      }
      const int widths[3] = {kMonthW, kDayW, kYearW};
      int bx = rowX;
      for (int i = 0; i < 3; ++i) {
        const bool selected = editField_ == firstFieldIdx + i;
        if (selected) {
          renderer.fillRect(bx, rowY, widths[i], kBoxH, true);
        } else {
          renderer.drawRect(bx, rowY, widths[i], kBoxH, true);
        }
        const int vw = renderer.getTextWidth(UI_12_FONT_ID, vals[i], EpdFontFamily::BOLD);
        renderer.drawText(UI_12_FONT_ID, bx + (widths[i] - vw) / 2, rowY + (kBoxH - edUi12Lh) / 2, vals[i],
                          !selected, EpdFontFamily::BOLD);
        bx += widths[i] + kBoxGap;
      }
      return rowY + kBoxH + 26;
    };

    y = drawDateRow(y, tr(STR_STATS_STARTED), currentStats.startDate, 0);
    y = drawDateRow(y, tr(STR_STATS_FINISHED_DATE), currentStats.finishDate, 3);

    // Usage hint.
    {
      const char* hint = "Hold Toggle to clear a date";
      const int hw = renderer.getTextWidth(SMALL_FONT_ID, hint);
      renderer.drawText(SMALL_FONT_ID, (screenWidth - hw) / 2, y + 4, hint, true);
      (void)edSmallLh;
    }

    const auto edLabels = mappedInput.mapLabels(tr(STR_DONE), tr(STR_NEXT), "-", "+");
    GUI.drawButtonHints(renderer, edLabels.btn1, edLabels.btn2, edLabels.btn3, edLabels.btn4);
    renderer.displayBuffer();
    return;
  }

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

  // Typography line-heights (declared here so the mini-heatmap branch below
  // and the stats grid further down both see the same values).
  const int ui12Lh = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLh = renderer.getLineHeight(SMALL_FONT_ID);

  // v18.9.9.472/475/199: reading heatmap block — month labels above, day
  // labels left, legend below. Shared by the charts page (both sources)
  // and by the All Books stats view, where it fills the cover slot.
  // bookOnly filters the reading-days log to the current book's hash.
  // Returns the y just past the legend labels.
  auto drawHeatmapBlock = [&](int hy, bool bookOnly) -> int {
    int y = hy;
    constexpr int kHmCell = 14;
    constexpr int kHmGap = 2;
    constexpr int kHmStride = kHmCell + kHmGap;
    constexpr int kHmWeeks = 20;
    constexpr int kHmDays = 7;
    constexpr int kHmLabelW = 34;  // matches ReadingHeatmap kLabelWidth
    const int gridW = kHmWeeks * kHmStride - kHmGap;
    const int gridH = kHmDays * kHmStride - kHmGap;
    // Center (labels + grid) as a unit horizontally with a small extra
    // left inset so day-labels ("Mon", "Wed", "Fri") aren't kissing the
    // screen edge — the standalone Heatmap draws labels at x=0 which the
    // user flagged.
    const int totalBlockW = kHmLabelW + gridW;
    const int blockX = std::max(contentX, (screenWidth - totalBlockW) / 2);
    const int gridX = blockX + kHmLabelW;

    // Data window: 20 weeks back from today, week starts on Sunday
    // (matches ReadingHeatmap; the "Sat below Fri" ask is satisfied by
    // this order — Sat is the last row). Same 20-week window for a
    // single book — sparse for a recent book, but the frame stays
    // consistent between sources.
    const auto aggregates = bookOnly ? ReadingStats::loadAggregatesForBook(currentBookPath.c_str())
                                     : ReadingStats::loadAggregates();
    uint32_t today = ReadingStats::todayEpochDay();
    if (today == 0) {
      for (const auto& agg : aggregates)
        if (agg.epochDay > today) today = agg.epochDay;
      if (today == 0) today = (kHmWeeks - 1) * 7 + 6;
    }
    const uint8_t todayDow = ReadingStats::epochDayToDow(today);
    const uint32_t firstDay = today - todayDow - (kHmWeeks - 1) * 7;

    const int windowDays = kHmWeeks * kHmDays;
    std::vector<uint16_t> minutes(windowDays, 0);
    uint16_t viewMax = 0;
    for (const auto& agg : aggregates) {
      if (agg.epochDay < firstDay || agg.epochDay > today) continue;
      const int idx = static_cast<int>(agg.epochDay - firstDay);
      if (idx < 0 || idx >= windowDays) continue;
      minutes[idx] = agg.minutesRead;
      if (agg.minutesRead > viewMax) viewMax = agg.minutesRead;
    }

    // Month labels ABOVE the grid: label the week whose first day starts a
    // new month, aligned to that week's column.
    const int monthLabelY = y;
    y += smallLh + 4;  // reserve row space
    const int gridY = y;
    {
      uint8_t prevMonth = 0;
      static const char* kMonthShort[13] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      for (int w = 0; w < kHmWeeks; ++w) {
        const uint32_t weekStart = firstDay + w * kHmDays;
        uint16_t yy = 0;
        uint8_t mm = 0, dd = 0;
        ReadingStats::epochDayToYmd(weekStart, yy, mm, dd);
        if (mm != prevMonth) {
          prevMonth = mm;
          const int lx = gridX + w * kHmStride;
          renderer.drawText(SMALL_FONT_ID, lx, monthLabelY, kMonthShort[mm]);
        }
      }
    }

    // Day labels on the left. Sun-first ordering (kDowLabels[0]="" for Sun,
    // [6]="" for Sat). Sat sits at row 6 — the visual bottom, below Fri.
    static const char* kDowLabels[7] = {"", "Mon", "", "Wed", "", "Fri", ""};
    for (int d = 0; d < kHmDays; ++d) {
      if (kDowLabels[d][0] == '\0') continue;
      const int ly = gridY + d * kHmStride + kHmCell - 4;
      renderer.drawText(SMALL_FONT_ID, blockX, ly, kDowLabels[d]);
    }

    // Grid.
    for (int w = 0; w < kHmWeeks; ++w) {
      for (int d = 0; d < kHmDays; ++d) {
        const int idx = w * kHmDays + d;
        const uint32_t cellDay = firstDay + idx;
        if (cellDay > today) continue;
        const int cx = gridX + w * kHmStride;
        const int cy = gridY + d * kHmStride;
        Color c = Color::White;
        if (viewMax > 0 && minutes[idx] > 0) {
          const uint32_t scaled = (static_cast<uint32_t>(minutes[idx]) * 3 + viewMax - 1) / viewMax;
          const uint8_t b = static_cast<uint8_t>(std::min<uint32_t>(3, scaled));
          c = (b == 1) ? Color::LightGray : (b == 2) ? Color::DarkGray : Color::Black;
        }
        renderer.fillRectDither(cx, cy, kHmCell, kHmCell, c);
      }
    }

    // Legend BELOW the grid: 4 tone squares, evenly spaced across the grid
    // width, each with its threshold label centered underneath. No "Less"/
    // "More" text — the squares + numbers are self-explanatory.
    const int legendY = gridY + kHmDays * kHmStride + 6;
    auto legendBuckets = [&](int idx, char* out, size_t n) {
      // Adaptive thresholds (matches standalone heatmap formula). If we
      // have no data, degrade to fixed "0/<30m/<60m/60m+" placeholders.
      const uint16_t m = viewMax > 0 ? viewMax : 90;
      switch (idx) {
        case 0: snprintf(out, n, "0"); break;
        case 1: snprintf(out, n, "<%um", m / 3); break;
        case 2: snprintf(out, n, "<%um", (m * 2) / 3); break;
        default: {
          const uint16_t hours = m / 60;
          const uint16_t mins = m % 60;
          if (hours > 0) snprintf(out, n, "%uh %um+", hours, mins);
          else snprintf(out, n, "%um+", mins);
          break;
        }
      }
    };
    // v18.9.9.476: tighter legend cluster. Was spread across gridW/4 slots
    // (~70px each) which put the labels far apart from adjacent squares.
    // Now fixed 62px between square centers, cluster centered under the
    // grid, label sitting directly under its own square with a 2px gap.
    constexpr int kLegendSlotW = 62;
    const int legendClusterW = kLegendSlotW * 4;
    const int legendStartX = gridX + std::max(0, (gridW - legendClusterW) / 2);
    for (int b = 0; b < 4; ++b) {
      const int slotCenterX = legendStartX + b * kLegendSlotW + kLegendSlotW / 2;
      const int squareX = slotCenterX - kHmCell / 2;
      renderer.fillRectDither(squareX, legendY, kHmCell, kHmCell,
                              (b == 0)   ? Color::White
                              : (b == 1) ? Color::LightGray
                              : (b == 2) ? Color::DarkGray
                                         : Color::Black);
      if (b == 0) renderer.drawRect(squareX, legendY, kHmCell, kHmCell, true);
      char lbl[16];
      legendBuckets(b, lbl, sizeof(lbl));
      const int lw = renderer.getTextWidth(SMALL_FONT_ID, lbl);
      // Label sits directly under its square (drawText y = top of line).
      renderer.drawText(SMALL_FONT_ID, slotCenterX - lw / 2, legendY + kHmCell + 2, lbl);
    }

    // Bottom of the painted block: legend squares + their labels + a pad.
    return legendY + kHmCell + 2 + smallLh + 4;
  };

  if (currentPage_ == 1) {
    // ─── Charts page: heatmap on top, then ToD + DoW bar cards. Data
    // source follows showAllBooks_ (Toggle). Renders fully here and
    // short-circuits the stats-page code below.
    constexpr int kP2CardTitleH = 28;
    constexpr int kP2BarH = 16;
    constexpr int kP2BarRowGap = 8;
    constexpr int kP2ChartPad = 8;
    constexpr int kP2CardGap = 8;
    constexpr int kP2LabelMinW = 74;
    constexpr int kP2LabelLeftInset = 10;
    constexpr int kP2BarLeftInset = 8;
    constexpr int kP2BarRightInset = 12;

    auto p2Frame = [&](int fx, int fy, int fw, int fh, const char* fTitle) {
      renderer.drawRect(fx, fy, fw, fh);
      renderer.drawLine(fx, fy + kP2CardTitleH, fx + fw, fy + kP2CardTitleH);
      const int titleTextW = renderer.getTextWidth(UI_10_FONT_ID, fTitle, EpdFontFamily::BOLD);
      const int titleLh = renderer.getLineHeight(UI_10_FONT_ID);
      renderer.drawText(UI_10_FONT_ID, fx + (fw - titleTextW) / 2, fy + (kP2CardTitleH - titleLh) / 2, fTitle, true,
                        EpdFontFamily::BOLD);
    };
    auto p2Bars = [&](int fx, int fy, int fw, const uint32_t* vals, const char* const* labs, int n) {
      uint32_t maxV = 0;
      for (int i = 0; i < n; ++i)
        if (vals[i] > maxV) maxV = vals[i];
      const int labelLh = renderer.getLineHeight(UI_10_FONT_ID);
      const int rowH = std::max(labelLh, kP2BarH);
      const int rowStride = rowH + kP2BarRowGap;
      int maxLabelW = 0;
      for (int i = 0; i < n; ++i) {
        const int lw = renderer.getTextWidth(UI_10_FONT_ID, labs[i]);
        if (lw > maxLabelW) maxLabelW = lw;
      }
      const int labelColW = std::max(kP2LabelMinW, kP2LabelLeftInset + maxLabelW + 6);
      const int barX = fx + labelColW + kP2BarLeftInset;
      const int barW = std::max(0, fw - labelColW - kP2BarLeftInset - kP2BarRightInset);
      const int contentTop = fy + kP2CardTitleH + kP2ChartPad;
      for (int i = 0; i < n; ++i) {
        const int rowTop = contentTop + i * rowStride;
        const int labY = rowTop + (rowH - labelLh) / 2;
        const int barY = rowTop + (rowH - kP2BarH) / 2;
        renderer.drawText(UI_10_FONT_ID, fx + kP2LabelLeftInset, labY, labs[i]);
        if (maxV > 0 && vals[i] > 0) {
          const int fillW = std::max(2, static_cast<int>((static_cast<int64_t>(barW) * vals[i]) / maxV));
          renderer.fillRect(barX, barY, fillW, kP2BarH, true);
        }
      }
    };

    int p2y = metrics.topPadding + metrics.headerHeight + 8;

    // Context line: which source the charts show ("All Books" or the
    // book title). UI_10 keeps it compact — the heatmap below is the star.
    {
      const char* ctx = showAllBooks_ ? tr(STR_STATS_ALL_TIME) : currentTitle.c_str();
      const std::string truncCtx = renderer.truncatedText(UI_10_FONT_ID, ctx, contentW, EpdFontFamily::BOLD);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, truncCtx.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, (screenWidth - tw) / 2, p2y, truncCtx.c_str(), true, EpdFontFamily::BOLD);
      p2y += renderer.getLineHeight(UI_10_FONT_ID) + 4;
    }

    // Heatmap on top — filtered to this book unless showing All Books.
    p2y = drawHeatmapBlock(p2y, !showAllBooks_) + kP2CardGap;

    // v18.9.9.477: card height must use the actual per-row content height
    // (max of label line-height and bar height), not just bar height.
    // Otherwise labelLh > barH causes the bottom row to bleed into the
    // card below — the reason "Night" and "Sun" were previously clipping
    // into their sibling cards' title bars.
    const int p2RowH = std::max(renderer.getLineHeight(UI_10_FONT_ID), kP2BarH);

    // Time of Day.
    {
      constexpr int rows = 4;
      const int cardH = kP2CardTitleH + kP2ChartPad * 2 + rows * p2RowH + (rows - 1) * kP2BarRowGap;
      p2Frame(contentX, p2y, contentW, cardH, tr(STR_STATS_TIME_OF_DAY));
      const auto& tod = showAllBooks_ ? globalStats.timeOfDaySeconds : currentStats.timeOfDaySeconds;
      const uint32_t vals[rows] = {tod[0], tod[1], tod[2], tod[3]};
      const char* labs[rows] = {tr(STR_STATS_MORNING), tr(STR_STATS_AFTERNOON), tr(STR_STATS_EVENING),
                                tr(STR_STATS_NIGHT)};
      p2Bars(contentX, p2y, contentW, vals, labs, rows);
      p2y += cardH + kP2CardGap;
    }

    // Day of Week.
    {
      constexpr int rows = 7;
      const int cardH = kP2CardTitleH + kP2ChartPad * 2 + rows * p2RowH + (rows - 1) * kP2BarRowGap;
      p2Frame(contentX, p2y, contentW, cardH, tr(STR_STATS_DAY_OF_WEEK));
      const auto& dow = showAllBooks_ ? globalStats.dayOfWeekSeconds : currentStats.dayOfWeekSeconds;
      const uint32_t vals[rows] = {dow[0], dow[1], dow[2], dow[3], dow[4], dow[5], dow[6]};
      const char* labs[rows] = {tr(STR_STATS_MON), tr(STR_STATS_TUE), tr(STR_STATS_WED), tr(STR_STATS_THU),
                                tr(STR_STATS_FRI), tr(STR_STATS_SAT), tr(STR_STATS_SUN)};
      p2Bars(contentX, p2y, contentW, vals, labs, rows);
      p2y += cardH + kP2CardGap;
    }

    // Page indicator dots — drawn inline because the charts page short-
    // circuits the shared code below.
    {
      constexpr int kDotSize = 8;
      constexpr int kDotGap = 8;
      constexpr int kTotalPages = 2;
      const int clusterW = kTotalPages * kDotSize + (kTotalPages - 1) * kDotGap;
      const int startX = (screenWidth - clusterW) / 2;
      const int dotY = screenHeight - metrics.buttonHintsHeight - kDotSize - 8;
      for (int i = 0; i < kTotalPages; ++i) {
        const int dx = startX + i * (kDotSize + kDotGap);
        if (i == currentPage_) {
          renderer.fillRect(dx, dotY, kDotSize, kDotSize, true);
        } else {
          renderer.drawRect(dx, dotY, kDotSize, kDotSize, true);
        }
      }
    }
    const bool booksNavigableP2 = !showAllBooks_ && nav.size() > 1;
    const char* prevLblP2 = booksNavigableP2 ? tr(STR_DIR_PREV) : "";
    const char* nextLblP2 = booksNavigableP2 ? tr(STR_DIR_NEXT) : "";
    const auto labelsP2 =
        mappedInput.mapLabels(backToHome ? tr(STR_HOME) : tr(STR_BACK), tr(STR_TOGGLE), prevLblP2, nextLblP2);
    GUI.drawButtonHints(renderer, labelsP2.btn1, labelsP2.btn2, labelsP2.btn3, labelsP2.btn4);
    renderer.displayBuffer();
    return;
  }

  // ─── Stats page (page 0) ──────────────────────────────────────────────────
  // All Books: the cover slot hosts the aggregate heatmap. This Book:
  // the cover (when one exists).
  if (showAllBooks_) {
    // Title + grid follow right below the heatmap — a tight gap beats
    // aligning with the (taller) cover band, which left a dead zone.
    y = drawHeatmapBlock(y, false) + 12;
  } else if (!currentCoverBmpPath.empty()) {
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

  // ─── Typography (already declared above cover section for mini-heatmap use) ─

  // ─── Section 2: title line ───────────────────────────────────────────────
  // v18.9.9.471: on the All Books page, "All Books" replaces the book title
  // in this slot (was drawn as a separate heading below the grid before,
  // which read as redundant next to a nameless cover).
  {
    const char* titleText = showAllBooks_ ? tr(STR_STATS_ALL_TIME) : currentTitle.c_str();
    const std::string truncTitle = renderer.truncatedText(UI_12_FONT_ID, titleText, contentW, EpdFontFamily::BOLD);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, (screenWidth - tw) / 2, y, truncTitle.c_str(), true, EpdFontFamily::BOLD);
    y += ui12Lh;
  }
  y += 5;

  // ─── Stat grid helpers ────────────────────────────────────────────────────
  // v18.9.9.470: 2-column UI_12 grid for global All Books view.
  // v18.9.9.475: added drawStatRow (variable cells-per-row) so page 0's
  // per-book view can use 3/3/4 layout (10 stats without vertical clipping).
  constexpr int gridColumns = 2;
  const int gridFullColW = contentW / gridColumns;
  constexpr int gridValueLabelGap = 5;
  constexpr int gridRowPadTop = 6;
  constexpr int gridRowPadBottom = 6;
  const int gridRowH = gridRowPadTop + ui12Lh + gridValueLabelGap + smallLh + gridRowPadBottom;
  // 4-cell rows use UI_10 for the value so long strings ("1h 5 min",
  // "Jul 21") don't get clipped by the ~112-px cell on X4.
  const int valueLhSmallRow = renderer.getLineHeight(UI_10_FONT_ID);
  const int gridRowHSmall = gridRowPadTop + valueLhSmallRow + gridValueLabelGap + smallLh + gridRowPadBottom;

  auto drawStatRow = [&](int rowY, int cellsInRow,
                         std::initializer_list<std::pair<const char*, const char*>> cells) -> int {
    const int cellW = contentW / cellsInRow;
    const int fontId = cellsInRow >= 4 ? UI_10_FONT_ID : UI_12_FONT_ID;
    const int useValueLh = cellsInRow >= 4 ? valueLhSmallRow : ui12Lh;
    const int useRowH = cellsInRow >= 4 ? gridRowHSmall : gridRowH;
    int i = 0;
    for (const auto& kv : cells) {
      const char* label = kv.first;
      const char* value = kv.second;
      const int cellX = contentX + i * cellW;
      const int valueY = rowY + gridRowPadTop;
      const int labelY = valueY + useValueLh + gridValueLabelGap;
      const int vw = renderer.getTextWidth(fontId, value, EpdFontFamily::BOLD);
      renderer.drawText(fontId, cellX + (cellW - vw) / 2, valueY, value, true, EpdFontFamily::BOLD);
      const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, cellX + (cellW - lw) / 2, labelY, label, true);
      ++i;
    }
    return rowY + useRowH;
  };

  auto drawStatGrid = [&](int gridY, std::initializer_list<std::pair<const char*, const char*>> stats) -> int {
    const int total = static_cast<int>(stats.size());
    const int totalRows = (total + gridColumns - 1) / gridColumns;
    int i = 0;
    for (const auto& kv : stats) {
      const char* label = kv.first;
      const char* value = kv.second;
      const int col = i % gridColumns;
      const int row = i / gridColumns;

      int cellsInRow = gridColumns;
      if (row == totalRows - 1) cellsInRow = total - row * gridColumns;
      const int rowOffsetX = (contentW - cellsInRow * gridFullColW) / 2;
      const int cellW = gridFullColW;

      const int cellX = contentX + rowOffsetX + col * cellW;
      const int cellY = gridY + row * gridRowH;
      const int valueY = cellY + gridRowPadTop;
      const int labelY = valueY + ui12Lh + gridValueLabelGap;

      const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, cellX + (cellW - vw) / 2, valueY, value, true, EpdFontFamily::BOLD);

      const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, cellX + (cellW - lw) / 2, labelY, label, true);
      ++i;
    }
    return gridY + totalRows * gridRowH;
  };

  // ─── Section 2 (continued): stat grid, source per showAllBooks_ ─────────
  char buf[10][24];  // shared scratch for cell values (max 10 cells)
  if (!showAllBooks_) {
    // Per-book stats — 10 cells in a 2×5 grid, covers everything
    // stats_v5.bin exposes:
    //   Reading Time, Time Left, Progress, Pages Turned, Sessions,
    //   Avg Session, Pages/Min, Daily Avg, Started, Est/Finished date.
    BookReadingStats::formatDuration(currentStats.totalReadingSeconds, buf[0], sizeof(buf[0]));

    {
      uint32_t timeLeftSecs = 0;
      if (estimatedTimeLeftFor(currentStats, currentProgressPercent_, timeLeftSecs)) {
        BookReadingStats::formatDuration(timeLeftSecs, buf[1], sizeof(buf[1]));
      } else {
        snprintf(buf[1], sizeof(buf[1]), "-");
      }
    }

    if (currentProgressPercent_ >= 0.0f) {
      snprintf(buf[2], sizeof(buf[2]), "%d%%", static_cast<int>(currentProgressPercent_ + 0.5f));
    } else {
      snprintf(buf[2], sizeof(buf[2]), "-");
    }

    snprintf(buf[3], sizeof(buf[3]), "%lu", static_cast<unsigned long>(currentStats.totalPagesTurned));

    snprintf(buf[4], sizeof(buf[4]), "%u", static_cast<unsigned>(currentStats.sessionCount));

    {
      const uint32_t avgSecs =
          currentStats.sessionCount > 0 ? currentStats.totalReadingSeconds / currentStats.sessionCount : 0;
      BookReadingStats::formatDuration(avgSecs, buf[5], sizeof(buf[5]));
    }

    if (currentStats.totalReadingSeconds > 60) {
      const float ppm = static_cast<float>(currentStats.totalPagesTurned) * 60.0f /
                        static_cast<float>(currentStats.totalReadingSeconds);
      snprintf(buf[6], sizeof(buf[6]), "%.1f", ppm);
    } else {
      snprintf(buf[6], sizeof(buf[6]), "0.0");
    }

    ReadingStatsDateTime todayDT;
    const bool hasToday = getCurrentLocalReadingStatsDateTime(todayDT);
    if (currentStats.startDate.isValid() && hasToday && currentStats.totalReadingSeconds > 0) {
      const uint16_t days = std::max<uint16_t>(1, readingSpanDaysElapsed(currentStats.startDate, todayDT.date));
      BookReadingStats::formatDuration(currentStats.totalReadingSeconds / days, buf[7], sizeof(buf[7]));
    } else {
      snprintf(buf[7], sizeof(buf[7]), "-");
    }

    if (currentStats.startDate.isValid()) {
      formatReadingStatsShortDate(currentStats.startDate, buf[8], sizeof(buf[8]));
    } else {
      snprintf(buf[8], sizeof(buf[8]), "-");
    }

    {
      ReadingStatsDate finishDate;
      bool haveDate = false;
      if (currentStats.isCompleted && currentStats.finishDate.isValid()) {
        finishDate = currentStats.finishDate;
        haveDate = true;
      } else if (hasToday) {
        uint32_t est = 0;
        if (estimatedTimeLeftFor(currentStats, currentProgressPercent_, est)) {
          if (estimateFinishDateFromDailyPace(currentStats, todayDT, est, finishDate)) {
            haveDate = true;
          }
        }
      }
      if (haveDate) {
        formatReadingStatsShortDate(finishDate, buf[9], sizeof(buf[9]));
      } else {
        snprintf(buf[9], sizeof(buf[9]), "-");
      }
    }

    // 3 / 3 / 4 layout — top two rows are the "core" 6 stats (matches
    // X4 Dashboard's stat set) with big UI_12 values; last row is the
    // 4 secondary date/count stats in UI_10 so they fit without clipping.
    y = drawStatRow(y, 3,
                    {
                        {tr(STR_STATS_TIME_LBL), buf[0]},
                        {tr(STR_TIME_LEFT), buf[1]},
                        {tr(STR_STATS_PROGRESS_LBL), buf[2]},
                    });
    y = drawStatRow(y, 3,
                    {
                        {tr(STR_STATS_SESSIONS_LBL), buf[4]},
                        {tr(STR_STATS_AVG_SESSION_LBL), buf[5]},
                        {tr(STR_STATS_PAGES_PER_MIN), buf[6]},
                    });
    y = drawStatRow(y, 4,
                    {
                        {tr(STR_STATS_PAGES_LBL), buf[3]},
                        {tr(STR_STATS_DAILY_AVG_LBL), buf[7]},
                        {tr(STR_STATS_STARTED), buf[8]},
                        {currentStats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE),
                         buf[9]},
                    });
  } else {
    // All Books aggregate — same 2-col grid. Title slot above already
    // says "All Books" (v18.9.9.471) so no separate heading here.
    snprintf(buf[0], sizeof(buf[0]), "%lu", static_cast<unsigned long>(globalStats.totalSessions));
    BookReadingStats::formatDuration(globalStats.totalReadingSeconds, buf[1], sizeof(buf[1]));
    snprintf(buf[2], sizeof(buf[2]), "%lu", static_cast<unsigned long>(globalStats.totalPagesTurned));
    {
      const uint32_t globalAvgSecs =
          globalStats.totalSessions > 0 ? globalStats.totalReadingSeconds / globalStats.totalSessions : 0;
      BookReadingStats::formatDuration(globalAvgSecs, buf[3], sizeof(buf[3]));
    }
    if (globalStats.totalReadingSeconds > 60) {
      const float gppm = static_cast<float>(globalStats.totalPagesTurned) * 60.0f /
                         static_cast<float>(globalStats.totalReadingSeconds);
      snprintf(buf[4], sizeof(buf[4]), "%.1f", gppm);
    } else {
      snprintf(buf[4], sizeof(buf[4]), "0.0");
    }
    snprintf(buf[5], sizeof(buf[5]), "%lu", static_cast<unsigned long>(globalStats.completedBooks));

    // v18.9.9.476: current + longest streak from the 730-day bitfield.
    ReadingStatsDateTime streakToday;
    const bool haveClock = getCurrentLocalReadingStatsDateTime(streakToday);
    const uint16_t curStreak = computeReadingHistoryCurrentStreak(
        globalStats.readingHistoryAnchorDay, globalStats.readingHistoryBits,
        haveClock ? &streakToday.date : nullptr);
    const uint16_t longestStreak = std::max(globalStats.longestReadingStreak, curStreak);
    snprintf(buf[6], sizeof(buf[6]), "%u", static_cast<unsigned>(curStreak));
    snprintf(buf[7], sizeof(buf[7]), "%u", static_cast<unsigned>(longestStreak));

    // v18.9.9.201: Books Started — recent books with any recorded reading.
    // (Scoped to the recents list; there's no global started-counter in
    // the stats file, and recents covers every actively-read book.)
    int booksStarted = 0;
    for (const auto& e : nav) {
      if (e.stats.sessionCount > 0 || e.stats.startDate.isValid()) ++booksStarted;
    }
    snprintf(buf[8], sizeof(buf[8]), "%d", booksStarted);

    // Reader Type — dominant time-of-day bucket across all reading.
    const char* readerType = tr(STR_STATS_NEW_READER);
    {
      const auto& todAll = globalStats.timeOfDaySeconds;
      uint32_t total = 0;
      size_t dominant = 0;
      for (size_t i = 0; i < todAll.size(); ++i) {
        total += todAll[i];
        if (todAll[i] > todAll[dominant]) dominant = i;
      }
      if (total > 0) {
        switch (static_cast<ReadingTimeBucket>(dominant)) {
          case ReadingTimeBucket::Morning: readerType = tr(STR_STATS_MORNING_READER); break;
          case ReadingTimeBucket::Afternoon: readerType = tr(STR_STATS_AFTERNOON_READER); break;
          case ReadingTimeBucket::Evening: readerType = tr(STR_STATS_EVENING_READER); break;
          case ReadingTimeBucket::Night:
          default: readerType = tr(STR_STATS_NIGHT_READER); break;
        }
      }
    }

    drawStatGrid(y, {
                        {tr(STR_STATS_SESSIONS_LBL), buf[0]},
                        {tr(STR_STATS_TIME_LBL), buf[1]},
                        {tr(STR_STATS_PAGES_LBL), buf[2]},
                        {tr(STR_STATS_AVG_SESSION_LBL), buf[3]},
                        {tr(STR_STATS_PAGES_PER_MIN), buf[4]},
                        {tr(STR_STATS_COMPLETED_LBL), buf[5]},
                        {tr(STR_STATS_CURRENT_STREAK_LBL), buf[6]},
                        {tr(STR_STATS_LONGEST_STREAK_LBL), buf[7]},
                        {tr(STR_STATS_BOOKS_STARTED_LBL), buf[8]},
                        {tr(STR_STATS_READER_TYPE_LBL), readerType},
                    });
  }

  // ─── Page indicator dots ─────────────────────────────────────────────────
  // 2 squares: filled = current page, outlined = the other. Centered
  // above the button hints (side rockers have no slot in the hint strip).
  {
    constexpr int kDotSize = 8;
    constexpr int kDotGap = 8;
    constexpr int kTotalPages = 2;
    const int clusterW = kTotalPages * kDotSize + (kTotalPages - 1) * kDotGap;
    const int startX = (screenWidth - clusterW) / 2;
    const int dotY = screenHeight - metrics.buttonHintsHeight - kDotSize - 8;
    for (int i = 0; i < kTotalPages; ++i) {
      const int dx = startX + i * (kDotSize + kDotGap);
      if (i == currentPage_) {
        renderer.fillRect(dx, dotY, kDotSize, kDotSize, true);
      } else {
        renderer.drawRect(dx, dotY, kDotSize, kDotSize, true);
      }
    }
  }

  // ─── Button hints ────────────────────────────────────────────────────────
  // Confirm = Toggle (This Book ↔ All Books). L/R cycle books only in
  // single-book mode.
  const bool booksNavigable = !showAllBooks_ && nav.size() > 1;
  const char* prevLbl = booksNavigable ? tr(STR_DIR_PREV) : "";
  const char* nextLbl = booksNavigable ? tr(STR_DIR_NEXT) : "";
  const auto labels = mappedInput.mapLabels(backToHome ? tr(STR_HOME) : tr(STR_BACK), tr(STR_TOGGLE), prevLbl, nextLbl);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
