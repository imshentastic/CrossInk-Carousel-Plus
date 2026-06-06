#include "EpubReaderBookmarkListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "QuoteViewerActivity.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// CrumBLE phase 6: rows show chapter + progress + up to 3 lines of
// highlight preview, wrapping the preview text to the row width. 96 px
// gives ~16 px per body line plus the chapter/% header at the top --
// enough that long highlights communicate their content at a glance.
// Rows without a preview keep the chapter/% on a tighter footprint by
// drawing the same row but skipping the preview block; visually that's
// just a slightly emptier card and is intentional.
static constexpr int ROW_HEIGHT = 96;
static constexpr int PREVIEW_MAX_LINES = 3;
static constexpr int LIST_START_Y = 60;
static constexpr unsigned long BOOKMARK_DELETE_HOLD_MS = 1000;

int EpubReaderBookmarkListActivity::getPageItems() const {
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = LIST_START_Y + hintGutterHeight;
  const int available = renderer.getScreenHeight() - startY - ROW_HEIGHT;
  return std::max(1, available / ROW_HEIGHT);
}

void EpubReaderBookmarkListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderBookmarkListActivity::onExit() { Activity::onExit(); }

void EpubReaderBookmarkListActivity::deleteSelectedBookmark() {
  if (bookmarks.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(bookmarks.size())) return;

  if (!BOOKMARKS.removeBookmarkAt(static_cast<size_t>(selectedIndex))) return;

  bookmarks = BOOKMARKS.getBookmarks();
  if (bookmarks.empty()) {
    selectedIndex = 0;
  } else if (selectedIndex >= static_cast<int>(bookmarks.size())) {
    selectedIndex = static_cast<int>(bookmarks.size()) - 1;
  }
  requestUpdate();
}

void EpubReaderBookmarkListActivity::showBookmarkActionMenu(bool ignoreInitialConfirmRelease) {
  if (bookmarks.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(bookmarks.size())) return;

  const Bookmark selectedBookmark = bookmarks[selectedIndex];
  const char* chapter = (selectedBookmark.chapterTitle[0] != '\0') ? selectedBookmark.chapterTitle : tr(STR_BOOKMARKS);
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(1);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, chapter, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, selectedBookmark](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult || static_cast<FileBrowserAction>(actionResult->action) != FileBrowserAction::Delete) {
          requestUpdate();
          return;
        }

        const auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&selectedBookmark](const Bookmark& bm) {
          return bm.spineIndex == selectedBookmark.spineIndex && bm.progress == selectedBookmark.progress;
        });
        if (it != bookmarks.end()) {
          selectedIndex = static_cast<int>(std::distance(bookmarks.begin(), it));
          deleteSelectedBookmark();
        } else {
          requestUpdate();
        }
      });
}

void EpubReaderBookmarkListActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (!bookmarks.empty() && !longPressConfirmHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= BOOKMARK_DELETE_HOLD_MS) {
    longPressConfirmHandled = true;
    showBookmarkActionMenu(true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressConfirmHandled) {
      longPressConfirmHandled = false;
      return;
    }
    if (!bookmarks.empty() && selectedIndex >= 0 && selectedIndex < static_cast<int>(bookmarks.size())) {
      // CrumBLE 4.2: Confirm opens the QuoteViewer over this bookmark
      // instead of jumping straight into the book. The viewer's Confirm
      // path returns a BookmarkResult that we forward up to our own
      // caller (the reader) -- net effect from the reader's perspective
      // is identical to the old direct-jump UX, with one extra screen
      // in between. The viewer's Back path returns a
      // QuoteViewerExitResult carrying the index the user was last
      // viewing; we restore selectedIndex to that so the list cursor
      // tracks what they were reading.
      startActivityForResult(
          std::make_unique<QuoteViewerActivity>(renderer, mappedInput, bookmarks, selectedIndex),
          [this](const ActivityResult& viewerResult) {
            if (!viewerResult.isCancelled) {
              // Confirm in viewer = jump to bookmark. Forward verbatim
              // so EpubReaderActivity's existing bookmark-list callback
              // sees the same BookmarkResult shape it always did.
              setResult(ActivityResult{viewerResult.data});
              finish();
              return;
            }
            // Back from viewer: update cursor to the last-viewed
            // bookmark and re-render the list. QuoteViewerExitResult is
            // the expected payload but we defend against missing data
            // (e.g. viewer aborted on an empty bookmark list, which
            // shouldn't happen but is defined to send an empty exit
            // result regardless).
            if (const auto* exit = std::get_if<QuoteViewerExitResult>(&viewerResult.data)) {
              if (exit->currentIndex >= 0 && exit->currentIndex < static_cast<int>(bookmarks.size())) {
                selectedIndex = exit->currentIndex;
              }
            }
            requestUpdate();
          });
    }
    return;
  }

  const int total = static_cast<int>(bookmarks.size());
  if (total == 0) return;

  const int pageItems = getPageItems();

  // CrumBLE: any navigation collapses the expanded row so the user
  // doesn't lose their place when scrolling away from it.
  auto collapseExpanded = [this]() { expandedIndex_ = -1; };

  buttonNavigator.onNextRelease([this, total, collapseExpanded] {
    collapseExpanded();
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, total, collapseExpanded] {
    collapseExpanded();
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, total, pageItems, collapseExpanded] {
    collapseExpanded();
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, total, pageItems, collapseExpanded] {
    collapseExpanded();
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
}

int EpubReaderBookmarkListActivity::previewLineCount(const Bookmark& bm, int contentWidth) const {
  if (bm.preview.empty()) return 0;
  const int previewMaxW = contentWidth - 40;
  // Use a generous cap (32 lines) so we get the true wrap count for
  // really long quotes -- the cap exists to bound the temp vector.
  auto lines = renderer.wrappedText(SMALL_FONT_ID, bm.preview.c_str(), previewMaxW, 32);
  return static_cast<int>(lines.size());
}

void EpubReaderBookmarkListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  if (bookmarks.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, LIST_START_Y + contentY + 20, tr(STR_NO_BOOKMARKS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  const int pageItems = getPageItems();
  const int total = static_cast<int>(bookmarks.size());
  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
  const int marginLeft = contentX + 20;
  const int previewMaxW = contentWidth - 40;
  const int previewLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int listStartY = LIST_START_Y + contentY;
  const int bottomReserve = ROW_HEIGHT;  // matches getPageItems() reservation
  const int listMaxY = renderer.getScreenHeight() - bottomReserve;

  int rowY = listStartY;
  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= total) break;
    if (rowY >= listMaxY) break;

    const bool isSelected = (itemIndex == selectedIndex);
    const Bookmark& bm = bookmarks[itemIndex];

    // CrumBLE: size this row dynamically. When the row is expanded
    // (first Confirm on a row whose preview overflows), grow it to
    // fit the full wrapped preview instead of capping at
    // PREVIEW_MAX_LINES. Other rows stay at the regular ROW_HEIGHT.
    const bool expanded = (itemIndex == expandedIndex_);
    std::vector<std::string> previewLines;
    if (!bm.preview.empty()) {
      // CrumBLE 4.2: with BOOKMARK_PREVIEW_MAX bumped from 160 -> 1024 for
      // the QuoteViewer, running wrappedText over the full preview string
      // for every visible row turned list scrolling visibly laggy. The
      // collapsed row only displays PREVIEW_MAX_LINES (3) regardless of
      // the wrap result, so for the non-expanded path we feed wrappedText
      // a trimmed copy of the preview just long enough to fill those
      // lines plus the overflow probe (PREVIEW_MAX_LINES + 1). ~200 chars
      // wraps to ~6-8 lines at SMALL_FONT_ID on both panels we ship, so
      // 4-line overflow probe still works correctly. Expanded rows keep
      // the full preview because the user explicitly asked to see all of
      // it.
      if (expanded) {
        previewLines = renderer.wrappedText(SMALL_FONT_ID, bm.preview.c_str(), previewMaxW, 32);
      } else {
        constexpr size_t kCollapsedScanCap = 200;
        char trimmed[kCollapsedScanCap + 1];
        const size_t copyLen = std::min(kCollapsedScanCap, bm.preview.size());
        std::memcpy(trimmed, bm.preview.data(), copyLen);
        trimmed[copyLen] = '\0';
        previewLines = renderer.wrappedText(SMALL_FONT_ID, trimmed, previewMaxW, PREVIEW_MAX_LINES + 1);
      }
    }
    int rowH = ROW_HEIGHT;
    if (expanded) {
      // metadata (6+lineH) + small gap (4) + N preview lines + bottom pad (6)
      const int previewLinesShown = static_cast<int>(previewLines.size());
      const int contentH = 6 + previewLineH + 4 + previewLinesShown * previewLineH + 6;
      rowH = std::max(ROW_HEIGHT, contentH);
    }
    // Clip the last visible row's bottom to the list area so we don't
    // bleed into the hint strip.
    if (rowY + rowH > listMaxY) rowH = listMaxY - rowY;
    if (rowH <= 0) break;

    if (isSelected) {
      renderer.fillRect(contentX, rowY, contentWidth - 1, rowH, true);
    }

    const char* chapter = (bm.chapterTitle[0] != '\0') ? bm.chapterTitle : tr(STR_UNKNOWN_CHAPTER);
    char metaBuf[BOOKMARK_CHAPTER_TITLE_MAX + 16];
    snprintf(metaBuf, sizeof(metaBuf), "%s  -  %d%%", chapter,
             static_cast<int>(std::lround(bm.progress * 100.0)));
    const std::string metaTrunc = renderer.truncatedText(SMALL_FONT_ID, metaBuf, previewMaxW);
    renderer.drawText(SMALL_FONT_ID, marginLeft, rowY + 6, metaTrunc.c_str(), !isSelected);

    if (!previewLines.empty()) {
      int yCursor = rowY + 6 + previewLineH + 4;
      const int maxLines = expanded ? static_cast<int>(previewLines.size()) : PREVIEW_MAX_LINES;
      const int linesToDraw = std::min<int>(previewLines.size(), maxLines);
      for (int li = 0; li < linesToDraw; ++li) {
        std::string line = previewLines[li];
        // Ellipsize the last visible line only when not expanded AND
        // there's more underneath (the expansion path is meant to
        // SHOW everything, so we never truncate when expanded).
        if (!expanded && li == PREVIEW_MAX_LINES - 1 &&
            static_cast<int>(previewLines.size()) > PREVIEW_MAX_LINES) {
          line = renderer.truncatedText(SMALL_FONT_ID, (line + "...").c_str(), previewMaxW,
                                        EpdFontFamily::ITALIC);
        }
        if (yCursor + previewLineH > rowY + rowH) break;  // clip if row got shrunk
        renderer.drawText(SMALL_FONT_ID, marginLeft, yCursor, line.c_str(), !isSelected,
                          EpdFontFamily::ITALIC);
        yCursor += previewLineH;
      }
    }
    rowY += rowH;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
