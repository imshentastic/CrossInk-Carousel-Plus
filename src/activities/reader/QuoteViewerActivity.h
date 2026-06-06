#pragma once

#include <string>
#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"

// CrumBLE 4.2: full-screen quote viewer launched from
// EpubReaderBookmarkListActivity when the user presses Confirm on a
// bookmark row. Shows the bookmark's preview text wrapped to the screen
// width with chapter context up top.
//
// Navigation:
//   Up/Down   prev / next bookmark in the list (wraparound)
//   Left/Right  prev / next "screen-page" of the current quote (no-op when
//               the preview fits on one screen, which is the common case
//               given BOOKMARK_PREVIEW_MAX = 160 chars; wired up so future
//               work that stores longer quote text doesn't need to revisit
//               the input layer)
//   Confirm   set BookmarkResult{spineIndex, progress} and finish --
//             EpubReaderBookmarkListActivity forwards this to its caller
//             so the reader jumps to the bookmark location in the book
//   Back      set QuoteViewerExitResult{currentIndex} (cancelled) and
//             finish -- the list activity restores its cursor to the
//             bookmark the user was last viewing
//
// The viewer does NOT mutate BookmarkStore. Deletions still go through
// the list's long-press action menu.
class QuoteViewerActivity final : public Activity {
 public:
  QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<Bookmark> bookmarks,
                      int initialIndex);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Recompute wrappedLines + linesPerPage for the bookmark at currentIndex_.
  // Called on onEnter, after every Up/Down navigation, and when the active
  // bookmark changes for any reason.
  void rewrapForCurrent();

  std::vector<Bookmark> bookmarks_;
  int currentIndex_ = 0;
  int pageOffset_ = 0;  // first wrappedLine index on the visible "page"

  std::vector<std::string> wrappedLines_;
  int lineHeight_ = 0;
  int linesPerPage_ = 0;
};
