#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "AppVersion.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CoverThumbStatus.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int systemVersionFooterSideMargin = 20;
constexpr int systemVersionFooterBottomInset = 15;

uint8_t enumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

void drawCenteredTextLine(const GfxRenderer& renderer, const int pageWidth, const int y, const std::string& text) {
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, text.c_str());
  const int labelX = (pageWidth - labelWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, labelX, y, text.c_str());
}

bool isVersionBreakChar(const char c) { return c == ' ' || c == '-' || c == '+' || c == '.' || c == '_'; }

void drawSystemVersionFooter(const GfxRenderer& renderer, const int pageWidth, const int pageHeight,
                             const ThemeMetrics& metrics) {
  static constexpr const char* versionLabel = "CrumBLE " CRUMBLE_VERSION " (CrossInk " CROSSINK_VERSION ")";
  const std::string label = versionLabel;
  const int maxWidth = pageWidth - systemVersionFooterSideMargin * 2;
  const int bottomLineY =
      pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - systemVersionFooterBottomInset;

  if (renderer.getTextWidth(SMALL_FONT_ID, label.c_str()) <= maxWidth) {
    drawCenteredTextLine(renderer, pageWidth, bottomLineY, label);
    return;
  }

  size_t fallbackBreak = std::string::npos;
  size_t preferredBreak = std::string::npos;
  for (size_t i = 1; i < label.size(); i++) {
    if (!isVersionBreakChar(label[i - 1])) continue;

    const std::string firstLine = label.substr(0, i);
    if (renderer.getTextWidth(SMALL_FONT_ID, firstLine.c_str()) > maxWidth) break;

    fallbackBreak = i;
    const std::string secondLine = label.substr(i);
    if (renderer.getTextWidth(SMALL_FONT_ID, secondLine.c_str()) <= maxWidth) {
      preferredBreak = i;
    }
  }

  const size_t lineBreak = preferredBreak != std::string::npos ? preferredBreak : fallbackBreak;
  const std::string firstLine = lineBreak == std::string::npos
                                    ? renderer.truncatedText(SMALL_FONT_ID, label.c_str(), maxWidth)
                                    : label.substr(0, lineBreak);
  const std::string secondLine = lineBreak == std::string::npos
                                     ? ""
                                     : renderer.truncatedText(SMALL_FONT_ID, label.substr(lineBreak).c_str(), maxWidth);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  drawCenteredTextLine(renderer, pageWidth, bottomLineY - lineHeight, firstLine);
  drawCenteredTextLine(renderer, pageWidth, bottomLineY, secondLine);
}

// Walks the flat settings list produced by getSettingsList() and pulls one
// SettingInfo entry by its StrId nameId. Used by the submenu builder below to
// pick rows out of the canonical registered list rather than redeclaring them
// here -- the registered list is what JsonSettingsIO + the web /settings page
// consume, so we keep a single source of truth.
const SettingInfo* findByNameId(const std::vector<SettingInfo>& all, StrId nameId) {
  const auto it = std::find_if(all.begin(), all.end(),
                               [nameId](const SettingInfo& s) { return s.nameId == nameId; });
  return it == all.end() ? nullptr : &*it;
}

// Same as findByNameId but matches on the JSON key. Required because the
// "Orientation Aware" entries for front buttons vs side buttons share a
// nameId (STR_ORIENTATION_AWARE) -- the key disambiguates them.
const SettingInfo* findByKey(const std::vector<SettingInfo>& all, const char* key) {
  const auto it = std::find_if(all.begin(), all.end(), [key](const SettingInfo& s) {
    return s.key != nullptr && std::strcmp(s.key, key) == 0;
  });
  return it == all.end() ? nullptr : &*it;
}

// Convenience: copy the row found by nameId into the children vector, or
// log an error if missing. Keeps the submenu builder readable.
void pushByName(std::vector<SettingInfo>& children, const std::vector<SettingInfo>& all, StrId nameId) {
  if (const auto* s = findByNameId(all, nameId)) {
    children.push_back(*s);
  } else {
    LOG_ERR("SET", "Submenu builder: missing setting nameId=%d", static_cast<int>(nameId));
  }
}

void pushByKey(std::vector<SettingInfo>& children, const std::vector<SettingInfo>& all, const char* key) {
  if (const auto* s = findByKey(all, key)) {
    children.push_back(*s);
  } else {
    LOG_ERR("SET", "Submenu builder: missing setting key=%s", key);
  }
}
}  // namespace

void SettingsActivity::rebuildSettingsLists() {
  rootSettings_.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran -- otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  const auto allSettings = getSettingsList(&sdFontSystem.registry());

  // === Display submenu ============================================
  std::vector<SettingInfo> displaySleepScreen;
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_SCREEN);
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_COVER_MODE);
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_COVER_FILTER);
  pushByName(displaySleepScreen, allSettings, StrId::STR_CYCLE_SCREENSAVER_ON_TAP);
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_CYCLE_SKIP_GRAYSCALE);
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_SCREEN_ORDER);
  pushByName(displaySleepScreen, allSettings, StrId::STR_QUICK_RESUME);

  std::vector<SettingInfo> displayThemeLayout;
  pushByName(displayThemeLayout, allSettings, StrId::STR_UI_THEME);
  pushByName(displayThemeLayout, allSettings, StrId::STR_RECENT_BOOKS_VIEW);

  std::vector<SettingInfo> displayGeneral;
  pushByName(displayGeneral, allSettings, StrId::STR_HIDE_BATTERY);
  pushByName(displayGeneral, allSettings, StrId::STR_REFRESH_FREQ);
  pushByName(displayGeneral, allSettings, StrId::STR_SUNLIGHT_FADING_FIX);

  std::vector<SettingInfo> displayChildren;
  displayChildren.push_back(SettingInfo::Submenu(StrId::STR_SLEEP_SCREEN, std::move(displaySleepScreen)));
  displayChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_THEME_LAYOUT, std::move(displayThemeLayout)));
  displayChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_GENERAL, std::move(displayGeneral)));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_DISPLAY, std::move(displayChildren)));

  // === Reader submenu =============================================
  std::vector<SettingInfo> readerFont;
  pushByName(readerFont, allSettings, StrId::STR_FONT_FAMILY);
  pushByName(readerFont, allSettings, StrId::STR_FONT_SIZE);
  readerFont.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));

  std::vector<SettingInfo> readerLayout;
  pushByName(readerLayout, allSettings, StrId::STR_LINE_SPACING);
  pushByName(readerLayout, allSettings, StrId::STR_READER_DARK_MODE);
  pushByName(readerLayout, allSettings, StrId::STR_ORIENTATION);
  pushByName(readerLayout, allSettings, StrId::STR_SCREEN_MARGIN);
  pushByName(readerLayout, allSettings, StrId::STR_PARA_ALIGNMENT);
  pushByName(readerLayout, allSettings, StrId::STR_EXTRA_SPACING);
  pushByName(readerLayout, allSettings, StrId::STR_FORCE_PARAGRAPH_INDENTS);

  std::vector<SettingInfo> readerStyle;
  pushByName(readerStyle, allSettings, StrId::STR_EMBEDDED_STYLE);
  pushByName(readerStyle, allSettings, StrId::STR_HYPHENATION);
  pushByName(readerStyle, allSettings, StrId::STR_TEXT_AA);
  pushByName(readerStyle, allSettings, StrId::STR_IMAGES);

  std::vector<SettingInfo> readerAids;
  pushByName(readerAids, allSettings, StrId::STR_BIONIC_READING);
  pushByName(readerAids, allSettings, StrId::STR_GUIDE_READING);

  std::vector<SettingInfo> readerChildren;
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_FONT, std::move(readerFont)));
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_LAYOUT, std::move(readerLayout)));
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_STYLE, std::move(readerStyle)));
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_READING_AIDS, std::move(readerAids)));
  readerChildren.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_READER, std::move(readerChildren)));

  // === Controls submenu ===========================================
  std::vector<SettingInfo> controlsPower;
  pushByName(controlsPower, allSettings, StrId::STR_SHORT_PWR_BTN);
  pushByName(controlsPower, allSettings, StrId::STR_LONG_PRESS_ACTION);

  std::vector<SettingInfo> controlsFront;
  controlsFront.push_back(SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  controlsFront.push_back(
      SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS_READER, SettingAction::RemapFrontButtonsReader));
  pushByKey(controlsFront, allSettings, "frontButtonOrientationAware");
  pushByName(controlsFront, allSettings, StrId::STR_LONG_PRESS_BEHAVIOR);
  pushByName(controlsFront, allSettings, StrId::STR_LONG_PRESS_MENU_ACTION);

  std::vector<SettingInfo> controlsSide;
  pushByName(controlsSide, allSettings, StrId::STR_SIDE_BTN_LAYOUT);
  pushByKey(controlsSide, allSettings, "sideButtonOrientationAware");
  pushByName(controlsSide, allSettings, StrId::STR_SIDE_BTN_LONG_PRESS);
  // x3-only Tilt row: surface inline at the bottom of Side Buttons when the
  // QMI8658 IMU is present, rather than its own 1-row submenu.
  if (halTiltSensor.isAvailable() && findByNameId(allSettings, StrId::STR_TILT_PAGE_TURN) != nullptr) {
    pushByName(controlsSide, allSettings, StrId::STR_TILT_PAGE_TURN);
  }

  std::vector<SettingInfo> controlsChildren;
  controlsChildren.push_back(SettingInfo::Submenu(StrId::STR_POWER_BUTTON, std::move(controlsPower)));
  controlsChildren.push_back(SettingInfo::Submenu(StrId::STR_FRONT_BUTTONS, std::move(controlsFront)));
  controlsChildren.push_back(SettingInfo::Submenu(StrId::STR_SIDE_BUTTONS, std::move(controlsSide)));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_CONTROLS, std::move(controlsChildren)));

  // === Library submenu ============================================
  std::vector<SettingInfo> libraryFiles;
  pushByName(libraryFiles, allSettings, StrId::STR_SHOW_HIDDEN_FILES);
  pushByName(libraryFiles, allSettings, StrId::STR_REMOVE_READ_FROM_RECENTS);
  pushByName(libraryFiles, allSettings, StrId::STR_MOVE_FINISHED_TO_READ);

  std::vector<SettingInfo> libraryChildren;
  libraryChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_FILES, std::move(libraryFiles)));
  pushByName(libraryChildren, allSettings, StrId::STR_SERIES_DETECTION);
  pushByName(libraryChildren, allSettings, StrId::STR_OPTIMIZE_CHAPTER_INDEXING);

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_LIBRARY, std::move(libraryChildren)));

  // === Sync & Network submenu =====================================
  std::vector<SettingInfo> syncChildren;
  syncChildren.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  syncChildren.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  syncChildren.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_SYNC_NETWORK, std::move(syncChildren)));

  // === System submenu =============================================
  std::vector<SettingInfo> systemChildren;
  pushByName(systemChildren, allSettings, StrId::STR_TIME_TO_SLEEP);
  systemChildren.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  systemChildren.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemChildren.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemChildren.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  // CrumBLE 4.4: manual retry for books whose cover gen previously failed
  // (most commonly the EOCD-scan-too-small bug fixed in 4.4). Sweeps
  // thumb_failed_v3_*.marker files so the bookshelf re-attempts on next
  // visit.
  systemChildren.push_back(SettingInfo::Action(StrId::STR_RETRY_FAILED_COVERS, SettingAction::RetryFailedCovers));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_SYSTEM, std::move(systemChildren)));
}

void SettingsActivity::enterSubmenu(const SettingInfo& row) {
  if (row.type != SettingType::SUBMENU) return;
  NavFrame frame;
  frame.settings = &row.children;
  frame.titleId = row.nameId;
  frame.selectedIndex = 0;
  navStack_.push_back(frame);
  requestUpdate();
}

void SettingsActivity::goBack() {
  if (navStack_.size() > 1) {
    navStack_.pop_back();
    requestUpdate();
    return;
  }
  SETTINGS.saveToFile();
  onGoHome();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();
  // Root frame: the six-entry top-level list. Title "Settings" is the
  // window header; the list itself has no title overlay.
  navStack_.clear();
  NavFrame root;
  root.settings = &rootSettings_;
  root.titleId = StrId::STR_SETTINGS_TITLE;
  root.selectedIndex = 0;
  navStack_.push_back(root);

  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int idx = currentFrame().selectedIndex;
    if (idx >= 0 && idx < currentSettingsCount()) {
      const auto& row = currentSettings()[idx];
      if (row.type == SettingType::SUBMENU) {
        // Push child list onto the nav stack. Confirm hint shows the row
        // name to telegraph that we'll drill in.
        enterSubmenu(row);
        return;
      }
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  // Up/Down navigation within the current frame. Skip SECTION_HEADER rows so
  // they read as static labels rather than focusable list items.
  buttonNavigator.onNextRelease([this] {
    const int n = currentSettingsCount();
    if (n == 0) return;
    int idx = currentFrame().selectedIndex;
    idx = ButtonNavigator::nextIndex(idx, n);
    while (n > 1 && currentSettings()[idx].type == SettingType::SECTION_HEADER) {
      idx = ButtonNavigator::nextIndex(idx, n);
    }
    currentFrame().selectedIndex = idx;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    const int n = currentSettingsCount();
    if (n == 0) return;
    int idx = currentFrame().selectedIndex;
    idx = ButtonNavigator::previousIndex(idx, n);
    while (n > 1 && currentSettings()[idx].type == SettingType::SECTION_HEADER) {
      idx = ButtonNavigator::previousIndex(idx, n);
    }
    currentFrame().selectedIndex = idx;
    requestUpdate();
  });
}

void SettingsActivity::toggleCurrentSetting() {
  const int idx = currentFrame().selectedIndex;
  if (idx < 0 || idx >= currentSettingsCount()) {
    return;
  }

  const auto& setting = currentSettings()[idx];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, currentValue);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::RemapFrontButtonsReader:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, true), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::RetryFailedCovers: {
        // CrumBLE 4.4: brief popup so the user gets feedback that something
        // happened (the sweep is a sub-second SD walk; without a popup the
        // selection just "clicks" with no visible result).
        const int removed = CoverThumbStatus::sweepAllMarkers();
        char msg[96];
        if (removed > 0) {
          std::snprintf(msg, sizeof(msg), tr(STR_COVERS_RETRY_DONE), removed);
        } else {
          std::snprintf(msg, sizeof(msg), "%s", tr(STR_COVERS_RETRY_NONE));
        }
        GUI.drawPopup(renderer, msg);
        // Brief dwell so the message is readable, then return to the menu.
        delay(1500);
        requestUpdate();
        break;
      }
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, StrId::STR_SLEEP_TIMER_STEP_HINT,
          SETTINGS.sleepTimeoutMinutes, CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES,
          CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& frame = currentFrame();

  // Header always shows the current frame's title -- "Settings" at root,
  // submenu name (e.g. "Display", "Sleep Screen") when drilled in.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(frame.titleId));

  // CrumBLE submenu redesign: no tab bar. The list starts directly below
  // the header so the available list height matches the old layout's
  // post-tab area, minus the (now absent) tabBarHeight band -- the extra
  // verticalSpacing keeps the same top breathing room the user is used to.
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight =
      pageHeight - (listTop + metrics.buttonHintsHeight + metrics.verticalSpacing);

  const auto& settings = currentSettings();
  const int count = currentSettingsCount();
  const int selectedIdx = frame.selectedIndex;

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, count, selectedIdx,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          const uint8_t displayValue = enumDisplayIndexForRawValue(setting, value);
          const size_t optionCount = settingEnumOptionCount(setting);
          const uint8_t safeValue = displayValue < optionCount ? displayValue : 0;
          valueText = settingEnumOptionLabel(setting, safeValue);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          valueText = settingEnumOptionLabel(setting, value);
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                     static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
            valueText = valueBuffer;
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::SUBMENU) {
          // Chevron-equivalent: a trailing ">" tells the user this row
          // drills in rather than toggling. Theme can swap this for a
          // glyph later without changing the SettingType plumbing.
          valueText = ">";
        }
        return valueText;
      },
      true, nullptr, [&settings](int i) { return settings[i].type == SettingType::SECTION_HEADER; });

  // Version footer on the System submenu (matches the old behaviour where
  // it only showed under the System tab).
  if (frame.titleId == StrId::STR_CAT_SYSTEM) {
    drawSystemVersionFooter(renderer, pageWidth, pageHeight, metrics);
  }

  // Confirm hint: SUBMENU rows show "Enter", actions show their nature,
  // value/toggle/enum rows show "Toggle". Back hint adapts: at root the
  // Back button leaves Settings entirely, deeper it pops one level.
  const char* confirmLabel = tr(STR_TOGGLE);
  if (selectedIdx >= 0 && selectedIdx < count) {
    const auto& row = settings[selectedIdx];
    if (row.type == SettingType::SUBMENU || row.type == SettingType::ACTION) {
      confirmLabel = tr(STR_SELECT);
    } else if (row.nameId == StrId::STR_TIME_TO_SLEEP) {
      confirmLabel = tr(STR_SELECT);
    }
  }
  // Back hint reads the same at root and deeper (no STR_EXIT in current
  // translations); meaning is contextual to depth.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
