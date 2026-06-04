#pragma once

#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarkListActivity final : public Activity {
 public:
  explicit EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::vector<Bookmark>& bookmarks)
      : Activity("EpubReaderBookmarkList", renderer, mappedInput), bookmarks(bookmarks) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  std::vector<Bookmark> bookmarks;
  int selectedIndex = 0;
  bool longPressConfirmHandled = false;
  ButtonNavigator buttonNavigator;
  // CrumBLE: when a row's preview overflows past PREVIEW_MAX_LINES, the
  // first Confirm tap expands the row (still in the list) so the user
  // can read the whole quote in place; the second tap jumps to the
  // book location. Any navigation (Up/Down/Left/Right) collapses it.
  // -1 = none expanded.
  int expandedIndex_ = -1;

  void deleteSelectedBookmark();
  void showBookmarkActionMenu(bool ignoreInitialConfirmRelease = false);
  int getPageItems() const;
  // Returns how many lines the preview text wraps to at the row width.
  // Used both to decide whether expansion is needed and to size the
  // expanded row.
  int previewLineCount(const Bookmark& bm, int contentWidth) const;
};
