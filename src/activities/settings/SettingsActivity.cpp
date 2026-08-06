#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "../../SilentRestart.h"
#include "../boot_sleep/SleepActivity.h"
#include "AppVersion.h"
#include "BluetoothSettingsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include <HalClock.h>
#include "CoverThumbStatus.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/home/HomeActivity.h"
#include "I18n.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "ReadingHeatmapActivity.h"
#include "RestoreCrossPointActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "LibraryIndex.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraFlowTheme.h"
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

// v18.9.9.23: settings that Simple Rendering / Compat mode forces to a fixed
// value at parse time. When APP_STATE.readerCompatModeActive is true (a
// reader is open with simpleRenderingActive_), these show a "· Compat"
// suffix in the value column and refuse to toggle -- otherwise users get
// confused by "Images: display" reading enabled while the page draws
// without images.
bool isCompatLockedSetting(const SettingInfo& setting) {
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

void drawSystemVersionFooter(const GfxRenderer& renderer, const int pageWidth, const int pageHeight,
                             const ThemeMetrics& metrics) {
  // CrumBLE 4.5: dropped the "(CrossInk X.Y.Z-tiny-bitter)" upstream-sync
  // suffix from the user-visible System footer. CROSSINK_VERSION stays in
  // the User-Agent header for OTA / bug-report correlation, just not shown
  // to end users.
  static constexpr const char* versionLabel = "CrumBLE " CRUMBLE_VERSION;
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
  // v18.9.9.306: sleep configuration split into two submenus. The prior
  // combined 7-row Sleep Screen menu conflated "which screen mode" with
  // "how sleep images render/cycle" -- splitting reads faster and lets
  // the two concerns evolve independently.
  //   Sleep Screen : which mode fires (Blank/Custom/Cover/etc), image
  //                  ordering (sequential vs random), Quick Resume.
  //   Sleep Image  : cover crop/fit + filter (grayscale/inverted), tap-
  //                  to-cycle behavior, skip-grayscale-on-cycle toggle.
  std::vector<SettingInfo> displaySleepScreen;
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_SCREEN);
  pushByName(displaySleepScreen, allSettings, StrId::STR_SLEEP_SCREEN_ORDER);
  pushByName(displaySleepScreen, allSettings, StrId::STR_QUICK_RESUME);

  std::vector<SettingInfo> displaySleepImage;
  pushByName(displaySleepImage, allSettings, StrId::STR_SLEEP_COVER_MODE);
  pushByName(displaySleepImage, allSettings, StrId::STR_SLEEP_COVER_FILTER);
  pushByName(displaySleepImage, allSettings, StrId::STR_CYCLE_SCREENSAVER_ON_TAP);
  pushByName(displaySleepImage, allSettings, StrId::STR_SLEEP_CYCLE_SKIP_GRAYSCALE);

  // Display > Theme & Layout: visual chrome for the everything-except-book
  // surfaces (Home header, Settings, File Browser). Customise Status Bar
  // moved OUT to Reader (v18.9.9.343) -- it only affects the in-book
  // status bar, so it belongs with the other reading knobs. STR_HOME_CLOCK
  // is its own toggle here so users can enable the Home header clock
  // without also enabling the in-book status-bar clock (they were
  // conflated on the same setting between v341-v342).
  std::vector<SettingInfo> displayThemeLayout;
  pushByName(displayThemeLayout, allSettings, StrId::STR_UI_THEME);
  pushByName(displayThemeLayout, allSettings, StrId::STR_RECENT_BOOKS_VIEW);
  pushByName(displayThemeLayout, allSettings, StrId::STR_HIDE_BATTERY);
  pushByName(displayThemeLayout, allSettings, StrId::STR_HOME_CLOCK);

  std::vector<SettingInfo> displayChildren;
  displayChildren.push_back(SettingInfo::Submenu(StrId::STR_SLEEP_SCREEN, std::move(displaySleepScreen)));
  displayChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_SLEEP_IMAGE, std::move(displaySleepImage)));
  displayChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_THEME_LAYOUT, std::move(displayThemeLayout)));
  pushByName(displayChildren, allSettings, StrId::STR_SUNLIGHT_FADING_FIX);

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_DISPLAY, std::move(displayChildren)));

  // === Reader submenu =============================================
  std::vector<SettingInfo> readerFont;
  pushByName(readerFont, allSettings, StrId::STR_FONT_FAMILY);
  pushByName(readerFont, allSettings, StrId::STR_FONT_SIZE);
  // v18.9.9.306: UI Font Fallback + Size picker restored for CJK / non-
  // Latin testing. Costs 15-25 KB when a family is picked (loads on first
  // glyph miss). Users who don't pick anything pay nothing. Under BT +
  // CJK, per-image heap gating suppresses more images than usual --
  // accepted trade-off for glyph coverage.
  pushByName(readerFont, allSettings, StrId::STR_UI_FONT_FALLBACK);
  pushByName(readerFont, allSettings, StrId::STR_UI_FONT_FALLBACK_SIZE);
  readerFont.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));

  // v18.9.9.306: Layout keeps geometry-only knobs. Dark Mode moved to
  // Style (it's a visual/theme choice, not a layout dimension).
  // Refresh Frequency moved IN here (from Reader root) -- it controls
  // page-render behaviour, so grouping with the other layout knobs
  // reads better than a bare row at Reader root.
  std::vector<SettingInfo> readerLayout;
  pushByName(readerLayout, allSettings, StrId::STR_LINE_SPACING);
  pushByName(readerLayout, allSettings, StrId::STR_ORIENTATION);
  pushByName(readerLayout, allSettings, StrId::STR_SCREEN_MARGIN);
  pushByName(readerLayout, allSettings, StrId::STR_PARA_ALIGNMENT);
  pushByName(readerLayout, allSettings, StrId::STR_EXTRA_SPACING);
  pushByName(readerLayout, allSettings, StrId::STR_FORCE_PARAGRAPH_INDENTS);
  pushByName(readerLayout, allSettings, StrId::STR_REFRESH_FREQ);

  // v18.9.9.306: Reading Aids submenu dissolved -- Bionic Reading and
  // Guide Dots are text-style choices and belong under Style, next to
  // Hyphenation and text AA.
  std::vector<SettingInfo> readerStyle;
  pushByName(readerStyle, allSettings, StrId::STR_EMBEDDED_STYLE);
  pushByName(readerStyle, allSettings, StrId::STR_HYPHENATION);
  pushByName(readerStyle, allSettings, StrId::STR_TEXT_AA);
  pushByName(readerStyle, allSettings, StrId::STR_READER_DARK_MODE);
  pushByName(readerStyle, allSettings, StrId::STR_BIONIC_READING);
  pushByName(readerStyle, allSettings, StrId::STR_GUIDE_READING);
  // v18.9.9.286: Stable Page Numbers moved to Customise Status Bar for
  // CrossInk-parity placement -- it's fundamentally a status-bar display
  // toggle, not a reading aid. See StatusBarSettingsActivity ITEM_STABLE_PAGE_NUMBERS.

  std::vector<SettingInfo> readerChildren;
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_FONT, std::move(readerFont)));
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_LAYOUT, std::move(readerLayout)));
  readerChildren.push_back(SettingInfo::Submenu(StrId::STR_SETTINGS_STYLE, std::move(readerStyle)));
  // v18.9.9.306: Images promoted to a Reader-root row (out of Style).
  // It's a per-book cost/quality knob, not a text-style choice.
  pushByName(readerChildren, allSettings, StrId::STR_IMAGES);
  // v18.9.9.343: Customise Status Bar back under Reader. It only affects
  // the in-book status bar, so it groups better with the other in-book
  // knobs than with Display>Theme&Layout (which now hosts the separate
  // STR_HOME_CLOCK toggle for the Home-header clock).
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
  // v18.9.9.306: reordered by frequency-of-use. WiFi is the foundation
  // (nothing else works without it), Sync Time is second because it
  // depends on WiFi and drives the reading-stats infra on X4 devices.
  // Bluetooth Setup then KOReader / OPDS follow.
  std::vector<SettingInfo> syncChildren;
  syncChildren.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  // v18.9.9.305: Sync Time (Clock & Stats). Relocated from Customise
  // Status Bar so X4 users (no DS3231 -> that menu never showed clock
  // items) can also trigger an NTP sync. Sets system time on both
  // devices; on X3 also writes the DS3231, on X4 the system clock is
  // what ReadingStats needs to start attributing minutes to a day.
  // v18.9.9.369: promoted to a SUBMENU. Both the NTP fetch and the UTC
  // offset picker are semantically "time configuration", so grouping
  // them together lives at Sync & Network > Sync Time. UTC Offset was
  // previously only reachable via Reader > Customise Status Bar (where
  // it couldn't be found without knowing to look there).
  std::vector<SettingInfo> syncTimeChildren;
  syncTimeChildren.push_back(SettingInfo::Action(StrId::STR_CLOCK_SYNC, SettingAction::ClockSync));
  syncTimeChildren.push_back(SettingInfo::Action(StrId::STR_CLOCK_UTC_OFFSET, SettingAction::ClockUtcOffset));
  syncChildren.push_back(SettingInfo::Submenu(StrId::STR_CLOCK_SYNC_NOW, std::move(syncTimeChildren)));
  // CrumBLE 4.5.3: Bluetooth page-turner pairing was only reachable from the
  // in-reader menu, so first-time users had to open a book before they could
  // bond their remote. Surface the same wizard here so the scan + bond flow
  // is discoverable from cold-boot Settings too. Pairing only -- page-turn
  // events still gate on being in the reader.
  syncChildren.push_back(SettingInfo::Action(StrId::STR_BLUETOOTH_SETUP, SettingAction::PageTurnerSetup));
  syncChildren.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  syncChildren.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_SYNC_NETWORK, std::move(syncChildren)));

  // === Functions submenu ==========================================
  // v18.9.9.306: bulk-action rows that used to hide in Display > General.
  // Grouping them as their own top-level "Functions" submenu makes them
  // easier to find (they aren't display settings, they're one-shot
  // maintenance operations) and clears out Display > General entirely.
  std::vector<SettingInfo> functionsChildren;
  // v18.9.9.212: manual bulk-bake for Flow carousel perspective + center
  // tiles. Complements the automatic lazy-bake so the user isn't
  // surprised by the first-render slowdown on each book.
  functionsChildren.push_back(SettingInfo::Action(StrId::STR_BAKE_COVER_TILES, SettingAction::BakeCoverTiles));
  // v18.9.9.258: bake .slp caches for every /.sleep/ image so runtime
  // sleep entry skips the PNG/BMP decoder + its transient buffers.
  functionsChildren.push_back(SettingInfo::Action(StrId::STR_BAKE_SLEEP_IMAGES, SettingAction::BakeSleepImages));
  // CrumBLE 4.4: re-attempt books whose cover gen failed under an
  // earlier firmware bug.
  functionsChildren.push_back(SettingInfo::Action(StrId::STR_RETRY_FAILED_COVERS, SettingAction::RetryFailedCovers));
  // v18.9.9.222: pick up author-key derivation fixes on pre-existing entries.
  functionsChildren.push_back(SettingInfo::Action(StrId::STR_REBUILD_AUTHOR_KEYS, SettingAction::RebuildAuthorKeys));

  rootSettings_.push_back(SettingInfo::Submenu(StrId::STR_CAT_FUNCTIONS, std::move(functionsChildren)));

  // === System submenu =============================================
  std::vector<SettingInfo> systemChildren;
  pushByName(systemChildren, allSettings, StrId::STR_TIME_TO_SLEEP);
  // v18.9.5: BT auto-disconnect timeout right next to the sleep timeout
  // (same slider UI, same category, closely related concept).
  pushByName(systemChildren, allSettings, StrId::STR_BT_AUTO_DISCONNECT);
  // v4.7.3: "Indexing page X of Y" popup toggle. Registered in SettingsList
  // under STR_CAT_SYSTEM since v18.9.9.172, but the device menus are curated by
  // hand rather than filled from the category, so it only ever appeared in the
  // web UI (which does enumerate by category). Defaults off.
  pushByName(systemChildren, allSettings, StrId::STR_INDEXING_SHOW_PAGE_COUNT);
  systemChildren.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  systemChildren.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemChildren.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemChildren.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));

  // v18.9.9.306: Recovery submenu flattened. Prior structure had a
  // Recovery > Restore CrossPoint sub-of-sub with only one entry, so
  // the wrapper submenu was dead weight. The real safety gate is
  // RestoreCrossPointActivity's hold-to-confirm on entry (plus a
  // battery >50% check), not menu depth -- pulling the submenu saves a
  // tap without weakening the guard. HTTPS-downloads the latest
  // crosspoint-reader release and flashes via the LAN-OTA install path.
  // Stock-firmware restore intentionally absent -- per research, stock
  // .bin isn't reliably redistributable.
  systemChildren.push_back(SettingInfo::Action(StrId::STR_RESTORE_CROSSPOINT, SettingAction::RestoreCrossPoint));

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
  SET_CHECKPOINT("settings:onEnter");
  LOG_INF("SET", "SettingsActivity onEnter: free=%u maxAlloc=%u",
          static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // v18.9.9.339: silent-restart pre-flight. getSettingsList() + view-cache
  // serialisation peaks at ~15-25 KB of allocations. Field crash observed
  // at checkpoint settings:rebuildLists with entry heap free=50756 /
  // maxAlloc=23540 -- one of the internal allocs exceeded 13 KB during
  // rebuild and threw. On a CJK-UI-fallback-loaded Home the maxAlloc
  // typically sits at 8-15 KB, so this is a common bad state. Silent-
  // restart to Settings gives fresh 60+ KB maxAlloc where rebuild fits.
  // isContinuingFromSilentReboot() guard prevents loop.
  constexpr uint32_t kSettingsMinFree = 55U * 1024U;
  constexpr uint32_t kSettingsMinMaxAlloc = 30U * 1024U;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (!isContinuingFromSilentReboot() &&
      (freeHeap < kSettingsMinFree || maxAlloc < kSettingsMinMaxAlloc)) {
    LOG_INF("SET",
            "Settings heap pre-flight tripped (free=%u<%u OR maxAlloc=%u<%u); "
            "silent-restart-to-self to avoid getSettingsList crash",
            freeHeap, static_cast<unsigned>(kSettingsMinFree),
            maxAlloc, static_cast<unsigned>(kSettingsMinMaxAlloc));
    silentRestartToSettings();
    return;  // never reached
  }

  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  // v18.9.9.446 (task #149): pre-arm silent-restart target so a mid-rebuild
  // bad_alloc → terminate lands the user back in Settings (via v440's target
  // preservation), not on Home carousel. Cleared in onExit for normal
  // navigation paths. The 8 magic value is SILENT_REBOOT_TARGET_SETTINGS
  // per main.cpp; using it directly avoids exposing the constant here.
  armSilentRestartTarget(/*SILENT_REBOOT_TARGET_SETTINGS=*/8);

  SET_CHECKPOINT("settings:rebuildLists");
  rebuildSettingsLists();
  SET_CHECKPOINT("settings:onEnter-done");
  LOG_INF("SET", "SettingsActivity onEnter done: free=%u maxAlloc=%u",
          static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
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

  // v18.9.9.446: clear our terminate-recovery arming so a later-session
  // terminate (e.g. an hour later on Home) doesn't get misrouted to
  // Settings. Paired with armSilentRestartTarget in onEnter.
  clearArmedSilentRestartTarget();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed

  // v18.9.9.361: silent-restart-to-Home for a clean single-flash
  // transition (matches FT exit UX). Multi-flash HALF/FULL refresh
  // reads as 3-4 flashes on X4; silent-restart's snapshot-restore
  // looks like one. Skips home-state preserve since we're rebooting.
  silentRestart();
  // never returns
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
  // v18.9.9.23: a reader is open in Compat mode and this setting is one of
  // the four it overrides at parse time. Reject the toggle and briefly show
  // a "locked" popup so the user learns why the value won't change.
  if (APP_STATE.readerCompatModeActive && isCompatLockedSetting(setting)) {
    GUI.drawPopup(renderer, tr(STR_SETTING_LOCKED_BY_COMPAT), 0, false, HalDisplay::NO_REFRESH);
    return;
  }
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  // v18.9.5.2: same interval-picker page as Time to Sleep (bar +/− 1 and
  // ±5 big steps, minute formatting).
  if (setting.nameId == StrId::STR_BT_AUTO_DISCONNECT) {
    openBtAutoDisconnectPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    // v18.9.9.361: mirror StatusBarSettings' ITEM_CLOCK sync trigger --
    // turning Home clock ON with no valid time is a strong signal the
    // user wants a working clock now, not "next reboot". Push
    // ClockSyncActivity inline; requestUpdate on return keeps the user
    // in Settings.
    if (setting.nameId == StrId::STR_HOME_CLOCK && !currentValue && !halClock.hasValidTime()) {
      SETTINGS.saveToFile();
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
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
      case SettingAction::PageTurnerSetup:
        // CrumBLE 4.5.3: launched from Settings (no parent book), so the
        // wizard should return us to Settings on successful bond instead of
        // auto-popping its parent. exitOnSuccessfulConnect=false keeps it
        // open after pair so the user can verify the device list / debug
        // monitor; they back out manually like any other Settings sub-page.
        startActivityForResult(
            std::make_unique<BluetoothSettingsActivity>(renderer, mappedInput, [] { activityManager.popActivity(); },
                                                        /*exitOnSuccessfulConnect=*/false),
            resultHandler);
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
      case SettingAction::ClockSync:
        startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ClockUtcOffset:
        // v18.9.9.369: launch the wheel-picker (same activity used by Reader >
        // Customise Status Bar > UTC Offset). ClockOffsetActivity saves on
        // exit, so resultHandler's saveToFile is a no-op safety belt.
        startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), resultHandler);
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
      case SettingAction::RestoreCrossPoint:
        startActivityForResult(std::make_unique<RestoreCrossPointActivity>(renderer, mappedInput), resultHandler);
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
        // v18.9.9.365: after the popup, silent-restart to Home so shelf
        // re-render gets fresh ~85 KB heap. Under Settings -> Functions,
        // the deep-nested state means silentRestart() lands back on Home
        // (not this menu) -- acceptable trade for actually resolving
        // heap-tight cover failures. See the HomeActivity retry handlers
        // for the same pattern.
        const int removed = CoverThumbStatus::sweepAllMarkers();
        char msg[96];
        if (removed > 0) {
          std::snprintf(msg, sizeof(msg), tr(STR_COVERS_RETRY_DONE), removed);
        } else {
          std::snprintf(msg, sizeof(msg), "%s", tr(STR_COVERS_RETRY_NONE));
        }
        GUI.drawPopup(renderer, msg);
        delay(1200);
        LOG_INF("SET", "Retry failed covers (Settings): swept=%d; silent-restart for fresh heap",
                removed);
        silentRestart();
        // never returns
      }
      case SettingAction::RegenerateAllCovers: {
        // CrumBLE 4.6: destructive sibling of RetryFailedCovers -- deletes every
        // thumb_<W>x<H>.bmp so the firmware regenerates covers using the
        // current Cover Tone setting. Walk + delete is sub-second; the actual
        // regen happens lazily on the next bookshelf paint.
        const int removed = CoverThumbStatus::regenerateAllCovers();
        // CrumBLE 4.6 fix: nuking the SD thumbs isn't enough -- multiple
        // in-RAM caches hold the OLD pre-tone bitmaps and serve them on
        // the next render. Drop them all so the next paint re-reads from
        // SD (which we just nuked), forcing thumb regen with the current
        // tone curve.
        //  - renderer.imageCache: shared bitmap cache (covers both Home and
        //    Bookshelf paint paths)
        //  - HomeActivity carousel/snapshot caches (invalidated lazily on
        //    next onEnter via the gHomeCoversInvalidated flag, since
        //    HomeActivity may be on the back stack with no live instance
        //    here)
        // v18.9.9.206: side-tile cache removed; no per-theme clear.
        renderer.clearImageCache();
        invalidateHomeCoverCachesGlobal();
        char msg[96];
        std::snprintf(msg, sizeof(msg), tr(STR_COVERS_RETRY_DONE), removed);
        GUI.drawPopup(renderer, msg);
        delay(1500);
        requestUpdate();
        break;
      }
      case SettingAction::BakeCoverTiles: {
        // v18.9.9.212: bulk-bake all 3 tile roles (L-side, R-side, center)
        // for every book in RECENT_BOOKS. Skips books whose tiles all
        // pass current format validation (idempotent -- running twice
        // in a row does no work). Blocking with progress popup.
        const auto& books = RecentBooksStore::getInstance().getBooks();
        const int total = static_cast<int>(books.size());
        if (total == 0) {
          GUI.drawPopup(renderer, tr(STR_BAKE_COVERS_NONE));
          delay(1500);
          requestUpdate();
          break;
        }
        const Rect popupRect = GUI.drawPopup(renderer, tr(STR_BAKING_COVERS));
        int totalNew = 0;
        for (int i = 0; i < total; ++i) {
          const int newCount = LyraFlowTheme::bakeAllTilesForCover(renderer, books[i].coverBmpPath);
          if (newCount > 0) totalNew += newCount;
          GUI.fillPopupProgress(renderer, popupRect, (i + 1) * 100 / total);
        }
        char msg[96];
        if (totalNew > 0) {
          std::snprintf(msg, sizeof(msg), tr(STR_BAKE_COVERS_DONE), totalNew);
        } else {
          std::snprintf(msg, sizeof(msg), "%s", tr(STR_BAKE_COVERS_NONE));
        }
        GUI.drawPopup(renderer, msg);
        delay(1500);
        requestUpdate();
        break;
      }
      case SettingAction::ReadingHeatmap: {
        startActivityForResult(std::make_unique<ReadingHeatmapActivity>(renderer, mappedInput),
                               resultHandler);
        break;
      }
      case SettingAction::BakeSleepImages: {
        // v18.9.9.258 -> v18.9.9.260: bake .slp caches for every
        // /.sleep/ (or /sleep/) image. PNG decode requires ~60 KB free
        // heap for PNGdec's working set, but by the time the user has
        // navigated Home -> Settings -> Bake, free heap is typically
        // ~25-30 KB. Rather than run the bake inline and fail every
        // decode, silent-restart to Home with a "pending bake" flag;
        // setup() runs the bake on the fresh ~93 KB heap right after
        // fonts + storage init, then continues to Home normally.
        silentRestartToBakeSleepImages();
        // never returns
        break;
      }
      case SettingAction::RebuildAuthorKeys: {
        // v18.9.9.222: wipe cached author keys so the next Sort by Author
        // re-populates via OPF peek. Picks up v220's ';'-split and
        // trailing-punctuation stripping on pre-existing entries.
        LibraryIndex::getInstance().resetAuthorKeys();
        GUI.drawPopup(renderer, tr(STR_AUTHOR_KEYS_REBUILT));
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

// v18.9.5.2: BT Auto-Disconnect uses the same interval-picker UI as Time
// to Sleep -- same range, same step (1 min), same big-step (5 min), same
// "N min" label format. The only differences are the title StrId and the
// destination field.
void SettingsActivity::openBtAutoDisconnectPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "BtAutoDisconnectInterval", StrId::STR_BT_AUTO_DISCONNECT,
          StrId::STR_SLEEP_TIMER_STEP_HINT, SETTINGS.btAutoDisconnectMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.btAutoDisconnectMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
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
          // v18.9.5.2: BT Auto-Disconnect shares the "N min" formatter so its
          // row reads the same as Time to Sleep.
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP ||
              setting.nameId == StrId::STR_BT_AUTO_DISCONNECT) {
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
        // v18.9.9.23: append the compat marker so users see WHY the setting
        // isn't taking effect while their book is open in Compat mode. Only
        // decorates the four rows compat forces; other rows are unchanged.
        if (APP_STATE.readerCompatModeActive && isCompatLockedSetting(setting)) {
          valueText += tr(STR_SETTING_COMPAT_SUFFIX);
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
    } else if (row.nameId == StrId::STR_TIME_TO_SLEEP ||
               row.nameId == StrId::STR_BT_AUTO_DISCONNECT) {
      // v18.9.5.2: BT Auto-Disconnect opens a picker page, so the confirm
      // hint should read "Select" (Enter) instead of "Toggle".
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
