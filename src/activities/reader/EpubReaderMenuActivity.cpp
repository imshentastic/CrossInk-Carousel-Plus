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
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, isCurrentPageBookmarked, isBookCompleted,
                               hasDictionary, hasLookupHistory, hasPendingHighlight)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      autoPageTurnActive(autoPageTurnActive),
      autoPageTurnIntervalSeconds(autoPageTurnIntervalSeconds) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool hasBookmarks,
                                                                                     bool isCurrentPageBookmarked,
                                                                                     bool isBookCompleted,
                                                                                     bool hasDictionary,
                                                                                     bool hasLookupHistory,
                                                                                     bool hasPendingHighlight) {
  std::vector<MenuItem> items;
  // 14 upstream items + 1 (CrumBLE Bluetooth entry).
  constexpr size_t baseItemCount = 15;
  const size_t totalItemCount = baseItemCount + (hasFootnotes ? 1u : 0u) + (hasBookmarks ? 2u : 0u) +
                                (hasDictionary ? 1u : 0u) + (hasLookupHistory ? 1u : 0u);
  items.reserve(totalItemCount);
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  // CrumBLE: dictionary entries (ported from SEEK reader). Surfaced near
  // the top of the menu because lookup is a quick, frequent action --
  // putting it next to Footnotes keeps the "look at this thing" actions
  // together. Only appear when a StarDict file is present on the SD.
  if (hasDictionary) {
    items.push_back({MenuAction::LOOKUP, StrId::STR_LOOKUP});
    if (hasLookupHistory) {
      items.push_back({MenuAction::LOOKED_UP_WORDS, StrId::STR_LOOKED_UP_WORDS});
    }
  }
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS});
  items.push_back({MenuAction::CONTROLS_OPTIONS, StrId::STR_CAT_CONTROLS});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL_SECONDS});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back(
      {MenuAction::BOOKMARK_TOGGLE, isCurrentPageBookmarked ? StrId::STR_REMOVE_BOOKMARK : StrId::STR_ADD_BOOKMARK});
  // CrumBLE: ranged highlight. With no held anchor, ADD_HIGHLIGHT starts
  // a fresh selection. With a held anchor (the user backed out with the
  // start placed for a cross-page highlight), surface FINISH + CANCEL
  // instead -- showing ADD here would leave two ways to start.
  if (hasPendingHighlight) {
    items.push_back({MenuAction::FINISH_HIGHLIGHT, StrId::STR_FINISH_HIGHLIGHT});
    items.push_back({MenuAction::CANCEL_HIGHLIGHT, StrId::STR_CANCEL_HIGHLIGHT});
  } else {
    items.push_back({MenuAction::ADD_HIGHLIGHT, StrId::STR_ADD_HIGHLIGHT});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::VIEW_BOOKMARKS, StrId::STR_VIEW_BOOKMARKS});
    items.push_back({MenuAction::EXPORT_BOOKMARKS, StrId::STR_EXPORT_HIGHLIGHTS});
    items.push_back({MenuAction::DELETE_BOOKMARKS, StrId::STR_DELETE_BOOKMARKS});
  }
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  items.push_back({MenuAction::BLUETOOTH, StrId::STR_BLUETOOTH});  // CrumBLE
  items.push_back(
      {MenuAction::TOGGLE_COMPLETED, isBookCompleted ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
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

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

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
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
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
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
