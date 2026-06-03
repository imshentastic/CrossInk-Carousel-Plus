#include "BookshelfPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <utility>

#include "../ActivityResult.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

BookshelfPickerActivity::BookshelfPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::vector<std::string> labels, int currentIndex)
    : Activity("BookshelfPicker", renderer, mappedInput),
      labels_(std::move(labels)),
      currentIndex_(currentIndex) {}

void BookshelfPickerActivity::onEnter() {
  Activity::onEnter();
  // Land the cursor on the currently-active collection when possible so
  // long-press -> Confirm is a no-op that closes the picker (matches the
  // FontSelectionActivity affordance).
  selectedIndex_ =
      (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(labels_.size())) ? currentIndex_ : 0;
  requestUpdate();
}

void BookshelfPickerActivity::onExit() { Activity::onExit(); }

std::string BookshelfPickerActivity::currentLayoutLabel() const {
  switch (SETTINGS.bookshelfLayout) {
    case CrossPointSettings::BOOKSHELF_LAYOUT_4X4:
      return tr(STR_LAYOUT_4X4);
    case CrossPointSettings::BOOKSHELF_LAYOUT_2X2:
      return tr(STR_LAYOUT_2X2);
    case CrossPointSettings::BOOKSHELF_LAYOUT_3X3:
    default:
      return tr(STR_LAYOUT_3X3);
  }
}

void BookshelfPickerActivity::cycleLayout() {
  // 3x3 -> 4x4 -> 2x2 -> 3x3. Saves immediately so the user sees the
  // new layout the next time they return to the grid even after a
  // power cycle / sleep, without requiring an explicit "save" press.
  uint8_t next = SETTINGS.bookshelfLayout + 1;
  if (next >= CrossPointSettings::BOOKSHELF_LAYOUT_COUNT) next = 0;
  SETTINGS.bookshelfLayout = next;
  SETTINGS.saveToFile();
}

std::string BookshelfPickerActivity::currentTitlePlacementLabel() const {
  switch (SETTINGS.bookshelfTitlePlacement) {
    case CrossPointSettings::BOOKSHELF_TITLE_PLACEMENT_TOP:
      return tr(STR_PLACEMENT_TOP);
    case CrossPointSettings::BOOKSHELF_TITLE_PLACEMENT_BOTTOM:
    default:
      return tr(STR_PLACEMENT_BOTTOM);
  }
}

void BookshelfPickerActivity::cycleTitlePlacement() {
  // Bottom -> Top -> Bottom. Same persistence model as cycleLayout.
  uint8_t next = SETTINGS.bookshelfTitlePlacement + 1;
  if (next >= CrossPointSettings::BOOKSHELF_TITLE_PLACEMENT_COUNT) next = 0;
  SETTINGS.bookshelfTitlePlacement = next;
  SETTINGS.saveToFile();
}

void BookshelfPickerActivity::loop() {
  using B = MappedInputManager::Button;

  // Swallow the Confirm release that follows the long-press that opened
  // the picker, so it doesn't immediately trigger "select" on the row
  // currentIndex_ landed on. After the held Confirm has been released
  // (or was never held at the moment of entry), the gate falls open.
  if (ignoreConfirmRelease_) {
    if (mappedInput.wasReleased(B::Confirm)) {
      ignoreConfirmRelease_ = false;
      return;
    }
    if (!mappedInput.isPressed(B::Confirm)) {
      ignoreConfirmRelease_ = false;
    }
  }

  const int listSize = totalRows();
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  if (mappedInput.wasReleased(B::Confirm)) {
    // CrumBLE #133: virtual rows stay in the picker -- cycle the
    // value and redraw so the user can keep tapping to find the
    // layout / placement they want, then either pick a collection
    // (Confirm on a collection row) or close (Back).
    if (selectedIndex_ == layoutRowIndex()) {
      cycleLayout();
      requestUpdate();
      return;
    }
    if (selectedIndex_ == titlePlacementRowIndex()) {
      cycleTitlePlacement();
      requestUpdate();
      return;
    }
    ChoicePromptResult res;
    res.choice = selectedIndex_;
    setResult(ActivityResult{res});
    finish();
    return;
  }

  if (mappedInput.wasReleased(B::Back)) {
    ChoicePromptResult res;
    res.choice = -1;  // Back = cancel sentinel
    ActivityResult ar;
    ar.isCancelled = true;
    ar.data = res;
    setResult(std::move(ar));
    finish();
    return;
  }
}

void BookshelfPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOKSHELF));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // CrumBLE #133 (+ follow-up): the last TWO items in the list are
  // the Layout and Title Placement virtual rows. Their rowValue
  // shows the current setting; collection rows keep their
  // "Selected" trailing where applicable. A horizontal divider is
  // rendered after drawList between the last collection and the
  // first virtual row.
  const int layoutIdx = layoutRowIndex();
  const int titleIdx = titlePlacementRowIndex();
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalRows(), selectedIndex_,
      [this, layoutIdx, titleIdx](int index) -> std::string {
        if (index == layoutIdx) return tr(STR_BOOKSHELF_LAYOUT);
        if (index == titleIdx) return tr(STR_BOOKSHELF_TITLE_PLACEMENT);
        return labels_[index];
      },
      nullptr, nullptr,
      [this, layoutIdx, titleIdx](int index) -> std::string {
        if (index == layoutIdx) return currentLayoutLabel();
        if (index == titleIdx) return currentTitlePlacementLabel();
        return index == currentIndex_ ? tr(STR_SELECTED) : "";
      },
      true);

  // Divider between the last collection and the Layout row. Uses the
  // shared listRowHeight (LyraTheme = 36); RoundedRaff/MinimalTheme
  // override drawList but inherit the same row stride for picker-style
  // lists, so this Y math holds across themes. Skip when the
  // collection list is empty (no rows above to separate from).
  if (!labels_.empty()) {
    constexpr int kListRowHeight = 36;
    const int dividerY = contentTop + static_cast<int>(labels_.size()) * kListRowHeight;
    if (dividerY >= contentTop && dividerY < contentTop + contentHeight) {
      renderer.drawLine(0, dividerY, pageWidth, dividerY, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
