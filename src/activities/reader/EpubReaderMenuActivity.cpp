#include "EpubReaderMenuActivity.h"

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstring>

#include "../settings/BluetoothSettingsActivity.h"
#include "../util/ConfirmationActivity.h"
#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"  // prewarmReaderTextBuffer
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

struct ReaderLayoutSettingsSnapshot {
  uint8_t fontFamily;
  uint8_t fontSize;
  uint8_t sdFontSizeRange;
  uint8_t lineSpacing;
  uint8_t orientation;
  uint8_t screenMargin;
  uint8_t paragraphAlignment;
  uint8_t embeddedStyle;
  uint8_t hyphenationEnabled;
  uint8_t imageRendering;
  uint8_t extraParagraphSpacing;
  uint8_t forceParagraphIndents;
  uint8_t bionicReadingEnabled;
  uint8_t guideReadingEnabled;
  char sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName)] = {};

  bool operator==(const ReaderLayoutSettingsSnapshot& other) const {
    return fontFamily == other.fontFamily && fontSize == other.fontSize && sdFontSizeRange == other.sdFontSizeRange &&
           lineSpacing == other.lineSpacing && orientation == other.orientation && screenMargin == other.screenMargin &&
           paragraphAlignment == other.paragraphAlignment && embeddedStyle == other.embeddedStyle &&
           hyphenationEnabled == other.hyphenationEnabled && imageRendering == other.imageRendering &&
           extraParagraphSpacing == other.extraParagraphSpacing &&
           forceParagraphIndents == other.forceParagraphIndents && bionicReadingEnabled == other.bionicReadingEnabled &&
           guideReadingEnabled == other.guideReadingEnabled &&
           std::strncmp(sdFontFamilyName, other.sdFontFamilyName, sizeof(sdFontFamilyName)) == 0;
  }
  bool operator!=(const ReaderLayoutSettingsSnapshot& other) const { return !(*this == other); }
};

ReaderLayoutSettingsSnapshot captureReaderLayoutSettings() {
  ReaderLayoutSettingsSnapshot snapshot{
      SETTINGS.fontFamily,
      SETTINGS.fontSize,
      SETTINGS.sdFontSizeRange,
      SETTINGS.lineSpacing,
      SETTINGS.orientation,
      SETTINGS.screenMargin,
      SETTINGS.paragraphAlignment,
      SETTINGS.embeddedStyle,
      SETTINGS.hyphenationEnabled,
      SETTINGS.imageRendering,
      SETTINGS.extraParagraphSpacing,
      SETTINGS.forceParagraphIndents,
      SETTINGS.bionicReadingEnabled,
      SETTINGS.guideReadingEnabled,
  };
  std::strncpy(snapshot.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(snapshot.sdFontFamilyName) - 1);
  snapshot.sdFontFamilyName[sizeof(snapshot.sdFontFamilyName) - 1] = '\0';
  return snapshot;
}

bool haveReaderLayoutSettingsChanged(const ReaderLayoutSettingsSnapshot& before) {
  return before != captureReaderLayoutSettings();
}

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool isCurrentPageBookmarked, const bool isBookCompleted,
                                               const bool autoPageTurnActive,
                                               const uint16_t autoPageTurnIntervalSeconds,
                                               const bool hasDictionary, const bool hasLookupHistory,
                                               const bool hasPendingHighlight)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMainMenuItems(hasFootnotes, hasBookmarks, isCurrentPageBookmarked, isBookCompleted,
                                   hasDictionary, hasLookupHistory, hasPendingHighlight)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      autoPageTurnActive(autoPageTurnActive),
      autoPageTurnIntervalSeconds(autoPageTurnIntervalSeconds),
      hasFootnotes_(hasFootnotes),
      hasBookmarks_(hasBookmarks),
      isCurrentPageBookmarked_(isCurrentPageBookmarked),
      isBookCompleted_(isBookCompleted),
      hasDictionary_(hasDictionary),
      hasLookupHistory_(hasLookupHistory),
      hasPendingHighlight_(hasPendingHighlight) {
  // First selectable row may be a SECTION_BREAK depending on which
  // conditional rows surfaced; skip past it.
  if (!menuItems.empty() && menuItems[0].action == MenuAction::SECTION_BREAK) {
    selectedIndex = skipSectionBreakForward(0);
  }
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMainMenuItems(bool hasFootnotes,
                                                                                         bool hasBookmarks,
                                                                                         bool isCurrentPageBookmarked,
                                                                                         bool isBookCompleted,
                                                                                         bool hasDictionary,
                                                                                         bool hasLookupHistory,
                                                                                         bool hasPendingHighlight) {
  // CrumBLE in-book menu reorg v2: three sections separated by visual line
  // dividers (SECTION_BREAK rows). The theme draws a horizontal line under
  // each break row with no label, so the menu reads as three clusters
  // without forcing section titles.
  //
  // Section 1 (quick actions): Footnotes, Lookup, Looked-Up Words,
  //   Add/Finish/Cancel Highlight, Reading Stats, Auto Page Turn.
  // Section 2 (navigate + customise): Select Chapter, Go to %, Sync
  //   Progress, Reader Options (Orientation lives inside it via the
  //   STR_CAT_READER category), Controls, Bookmarks (opens an inline
  //   sub-screen with Add Highlight + View / Export / Clear), Bluetooth.
  // Section 3 (output / state): Display QR, Screenshot, Mark Finished,
  //   Delete Book Cache.
  //
  // Dropped vs the old menu:
  //   - Go Home (user request -- close the menu and the front-button
  //     remap or device button takes you home anyway)
  //   - Orientation top-level row (Reader Options already exposes it)
  //   - Add Bookmark legacy row (still removed; the bookmarks
  //     management lens is the Bookmarks sub-screen)
  std::vector<MenuItem> items;
  items.reserve(24);

  // === Section 1 : Quick actions ===
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasDictionary) {
    items.push_back({MenuAction::LOOKUP, StrId::STR_LOOKUP});
    if (hasLookupHistory) {
      items.push_back({MenuAction::LOOKED_UP_WORDS, StrId::STR_LOOKED_UP_WORDS});
    }
  }
  // Highlight quick action. Pending-hold state replaces Add with the
  // Finish/Cancel pair so the menu doesn't dangle two ways to start.
  if (hasPendingHighlight) {
    items.push_back({MenuAction::FINISH_HIGHLIGHT, StrId::STR_FINISH_HIGHLIGHT});
    items.push_back({MenuAction::CANCEL_HIGHLIGHT, StrId::STR_CANCEL_HIGHLIGHT});
  } else {
    items.push_back({MenuAction::ADD_HIGHLIGHT, StrId::STR_ADD_HIGHLIGHT});
  }
  items.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL_SECONDS});

  // === Section 2 : Navigate + customise ===
  items.push_back({MenuAction::SECTION_BREAK, StrId::STR_NONE_OPT});
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS});
  items.push_back({MenuAction::CONTROLS_OPTIONS, StrId::STR_CAT_CONTROLS});
  // CrumBLE: Bookmarks is a sub-screen with the four management entries.
  // Surfaced unconditionally so the user can always reach Add Highlight
  // through it even on a book with no existing bookmarks. The label here
  // is "Bookmarks" -- the inner "View Bookmarks" row owns the list view.
  items.push_back({MenuAction::OPEN_BOOKMARKS_SUBMENU, StrId::STR_BOOKMARKS});
  items.push_back({MenuAction::BLUETOOTH, StrId::STR_BLUETOOTH});  // CrumBLE

  // === Section 3 : Output + state ===
  items.push_back({MenuAction::SECTION_BREAK, StrId::STR_NONE_OPT});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back(
      {MenuAction::TOGGLE_COMPLETED, isBookCompleted ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});

  // isCurrentPageBookmarked / hasBookmarks are not used here directly --
  // those drive what shows in the Bookmarks sub-screen (built separately).
  (void)isCurrentPageBookmarked;
  (void)hasBookmarks;
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildBookmarksSubmenuItems(
    bool hasBookmarks, bool isCurrentPageBookmarked) {
  // CrumBLE: inline Bookmarks sub-screen. Reachable from the main menu's
  // "Bookmarks" row. Mirrors the management actions that used to live
  // inline at the top of the menu, plus Add Highlight as the canonical
  // entry point for the highlight flow.
  std::vector<MenuItem> items;
  items.reserve(5);
  items.push_back({MenuAction::ADD_HIGHLIGHT, StrId::STR_ADD_HIGHLIGHT});
  if (hasBookmarks) {
    items.push_back({MenuAction::VIEW_BOOKMARKS, StrId::STR_VIEW_BOOKMARKS});
    items.push_back({MenuAction::EXPORT_BOOKMARKS, StrId::STR_EXPORT_HIGHLIGHTS});
    items.push_back({MenuAction::DELETE_BOOKMARKS, StrId::STR_DELETE_BOOKMARKS});
  }
  // Remove-bookmark for the current page lives here too, only when set.
  if (isCurrentPageBookmarked) {
    items.push_back({MenuAction::BOOKMARK_TOGGLE, StrId::STR_REMOVE_BOOKMARK});
  }
  return items;
}

int EpubReaderMenuActivity::skipSectionBreakForward(int from) const {
  const int n = static_cast<int>(menuItems.size());
  if (n == 0) return from;
  int idx = from;
  for (int i = 0; i < n; i++) {
    if (idx >= 0 && idx < n && menuItems[idx].action != MenuAction::SECTION_BREAK) return idx;
    idx = ButtonNavigator::nextIndex(idx, n);
  }
  return from;
}

int EpubReaderMenuActivity::skipSectionBreakBackward(int from) const {
  const int n = static_cast<int>(menuItems.size());
  if (n == 0) return from;
  int idx = from;
  for (int i = 0; i < n; i++) {
    if (idx >= 0 && idx < n && menuItems[idx].action != MenuAction::SECTION_BREAK) return idx;
    idx = ButtonNavigator::previousIndex(idx, n);
  }
  return from;
}

void EpubReaderMenuActivity::enterBookmarksSubmenu() {
  mode_ = MenuMode::Bookmarks;
  menuItems = buildBookmarksSubmenuItems(hasBookmarks_, isCurrentPageBookmarked_);
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderMenuActivity::exitBookmarksSubmenu() {
  mode_ = MenuMode::Main;
  menuItems = buildMainMenuItems(hasFootnotes_, hasBookmarks_, isCurrentPageBookmarked_, isBookCompleted_,
                                 hasDictionary_, hasLookupHistory_, hasPendingHighlight_);
  selectedIndex = 0;
  if (!menuItems.empty() && menuItems[0].action == MenuAction::SECTION_BREAK) {
    selectedIndex = skipSectionBreakForward(0);
  }
  requestUpdate();
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation. SECTION_BREAK rows are visual dividers -- skip them
  // when stepping the cursor so they read as decoration, not list items.
  buttonNavigator.onNext([this] {
    const int n = static_cast<int>(menuItems.size());
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, n);
    if (selectedIndex >= 0 && selectedIndex < n &&
        menuItems[selectedIndex].action == MenuAction::SECTION_BREAK) {
      selectedIndex = skipSectionBreakForward(selectedIndex);
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    const int n = static_cast<int>(menuItems.size());
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, n);
    if (selectedIndex >= 0 && selectedIndex < n &&
        menuItems[selectedIndex].action == MenuAction::SECTION_BREAK) {
      selectedIndex = skipSectionBreakBackward(selectedIndex);
    }
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;

    // SECTION_BREAK is a decoration row; Confirm on it is a no-op even if
    // the cursor somehow landed on one (shouldn't happen with skip logic).
    if (selectedAction == MenuAction::SECTION_BREAK) {
      return;
    }

    // Bookmarks sub-screen entry: swap menuItems to the four management
    // rows and stay in this activity. Back later pops us out.
    if (selectedAction == MenuAction::OPEN_BOOKMARKS_SUBMENU) {
      enterBookmarksSubmenu();
      return;
    }

    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::READER_OPTIONS) {
      const auto before = captureReaderLayoutSettings();
      auto& bt = BluetoothHIDManager::getInstance();
      const bool bleWasOn = bt.isEnabled();

      // CrumBLE: silent BLE drop on entry. The previous "Turn off Bluetooth?"
      // prompt is gone -- the disconnect is a forced consequence of the heap
      // pressure (Reader Options' settings-list build OOM-crashes under BLE),
      // not a user choice. Just do it. Auto-reconnect via requestEnableLater()
      // when the user exits Reader Options, so they don't have to manually
      // re-enable from the BT menu afterwards.
      if (bleWasOn) {
        GUI.drawPopup(renderer, "Updating layout...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        bt.disable();
      }

      startActivityForResult(std::make_unique<ReaderOptionsActivity>(renderer, mappedInput),
                             [this, before, bleWasOn](const ActivityResult&) {
                               settingsChanged = settingsChanged || haveReaderLayoutSettingsChanged(before);
                               pendingOrientation = SETTINGS.orientation;  // sync in case orientation changed
                               if (bleWasOn) {
                                 // CrumBLE Phase 1 fast-open: pre-grow the glyph
                                 // buffer before the deferred enable drains, so
                                 // NimBLE init doesn't race a cold-buffer first
                                 // text-page render.
                                 EpubReaderActivity::prewarmReaderTextBuffer(renderer);
                                 // Deferred: drained on the main loop AFTER any pending
                                 // re-layout from settings changes finishes. Prevents
                                 // NimBLE init from racing the section rebuild.
                                 BluetoothHIDManager::getInstance().requestEnableLater();
                               }
                               requestUpdate();
                             });
      return;
    }

    if (selectedAction == MenuAction::CONTROLS_OPTIONS) {
      startActivityForResult(std::make_unique<ControlsOptionsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               ActivityResult result;
                               result.isCancelled = true;
                               result.data = MenuResult{-1, pendingOrientation, settingsChanged};
                               setResult(std::move(result));
                               finish();
                             });
      return;
    }

    if (selectedAction == MenuAction::BLUETOOTH) {
#ifndef SIMULATOR
      // exitOnSuccessfulConnect=true: when the user pairs a new remote or
      // reconnects to a bonded one from inside the book, BluetoothSettings
      // sets MenuResult.autoExitParent=true and pops itself. We then also
      // finish this menu so the user lands straight back in the book —
      // ActivityManager chains the two pops in the same loop iteration so
      // the menu never visibly re-renders.
      startActivityForResult(
          std::make_unique<BluetoothSettingsActivity>(renderer, mappedInput, [] { activityManager.popActivity(); },
                                                      /*exitOnSuccessfulConnect=*/true),
          [this](const ActivityResult& result) {
            const auto* menu = std::get_if<MenuResult>(&result.data);
            if (menu && menu->autoExitParent) {
              ActivityResult myResult;
              myResult.isCancelled = true;
              myResult.data = MenuResult{-1, pendingOrientation, settingsChanged};
              setResult(std::move(myResult));
              finish();
              return;
            }
            requestUpdate();
          });
#endif  // SIMULATOR: BLE pairing UI needs NimBLE; no-op in the native simulator.
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, settingsChanged});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Bookmarks sub-screen: Back pops back to the Main menu instead of
    // closing the whole menu activity.
    if (mode_ == MenuMode::Bookmarks) {
      exitBookmarksSubmenu();
      return;
    }
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, settingsChanged};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Header shows the book title in Main mode, "Bookmarks" in the sub-screen
  // so the user has explicit confirmation of where they are.
  const std::string headerTitle = mode_ == MenuMode::Bookmarks ? std::string(tr(STR_BOOKMARKS)) : title;
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 headerTitle.c_str());

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      // Row label. SECTION_BREAK rows return an empty string so the divider
      // renders as a bare horizontal line with no header text above it.
      [this](int index) -> std::string {
        if (menuItems[index].action == MenuAction::SECTION_BREAK) return "";
        return I18N.get(menuItems[index].labelId);
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          // Render current page turn value on the right edge of the content area.
          return autoPageTurnActive ? std::to_string(autoPageTurnIntervalSeconds) : "";
        } else {
          return "";
        }
      },
      true, nullptr,
      // isHeaderRow predicate: SECTION_BREAK rows become non-selectable
      // theme-drawn dividers (horizontal line under an empty label).
      [this](int index) { return menuItems[index].action == MenuAction::SECTION_BREAK; });

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
