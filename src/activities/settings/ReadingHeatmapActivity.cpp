#include "ReadingHeatmapActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReadingStats.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Grid geometry. Keep everything in one place so future Home-integration
// can reuse these constants with a different origin.
// v18.9.9.366: widened from 12 weeks (trailing only) to 16 past + 4 future.
// User feedback: heatmap was cramped relative to the page; showing a few
// future weeks to the right feels less "packed to one edge" and gives the
// user visual anchoring for upcoming days. Grid width in pixels:
// 20 * (16 + 2) = 360, still comfortably inside the 464 X4 screen with
// label column + margin.
constexpr int kPastWeeks = 16;
constexpr int kFutureWeeks = 4;
constexpr int kWeeksInView = kPastWeeks + kFutureWeeks;
constexpr int kDaysPerWeek = 7;
constexpr int kCellSize = 16;            // pixels per cell
constexpr int kCellGap = 2;
constexpr int kLegendGap = 20;
// v18.9.9.293: dow labels widened so "Mon"/"Wed"/"Fri" fully clear the
// grid start. Old value (22 px) truncated to "Mor"/"Wec".
constexpr int kLabelWidth = 34;

// Days-of-week labels (Sun-first per epochDayToDow convention).
constexpr const char* kDowLabels[7] = {"", "Mon", "", "Wed", "", "Fri", ""};

// Bucket a minutes value against a per-view max. 4 tones: none / low /
// medium / high. Empty (0 minutes) always maps to the empty bucket so
// unused days read as blank rather than "just barely coloured."
uint8_t bucket(uint16_t minutes, uint16_t viewMax) {
  if (minutes == 0) return 0;
  if (viewMax == 0) return 0;
  const uint32_t scaled = (static_cast<uint32_t>(minutes) * 3 + viewMax - 1) / viewMax;
  return static_cast<uint8_t>(std::min<uint32_t>(3, scaled));
}

// The four cell tones. Chosen to read distinctly on the e-ink panel
// through dither rather than trying to render true grey via subpixel.
Color bucketToColor(uint8_t b) {
  switch (b) {
    case 0: return Color::White;
    case 1: return Color::LightGray;
    case 2: return Color::DarkGray;
    default: return Color::Black;
  }
}

// Human-readable "3h 15m" from a raw minutes count.
std::string formatMinutes(uint32_t m) {
  char buf[32];
  if (m >= 60) {
    snprintf(buf, sizeof(buf), "%uh %um", m / 60, m % 60);
  } else {
    snprintf(buf, sizeof(buf), "%um", m);
  }
  return buf;
}
}  // namespace

void ReadingHeatmapActivity::onEnter() {
  Activity::onEnter();
  // v18.9.9.474: arm terminate-recovery to Home so a mid-render OOM
  // (aggregate load + chart draw against a large reading-days.bin) lands
  // on a clean Home rather than whatever was previously armed.
  armSilentRestartTarget(/*SILENT_REBOOT_TARGET_HOME=*/0);
  requestUpdate();
}

void ReadingHeatmapActivity::onExit() {
  Activity::onExit();
  // v18.9.9.474: clear our terminate-recovery arming.
  clearArmedSilentRestartTarget();
  // v18.9.9.361: silent-restart-to-Home instead of pendingHomeFullRefresh
  // + FULL_REFRESH. Field feedback: multi-flash refresh reads as 3-4
  // separate flashes on X4, while FT's silent-restart-to-Home visually
  // looks like ONE clean transition (framebuffer snapshot restored on
  // boot). Trade ~5-8 s reboot cycle for cleaner visuals; matches FT
  // exit UX. Note: this WIPES current Home focus/state; user lands on
  // default Home.
  silentRestart();
  // never returns
}

void ReadingHeatmapActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ReadingHeatmapActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Header
  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Reading Heatmap");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // v18.9.9.361: render the grid always. Previously, when clock was
  // invalid we short-circuited to a "Device clock not set" message and
  // never showed any grid data. User feedback: heatmap should be viewable
  // fully offline; historical reading data is still there, just no
  // "today" marker or new entries. Fall through to normal render; the
  // legend line at the bottom (STR "clock not set" hint) already
  // communicates the no-time state gracefully.

  // Pull all aggregates and find the visible window.
  const auto aggregates = ReadingStats::loadAggregates();
  uint32_t today = ReadingStats::todayEpochDay();
  // v18.9.9.361: fall back to the latest recorded aggregate when clock is
  // invalid (todayEpochDay returns 0). Prevents the unsigned underflow
  // in firstVisibleEpochDay math (would wrap to ~4B) and lets the
  // historical data anchor on the user's last active day. If there's
  // no data at all AND no clock, use a dummy anchor -- grid renders
  // fully empty which is honest.
  if (today == 0) {
    uint32_t latest = 0;
    for (const auto& agg : aggregates) {
      if (agg.epochDay > latest) latest = agg.epochDay;
    }
    today = latest > 0 ? latest : ((kWeeksInView - 1) * 7 + 6);
  }
  // v18.9.9.366: place today's week towards the right but leave kFutureWeeks
  // of empty columns to the right of it. Days beyond `today` skip render
  // (loop below) so future cells appear as clean whitespace instead of
  // suspicious empty outlines.
  const uint8_t todayDow = ReadingStats::epochDayToDow(today);
  const uint32_t firstVisibleEpochDay = today - todayDow - (kPastWeeks - 1) * 7;

  // Build a dense window array (kWeeksInView * 7) of minutes per day.
  // Uninitialised entries stay 0 = no reading. Sparse source, dense
  // dest -- keeps per-cell lookup at O(1) during draw.
  const int windowDays = kWeeksInView * kDaysPerWeek;
  std::vector<uint16_t> minutes(windowDays, 0);
  std::vector<uint16_t> pages(windowDays, 0);
  uint16_t viewMax = 0;
  uint32_t totalMinutes = 0;
  uint32_t totalPages = 0;
  uint16_t daysWithActivity = 0;
  for (const auto& agg : aggregates) {
    if (agg.epochDay < firstVisibleEpochDay || agg.epochDay > today) continue;
    const int idx = static_cast<int>(agg.epochDay - firstVisibleEpochDay);
    if (idx < 0 || idx >= windowDays) continue;
    minutes[idx] = agg.minutesRead;
    pages[idx] = agg.pagesTurned;
    if (agg.minutesRead > viewMax) viewMax = agg.minutesRead;
    totalMinutes += agg.minutesRead;
    totalPages += agg.pagesTurned;
    if (agg.minutesRead > 0) daysWithActivity++;
  }

  // Grid layout: labels column | 12 week columns
  const int gridX = kLabelWidth + 10;
  const int gridY = contentTop + 40;  // room for summary line
  const int cellStride = kCellSize + kCellGap;

  // Summary line -- v18.9.9.293: use SMALL_FONT_ID so "Last 12 weeks..."
  // fits comfortably above the grid without wrapping or eating the
  // month-labels row.
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "Last %d weeks:  %s across %u days,  %u pages",
             kWeeksInView, formatMinutes(totalMinutes).c_str(), daysWithActivity, totalPages);
    renderer.drawText(SMALL_FONT_ID, kLabelWidth, contentTop + 10, buf);
  }

  // Day-of-week labels down the left column
  for (int d = 0; d < kDaysPerWeek; ++d) {
    if (kDowLabels[d][0] == '\0') continue;
    const int y = gridY + d * cellStride + kCellSize - 4;
    renderer.drawText(SMALL_FONT_ID, 0, y, kDowLabels[d]);
  }

  // The grid itself. Column 0 is the oldest week; column kWeeksInView-1
  // is the current partial week ending on today.
  for (int w = 0; w < kWeeksInView; ++w) {
    for (int d = 0; d < kDaysPerWeek; ++d) {
      const int idx = w * kDaysPerWeek + d;
      const uint32_t cellEpochDay = firstVisibleEpochDay + idx;
      const int cx = gridX + w * cellStride;
      const int cy = gridY + d * cellStride;
      // Days beyond today (last partial week) stay unrendered so users
      // don't see suspicious "future" empty cells with a border.
      if (cellEpochDay > today) continue;
      const uint8_t b = bucket(minutes[idx], viewMax);
      renderer.fillRectDither(cx, cy, kCellSize, kCellSize, bucketToColor(b));
      // Thin outline so empty cells are still visible on white bg.
      // Empty cells stay white; the cell gap makes the grid readable
      // without an outline. Coloured cells self-outline via their fill.
    }
  }

  // Month labels along the top of the grid. Show a label at each week
  // whose first day is in a different month than the previous week's.
  uint8_t prevMonth = 0;
  for (int w = 0; w < kWeeksInView; ++w) {
    const uint32_t weekStart = firstVisibleEpochDay + w * kDaysPerWeek;
    uint16_t yy = 0;
    uint8_t mm = 0, dd = 0;
    ReadingStats::epochDayToYmd(weekStart, yy, mm, dd);
    if (mm != prevMonth) {
      prevMonth = mm;
      static const char* kMonthShort[13] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      const int lx = gridX + w * cellStride;
      renderer.drawText(SMALL_FONT_ID, lx, gridY - 4, kMonthShort[mm]);
    }
  }

  // Legend at the bottom: 4 sample cells with an above-max label so the
  // user knows what "high" corresponds to in absolute minutes.
  const int legendY = gridY + kDaysPerWeek * cellStride + kLegendGap;
  renderer.drawText(SMALL_FONT_ID, kLabelWidth, legendY + kCellSize - 4, "Less");
  const int legendXStart = kLabelWidth + 40;
  for (int b = 0; b < 4; ++b) {
    const int lx = legendXStart + b * cellStride;
    renderer.fillRectDither(lx, legendY, kCellSize, kCellSize, bucketToColor(static_cast<uint8_t>(b)));
    if (b == 0) renderer.drawRect(lx, legendY, kCellSize, kCellSize, true);  // outline for empty legend cell
  }
  const int legendMoreX = legendXStart + 4 * cellStride + 4;
  renderer.drawText(SMALL_FONT_ID, legendMoreX, legendY + kCellSize - 4, "More");
  // v18.9.9.293: always show absolute thresholds so users know what each
  // shade means. Adaptive: bucket boundaries scale with the busiest day
  // in view, so the darkest cell is always the day with the most reading.
  // v18.9.9.366: split the "clock not set" hint onto its own line below
  // the legend so it doesn't run off the right screen edge. The threshold
  // slot only shows "(no data yet)" or the numeric buckets; the multi-word
  // "clock not set - Sync & Network..." message lives on the wrap line.
  {
    char buf[80];
    if (viewMax > 0) {
      snprintf(buf, sizeof(buf), "0 / <%s / <%s / %s+",
               formatMinutes(viewMax / 3).c_str(),
               formatMinutes((viewMax * 2) / 3).c_str(),
               formatMinutes(viewMax).c_str());
      renderer.drawText(SMALL_FONT_ID, legendMoreX + 40, legendY + kCellSize - 4, buf);
    } else if (halClock.hasValidTime()) {
      snprintf(buf, sizeof(buf), "(no data yet)");
      renderer.drawText(SMALL_FONT_ID, legendMoreX + 40, legendY + kCellSize - 4, buf);
    } else {
      // v18.9.9.305: distinguish "haven't read yet" from "can't track because
      // wall-clock isn't set". Field-common on X4 after a cold boot with no
      // WiFi contact -- ReadingStats needs a valid epochDay to attribute
      // sessions to a day, so it silently drops everything until SNTP fires.
      // v18.9.9.366: wrapped onto its own line under the legend.
      renderer.drawText(SMALL_FONT_ID, kLabelWidth, legendY + kCellSize + 12,
                        "Clock not set - Sync & Network > Sync Time");
    }
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
