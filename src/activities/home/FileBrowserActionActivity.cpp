#include "FileBrowserActionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kTitleFontId = UI_10_FONT_ID;
constexpr int kTitleMaxLines = 2;
constexpr int kCompactTitleY = 14;
constexpr int kTallHeaderTitleBottomPadding = 8;
constexpr int kCompactHeaderTitleBottomPadding = 4;
constexpr int kTitleLineGap = 1;
constexpr int kBatteryTextReserveWidth = 90;
}  // namespace

void FileBrowserActionActivity::onEnter() {
  Activity::onEnter();
  // CrumBLE 4.2: -1 is the "Optimized" header focus sentinel; 0..N-1 are
  // regular menu items. We always START on item 0 (most-likely action)
  // even when the Optimized header is interactive -- the user has to
  // explicitly press UP to focus the badge.
  selectedIndex = 0;
  requestUpdate();
}

void FileBrowserActionActivity::loop() {
  // CrumBLE 4.4: the prebake badge header is selectable when this activity
  // was opened with one of the v4.4 tier labels (IMG, IMG+CHAP,
  // IMG+CHAP+CP.FONT). selectedIndex == -1 then means the header is focused
  // -- pressing Confirm fires ViewOptimizedDetails. We match on the
  // leading "IMG" so all tiers gate identically. Other right-label text
  // (sort indicators, etc.) doesn't start with "IMG".
  const bool headerSelectable = !headerRightLabel.empty() &&
                                 headerRightLabel.compare(0, 3, "IMG") == 0;

  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Header focused (sentinel index -1) -- fire the ViewOptimizedDetails
    // sentinel action and finish. The caller's switch dispatches it to
    // PrebakeManifestViewerActivity.
    if (selectedIndex == -1 && headerSelectable) {
      setResult(FileBrowserActionResult{static_cast<int>(FileBrowserAction::ViewOptimizedDetails)});
      finish();
      return;
    }
    // CrumBLE: items wired with inlineToggle run the callback in place and
    // stay in the menu. The row's rightValueGetter will re-evaluate on
    // the next paint so the user sees the change without leaving the menu.
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size()) &&
        items[selectedIndex].inlineToggle) {
      items[selectedIndex].inlineToggle();
      requestUpdate();
      return;
    }
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size())) {
      setResult(FileBrowserActionResult{static_cast<int>(items[selectedIndex].action)});
      finish();
    }
    return;
  }

  buttonNavigator.onNext([this, headerSelectable] {
    const int n = static_cast<int>(items.size());
    if (n == 0) return;
    // Cycle: header (-1) -> 0 -> 1 -> ... -> N-1 -> header -> 0 -> ...
    // When headerSelectable is false, just modulo-wrap N items.
    if (!headerSelectable) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, n);
    } else if (selectedIndex == -1) {
      selectedIndex = 0;
    } else if (selectedIndex == n - 1) {
      selectedIndex = -1;
    } else {
      selectedIndex = selectedIndex + 1;
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, headerSelectable] {
    const int n = static_cast<int>(items.size());
    if (n == 0) return;
    if (!headerSelectable) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, n);
    } else if (selectedIndex == -1) {
      selectedIndex = n - 1;
    } else if (selectedIndex == 0) {
      selectedIndex = -1;
    } else {
      selectedIndex = selectedIndex - 1;
    }
    requestUpdate();
  });
}

void FileBrowserActionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int titleX = metrics.contentSidePadding;
  const int titleMaxWidth = std::max(0, pageWidth - titleX - metrics.contentSidePadding - kBatteryTextReserveWidth);
  // CrumBLE 4.2: when a subtitle is provided (typically the book author)
  // the header switches to a fixed two-line layout: bold title on line 1
  // (truncated, never wrapped), regular subtitle on line 2 (truncated),
  // right-justified label on line 2 so it can't collide with the title
  // text. Without a subtitle the original behavior persists -- title may
  // word-wrap up to two lines and the right label rides on line 1.
  const bool hasSubtitle = !subtitle.empty();
  constexpr int kSubtitleFontId = UI_10_FONT_ID;
  std::vector<std::string> titleLines;
  if (hasSubtitle) {
    titleLines.push_back(renderer.truncatedText(kTitleFontId, title.c_str(), titleMaxWidth));
  } else {
    titleLines =
        renderer.wrappedText(kTitleFontId, title.c_str(), titleMaxWidth, kTitleMaxLines, EpdFontFamily::BOLD);
  }
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int subtitleLineHeight = renderer.getLineHeight(kSubtitleFontId);
  const int totalLineCount = static_cast<int>(titleLines.size()) + (hasSubtitle ? 1 : 0);
  const int titleBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight +
                               (hasSubtitle ? (subtitleLineHeight + kTitleLineGap) : 0) +
                               std::max(0, totalLineCount - 1) * kTitleLineGap -
                               (hasSubtitle ? kTitleLineGap : 0);  // avoid double-counting the gap baked above
  const bool tallHeader = metrics.headerHeight > 60;
  const int titleY = metrics.topPadding + (tallHeader ? metrics.batteryBarHeight + 3 : kCompactTitleY);
  const int titleBottomPadding = tallHeader ? kTallHeaderTitleBottomPadding : kCompactHeaderTitleBottomPadding;
  // CrumBLE 4.4: when the prebake-status badge ("IMG"-prefixed) is present,
  // give it its OWN row below the title block instead of sharing line 1
  // with the title (no-subtitle case) or line 2 with the subtitle. The
  // header's bottom edge -- the visible divider before the action list --
  // moves down accordingly. Without this, long filenames on a never-opened
  // book (where title=filename and there's no author subtitle) get squeezed
  // to roughly half-width because the badge reserves the other half. With
  // it, the title gets full width on every line and the badge sits flush
  // right on its own row above the divider. Other right-labels (e.g. the
  // shelf-header sort-mode label) stay inline -- only the IMG-prefixed
  // prebake badge moves down.
  const bool badgeOnOwnRow = !headerRightLabel.empty() && headerRightLabel.compare(0, 3, "IMG") == 0;
  const int badgeRowHeight = badgeOnOwnRow ? (subtitleLineHeight + kTitleLineGap) : 0;
  const int actionHeaderHeight =
      std::max(metrics.headerHeight,
               titleY - metrics.topPadding + titleBlockHeight + badgeRowHeight + titleBottomPadding);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, actionHeaderHeight}, "");

  for (int i = 0; i < static_cast<int>(titleLines.size()); ++i) {
    renderer.drawText(kTitleFontId, titleX, titleY + i * (titleLineHeight + kTitleLineGap), titleLines[i].c_str(), true,
                      EpdFontFamily::BOLD);
  }
  // Subtitle (author) on the second line in regular weight.
  const int subtitleY = hasSubtitle ? (titleY + titleLineHeight + kTitleLineGap) : titleY;
  if (hasSubtitle) {
    const std::string sub = renderer.truncatedText(kSubtitleFontId, subtitle.c_str(), titleMaxWidth);
    renderer.drawText(kSubtitleFontId, titleX, subtitleY, sub.c_str(), true, EpdFontFamily::REGULAR);
  }

  // CrumBLE: optional secondary label right-justified on the title row
  // (used by the shelf-header menu to surface the current sort mode at
  // a glance, e.g. "Favorites    Title (A-Z)"). Drawn in REGULAR weight
  // and a smaller font so the title still reads as primary. Clamped to
  // the title's reserved area so it never collides with the battery
  // readout on the far right.
  //
  // CrumBLE 4.2: when the label is "Optimized" we paint a small lightning
  // bolt glyph after the text. The bolt is a pair of triangles (sharp
  // points on both ends; no Unicode dependency since none of the slim-
  // build fonts ship U+26A1) sized + vertically centered against the
  // label line.
  //
  // When a subtitle is present, the right label moves to the subtitle
  // line so the long-press menu can show "Book Title" on line 1 without
  // overlap risk and "Author Name                      Optimized" on
  // line 2. Otherwise it rides line 1 as before.
  if (!headerRightLabel.empty()) {
    constexpr int kRightLabelFontId = UI_10_FONT_ID;
    // Match all three v4.4 tiers (IMG, IMG+CHAP, IMG+CHAP+CP.FONT) by the
    // "IMG" prefix -- same gate as headerSelectable above. The bolt glyph
    // is drawn next to the label as the "optimized" indicator, since the
    // device's UI font doesn't render U+2713 ✓.
    const bool drawBolt = !headerRightLabel.empty() &&
                          headerRightLabel.compare(0, 3, "IMG") == 0;
    // CrumBLE 4.2: when the user navigates UP from item 0 onto the
    // header, paint the label inside a filled black box with inverted
    // (white) text so it reads as "selected" like the regular rows.
    const bool headerFocused = drawBolt && (selectedIndex == -1);
    constexpr int kBoltWidth = 8;
    constexpr int kBoltHeight = 13;
    constexpr int kBoltGap = 3;
    const int reserveForBolt = drawBolt ? (kBoltWidth + kBoltGap) : 0;
    // CrumBLE 4.4: when the prebake badge gets its own row (badgeOnOwnRow
    // above), it has the FULL header width to itself -- no battery icon,
    // no title text competing on the same line. Anchor flush right and
    // budget the full width (minus paddings + bolt reserve). Other right-
    // labels (sort mode, etc.) stay inline with the legacy budget split.
    const int rightAnchorX =
        (badgeOnOwnRow || hasSubtitle) ? (pageWidth - metrics.contentSidePadding) : (titleX + titleMaxWidth);
    const int legacyBudget = (hasSubtitle ? (titleMaxWidth * 2 / 3) : (titleMaxWidth / 2)) - reserveForBolt;
    const int ownRowBudget = pageWidth - 2 * metrics.contentSidePadding - reserveForBolt;
    const int textBudget = std::max(0, badgeOnOwnRow ? ownRowBudget : legacyBudget);
    const std::string rightLabel = renderer.truncatedText(kRightLabelFontId, headerRightLabel.c_str(), textBudget);
    const int rw = renderer.getTextWidth(kRightLabelFontId, rightLabel.c_str(), EpdFontFamily::REGULAR);
    // CrumBLE 4.4: bolt moved to the LEFT of the text. The block-as-a-whole
    // (bolt + gap + text) right-aligns to rightAnchorX; the bolt sits
    // immediately to the left of the text. Order on screen left-to-right:
    // [bolt][gap][text]. Matches the FT page's "⚡IMG+CHAP+CP.FONT" layout.
    const int rx = rightAnchorX - rw;
    // labelY: own row sits below the existing title block (one
    // subtitleLineHeight + gap further down). Otherwise legacy behavior:
    // line 2 if there's a subtitle, line 1 otherwise.
    const int titleBlockEndY =
        titleY + static_cast<int>(titleLines.size()) * titleLineHeight +
        std::max(0, static_cast<int>(titleLines.size()) - 1) * kTitleLineGap +
        (hasSubtitle ? (kTitleLineGap + subtitleLineHeight) : 0);
    const int labelY = badgeOnOwnRow ? (titleBlockEndY + kTitleLineGap)
                                     : (hasSubtitle ? subtitleY : titleY);

    // Draw the selection box BEFORE the text so the text/glyph render on
    // top of the filled background. Padding picked to look balanced with
    // the rounded-corner pattern other selected rows use.
    if (headerFocused) {
      constexpr int kPadX = 3;
      constexpr int kPadY = 2;
      // Box covers bolt + gap + text. Bolt sits at (rx - kBoltGap - kBoltWidth).
      const int boxX = rx - reserveForBolt - kPadX;
      const int boxY = labelY - kPadY;
      const int boxW = rw + reserveForBolt + 2 * kPadX;
      const int boxH = subtitleLineHeight + 2 * kPadY;
      renderer.fillRect(boxX, boxY, boxW, boxH, true);
    }
    // Text color inverts when the box is filled black. drawText's
    // `black` flag controls the foreground: true = black ink, false =
    // white knockout against whatever's beneath.
    renderer.drawText(kRightLabelFontId, rx, labelY, rightLabel.c_str(), !headerFocused, EpdFontFamily::REGULAR);

    if (drawBolt) {
      // Two filled triangles forming a Z-shape ⚡. Upper triangle: top
      // apex on upper-right at (bx+7,0), base on the lower-left from
      // (bx+0,7) to (bx+3,7). Lower triangle: top base on upper-right at
      // (bx+4,6) to (bx+7,6) -- offset RIGHT of upper's base so the two
      // form the canonical Z kink at the middle -- and bottom apex on
      // lower-left at (bx+1,13). Sharp points on both ends; no flat
      // edges meeting the bounding box.
      // bx is the LEFT edge of the bolt glyph; sits to the left of text.
      const int bx = rx - kBoltGap - kBoltWidth;
      // Center the bolt vertically against the label's line height. The
      // ratio of (lineHeight - boltHeight) / 2 nudges by half the slack
      // so the glyph reads as a glyph rather than floating to the cap
      // line. Floor instead of round to bias one pixel up, matching the
      // visual feel of text baselines.
      const int slack = std::max(0, subtitleLineHeight - kBoltHeight);
      const int by = labelY + slack / 2;
      // Bolt ink color matches the label's color: black on white normally,
      // white (knockout) on the filled black box when focused.
      const bool boltInk = !headerFocused;
      // Upper triangle: top apex upper-right, base lower-left.
      const int upperX[3] = {bx + 7, bx + 3, bx + 0};
      const int upperY[3] = {by + 0, by + 7, by + 7};
      renderer.fillPolygon(upperX, upperY, 3, boltInk);
      // Lower triangle: top base upper-right (offset RIGHT of upper's
      // base by 4px to make the Z-kink visible), bottom apex lower-left.
      const int lowerX[3] = {bx + 7, bx + 4, bx + 1};
      const int lowerY[3] = {by + 6, by + 6, by + 13};
      renderer.fillPolygon(lowerX, lowerY, 3, boltInk);
    }
  }

  const int contentTop = metrics.topPadding + actionHeaderHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  // CrumBLE: highlightValue=true matches the visual style of the main Settings
  // menu -- the right-justified value box inverts on the selected row -- so
  // the shelf-header toggles read as "real" settings rather than action items
  // with extra text. drawList still falls back to plain text when rowValue
  // returns "", so action-only rows (Rename, Sort by, Rescan) render normally.
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(items.size()), selectedIndex,
               [this](int index) { return std::string(I18N.get(items[index].labelId)); },
               /*rowSubtitle=*/nullptr,
               /*rowIcon=*/nullptr,
               [this](int index) {
                 // Getter wins over static value -- lets rows update live
                 // as inlineToggle flips the underlying state.
                 return items[index].rightValueGetter ? items[index].rightValueGetter() : items[index].rightValue;
               },
               /*highlightValue=*/true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
