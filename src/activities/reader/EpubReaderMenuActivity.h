#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "ControlsOptionsActivity.h"
#include "ReaderOptionsActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    READING_STATS,
    TOGGLE_COMPLETED,
    READER_OPTIONS,
    CONTROLS_OPTIONS,
    BOOKMARK_TOGGLE,
    VIEW_BOOKMARKS,
    DELETE_BOOKMARKS,
    BLUETOOTH,
    // CrumBLE: dictionary lookup (port of SEEK reader's feature). LOOKUP
    // opens a word-selection overlay over the current page; LOOKED_UP_
    // WORDS shows the per-book history. Only added to the menu when
    // Dictionary::exists() (StarDict files are on the SD card).
    LOOKUP,
    LOOKED_UP_WORDS,
    // CrumBLE: highlights = ranged bookmarks. ADD_HIGHLIGHT opens the
    // word-select activity in HighlightRange mode; user taps the start
    // word, navigates to the end, taps again. Always available alongside
    // BOOKMARK_TOGGLE -- the latter stays for fast page-marking.
    ADD_HIGHLIGHT,
    // CrumBLE: cross-page / cross-chapter flow. After ADD_HIGHLIGHT's
    // first-word anchor is placed, hitting Back ("Hold") emits an
    // anchor-only result; the reader stores it as pendingHighlightStart_
    // and surfaces FINISH (pick end on whatever later page) and CANCEL.
    // ADD_HIGHLIGHT is hidden when the pending state is held so the menu
    // doesn't dangle two ways to start a new one.
    FINISH_HIGHLIGHT,
    CANCEL_HIGHLIGHT
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, const bool hasBookmarks,
                                  const bool isCurrentPageBookmarked, const bool isBookCompleted,
                                  const bool autoPageTurnActive = false,
                                  const uint16_t autoPageTurnIntervalSeconds = 0,
                                  const bool hasDictionary = false, const bool hasLookupHistory = false,
                                  const bool hasPendingHighlight = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool isCurrentPageBookmarked,
                                              bool isBookCompleted, bool hasDictionary, bool hasLookupHistory,
                                              bool hasPendingHighlight);

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  bool autoPageTurnActive = false;
  uint16_t autoPageTurnIntervalSeconds = 0;
  bool settingsChanged = false;
};
