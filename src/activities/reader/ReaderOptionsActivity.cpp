#include "ReaderOptionsActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include "../../SilentRestart.h"

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// v18.9.9.27: sidecar helpers duplicated here to avoid pulling in
// EpubReaderActivity's private namespace. Same filename + semantics as the
// reader's simpleRenderingSidecarSet / write / clear at
// EpubReaderActivity.cpp:207. If either callsite grows, extract to a shared
// header.
// v18.9.9.58: split per render path so the toggle applies to whichever path
// the book is currently on (see APP_STATE.readerActivePath).
constexpr const char* kCompatFlagPrepared = "/compat_prepared.flag";
constexpr const char* kCompatFlagCustom = "/compat_custom.flag";

const char* activeCompatFlagBasename() {
  return APP_STATE.readerActivePath == 0 ? kCompatFlagPrepared : kCompatFlagCustom;
}

std::string activeBookCachePath() {
  if (APP_STATE.openEpubPath.empty()) return {};
  return Epub::cachePathForFilePath(APP_STATE.openEpubPath, "/.crosspoint");
}

bool readSidecar() {
  const auto cachePath = activeBookCachePath();
  if (cachePath.empty()) return false;
  return Storage.exists((cachePath + activeCompatFlagBasename()).c_str());
}

void writeSidecar() {
  const auto cachePath = activeBookCachePath();
  if (cachePath.empty()) return;
  const std::string path = cachePath + activeCompatFlagBasename();
  if (Storage.exists(path.c_str())) return;
  HalFile f;
  if (Storage.openFileForWrite("ROA", path.c_str(), f)) {
    f.close();
    LOG_INF("ROA", "Compat sidecar written: %s", path.c_str());
  } else {
    LOG_ERR("ROA", "Failed to write compat sidecar: %s", path.c_str());
  }
}

void clearSidecar() {
  const auto cachePath = activeBookCachePath();
  if (cachePath.empty()) return;
  const std::string path = cachePath + activeCompatFlagBasename();
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    LOG_INF("ROA", "Compat sidecar cleared: %s", path.c_str());
  }
}

// The four settings compat overrides (embedded style, images, tables, bionic
// reading, guide reading -- five as of v18.9.9.24). When Compat is on, these
// rows show "(Compat)" and reject toggles. Kept in sync with
// SettingsActivity::isCompatLockedSetting and
// BookSettingsDrawerActivity::isDrawerCompatLockedSetting -- three separate
// activities render the same rows and each needs the same lock behavior.
bool isCompatLockedRow(const SettingInfo& setting) {
  switch (setting.nameId) {
    case StrId::STR_IMAGES:
    case StrId::STR_TABLES:
    case StrId::STR_EMBEDDED_STYLE:
    case StrId::STR_BIONIC_READING:
    case StrId::STR_GUIDE_READING:
      return true;
    default:
      return false;
  }
}
}  // namespace

namespace {
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
}  // namespace

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();

  rebuildSettingsList();
  requestUpdate();
}

void ReaderOptionsActivity::rebuildSettingsList() {
  settings.clear();
  settingsCount = 0;
  selectedIndex = 0;
  lowHeap_ = false;
  viewRows_.clear();
  viewMode_ = false;

  // CrumBLE: getSettingsList() returns a full std::vector<SettingInfo> -- every
  // category, each row carrying nested enumRawValues vectors and std::function
  // getter/setter slots. The temporary plus the std::copy_if into our member
  // vector add up to several KB of churn. Opening Reader Options mid-BLE-read
  // (after image-heavy pages, when free heap is ~25 KB and maxAlloc has
  // collapsed to a few KB) bad_alloc'd inside that build and abort-rebooted
  // the device. Same shape as BookSettingsDrawerActivity's onEnter OOM. Gate
  // matches the drawer's 28 KB free / 14 KB maxAlloc floor; on fail, leave
  // settingsCount=0 and let render() draw an explanatory message instead.
  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, 28u * 1024u, 14u * 1024u)) {
    // v18.9.9.50: try the SD-cached settings snapshot before falling
    // back to the hardcoded low-heap message. The cache has no getters/
    // setters and no per-row vectors, so populating viewRows_ costs
    // only the deserialize + small vector allocations (well under
    // maxAlloc even at ~10 KB). If it loads, we render a view-only
    // list; if it fails, we drop to the original lowHeap_ message.
    if (loadSettingsViewCache(viewRows_) && !viewRows_.empty()) {
      viewMode_ = true;
      settingsCount = static_cast<int>(viewRows_.size());
      LOG_INF("ROA",
              "Low heap (free=%u maxAlloc=%u); rendering view-only list from SD cache (%u rows)",
              heap.freeHeap, heap.maxAllocHeap, static_cast<unsigned>(viewRows_.size()));
      return;
    }
    lowHeap_ = true;
    LOG_INF("ROA", "Low heap (free=%u maxAlloc=%u) and no view cache; refusing to build settings list",
            heap.freeHeap, heap.maxAllocHeap);
    return;
  }

  sdFontSystem.refreshIfDirty();
  const auto allSettings = getSettingsList(&sdFontSystem.registry());
  settings.reserve(allSettings.size() + 3);

  // v18.9.9.27: synthetic "Compatibility Mode" toggle at the top of the list.
  // Enum (On/Off) with dynamic getter/setter that read/write the per-book
  // simple_rendering.flag sidecar. Sidecar is the source of truth for user
  // preference; the reader's simpleRenderingActive_ resyncs from it on the
  // next section load (see the sidecar re-check on the loadSectionFile
  // call path in EpubReaderActivity). Setter also flips
  // APP_STATE.compatModeChanged so the reader-menu return handler treats
  // the toggle like any other layout-affecting setting change and rebuilds.
  {
    SettingInfo compat;
    compat.nameId = StrId::STR_COMPAT_MODE;
    compat.type = SettingType::ENUM;
    compat.enumValues = {StrId::STR_STATE_OFF, StrId::STR_STATE_ON};
    compat.category = StrId::STR_CAT_READER;
    compat.valueGetter = []() -> uint8_t { return readSidecar() ? 1 : 0; };
    compat.valueSetter = [](uint8_t v) {
      if (v == 0) {
        clearSidecar();
        APP_STATE.readerCompatModeActive = false;
        // v18.9.9.59: mark the manual-off so a subsequent Layer 2 auto-write
        // in the same book open queues the "Compatibility Mode required"
        // toast on the next boot.
        APP_STATE.compatUserDisabledThisSession = true;
      } else {
        writeSidecar();
        // Immediate UI signal so decorators pick up the "(Compat)" suffix on
        // the next render tick. simpleRenderingActive_ inside the reader
        // resyncs from the sidecar at rebuild time regardless.
        APP_STATE.readerCompatModeActive = true;
        // v18.9.9.59: user re-enabled manually -- clear the marker so a
        // later Layer 2 auto-write doesn't wrongly credit them with a toast.
        APP_STATE.compatUserDisabledThisSession = false;
      }
      APP_STATE.compatModeChanged = true;
    };
    settings.push_back(std::move(compat));
  }

  std::copy_if(allSettings.begin(), allSettings.end(), std::back_inserter(settings),
               [](const auto& s) { return s.category == StrId::STR_CAT_READER; });

  const auto fontSizeSetting = std::find_if(settings.begin(), settings.end(),
                                            [](const auto& setting) { return setting.nameId == StrId::STR_FONT_SIZE; });
  const auto manageFontsSetting = SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts);
  settings.insert(fontSizeSetting == settings.end() ? settings.end() : fontSizeSetting + 1, manageFontsSetting);
  settings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  settingsCount = static_cast<int>(settings.size());
  selectedIndex = 0;
}

void ReaderOptionsActivity::onExit() { Activity::onExit(); }

std::string ReaderOptionsActivity::viewRowValueText(const SettingsViewRow& row) const {
  if (row.type == SettingType::TOGGLE) {
    return row.currentValue ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
  }
  if (row.type == SettingType::ENUM) {
    // Find the enum entry whose raw value matches the current value; if
    // no match (settings shifted since the cache was written), return the
    // raw number as fallback rather than displaying garbage.
    for (size_t i = 0; i < row.enumRawValues.size(); ++i) {
      if (row.enumRawValues[i] == row.currentValue) {
        if (i < row.enumStringLabels.size() && !row.enumStringLabels[i].empty()) {
          return row.enumStringLabels[i];
        }
        if (i < row.enumStrIds.size() && row.enumStrIds[i] != StrId::STR_NONE_OPT) {
          return std::string(I18N.get(row.enumStrIds[i]));
        }
      }
    }
    return std::to_string(row.currentValue);
  }
  if (row.type == SettingType::VALUE) {
    return std::to_string(row.currentValue);
  }
  if (row.type == SettingType::STRING) {
    return row.stringValue;
  }
  return "";
}

void ReaderOptionsActivity::toggleCurrentSetting() {
  if (selectedIndex < 0 || selectedIndex >= settingsCount) return;
  const auto& setting = settings[selectedIndex];

  // v18.9.9.27: refuse toggles on the four/five rows compat forces at parse
  // time. Same behaviour as SettingsActivity's isCompatLockedSetting reject
  // path -- match the "(Compat)" suffix shown in the value column.
  if (APP_STATE.readerCompatModeActive && isCompatLockedRow(setting)) {
    GUI.drawPopup(renderer, tr(STR_SETTING_LOCKED_BY_COMPAT), 0, false, HalDisplay::NO_REFRESH);
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !cur;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t cur = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, cur);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               sdFontSystem.refreshIfDirty();
                               rebuildSettingsList();
                               requestUpdate();
                             });
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = cur + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (setting.action == SettingAction::DownloadFonts) {
      startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               sdFontSystem.refreshIfDirty();
                               rebuildSettingsList();
                               requestUpdate();
                             });
      return;
    }
    if (setting.action == SettingAction::CustomiseStatusBar) {
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                             [](const ActivityResult&) { SETTINGS.saveToFile(); });
      return;
    }
  }
}

void ReaderOptionsActivity::loop() {
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, settingsCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, settingsCount);
    requestUpdate();
  });

  // CrumBLE: low-heap fallback path has no list to toggle. Ignore Confirm so
  // toggleCurrentSetting() doesn't index settings[0] on an empty vector. Back
  // still works (and we don't call SETTINGS.saveToFile() either, since nothing
  // was changed -- save itself is hardened but skipping is cheaper still).
  if (lowHeap_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // v18.9.9.50: in view-mode the settings[] vector wasn't populated, so
  // Confirm can't drive toggleCurrentSetting(). Redirect to
  // silent-restart-with-OpenReaderOptions so the fresh boot heap can
  // build the live editable list. Back closes as normal.
  if (viewMode_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      LOG_INF("ROA", "View-mode Confirm tapped; silent-restart to open editable Reader Options on fresh heap");
      silentRestartToReaderWithAction(ReaderPostBootAction::OpenReaderOptions);
      // never returns
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    finish();
    return;
  }
}

void ReaderOptionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.buttonHintsHeight : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;

  GUI.drawHeader(renderer, Rect{contentX, metrics.topPadding, contentWidth, metrics.headerHeight},
                 tr(STR_READER_OPTIONS), nullptr);

  if (lowHeap_) {
    // CrumBLE: low-heap fallback. We didn't build the settings list, so don't
    // call GUI.drawList (its label getter indexes settings[i]). Show a brief
    // hardcoded message in the body area instead. Hardcoded English -- this
    // is a rare degradation path; wiring i18n for it isn't worth the 25
    // translation files. Header + Back button hint remain intact so the user
    // knows what they tried to open and how to leave.
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight +
                                         metrics.verticalSpacing * 2);
    const int lineHeight = renderer.getFontAscenderSize(UI_10_FONT_ID) + 4;
    const char* lines[] = {
        "Memory low.",
        "Close Bluetooth from the reader menu",
        "or read further to free memory,",
        "then reopen Reader Options.",
    };
    constexpr int lineCount = sizeof(lines) / sizeof(lines[0]);
    const int blockHeight = lineHeight * lineCount;
    int currentY = listTop + std::max(0, (listHeight - blockHeight) / 2);
    for (int i = 0; i < lineCount; ++i) {
      renderer.drawCenteredText(UI_10_FONT_ID, currentY, lines[i]);
      currentY += lineHeight;
    }
  } else if (viewMode_) {
    // v18.9.9.50: view-only render from the SD-cached snapshot. Same
    // GUI.drawList shape as the normal path but the label / value
    // callbacks read from viewRows_ instead of settings[]. Selection is
    // still shown so users can scroll to spot-check any value; any
    // Confirm tap goes to loop() which redirects to a silent-restart
    // (see loop() view-mode branch).
    GUI.drawList(
        renderer,
        Rect{contentX, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, contentWidth,
             pageHeight -
                 (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight + metrics.verticalSpacing * 2)},
        settingsCount, selectedIndex,
        [this](int i) { return std::string(I18N.get(viewRows_[i].nameId)); }, nullptr, nullptr,
        [this](int i) { return viewRowValueText(viewRows_[i]); }, true);
  } else {
    GUI.drawList(
        renderer,
        Rect{contentX, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, contentWidth,
             pageHeight -
                 (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight + metrics.verticalSpacing * 2)},
        settingsCount, selectedIndex, [this](int i) { return std::string(I18N.get(settings[i].nameId)); }, nullptr,
        nullptr,
        [this](int i) {
          const auto& setting = settings[i];
          std::string valueText;
          if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
            valueText = SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
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
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
          // v18.9.9.27: append the compat marker on the same rows as
          // SettingsActivity + drawer. Skip the compat toggle itself so we
          // don't decorate the row that IS the state.
          if (!valueText.empty() && APP_STATE.readerCompatModeActive && setting.nameId != StrId::STR_COMPAT_MODE &&
              isCompatLockedRow(setting)) {
            valueText += tr(STR_SETTING_COMPAT_SUFFIX);
          }
          return valueText;
        },
        true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), lowHeap_ ? "" : tr(STR_TOGGLE),
                                            lowHeap_ ? "" : tr(STR_DIR_UP), lowHeap_ ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
