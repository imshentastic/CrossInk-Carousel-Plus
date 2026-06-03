#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// CrumBLE #81: full-screen list modal for picking which collection's
// books to view in the Bookshelf grid. Triggered by long-press on the
// home icon-bar's Bookshelf entry. Visual style mirrors
// ReaderOptionsActivity (header + scrollable list + button hints) so
// the two modals feel like siblings instead of a popup-vs-list mix.
//
// Result: ChoicePromptResult { choice }. choice == -1 means Back/Cancel;
// choice >= 0 indexes into the labels vector the caller passed in.
//
// CrumBLE #133: an extra "Layout" row is appended below the collection
// list with a divider line above it. Confirm-on-layout cycles
// SETTINGS.bookshelfLayout (3x3 -> 4x4 -> 2x2 -> 3x3) WITHOUT closing
// the picker -- the user can keep cycling until they like the value,
// then either Confirm on a collection (open it) or Back (close).
class BookshelfPickerActivity final : public Activity {
 public:
  // labels: collection names to show in the list (1:1 with caller-side
  // id table; the caller maps choice back to a collection id).
  // currentIndex: which row to mark as currently-active with the
  // "Selected" trailing label, mirroring FontSelectionActivity. Pass -1
  // if none should be marked.
  BookshelfPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<std::string> labels,
                          int currentIndex);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  ButtonNavigator buttonNavigator_;
  std::vector<std::string> labels_;
  int currentIndex_ = -1;
  int selectedIndex_ = 0;
  // The picker is spawned from a long-press of Confirm. Without this
  // gate, the eventual Confirm release would immediately re-fire as a
  // "select" on whatever row currentIndex_ landed on. Mirrors
  // ChoicePromptActivity's ignoreConfirmRelease_.
  bool ignoreConfirmRelease_ = true;
  // CrumBLE #133 (+ follow-up): virtual rows at the bottom of the
  // list, in order:
  //   collections...
  //   [Layout: 3x3/4x4/2x2]      <- layoutRowIndex()
  //   [Title Placement: Top/Bot] <- titlePlacementRowIndex()
  // Confirm on either virtual row cycles its value in-place; doesn't
  // close the picker. A single divider line is drawn above the
  // virtual rows -- they read as a related "view options" cluster.
  int layoutRowIndex() const { return static_cast<int>(labels_.size()); }
  int titlePlacementRowIndex() const { return static_cast<int>(labels_.size()) + 1; }
  int totalRows() const { return static_cast<int>(labels_.size()) + 2; }
  // Localised current-layout label ("3x3" / "4x4" / "2x2"). Reads
  // SETTINGS at call time so the value refreshes as the user cycles.
  std::string currentLayoutLabel() const;
  std::string currentTitlePlacementLabel() const;
  // Cycles 3x3 -> 4x4 -> 2x2 -> 3x3 and saves SETTINGS.
  void cycleLayout();
  // Cycles Bottom -> Top -> Bottom and saves SETTINGS.
  void cycleTitlePlacement();
};
