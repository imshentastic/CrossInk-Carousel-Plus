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
    DELETE_STATS,  // CrumBLE 4.4: ported from CrossInk v1.3.3 reading-stats split
    READING_STATS,
    TOGGLE_COMPLETED,
    READER_OPTIONS,
    CONTROLS_OPTIONS,
    BOOKMARK_TOGGLE,
    VIEW_BOOKMARKS,
    DELETE_BOOKMARKS,
    EXPORT_BOOKMARKS,
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
    CANCEL_HIGHLIGHT,
    // CrumBLE in-book menu reorg: SECTION_BREAK is a non-selectable
    // visual divider row. The theme draws a horizontal line under it
    // with no label, separating logical clusters of menu actions.
    SECTION_BREAK,
    // CrumBLE in-book menu reorg: opens an inline "Bookmarks" sub-screen
    // showing Add Highlight + the existing bookmark management actions.
    // Handled inside this activity via mode_ rather than starting a new
    // activity, so the existing Back-to-reader flow stays a single
    // finish().
    OPEN_BOOKMARKS_SUBMENU,
  };

  // CrumBLE in-book menu reorg: tracks whether we're rendering the main
  // menu or the inline Bookmarks sub-screen. Back from Bookmarks pops
  // back to Main; Back from Main exits the whole menu activity.
  enum class MenuMode { Main, Bookmarks };

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

  static std::vector<MenuItem> buildMainMenuItems(bool hasFootnotes, bool hasBookmarks, bool isCurrentPageBookmarked,
                                                  bool isBookCompleted, bool hasDictionary, bool hasLookupHistory,
                                                  bool hasPendingHighlight);
  static std::vector<MenuItem> buildBookmarksSubmenuItems(bool hasBookmarks, bool isCurrentPageBookmarked);

  // CrumBLE in-book menu reorg: menuItems is now mutable so we can swap it
  // between the Main layout and the inline Bookmarks sub-screen.
  std::vector<MenuItem> menuItems;

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

  // CrumBLE: rebuild context preserved for switching back to Main from the
  // Bookmarks sub-screen without re-querying the reader for these flags.
  bool hasFootnotes_ = false;
  bool hasBookmarks_ = false;
  bool isCurrentPageBookmarked_ = false;
  bool isBookCompleted_ = false;
  bool hasDictionary_ = false;
  bool hasLookupHistory_ = false;
  bool hasPendingHighlight_ = false;

  MenuMode mode_ = MenuMode::Main;

  // Helpers to switch between Main and Bookmarks sub-screen.
  void enterBookmarksSubmenu();
  void exitBookmarksSubmenu();

  // Move selection past a SECTION_BREAK row in the indicated direction.
  // Returns the next selectable index; falls back to the input if all rows
  // happen to be section breaks (shouldn't happen but kept safe).
  int skipSectionBreakForward(int from) const;
  int skipSectionBreakBackward(int from) const;
};
