#include "EpubReaderBookmarkListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
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
      setResult(BookmarkResult{bookmarks[selectedIndex].spineIndex, bookmarks[selectedIndex].progress});
      finish();
    }
    return;
  }

  const int total = static_cast<int>(bookmarks.size());
  if (total == 0) return;

  const int pageItems = getPageItems();

  buttonNavigator.onNextRelease([this, total] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, total] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
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

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= total) break;

    const int rowY = LIST_START_Y + contentY + i * ROW_HEIGHT;
    const bool isSelected = (itemIndex == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, rowY, contentWidth - 1, ROW_HEIGHT, true);
    }

    const Bookmark& bm = bookmarks[itemIndex];
    const char* chapter = (bm.chapterTitle[0] != '\0') ? bm.chapterTitle : tr(STR_UNKNOWN_CHAPTER);

    // CrumBLE: preview-first layout. Stack order top -> bottom:
    //   line 1..N: italic preview (up to PREVIEW_MAX_LINES, wraps)
    //   bottom row: "<Chapter title>  -  NN%"  (single line)
    // Order swapped from the previous chapter-first layout so the user
    // scans the actual quote at-a-glance and the metadata sits as a
    // subtle caption underneath -- matches how the saved book progress
    // dot reads on the home shelf.
    const int previewMaxW = contentWidth - 40;
    int yCursor = rowY + 6;
    if (bm.preview[0] != '\0') {
      auto previewLines =
          renderer.wrappedText(SMALL_FONT_ID, bm.preview, previewMaxW, PREVIEW_MAX_LINES + 1);
      const int previewLineH = renderer.getLineHeight(SMALL_FONT_ID);
      const int linesToDraw = std::min<int>(previewLines.size(), PREVIEW_MAX_LINES);
      for (int li = 0; li < linesToDraw; ++li) {
        std::string line = previewLines[li];
        if (li == PREVIEW_MAX_LINES - 1 && static_cast<int>(previewLines.size()) > PREVIEW_MAX_LINES) {
          line = renderer.truncatedText(SMALL_FONT_ID, (line + "...").c_str(), previewMaxW,
                                        EpdFontFamily::ITALIC);
        }
        renderer.drawText(SMALL_FONT_ID, marginLeft, yCursor, line.c_str(), !isSelected,
                          EpdFontFamily::ITALIC);
        yCursor += previewLineH;
      }
      yCursor += 4;  // breathing room between preview and metadata
    }

    // Metadata caption: chapter title + percent, pinned near the row's
    // bottom edge so it's always at the same baseline regardless of how
    // many preview lines actually wrapped.
    char metaBuf[BOOKMARK_CHAPTER_TITLE_MAX + 16];
    snprintf(metaBuf, sizeof(metaBuf), "%s  -  %d%%", chapter,
             static_cast<int>(std::lround(bm.progress * 100.0)));
    const std::string metaTrunc = renderer.truncatedText(SMALL_FONT_ID, metaBuf, previewMaxW);
    const int metaY = rowY + ROW_HEIGHT - renderer.getLineHeight(SMALL_FONT_ID) - 6;
    renderer.drawText(SMALL_FONT_ID, marginLeft, metaY, metaTrunc.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
