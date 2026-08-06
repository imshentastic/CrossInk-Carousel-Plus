#include <Arduino.h>
#include <BluetoothHIDManager.h>
#include <BoardConfig.h>   // CrumBLE 4.7.2: panel-controller profile selection
#include <XteinkDetect.h>  // CrumBLE 4.7.2: pre-SD panel-controller probe

// v18.9.9.245: for BT-off-by-default boot-time memory release. Same header
// used by CrossPointWebServerActivity's FT-enter release path.
#ifndef SIMULATOR
#include <esp_bt.h>
#endif

#include <Epub.h>
#include <Epub/parsers/ChapterHtmlSlimParserGuards.h>  // v18.9.3: BT-aware table guard
#include <exception>  // CrumBLE 4.5.5: std::set_terminate trap (see setup())
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <WiFi.h>
#include "WifiCredentialStore.h"
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSpiBus.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#ifdef SIMULATOR
using esp_reset_reason_t = int;
using esp_sleep_wakeup_cause_t = int;
enum : int {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
  ESP_RST_USB,
  ESP_RST_JTAG,
  ESP_RST_EFUSE,
  ESP_RST_PWR_GLITCH,
  ESP_RST_CPU_LOCKUP
};
enum : int {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_ALL,
  ESP_SLEEP_WAKEUP_EXT0,
  ESP_SLEEP_WAKEUP_EXT1,
  ESP_SLEEP_WAKEUP_TIMER,
  ESP_SLEEP_WAKEUP_TOUCHPAD,
  ESP_SLEEP_WAKEUP_ULP,
  ESP_SLEEP_WAKEUP_GPIO,
  ESP_SLEEP_WAKEUP_UART,
  ESP_SLEEP_WAKEUP_WIFI,
  ESP_SLEEP_WAKEUP_COCPU,
  ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG,
  ESP_SLEEP_WAKEUP_BT
};
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_UNKNOWN; }
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
#else
#include <esp_sleep.h>
#include <esp_system.h>
#endif

#include <algorithm>
#include <cstring>

#include "AppVersion.h"
#include "CoverThumbStatus.h"
#include "network/FirmwareFlasher.h"
#include "CrossPointSettings.h"
#include "ReadingStats.h"
#include "CrossPointState.h"
#include "SilentRestart.h"  // CrumBLE 4.4: ReaderPostBootAction enum + decls
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "CollectionsStore.h"
#include "LibraryIndex.h"
#include "RecentBooksStore.h"
#include "Epub/Section.h"
#include "SeriesIndex.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "util/SettingsViewCache.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/home/BookActions.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/reader/StatsBackup.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#ifdef SIMULATOR
#include "simulator/SimulatorSmokeTest.h"
#endif
#include "images/LoadingIcon.h"
#include "images/Logo120.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Updated each main-loop iteration; read by the BLE HID manager via a
// callback so it can decide whether to inject reader-only buttons.
static bool gBluetoothReaderContext = false;

// Fonts
#ifndef OMIT_LEXENDDECA_FONT
#ifndef OMIT_MEDIUM_FONT
EpdFont lexenddeca14RegularFont(&lexenddeca_14_regular);
EpdFont lexenddeca14BoldFont(&lexenddeca_14_bold);
EpdFont lexenddeca14ItalicFont(&lexenddeca_14_italic);
EpdFont lexenddeca14BoldItalicFont(&lexenddeca_14_bolditalic);
EpdFontFamily lexenddeca14FontFamily(&lexenddeca14RegularFont, &lexenddeca14BoldFont, &lexenddeca14ItalicFont,
                                     &lexenddeca14BoldItalicFont);
#endif
#endif  // OMIT_LEXENDDECA_FONT
// CrumBLE: OMIT_CHAREINK_FONT drops the entire CharEink family (see all.h).
#ifndef OMIT_CHAREINK_FONT
#ifndef OMIT_TEENSY_FONT
EpdFont charein8RegularFont(&charein_8_regular);
EpdFont charein8BoldFont(&charein_8_bold);
EpdFont charein8ItalicFont(&charein_8_italic);
EpdFont charein8BoldItalicFont(&charein_8_bolditalic);
EpdFontFamily charein8FontFamily(&charein8RegularFont, &charein8BoldFont, &charein8ItalicFont, &charein8BoldItalicFont);
#endif
#ifndef OMIT_ITTY_BITTY_FONT
EpdFont charein9RegularFont(&charein_9_regular);
EpdFont charein9BoldFont(&charein_9_bold);
EpdFont charein9ItalicFont(&charein_9_italic);
EpdFont charein9BoldItalicFont(&charein_9_bolditalic);
EpdFontFamily charein9FontFamily(&charein9RegularFont, &charein9BoldFont, &charein9ItalicFont, &charein9BoldItalicFont);
#endif
#ifndef OMIT_TINY_FONT
EpdFont charein10RegularFont(&charein_10_regular);
EpdFont charein10BoldFont(&charein_10_bold);
EpdFont charein10ItalicFont(&charein_10_italic);
EpdFont charein10BoldItalicFont(&charein_10_bolditalic);
EpdFontFamily charein10FontFamily(&charein10RegularFont, &charein10BoldFont, &charein10ItalicFont,
                                  &charein10BoldItalicFont);
#endif
#ifndef OMIT_SMALL_FONT
EpdFont charein12RegularFont(&charein_12_regular);
EpdFont charein12BoldFont(&charein_12_bold);
EpdFont charein12ItalicFont(&charein_12_italic);
EpdFont charein12BoldItalicFont(&charein_12_bolditalic);
EpdFontFamily charein12FontFamily(&charein12RegularFont, &charein12BoldFont, &charein12ItalicFont,
                                  &charein12BoldItalicFont);
#endif
#ifndef OMIT_MEDIUM_FONT
EpdFont charein14RegularFont(&charein_14_regular);
EpdFont charein14BoldFont(&charein_14_bold);
EpdFont charein14ItalicFont(&charein_14_italic);
EpdFont charein14BoldItalicFont(&charein_14_bolditalic);
EpdFontFamily charein14FontFamily(&charein14RegularFont, &charein14BoldFont, &charein14ItalicFont,
                                  &charein14BoldItalicFont);
#endif
#ifndef OMIT_LARGE_FONT
EpdFont charein16RegularFont(&charein_16_regular);
EpdFont charein16BoldFont(&charein_16_bold);
EpdFont charein16ItalicFont(&charein_16_italic);
EpdFont charein16BoldItalicFont(&charein_16_bolditalic);
EpdFontFamily charein16FontFamily(&charein16RegularFont, &charein16BoldFont, &charein16ItalicFont,
                                  &charein16BoldItalicFont);
#endif
#ifndef OMIT_XLARGE_FONT
EpdFont charein18RegularFont(&charein_18_regular);
EpdFont charein18BoldFont(&charein_18_bold);
EpdFont charein18ItalicFont(&charein_18_italic);
EpdFont charein18BoldItalicFont(&charein_18_bolditalic);
EpdFontFamily charein18FontFamily(&charein18RegularFont, &charein18BoldFont, &charein18ItalicFont,
                                  &charein18BoldItalicFont);
#endif
#ifndef OMIT_HUGE_FONT
EpdFont charein20RegularFont(&charein_20_regular);
EpdFont charein20BoldFont(&charein_20_bold);
EpdFont charein20ItalicFont(&charein_20_italic);
EpdFont charein20BoldItalicFont(&charein_20_bolditalic);
EpdFontFamily charein20FontFamily(&charein20RegularFont, &charein20BoldFont, &charein20ItalicFont,
                                  &charein20BoldItalicFont);
#endif
#endif  // OMIT_CHAREINK_FONT
// CrumBLE: OMIT_LEXENDDECA_FONT drops the entire Lexend Deca family (see all.h).
#ifndef OMIT_LEXENDDECA_FONT
#ifndef OMIT_TEENSY_FONT
EpdFont lexenddeca8RegularFont(&lexenddeca_8_regular);
EpdFont lexenddeca8BoldFont(&lexenddeca_8_bold);
EpdFont lexenddeca8ItalicFont(&lexenddeca_8_italic);
EpdFont lexenddeca8BoldItalicFont(&lexenddeca_8_bolditalic);
EpdFontFamily lexenddeca8FontFamily(&lexenddeca8RegularFont, &lexenddeca8BoldFont, &lexenddeca8ItalicFont,
                                    &lexenddeca8BoldItalicFont);
#endif
#ifndef OMIT_ITTY_BITTY_FONT
EpdFont lexenddeca9RegularFont(&lexenddeca_9_regular);
EpdFont lexenddeca9BoldFont(&lexenddeca_9_bold);
EpdFont lexenddeca9ItalicFont(&lexenddeca_9_italic);
EpdFont lexenddeca9BoldItalicFont(&lexenddeca_9_bolditalic);
EpdFontFamily lexenddeca9FontFamily(&lexenddeca9RegularFont, &lexenddeca9BoldFont, &lexenddeca9ItalicFont,
                                    &lexenddeca9BoldItalicFont);
#endif
#ifndef OMIT_TINY_FONT
EpdFont lexenddeca10RegularFont(&lexenddeca_10_regular);
EpdFont lexenddeca10BoldFont(&lexenddeca_10_bold);
EpdFont lexenddeca10ItalicFont(&lexenddeca_10_italic);
EpdFont lexenddeca10BoldItalicFont(&lexenddeca_10_bolditalic);
EpdFontFamily lexenddeca10FontFamily(&lexenddeca10RegularFont, &lexenddeca10BoldFont, &lexenddeca10ItalicFont,
                                     &lexenddeca10BoldItalicFont);
#endif
#ifndef OMIT_SMALL_FONT
EpdFont lexenddeca12RegularFont(&lexenddeca_12_regular);
EpdFont lexenddeca12BoldFont(&lexenddeca_12_bold);
EpdFont lexenddeca12ItalicFont(&lexenddeca_12_italic);
EpdFont lexenddeca12BoldItalicFont(&lexenddeca_12_bolditalic);
EpdFontFamily lexenddeca12FontFamily(&lexenddeca12RegularFont, &lexenddeca12BoldFont, &lexenddeca12ItalicFont,
                                     &lexenddeca12BoldItalicFont);
#endif
#ifndef OMIT_LARGE_FONT
EpdFont lexenddeca16RegularFont(&lexenddeca_16_regular);
EpdFont lexenddeca16BoldFont(&lexenddeca_16_bold);
EpdFont lexenddeca16ItalicFont(&lexenddeca_16_italic);
EpdFont lexenddeca16BoldItalicFont(&lexenddeca_16_bolditalic);
EpdFontFamily lexenddeca16FontFamily(&lexenddeca16RegularFont, &lexenddeca16BoldFont, &lexenddeca16ItalicFont,
                                     &lexenddeca16BoldItalicFont);
#endif
#ifndef OMIT_XLARGE_FONT
EpdFont lexenddeca18RegularFont(&lexenddeca_18_regular);
EpdFont lexenddeca18BoldFont(&lexenddeca_18_bold);
EpdFont lexenddeca18ItalicFont(&lexenddeca_18_italic);
EpdFont lexenddeca18BoldItalicFont(&lexenddeca_18_bolditalic);
EpdFontFamily lexenddeca18FontFamily(&lexenddeca18RegularFont, &lexenddeca18BoldFont, &lexenddeca18ItalicFont,
                                     &lexenddeca18BoldItalicFont);
#endif
#ifndef OMIT_HUGE_FONT
EpdFont lexenddeca20RegularFont(&lexenddeca_20_regular);
EpdFont lexenddeca20BoldFont(&lexenddeca_20_bold);
EpdFont lexenddeca20ItalicFont(&lexenddeca_20_italic);
EpdFont lexenddeca20BoldItalicFont(&lexenddeca_20_bolditalic);
EpdFontFamily lexenddeca20FontFamily(&lexenddeca20RegularFont, &lexenddeca20BoldFont, &lexenddeca20ItalicFont,
                                     &lexenddeca20BoldItalicFont);
#endif
#endif  // OMIT_LEXENDDECA_FONT

// CrumBLE 4.2.1: OMIT_BITTER_FONT drops the entire Bitter family (see all.h).
// Used by the tiny-lexend and tiny-chareink variant builds.
#ifndef OMIT_BITTER_FONT
#ifndef OMIT_TEENSY_FONT
EpdFont bitter8RegularFont(&bitter_8_regular);
EpdFont bitter8BoldFont(&bitter_8_bold);
EpdFont bitter8ItalicFont(&bitter_8_italic);
EpdFont bitter8BoldItalicFont(&bitter_8_bolditalic);
EpdFontFamily bitter8FontFamily(&bitter8RegularFont, &bitter8BoldFont, &bitter8ItalicFont, &bitter8BoldItalicFont);
#endif
#ifndef OMIT_ITTY_BITTY_FONT
EpdFont bitter9RegularFont(&bitter_9_regular);
EpdFont bitter9BoldFont(&bitter_9_bold);
EpdFont bitter9ItalicFont(&bitter_9_italic);
EpdFont bitter9BoldItalicFont(&bitter_9_bolditalic);
EpdFontFamily bitter9FontFamily(&bitter9RegularFont, &bitter9BoldFont, &bitter9ItalicFont, &bitter9BoldItalicFont);
#endif
#ifndef OMIT_TINY_FONT
EpdFont bitter10RegularFont(&bitter_10_regular);
EpdFont bitter10BoldFont(&bitter_10_bold);
EpdFont bitter10ItalicFont(&bitter_10_italic);
EpdFont bitter10BoldItalicFont(&bitter_10_bolditalic);
EpdFontFamily bitter10FontFamily(&bitter10RegularFont, &bitter10BoldFont, &bitter10ItalicFont, &bitter10BoldItalicFont);
#endif
#ifndef OMIT_SMALL_FONT
EpdFont bitter12RegularFont(&bitter_12_regular);
EpdFont bitter12BoldFont(&bitter_12_bold);
EpdFont bitter12ItalicFont(&bitter_12_italic);
EpdFont bitter12BoldItalicFont(&bitter_12_bolditalic);
EpdFontFamily bitter12FontFamily(&bitter12RegularFont, &bitter12BoldFont, &bitter12ItalicFont, &bitter12BoldItalicFont);
#endif
#ifndef OMIT_MEDIUM_FONT
EpdFont bitter14RegularFont(&bitter_14_regular);
EpdFont bitter14BoldFont(&bitter_14_bold);
EpdFont bitter14ItalicFont(&bitter_14_italic);
EpdFont bitter14BoldItalicFont(&bitter_14_bolditalic);
EpdFontFamily bitter14FontFamily(&bitter14RegularFont, &bitter14BoldFont, &bitter14ItalicFont, &bitter14BoldItalicFont);
#endif
#ifndef OMIT_LARGE_FONT
EpdFont bitter16RegularFont(&bitter_16_regular);
EpdFont bitter16BoldFont(&bitter_16_bold);
EpdFont bitter16ItalicFont(&bitter_16_italic);
EpdFont bitter16BoldItalicFont(&bitter_16_bolditalic);
EpdFontFamily bitter16FontFamily(&bitter16RegularFont, &bitter16BoldFont, &bitter16ItalicFont, &bitter16BoldItalicFont);
#endif
#ifndef OMIT_XLARGE_FONT
EpdFont bitter18RegularFont(&bitter_18_regular);
EpdFont bitter18BoldFont(&bitter_18_bold);
EpdFont bitter18ItalicFont(&bitter_18_italic);
EpdFont bitter18BoldItalicFont(&bitter_18_bolditalic);
EpdFontFamily bitter18FontFamily(&bitter18RegularFont, &bitter18BoldFont, &bitter18ItalicFont, &bitter18BoldItalicFont);
#endif
#ifndef OMIT_HUGE_FONT
EpdFont bitter20RegularFont(&bitter_20_regular);
EpdFont bitter20BoldFont(&bitter_20_bold);
EpdFont bitter20ItalicFont(&bitter_20_italic);
EpdFont bitter20BoldItalicFont(&bitter_20_bolditalic);
EpdFontFamily bitter20FontFamily(&bitter20RegularFont, &bitter20BoldFont, &bitter20ItalicFont, &bitter20BoldItalicFont);
#endif
#endif  // OMIT_BITTER_FONT

EpdFont smallFont(&inter_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&inter_10_regular);
EpdFont ui10BoldFont(&inter_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&inter_12_regular);
EpdFont ui12BoldFont(&inter_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Set when the screenshot combo (Power + Volume Down) fires, so the subsequent
// power button release does not also trigger a short-press action (e.g. sleep).
static bool screenshotComboHandled = false;

const char* resetReasonName(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

const char* wakeupCauseName(const esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    case ESP_SLEEP_WAKEUP_ALL:
      return "ALL";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:
      return "UART";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU:
      return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
      return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT:
      return "BT";
    default:
      return "UNKNOWN";
  }
}

const char* wakeupRouteName(const HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "PowerButton";
    case HalGPIO::WakeupReason::AfterFlash:
      return "AfterFlash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "AfterUSBPower";
    case HalGPIO::WakeupReason::Other:
    default:
      return "Other";
  }
}

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;

// v18.9.9.446: SEPARATE terminate-recovery arming (not the boot-dispatcher magic).
// Set by armSilentRestartTarget() from activity onEnter. Terminate handler
// consults it to pick the recovery target, but boot dispatcher IGNORES it
// entirely — otherwise a plain sleep+wake would trip the "silent-restart to
// last-armed activity" path (v447 bug where wake from sleep landed in Settings).
RTC_NOINIT_ATTR uint32_t terminateRecoveryMagic;
RTC_NOINIT_ATTR uint32_t terminateRecoveryTarget;
constexpr uint32_t TERMINATE_RECOVERY_MAGIC = 0x7E12ADEC;

// v18.9.9.1: set ONLY by the std::terminate handler. Signals the boot path
// to skip the seamless/HALF-refresh silent-resume paint and force a full
// panel resync + FULL refresh over a clean framebuffer. Without this a
// terminate that fires mid-render leaves the panel in a partial state
// which the HALF resume then paints OVER, producing heavy ghosting.
RTC_NOINIT_ATTR uint32_t silentRebootHardRestart;
constexpr uint32_t SILENT_REBOOT_HARD_RESTART_MAGIC = 0xC0DEDEAD;
// v18.9.9.5: Level 1 defrag retry magic. Set by the reader's page-load-refuse
// path BEFORE calling silentRestartToReaderWithAction(EnableBt). Persists
// across the restart so boot 2's reader knows "we already spent our one-shot
// defrag budget for this book -- next failure must escalate to content
// suppression (Level 2 tables-only) or full Simple Rendering (Level 3),
// not another defrag hop that would just repeat the same crash".
RTC_NOINIT_ATTR uint32_t silentRebootDefragRetryMagic;
constexpr uint32_t SILENT_REBOOT_DEFRAG_RETRY_MAGIC = 0xDEF4A611;
// v18.9.9.40 (task #25): XTC-scoped defrag continuation magic. Separate
// from the EPUB reader's magic above so a mid-session EPUB defrag hop
// doesn't consume the XTC restart budget (or vice versa). Set only by
// silentRestartToXtcReaderWithDefragRetry, cleared at boot after being
// snapshotted into g_xtcDefragRetryContinuation.
RTC_NOINIT_ATTR uint32_t silentRebootDefragXtcMagic;
constexpr uint32_t SILENT_REBOOT_DEFRAG_XTC_MAGIC = 0xDEF4A6C0;
// CrumBLE: optional mode hint for SILENT_REBOOT_TARGET_FILE_TRANSFER. Set
// by the FT activity right before silentRestartToFileTransfer so the
// next boot's FT entry can skip NetworkModeSelectionActivity and go
// straight to onNetworkModeSelected(<saved>). 0 = no hint, fall through
// to the normal mode-picker. 1 = JOIN_NETWORK, 2 = CREATE_HOTSPOT.
RTC_NOINIT_ATTR uint32_t silentRebootFtModeHint;
// v18.9.9.86: 1 when the previous FT session silent-restarted because an HTML
// serve (typically /files @ 30 KB gzip) couldn't fit contiguous heap. Set by
// the html-serve auto-recovery branch in CrossPointWebServerActivity; read
// and cleared in FT onEnter so the next html-serve failure this cycle bails
// instead of looping. RTC_NOINIT so it survives the restart. Separate from
// silentRebootFtModeHint so we still auto-restore WiFi mode (hint stays 1/2).
RTC_NOINIT_ATTR uint32_t g_ftHtmlServeLowHeapRestart;
// CrumBLE 4.4 post-bisect: post-boot action queued by silentRestartToReaderWithAction.
// Holds a ReaderPostBootAction value (cast to uint32_t).
RTC_NOINIT_ATTR uint32_t silentRebootReaderPostAction;
// CrumBLE 4.4 post-bisect: target spine for ResumeAtSpine post-boot action.
RTC_NOINIT_ATTR uint32_t silentRebootTargetSpine;
// v18.9.9.25: target drawer group id for OpenBookSettingsDrawer post-boot
// action. Set by silentRestartToReaderOpeningDrawerAt when the drawer's
// ensureSettingsSrcBuilt refuses on tight heap. On boot the reader opens
// the drawer with this group pre-expanded so the user doesn't have to
// re-tap after the silent-restart.
RTC_NOINIT_ATTR uint32_t silentRebootDrawerExpandGroup;
// v18.9.9.343: Marks that the current boot chain has already attempted the
// auto Home clock sync. Set by HomeActivity right before firing
// silentRestartToHomeClockSync(); cleared on cold-boot detection.
// Prevents a retry loop when NTP sync fails (halClock.hasValidTime()
// stays false so the Home onEnter check would otherwise trip every
// return-to-Home).
constexpr uint32_t HOME_BOOT_CLOCK_SYNC_ATTEMPTED_MAGIC = 0xC10CB007u;
RTC_NOINIT_ATTR uint32_t homeBootClockSyncAttemptedMagic;
// v18.9.9.58: reader's active render path (0=PreparedLayout, 1=CustomSettings)
// preserved across silent-restart so the reader doesn't drop to CustomSettings
// after each restart and lose track of a Prepared-layout choice the user
// already answered this book. Magic guards against uninitialized RTC on cold
// boot (which reads back garbage) -- if magic doesn't match, the reader
// re-determines at book open from prebake+SETTINGS match.
constexpr uint32_t READER_ACTIVE_PATH_MAGIC = 0x5250534Bu;  // 'RPSK'
RTC_NOINIT_ATTR uint32_t silentRebootReaderActivePathMagic;
RTC_NOINIT_ATTR uint32_t silentRebootReaderActivePath;
// v18.9.9.59: "Compatibility Mode required" toast arm. Layer 2 sets this
// when it force-re-enables compat AND APP_STATE.compatUserDisabledThisSession
// was true (the user had just manually turned it off in the drawer or ROA).
// Reader's onEnter checks and consumes; on match, briefly draws a popup
// explaining why compat came back on before dismissing to the normal render.
constexpr uint32_t COMPAT_TOAST_MAGIC = 0x43544D41u;  // 'CTMA'
RTC_NOINIT_ATTR uint32_t silentRebootCompatToastMagic;
// v18.9.9.438: chapter-heap-refuse toast state. When the reader hits a
// prebake+heap-refuse failure post-silent-restart (see EpubReaderActivity
// escalation gate v437), we DON'T write compat_custom.flag (would abandon
// prebake -- wrong lesson). Instead: silent-restart-to-reader WITHOUT the
// resumeSpine override so progress.bin's last-committed spine is used,
// and arm this toast so the next boot's reader onEnter shows a short
// popup explaining what happened. High 16 bits store magic (validity),
// low 16 bits store the target spine index the user tried to jump to.
constexpr uint32_t CHAP_HEAP_TOAST_MAGIC_MASK = 0x48545400u;  // 'HTT\0'
RTC_NOINIT_ATTR uint32_t silentRebootChapterHeapToast;
// CrumBLE 4.4 post-bisect: word string for OpenDefinition action. 63
// chars + null is enough for any single dictionary lookup word.
RTC_NOINIT_ATTR char silentRebootDefinitionWord[64];
// v18.9.9.249: for chunked-reader silent-restart. When Down at a chunk
// boundary can't safely load the next chunk on the current heap, we
// arm a silentRestartToReaderWithDefinitionAtChunk that carries not
// only the word but ALSO the byte offset within the target entry we
// want to open on. Post-boot dispatch consumes this and passes it to
// the word-select activity; performDefinitionLookup calls
// loadChunkForCurrentEntry(chunkStart) instead of the hardcoded 0u so
// the user lands on the same chunk they were trying to page into.
// UINT32_MAX means "no chunk offset queued -- start-of-entry".
constexpr uint32_t DEFINITION_CHUNK_START_NONE = 0xFFFFFFFFu;
RTC_NOINIT_ATTR uint32_t silentRebootDefinitionChunkStart;
// CrumBLE 4.6: OTA install state persisted across silent-restart. After the
// check-for-update handshake confirms a new version and the user accepts,
// we silent-restart so the install begins with a fresh ~94KB heap budget --
// the post-check residue + LWIP keep-alive eats ~17KB that mbedtls SSL
// setup needs for the install download's own handshake.
RTC_NOINIT_ATTR char silentRebootOtaUrl[256];
RTC_NOINIT_ATTR uint32_t silentRebootOtaSize;
RTC_NOINIT_ATTR char silentRebootOtaVersion[40];
// v18.9.9.37 (task #22): XTC file path override for silent-restart-to-XTC.
// XtcReaderActivity's renderPage() needs a ~96 KB contiguous page buffer
// alloc; on a fragmented heap that malloc fails and the viewer used to
// dead-end at "Memory error and nothing else". When we hit the failure
// with defrag budget available, we set this path and silent-restart --
// boot dispatch reads it and re-opens the file via the same
// ReaderActivity path that home->tap uses (extension detection routes
// to XtcReaderActivity). Cleared at boot alongside other RTC vars.
RTC_NOINIT_ATTR char silentRebootXtcPath[256];

// v18.9.9.397: parallel RTC-preserved EPUB path for silent-restart-to-reader.
// Field bug: chapter jump silent-restarted successfully at spine=8, but the
// boot's CPS load failed (JSON parse error: IncompleteInput on an SD with
// a bad cache dir), so APP_STATE.openEpubPath came back empty and the boot
// dispatcher fell through to Home. RTC survives the CPS corruption. Boot
// prefers this path over APP_STATE.openEpubPath when the magic is valid.
// Cleared after successful consume, same lifecycle as silentRebootXtcPath.
constexpr uint32_t SILENT_REBOOT_EPUB_PATH_MAGIC = 0xB00C1EAB;
RTC_NOINIT_ATTR uint32_t silentRebootEpubPathMagic;
RTC_NOINIT_ATTR char silentRebootEpubPath[256];
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
// CrumBLE: heap-defrag reboot from the FT web server when the heap is
// too fragmented to serve. After this reboot, setup() routes straight
// to CrossPointWebServerActivity so the user doesn't have to
// re-navigate to File Transfer from Home -- mirrors the path they'd
// take by hitting Back manually but skips the trip through the menu.
constexpr uint32_t SILENT_REBOOT_TARGET_FILE_TRANSFER = 2;
// CrumBLE 4.5: heap-defrag reboot from OtaUpdateActivity when the pre-flight
// finds MaxAlloc below the SSL-handshake floor. After this reboot, setup()
// routes straight to OtaUpdateActivity so the user doesn't see Home flash
// through. They re-select WiFi but on a fresh ~115KB heap the mbedtls SSL
// setup (~40-50KB cert + handshake) fits comfortably.
constexpr uint32_t SILENT_REBOOT_TARGET_OTA_UPDATE = 3;
// CrumBLE 4.6: silent-restart between check-for-update and install. After
// check completes and the user confirms install, we silent-restart so the
// install's own HTTPS handshake (separate connection to the CDN) runs on a
// fresh heap rather than fighting with the post-check residue (~17KB lost
// to LWIP keep-alive + HTTP client cleanup). On this boot the saved URL +
// size + version are pulled from silentRebootOta* RTC vars and fed straight
// to OtaUpdater so the install bypasses the check phase.
constexpr uint32_t SILENT_REBOOT_TARGET_OTA_INSTALL = 4;

// CrumBLE 4.5.3: BT enable / scan needs ~66 KB free heap + ~8 KB MaxAlloc.
// In a typical reading session (book open, glyph caches warm) that's
// rarely available. Previously the user got "Memory low. Restart device."
// and had to power-cycle manually. This target lets BluetoothSettings
// silent-restart itself when its pre-flight fails; post-restart heap is
// ~150 KB free + ~100 KB MaxAlloc so the next enable/scan attempt
// proceeds. g_postBtSilentReboot below guards against an infinite loop
// if even a fresh boot is somehow under the floor.
constexpr uint32_t SILENT_REBOOT_TARGET_BT_SETTINGS = 5;
// CrumBLE 4.5.4: KOReader auth needs WiFi (+58 KB) + HTTPS handshake
// (~40-50 KB contiguous for mbedtls cert chain) -- same heap profile as
// BT enable. Field reports of "Memory low. Restart device." in mid-
// reading sessions traced to ~30-50 KB MaxAlloc after a book had been
// open. This target self-restarts the activity onto a ~150 KB free heap
// so the auth POST completes. g_postKoreaderSilentReboot guards against
// looping if a fresh boot is also under floor (e.g. wifi-enabled-at-
// boot eats too much).
constexpr uint32_t SILENT_REBOOT_TARGET_KOREADER_AUTH = 6;
// CrumBLE 4.5.4: OPDS browser hits the same wifi+https heap profile,
// plus the OPDS feed parse which can be 100+ KB of XML through the
// parser's chunk buffer. Pre-flight + silent-restart-to-self with the
// same shape as KOReader. Post-restart dispatch goes back through
// goToBrowser() which picks single-server-direct OR server-picker as
// appropriate, so we don't have to persist server selection across the
// restart.
constexpr uint32_t SILENT_REBOOT_TARGET_OPDS_BROWSER = 7;
// v18.9.9.308: Manage Fonts hits the same WiFi + HTTPS heap profile as
// KOReader/OPDS. Field crash: user entered at maxAlloc=26612, WiFi RX
// init failed to allocate 4 buffers, then dereferenced null -> panic.
// Same pre-flight + silent-restart-to-self shape; post-restart dispatch
// lands the user back in Manage Fonts on a fresh ~150 KB heap so their
// navigation intent is preserved (vs dumping to Home and making them
// re-navigate through Settings > Reader > Font). g_postFontDownloadSilentReboot
// guards against loop if a fresh boot is somehow also under floor.
constexpr uint32_t SILENT_REBOOT_TARGET_FONT_DOWNLOAD = 9;
// v18.9.9.336: WifiSelectionActivity direct dispatch. Same pattern as font
// download: user hits WiFi from Settings, and rather than call WiFi.mode
// on the current (possibly heap-fragmented) state -- where ESP-IDF's
// wpa_supplicant/eloop.c has been observed to null-deref on init -- we
// silent-restart to this target which lands cleanly with ~150 KB free
// heap. g_postWifiSelectionSilentReboot guards against loop.
constexpr uint32_t SILENT_REBOOT_TARGET_WIFI_SELECTION = 10;
// v18.9.9.337: same pattern for ClockSync. ClockSync fires WiFi.begin() to
// reach NTP; user-observed crash: MEPC 0x4218dbf2 (wpa_supplicant/eloop.c)
// on the WiFi.mode(WIFI_STA) call inside ClockSync's runSync() when the
// in-book fragmented heap left wpa_supplicant in a null-derefable state.
constexpr uint32_t SILENT_REBOOT_TARGET_CLOCK_SYNC = 11;
// v18.9.9.343: same target as CLOCK_SYNC but with a return-to-Home flag.
// Fired from HomeActivity's first-entry boot-sync check when the user has
// SETTINGS.homeClockShow on but time hasn't been NTP-synced yet (X4).
// Boot lands lean in ClockSync, runs the sync, then silent-restarts back
// to Home so the clock shows without any user interaction. Distinct
// target so the return-to-Home behaviour doesn't fire when the user
// entered ClockSync manually via Settings > Sync & Network > Sync Time.
constexpr uint32_t SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC = 12;
// v18.9.5: Back-from-BT-menu w/ disableOnExit path lands here so the user
// stays in Settings (one level up from BT Setup) instead of getting dumped
// on Home. Same defrag purpose as SILENT_REBOOT_TARGET_HOME -- fresh heap
// after NimBLE teardown -- but preserves the "one Back = one level up"
// navigation model.
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 8;

// CrumBLE 4.5.4: auto-recover from a panic mid-WS-upload by silent-restarting
// straight back to FT instead of cold-booting into Home. Without this, a
// device that crashes while serving a long upload (heap pressure, panic, etc.)
// comes back into Home with no web server -- the user has no way to know
// they need to manually re-enter FT to continue, and the browser's auto-
// retry loop just spins on a closed port. With this:
//
//   1. WS upload START accept sets ftUploadInProgressFlag = MAGIC.
//   2. WS upload DONE / abort / FT exit clears it back to 0.
//   3. On boot, if flag == MAGIC and we're NOT in any other silent-restart
//      path, increment ftUploadResumeFailCount + silentRestartToFileTransfer.
//      Browser's WS retry naturally reconnects + the server's RESUME protocol
//      picks up at the saved byte offset, so no progress is lost.
//   4. Counter guards against infinite-panic loops: after MAX consecutive
//      auto-resume attempts (FT mode itself crashes), we clear the flag and
//      fall through to normal Home boot so user can at least navigate.
//
// Counter resets to 0 on every clean upload completion / FT exit, so a one-
// off panic doesn't permanently burn the retry budget.
RTC_NOINIT_ATTR uint32_t ftUploadInProgressFlag;
RTC_NOINIT_ATTR uint32_t ftUploadResumeFailCount;
constexpr uint32_t FT_UPLOAD_FLAG_MAGIC = 0xF7AB1234;

// v18.9.9.438: rolling panic count for FT auto-recovery. v403 killed the
// auto-recovery mechanism entirely because the pre-flight silent-restart
// dispatch hit UI_12-not-yet-registered null-deref during the pre-boot
// window (see line ~2595 for the full rationale). This variant sidesteps
// that: instead of triggering a silent-restart from the early boot check,
// we set a "route to FT after normal setup" flag consumed by the boot
// activity dispatcher (line ~3200), which runs AFTER setupDisplayAnd
// Fonts. The rate limit caps how many consecutive panics can auto-recover
// before we give up and land at Home. Reset to 0 on any clean upload
// completion (setFtUploadInProgress(false)); incremented once per panic-
// during-upload boot.
RTC_NOINIT_ATTR uint32_t ftPanicRecoveryAttempts;
constexpr uint32_t FT_PANIC_RECOVERY_MAX_ATTEMPTS = 3;
// Consumed by the boot activity dispatcher; set by the panic-check when
// rate-limit budget allows auto-recovery. Volatile RAM only; the RTC
// counter above holds the persistent state.
bool g_ftPanicRecoveryPendingToFt = false;

// 4.5.5+: one-shot "I already rebooted for cover-gen heap recovery this
// boot session" flag. Used by HomeActivity::loadShelfCovers: if it sees
// fragmented heap (maxAlloc < 40 KB) AND has missing thumbs to gen AND
// this flag is unset, it sets the flag and silentRestart()s. After
// restart, the fresh ~85 KB heap makes cover gen succeed, and the flag
// is consumed (cleared after a successful render so the next idle->wake
// cycle can use the mechanism again). If the flag is ALREADY set on
// entry, loadShelfCovers falls through to placeholder rendering -- no
// second restart, no boot loop. RTC NOINIT survives reboot but is reset
// to 0 on cold boot (esp32 RTC memory is uninitialized on power-up).
RTC_NOINIT_ATTR uint32_t coverHeapRestartFlag;
constexpr uint32_t COVER_HEAP_RESTART_MAGIC = 0xC0BE4500;

// v18.9.9.260: pending "Bake sleep images" run at next boot. Sleep-image
// PNG decode needs ~60 KB contiguous free heap for the PNGdec working set
// -- Settings > Bake Sleep Images fires from a mid-session heap where
// free heap has already dropped to ~27 KB, so every decode fails. The
// action now silent-restarts to Home with this magic set; boot's setup()
// picks it up right after fonts+storage init (free ~93 KB, maxAlloc ~61
// KB) and runs the bake there before activity dispatch. Naturally zero
// on cold boot / power-cycle so a crash mid-bake doesn't re-enter the
// bake loop.
RTC_NOINIT_ATTR uint32_t pendingBakeSleepImagesFlag;
constexpr uint32_t PENDING_BAKE_SLEEP_MAGIC = 0xC0BE5EEDu;

// CrumBLE 4.5.7: when silent-restart-to-bt-settings fires from the reader's
// BT enable pre-flight defrag path, remember to return to the reader when
// the user Backs out of BT settings. Without this flag, Back pops to Home
// (the empty-stack default), so the user's "connect BT then keep reading"
// journey drops them on Home instead of their book. One-shot: cleared after
// consumption, naturally zero on cold boot.
RTC_NOINIT_ATTR uint32_t returnToReaderAfterBtMagic;  // constant declared in SilentRestart.h

// v18.9: scan-intent survives the BT-settings silent-restart. Set by the
// scan pre-flight in BluetoothSettingsActivity before it calls
// silentRestartToBluetoothSettings; the post-restart boot dispatch reads
// it into g_postBtSilentRebootScanIntent, then the activity's onEnter
// consumes that RAM flag to auto-startScanView after auto-enable. One-shot.
RTC_NOINIT_ATTR uint32_t postBtSilentRebootScanIntentMagic;
constexpr uint32_t POST_BT_SILENT_REBOOT_SCAN_INTENT_MAGIC = 0xC0BE4503;
// v18.9.9.388: dropped from 2 -> 1. Field report: a second recovery attempt
// after a panic-during-upload landed in a corrupted-heap state (garbage font
// IDs, empty panic reason on the follow-up crash) -- the retry cascade made
// things worse than a clean give-up-to-Home would have. Single attempt with a
// pre-restart pause preserves the "transient panic" recovery case while
// avoiding the compound-corruption pathology.
constexpr uint32_t FT_UPLOAD_MAX_RESUME_TRIES = 1;

// CrumBLE 4.5.5+: when the cover-heap-guard fires a silentRestart from
// loadShelfCovers, the user was almost always on the collection label
// (they had just pressed L/R to switch collections, which kicked the
// new collection's missing thumbs into view -> heap guard triggered).
// On the post-restart boot HomeActivity::onEnter wipes selectorIndex
// to 0 (= focused on carousel), which feels like the cursor randomly
// jumped away from where they were. This flag tells the post-restart
// onEnter to land focus back on the shelf header instead. One-shot:
// cleared after the next onEnter consumes it, and naturally zero on
// any cold boot (RTC NOINIT is uninitialized on power-up).
RTC_NOINIT_ATTR uint32_t pendingHomeFocusOnShelfHeaderMagic;
constexpr uint32_t HOME_FOCUS_SHELF_HEADER_MAGIC = 0xC0BE4501;

// Deferred-delete on silent restart. The on-device file browser's Delete
// confirmation calls Storage.remove() / removeDir() and then walks the book's
// prebake-cache (which can hold hundreds of section/atlas files for a CJK
// book) via BookActions::clearFileMetadata. On a heap-fragmented session
// (the broken-book case in particular: maxAlloc ~5 KB after the reader
// dropped its state) the recursive metadata cleanup OOMs, and the FT
// silent-restart safety net fires inappropriately -- the user thought they
// were deleting a file and instead lands in File Transfer with the file
// still present. Confusing, looks like a hijack.
//
// Instead, the file browser pre-checks heap before the delete. If too tight,
// it stashes the target path here and silent-restarts to home; main.cpp's
// boot path executes the delete with a fresh ~85 KB heap before goHome().
// User sees a brief "Loading..." popup, then home with the file gone.
RTC_NOINIT_ATTR uint32_t pendingDeleteMagic;
// Action type for the pending operation. Kept in the same uint8_t slot the
// original "isDir" boolean lived in -- existing values 0/1 retain their old
// meaning so any in-flight silent-restart from a prior firmware behaves the
// same way after the upgrade.
//   0 = delete a single file (Storage.remove)
//   1 = delete a directory tree (Storage.removeDir + per-child metadata sweep)
//   2 = clear an EPUB's prebake-cache subdir (Epub::clearCache); needed
//       because clearBookCache walks hundreds of files for CJK books and
//       OOMs the FT safety net under fragmented heap.
enum PendingDeleteAction : uint8_t {
  PENDING_ACTION_DELETE_FILE = 0,
  PENDING_ACTION_DELETE_DIR = 1,
  PENDING_ACTION_CLEAR_BOOK_CACHE = 2,
};
RTC_NOINIT_ATTR uint8_t pendingDeleteIsDir;  // see PendingDeleteAction
RTC_NOINIT_ATTR char pendingDeletePath[512];
constexpr uint32_t PENDING_DELETE_MAGIC = 0xDE1E7E00;

bool isFtUploadInProgress() {
  return ftUploadInProgressFlag == FT_UPLOAD_FLAG_MAGIC;
}

void setFtUploadInProgress(bool active) {
  if (active) {
    // v18.9.9.433: release the ~52 KB framebuffer on the FIRST upload of
    // this FT session and keep it released. The FT screen (QR/IP) is
    // retained on the e-ink panel without any host memory; nothing needs
    // to paint while a WS upload is grinding. All FT exit paths (user
    // back / WiFi loss / panic / heap watchdog) go through silentRestart
    // which reboots and reallocates a fresh framebuffer on next boot, so
    // we never need to realloc mid-session. HalDisplay paint methods are
    // null-guarded to no-op if any stray paint attempt slips through.
    const bool wasActive = (ftUploadInProgressFlag == FT_UPLOAD_FLAG_MAGIC);
    ftUploadInProgressFlag = FT_UPLOAD_FLAG_MAGIC;
    // v18.9.9.387: also reset the fail counter here. It's RTC_NOINIT so on
    // a cold boot the value is uninitialized garbage; if the first upload
    // panics before a clean DONE ever runs (which is where we previously
    // zero'd it), the panic-recovery boot reads garbage as the counter
    // (field log: 2775277453 consecutive boots, immediate give-up). Reset
    // at the START of each new upload so panic-recovery sees a clean 0.
    ftUploadResumeFailCount = 0;
    if (!wasActive && display.getFrameBuffer() != nullptr) {
      const uint32_t freeBefore = ESP.getFreeHeap();
      const uint32_t maxBefore = ESP.getMaxAllocHeap();
      // RenderLock: block until the render task's currently-running paint (if
      // any) finishes -- otherwise we'd free the buffer out from under it.
      // Cosmetic RSSI/bars repaints are already gated off during upload
      // (v421), so this typically completes immediately.
      // Route through GfxRenderer so its cached frameBuffer pointer is
      // nulled in lockstep -- direct display.releaseFrameBuffers() would
      // leave renderer holding a stale pointer to freed memory.
      {
        RenderLock lock;
        renderer.releaseFrameBufferForBuild();
      }
      LOG_INF("MAIN",
              "Upload START: released framebuffer, free %u -> %u (+%d), maxAlloc %u -> %u",
              freeBefore, ESP.getFreeHeap(),
              static_cast<int>(ESP.getFreeHeap()) - static_cast<int>(freeBefore),
              maxBefore, ESP.getMaxAllocHeap());
    }
  } else {
    ftUploadInProgressFlag = 0;
    ftUploadResumeFailCount = 0;  // clean exit -- restore full retry budget
    // v18.9.9.438: also reset panic-recovery counter on any clean upload
    // completion. Any successful transition through here means we're not
    // in a crash-loop, so future panics deserve the full 3-attempt budget.
    ftPanicRecoveryAttempts = 0;
    // Intentionally do NOT reallocFrameBuffers() here. Between-upload state
    // in the same FT session: panel keeps showing the last-painted FT
    // screen (e-ink retention), any RSSI/bars cosmetic repaints are safely
    // no-op'd by the HalDisplay null-guard until the user leaves FT
    // (silentRestart reboots into a fresh framebuffer).
  }
}

bool hasAttemptedCoverHeapRestart() {
  return coverHeapRestartFlag == COVER_HEAP_RESTART_MAGIC;
}

void markCoverHeapRestartAttempted() {
  coverHeapRestartFlag = COVER_HEAP_RESTART_MAGIC;
}

// v18.9.9.260: query-only helpers safe to reference above the
// snapshotFrameBufferForSilentRestart forward decl. The full
// silentRestartToBakeSleepImages() definition lives further down
// alongside the other silentRestart* helpers so it can call the
// snapshot fn without a duplicate forward decl.
bool hasPendingBakeSleepImages() {
  return pendingBakeSleepImagesFlag == PENDING_BAKE_SLEEP_MAGIC;
}
void clearPendingBakeSleepImages() { pendingBakeSleepImagesFlag = 0; }

void markPendingHomeFocusOnShelfHeader() {
  pendingHomeFocusOnShelfHeaderMagic = HOME_FOCUS_SHELF_HEADER_MAGIC;
}

bool consumePendingHomeFocusOnShelfHeader() {
  const bool pending = (pendingHomeFocusOnShelfHeaderMagic == HOME_FOCUS_SHELF_HEADER_MAGIC);
  pendingHomeFocusOnShelfHeaderMagic = 0;
  return pending;
}

bool hasPendingDelete() {
  return pendingDeleteMagic == PENDING_DELETE_MAGIC;
}

void setPendingDelete(const char* path, bool isDir) {
  if (!path) {
    pendingDeleteMagic = 0;
    return;
  }
  // Truncate paths that exceed the slot; if it happens we still emit a sane
  // log line and the delete just fails after restart (the path won't exist).
  // Real SD paths under /.crosspoint are well under 256 chars.
  std::strncpy(pendingDeletePath, path, sizeof(pendingDeletePath) - 1);
  pendingDeletePath[sizeof(pendingDeletePath) - 1] = '\0';
  pendingDeleteIsDir = isDir ? PENDING_ACTION_DELETE_DIR : PENDING_ACTION_DELETE_FILE;
  pendingDeleteMagic = PENDING_DELETE_MAGIC;
}

void setPendingClearBookCache(const char* path) {
  if (!path) {
    pendingDeleteMagic = 0;
    return;
  }
  std::strncpy(pendingDeletePath, path, sizeof(pendingDeletePath) - 1);
  pendingDeletePath[sizeof(pendingDeletePath) - 1] = '\0';
  pendingDeleteIsDir = PENDING_ACTION_CLEAR_BOOK_CACHE;
  pendingDeleteMagic = PENDING_DELETE_MAGIC;
}

void clearPendingDelete() {
  pendingDeleteMagic = 0;
  pendingDeletePath[0] = '\0';
  pendingDeleteIsDir = 0;
}

const char* getPendingDeletePath() {
  return pendingDeletePath;
}

bool getPendingDeleteIsDir() {
  return pendingDeleteIsDir == PENDING_ACTION_DELETE_DIR;
}

uint8_t getPendingDeleteAction() {
  return pendingDeleteIsDir;
}

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// CrumBLE 4.5.7: forward-declare so the target-specific restart wrappers below
// can use the same NO_REFRESH + snapshot pattern that silentRestartToReader
// established. Definition lives further down alongside the reader helpers.
static void snapshotFrameBufferForSilentRestart();

// v18.9.9.435: ensure the display framebuffer is allocated before a silent
// restart tries to paint its "Loading" popup or snapshot the panel. v433's
// setFtUploadInProgress(true) releases the framebuffer for the whole FT
// session; when the WS-wedge detector (or any other post-upload trigger)
// then calls silentRestartToFileTransfer(), GUI.drawPopup + snapshotFrame
// BufferForSilentRestart deref GfxRenderer.frameBuffer=nullptr and crash
// with MTVAL ~= byteIndex (~20 KB). Returns true if the framebuffer is
// usable when we return; false if realloc failed (caller must skip paint
// operations and reboot with the panel showing its retained image).
static bool ensureFrameBufferForSilentRestart() {
  if (display.getFrameBuffer() != nullptr) return true;
  // renderer.restoreFrameBufferAfterBuild reallocs display buffers AND
  // updates GfxRenderer's cached frameBuffer pointer -- the two must stay
  // in sync or drawText/drawRect/fillRect still deref stale null.
  const bool ok = renderer.restoreFrameBufferAfterBuild();
  if (ok) {
    LOG_INF("MAIN", "Silent restart: reallocated framebuffer for popup+snapshot");
  } else {
    LOG_ERR("MAIN",
            "Silent restart: framebuffer realloc failed (free=%u maxAlloc=%u); "
            "skipping popup+snapshot to avoid crash",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  return ok;
}
// v18.9.9.366: renamed from flushDeferredPersistenceBeforeHomeRestart. With
// v363's debounced saves, EVERY silent-restart target loses in-memory settings
// mutations if we don't flush. The v343 concern about bad_alloc-from-flush
// clobbering the target is still real, but the alternative is much worse:
// silent-restart-with-BT-enable persists bluetoothEnabled=1 in memory only,
// the flush is skipped for non-home targets, next boot reads bluetoothEnabled=0
// from SD, releases BLE mem, BT enable fails again -- infinite loop.
static void flushDeferredPersistenceForSilentRestart();

// v18.9.9.260: settings-triggered defrag restart specifically for the
// sleep-image bake. Mirrors silentRestart() but stamps the pending
// magic so setup() runs the bake before activity dispatch on the
// freshly-booted heap (~93 KB free, well above PNGdec's ~60 KB floor).
void silentRestartToBakeSleepImages() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  pendingBakeSleepImagesFlag = PENDING_BAKE_SLEEP_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=home, pendingBakeSleepImages)");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // v18.9.9.366: flush moved into snapshotFrameBufferForSilentRestart so
  // every silent-restart target (not just HOME) persists in-memory saves.
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  //
  // CrumBLE 4.5.7: NO_REFRESH + snapshot to hide the pre-restart flash the
  // same way silentRestartToReader does. The popup pixels are captured in
  // the sleep frame; the boot HALF_REFRESH restore paints them in one flash
  // instead of pre-flash + boot-flash.
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

// v18.9.5.8: variant of silentRestart() that does NOT overlay a "Loading"
// popup onto the framebuffer before snapshotting. Callers use this when
// the framebuffer already contains an appropriate transition popup (e.g.
// exitToHomeWithPopup's "Going Home"). Overlaying our own "Loading" on
// top produced a visible pile-up after the boot restore.
void silentRestartPreservingFrame() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // v18.9.9.366: flush moved into snapshotFrameBufferForSilentRestart.
  LOG_DBG("MAIN", "Silent restart (target=home, preserving current frame)");
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

void silentRestartToFileTransfer() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_FILE_TRANSFER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=file-transfer, modeHint=%u)",
          static_cast<unsigned>(silentRebootFtModeHint));
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestartToOtaUpdate() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_OTA_UPDATE;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=ota-update) — heap pre-flight tripped");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

// Set true at boot dispatch when snapshotTarget == SILENT_REBOOT_TARGET_BT_
// SETTINGS so BluetoothSettingsActivity knows not to silent-restart again
// on a recurring heap pre-flight failure (prevents infinite loop). Reset
// to false on any other entry path.
bool g_postBtSilentReboot = false;

// v18.9.9.252: set true by the v245 boot BT-off release branch when
// esp_bt_mem_release(ESP_BT_MODE_BLE) has actually run. Cleared on cold
// boot (BSS zero). Read by BluetoothHIDManager::enable() (extern
// declaration in the .cpp) so a subsequent enable() call in the SAME
// boot -- e.g. user navigates to BT Settings and toggles BT on -- can
// return false cleanly instead of crashing inside NimBLEDevice::init on
// a controller whose BLE memory was already freed. The existing
// enable-failure fallback in BluetoothSettingsActivity silent-restarts,
// and on the next boot bluetoothEnabled=1 skips the v245 release.
bool g_bleControllerMemReleased = false;

// v18.9: user tapped "Scan & Pair" but the scan pre-flight failed the heap
// floor and we silent-restarted. Post-restart, BluetoothSettingsActivity's
// onEnter reads this and auto-enters scan view so the user isn't dropped
// back on the menu with focus on row 0. One-shot, cleared on consume.
bool g_postBtSilentRebootScanIntent = false;

void silentRestartToBluetoothSettings(bool fromReader) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_BT_SETTINGS;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // v18.9.9.367: only arm the "return to reader on Back" magic when the
  // caller actually is in the reader. The prior heuristic (openEpubPath
  // non-empty) mis-fired for Settings > BT Setup because openEpubPath is
  // last-book-this-session, not "reader is the current activity". Callers
  // must pass fromReader=true only when the current activity really is
  // EpubReaderActivity (or the reader's own drawer/menu on top of it).
  if (fromReader) {
    returnToReaderAfterBtMagic = RETURN_TO_READER_AFTER_BT_MAGIC;
  } else {
    // Explicitly clear so a stale magic from an earlier reader-context
    // restart in the same boot doesn't leak into a Settings-launched one.
    returnToReaderAfterBtMagic = 0;
  }
  // v18.9.5.1: reset cover-guard flag -- see silentRestartToReaderWithAction.
  coverHeapRestartFlag = 0;
  LOG_INF("MAIN", "Silent restart (target=bt-settings) — heap pre-flight tripped");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

// v18.9.9.446: pre-arm terminate-recovery target for activities that might
// bad_alloc mid-execution. Writes to SEPARATE RTC vars (not the boot-
// dispatcher magic) so a plain sleep+wake doesn't get routed to the
// last-armed activity. Only the terminate handler consults these vars.
// If terminate fires while armed, the recovery target is copied into the
// boot-dispatcher vars so the post-restart boot lands on the intended
// activity instead of Home.
void armSilentRestartTarget(uint32_t target) {
  if (deepSleepInProgress) return;
  if (target > SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC) return;  // out-of-range guard
  terminateRecoveryTarget = target;
  terminateRecoveryMagic = TERMINATE_RECOVERY_MAGIC;
}

void clearArmedSilentRestartTarget() {
  terminateRecoveryMagic = 0;
  terminateRecoveryTarget = 0;
}

void silentRestartToSettings() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_SETTINGS;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // v18.9.5.1: reset cover-guard flag -- see rationale in
  // silentRestartToReaderWithAction. Any Home entry following this restart
  // may legitimately want to fire the guard again.
  coverHeapRestartFlag = 0;
  LOG_INF("MAIN", "Silent restart (target=settings) — defrag after BT disable");
  // v18.9.9.343: no "Loading" popup for Settings entry -- users found the
  // extra transition jarring on an already-slow silent-restart path.
  // Snapshot the current (Home) framebuffer so the boot-image restore
  // shows Home briefly, then Settings paints over it.
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

void silentRestartToBluetoothSettingsWithScanIntent(bool fromReader) {
  // v18.9: arm the RTC scan-intent magic, then piggyback on the shared
  // silent-restart path. Boot dispatch consumes the magic and lifts it into
  // g_postBtSilentRebootScanIntent for the activity to read from onEnter.
  postBtSilentRebootScanIntentMagic = POST_BT_SILENT_REBOOT_SCAN_INTENT_MAGIC;
  silentRestartToBluetoothSettings(fromReader);
}

// CrumBLE 4.5.4: mirror the BT pre-flight pattern for the two other auth-
// heavy entry points users hit mid-session.
bool g_postKoreaderSilentReboot = false;
bool g_postOpdsSilentReboot = false;
bool g_postFontDownloadSilentReboot = false;
bool g_postWifiSelectionSilentReboot = false;
bool g_postClockSyncSilentReboot = false;
bool g_postHomeClockSyncSilentReboot = false;

bool homeBootClockSyncAlreadyAttempted() {
  return homeBootClockSyncAttemptedMagic == HOME_BOOT_CLOCK_SYNC_ATTEMPTED_MAGIC;
}
void markHomeBootClockSyncAttempted() {
  homeBootClockSyncAttemptedMagic = HOME_BOOT_CLOCK_SYNC_ATTEMPTED_MAGIC;
}

// v18.9.9.349: inline boot-time WiFi+NTP sync. Replaces v343's post-Home
// silent-restart-to-ClockSync flow with a synchronous sync during boot,
// so users see one continuous "booting" experience instead of Home
// render -> Loading flash -> boot -> Home with time. Caller must have
// already validated that: cold boot to Home, SETTINGS.homeClockShow
// enabled, halClock time not valid, WIFI_STORE has saved networks,
// no other silent-restart target pending. Draws a "Syncing time..."
// popup over whatever is on screen (typically the boot splash) and
// returns after WiFi.begin+NTP either succeed OR overall timeout hits.
// Tears down WiFi on exit so the follow-on Home render has fresh heap.
void runBootTimeNtpSyncOverBootScreen() {
  SET_CHECKPOINT("boot:ntpSync");
  const uint32_t freeBefore = ESP.getFreeHeap();
  LOG_INF("BOOT", "Boot-time NTP sync starting (free=%u maxAlloc=%u)",
          freeBefore, ESP.getMaxAllocHeap());

  // v18.9.9.352: no popup. Boot splash ("Booting..." + logo) stays on
  // screen while WiFi+NTP run silently. Adds ~5-8 s to perceived boot
  // time but user just sees a slightly longer splash instead of two
  // screens flashing. If sync fails, no user-visible change; Home
  // just shows without a clock this session.

  // Lazy load WIFI_STORE if not already loaded.
  if (WIFI_STORE.getCredentials().empty()) {
    WIFI_STORE.loadFromFile();
  }
  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    LOG_INF("BOOT", "Boot NTP sync: no saved WiFi networks -- skipping");
    return;
  }

  // Build attempt order: lastConnectedSsid first, then rest in order.
  // Mirrors ClockSyncActivity::runSync so behavior is consistent.
  const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
  std::vector<const WifiCredential*> attemptOrder;
  attemptOrder.reserve(creds.size());
  if (!lastSsid.empty()) {
    if (const WifiCredential* lastCred = WIFI_STORE.findCredential(lastSsid)) {
      attemptOrder.push_back(lastCred);
    }
  }
  for (const auto& c : creds) {
    if (attemptOrder.empty() || c.ssid != attemptOrder.front()->ssid) {
      attemptOrder.push_back(&c);
    }
  }

  WiFi.mode(WIFI_STA);
  bool connected = false;
  constexpr uint32_t kPerAttemptMs = 4000;  // shorter than ClockSync's 6s -- cap total boot time
  constexpr uint32_t kOverallBudgetMs = 12000;
  const uint32_t startMs = millis();
  for (const WifiCredential* cred : attemptOrder) {
    if (millis() - startMs > kOverallBudgetMs) break;
    LOG_INF("BOOT", "Boot NTP: trying '%s'", cred->ssid.c_str());
    if (cred->password.empty()) {
      WiFi.begin(cred->ssid.c_str());
    } else {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    }
    const uint32_t attemptStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - attemptStart < kPerAttemptMs) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      LOG_INF("BOOT", "Boot NTP: connected to '%s' in %lu ms", cred->ssid.c_str(),
              static_cast<unsigned long>(millis() - attemptStart));
      break;
    }
    WiFi.disconnect(false);
    delay(50);
  }

  if (connected) {
    SET_CHECKPOINT("boot:ntpBegin");
    const bool ok = halClock.syncFromNTP();
    SET_CHECKPOINT("boot:ntpDone");
    if (ok) {
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.saveToFile();
      ReadingStats::reevaluateClockStatus();
      LOG_INF("BOOT", "Boot NTP sync OK (elapsed %lu ms)",
              static_cast<unsigned long>(millis() - startMs));
    } else {
      LOG_ERR("BOOT", "Boot NTP sync failed after connect");
    }
  } else {
    LOG_INF("BOOT", "Boot NTP: no network reachable within budget -- continuing without sync");
  }

  // Tear down WiFi so Home render gets fresh heap. WiFi.mode(WIFI_OFF)
  // releases the ~40 KB stack allocations.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_INF("BOOT", "Boot NTP sync end (free=%u maxAlloc=%u)",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void silentRestartToKoreaderAuth() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_KOREADER_AUTH;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=koreader-auth) — heap pre-flight tripped");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestartToOpdsBrowser() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_OPDS_BROWSER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=opds-browser) — heap pre-flight tripped");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestartToFontDownload() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_FONT_DOWNLOAD;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=font-download) — heap pre-flight tripped");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

// v18.9.9.336: WiFi Selection direct-land helper. Same pattern as font
// download: user hits Network -> WiFi from Settings and rather than call
// WiFi.mode(WIFI_STA) on the current possibly-fragmented heap (which has
// been observed to null-deref inside wpa_supplicant/eloop.c on X4 -- see
// checkpoint wifi:mode-STA), we silent-restart to this target. Post-boot
// dispatch lands the user directly in WifiSelectionActivity on a fresh
// ~150 KB heap. g_postWifiSelectionSilentReboot guards against loops.
void silentRestartToWifiSelection() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_WIFI_SELECTION;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=wifi-selection) — WiFi pre-flight tripped to avoid wpa_supplicant crash");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestartToClockSync() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_CLOCK_SYNC;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=clock-sync) — WiFi pre-flight tripped to avoid wpa_supplicant crash");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestartToHomeClockSync() {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=home-clock-sync) — auto boot sync so Home clock has time");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void silentRestartToOtaInstall(const char* url, uint32_t size, const char* version) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_OTA_INSTALL;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  silentRebootOtaSize = size;
  if (url) {
    strncpy(silentRebootOtaUrl, url, sizeof(silentRebootOtaUrl) - 1);
    silentRebootOtaUrl[sizeof(silentRebootOtaUrl) - 1] = '\0';
  } else {
    silentRebootOtaUrl[0] = '\0';
  }
  if (version) {
    strncpy(silentRebootOtaVersion, version, sizeof(silentRebootOtaVersion) - 1);
    silentRebootOtaVersion[sizeof(silentRebootOtaVersion) - 1] = '\0';
  } else {
    silentRebootOtaVersion[0] = '\0';
  }
  LOG_INF("MAIN", "Silent restart (target=ota-install) — fresh heap for install handshake");
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

void setSilentRebootFtModeHint(uint32_t mode) {
  silentRebootFtModeHint = mode;
}

// Snapshot taken at setup() time, AFTER the silent-reboot magic check
// validated that the RTC state is genuinely ours (cold boot leaves
// RTC_NOINIT memory uninitialized). FT activity reads this from
// onEnter and clears it; surviving exactly one consume call ensures
// a later normal entry doesn't accidentally auto-restore.
static uint32_t g_pendingFtModeHintSnapshot = 0;

uint32_t consumeSilentRebootFtModeHint() {
  const uint32_t v = g_pendingFtModeHintSnapshot;
  g_pendingFtModeHintSnapshot = 0;
  return v;
}

// CrumBLE 4.6: snapshot of the pending OTA install state, populated in
// setup() from RTC vars when target=SILENT_REBOOT_TARGET_OTA_INSTALL.
// consumePendingOtaInstall returns once and self-clears.
static char g_pendingOtaUrl[256] = {0};
static uint32_t g_pendingOtaSize = 0;
static char g_pendingOtaVersion[40] = {0};
static bool g_pendingOtaInstallReady = false;

// v18.9.9.37 (task #22): snapshot of the XTC file path to reopen after a
// silent-restart-with-defrag for the XTC viewer. Populated in setup()
// from silentRebootXtcPath when isSilentReboot && target=READER. Boot
// dispatch reads this in preference to APP_STATE.openEpubPath so we
// reopen the XTC file (routed through ReaderActivity's extension
// detection) rather than the last EPUB.
static char g_pendingXtcPath[256] = {0};

bool consumePendingOtaInstall(char* outUrl, size_t outUrlSize, uint32_t* outSize, char* outVersion,
                              size_t outVersionSize) {
  if (!g_pendingOtaInstallReady) return false;
  g_pendingOtaInstallReady = false;
  if (outUrl && outUrlSize > 0) {
    strncpy(outUrl, g_pendingOtaUrl, outUrlSize - 1);
    outUrl[outUrlSize - 1] = '\0';
  }
  if (outVersion && outVersionSize > 0) {
    strncpy(outVersion, g_pendingOtaVersion, outVersionSize - 1);
    outVersion[outVersionSize - 1] = '\0';
  }
  if (outSize) *outSize = g_pendingOtaSize;
  g_pendingOtaUrl[0] = '\0';
  g_pendingOtaVersion[0] = '\0';
  g_pendingOtaSize = 0;
  return true;
}

// Forward declaration: the silent-restart functions below snapshot the
// framebuffer to SD via this helper (defined later alongside loadSleepFrameBuffer).
static void saveSleepFrameBuffer();

// Hold RenderLock + the recursive SPI bus mutex across the multi-step SD save.
// RenderLock ensures the render task isn't mid-render (so the framebuffer is in
// a consistent post-paint state, not a partially-drawn intermediate); HalSpiBus::Lock
// ensures the SD bus operations aren't interleaved with any in-flight display SPI
// activity. saveSleepFrameBuffer internally calls openFileForWrite + write + close,
// each of which acquires StorageLock (StorageLock takes HalSpiBus::Lock + storageMutex
// recursively, no deadlock).
//
// Lock order: RenderLock first, then HalSpiBus::Lock. The render task's display path
// already takes RenderLock before HalSpiBus::Lock, so this ordering avoids deadlock.
//
// CrumBLE 4.4 post-bisect: without RenderLock, snapshots taken during the render
// task's render() showed up at boot as half-painted frames, producing a visible
// "full black/white flash before the small flash" instead of the QuickResume-style
// smooth restore.
// CrumBLE 4.5.7: check "am I already holding renderingMutex" before taking a
// nested RenderLock. When silentRestart() is invoked from the render task
// itself -- e.g. HomeActivity's Cover heap-guard fires mid-render, or any
// other in-render heap-recovery path -- the caller already owns the mutex.
// The base RenderLock uses xSemaphoreCreateMutex (non-recursive), so a
// blind take here would deadlock (the observed symptom: "Loading..." never
// paints, no restart, MEM logs continue on a tick). Take only when we do
// NOT currently hold it.
// v18.9.9.366: universal flush for every silent-restart target.
//
// History:
//   v343 kept this HOME-target-only because a bad_alloc during JSON build
//   would trip std::terminate, whose handler force-resets target to HOME
//   and the user's intended target (Settings, BT, etc.) was lost. Under
//   the pre-v363 "save is immediate" world, this trade-off was safe:
//   non-home silent-restarts had nothing to flush anyway.
//
//   v363 made saveToFile() a mark-dirty (debounced 5s). Now EVERY silent-
//   restart target loses the setting mutations made in the calling activity
//   -- and the concrete cost is much bigger than the target-clobber risk:
//   silentRestartToBluetoothSettings persists SETTINGS.bluetoothEnabled=1
//   in memory only, next boot reads bluetoothEnabled=0 from SD, main.cpp's
//   BT-off release branch fires and BLE mem is unrecoverable this boot,
//   BluetoothHIDManager::enable() refuses ("BLE controller memory released
//   this boot -- restart needed"), which silent-restarts again... infinite
//   loop. User has to manually cold-boot to escape.
//
//   v366 accepts the terminate-clobber trade-off: on the vanishingly rare
//   bad_alloc-during-flush event, target falls back to HOME and the user
//   loses the intended target. That is strictly better than losing every
//   setting mutation on every silent-restart.
static void flushDeferredPersistenceForSilentRestart() {
  // v18.9.9.363: with debounced saves, we MUST call the "flush now"
  // variants -- retry-if-needed respects the debounce window and
  // would skip. flushIfDirtyNow / flushDeferredSaveNowBypassGate
  // ignore debounce and write immediately since we're about to lose
  // in-memory state to ESP.restart().
  SETTINGS.flushIfDirtyNow();
  CollectionsStore::getInstance().flushDeferredSaveNowBypassGate();
}

static void snapshotFrameBufferForSilentRestart() {
  // v18.9.9.435: defense-in-depth null-guard. Callers now go through
  // ensureFrameBufferForSilentRestart() which reallocs on demand, but if
  // anything ever calls snapshot directly with a released framebuffer,
  // saveSleepFrameBuffer would deref renderer.getFrameBuffer() = nullptr.
  // Skip the snapshot instead; the flush below still runs.
  if (display.getFrameBuffer() == nullptr) {
    flushDeferredPersistenceForSilentRestart();
    LOG_ERR("MAIN", "Silent restart: framebuffer null; skipping snapshot");
    return;
  }
  // v18.9.9.366: flush deferred persistence here so every silent-restart
  // path (not just HOME) gets its in-memory settings/collections mutations
  // written to SD before ESP.restart(). See flushDeferredPersistenceForSilentRestart
  // for the rationale (previously BT-enable silent-restart lost bluetoothEnabled=1
  // and looped indefinitely). Kept before the framebuffer snapshot so a
  // terminate-from-flush doesn't leave a stale snapshot on the panel.
  flushDeferredPersistenceForSilentRestart();
  // Lock order (documented convention): RenderLock BEFORE HalSpiBus::Lock.
  // The render task's display path takes RenderLock first then HalSpiBus,
  // so this ordering avoids the ABBA deadlock.
  if (RenderLock::heldByCurrentTask()) {
    // Reentered from within a render context (e.g. HomeActivity's Cover
    // heap-guard fires silentRestart() mid-render, holding renderingMutex).
    // renderingMutex is non-recursive (xSemaphoreCreateMutex), so a blind
    // take would deadlock. This task owns the framebuffer, so snapshot
    // directly without re-acquiring.
    HalSpiBus::Lock spiLock;
    saveSleepFrameBuffer();
    return;
  }
  // Try to acquire RenderLock with a bounded deadline. If we cannot,
  // skip the snapshot rather than hang the entire silent restart.
  //
  // v18.9.9.240: raised timeout from 300 ms to 2000 ms. 300 ms was set
  // when silent restart was triggered from long-idle contexts (BT drop,
  // FT mode); the render task was usually quiescent. Dictionary
  // auto-recover (v235+) fires WHILE a word-select re-render is
  // typically in flight because closeDefinitionOverlay's invalid-capture
  // path uses requestUpdate (async) rather than requestUpdateAndWait
  // (sync). X3 word-select re-render + panel refresh = ~1200-1500 ms;
  // 300 ms window skipped the snapshot 100% of the time on that path,
  // which is what caused the visible white "Loading" screen in v226,
  // v227, v231, and v239 field tests. 2000 ms comfortably covers the
  // slowest word-select repaint. Silent restart adds up to 2 s to boot
  // only when contended -- acceptable given that alternative is a
  // ~3 s visible white flash across the reboot.
  if (RenderLock::tryLockFor(pdMS_TO_TICKS(2000))) {
    HalSpiBus::Lock spiLock;
    saveSleepFrameBuffer();
    RenderLock::forceUnlock();
    return;
  }
  LOG_ERR("MAIN", "Silent restart: skipping sleep-frame snapshot -- RenderLock contended");
  // v18.9.9.84: mark this restart as needing panel resync + FULL refresh, same
  // path the std::terminate handler uses. Without a sleep-frame snapshot, the
  // panel is in whatever half-painted state it was in when the render task
  // was interrupted — and freeink-sdk's partial-refresh path can't clear that
  // cleanly, leading to persistent ghost images across the restart. The
  // terminate handler already fixed the same class of bug via
  // silentRebootHardRestart; reusing it here means the next boot resets the
  // panel controller and does a full 1.7 s refresh, ghost-free.
  silentRebootHardRestart = SILENT_REBOOT_HARD_RESTART_MAGIC;
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::None);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  // CrumBLE 4.5.6: draw popup into framebuffer WITHOUT refreshing the display.
  // freeink-sdk's FAST_REFRESH is more visible than the old open-x4-sdk's
  // subtle custom-LUT path, so a pre-restart FAST refresh + boot HALF refresh
  // is two visible flashes. Snapshot captures the popup pixels; the boot
  // HALF_REFRESH restore paints them in ONE flash instead of two.
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

// CrumBLE 4.4 post-bisect: post-boot action snapshot. setup() pulls this
// from the RTC slot at boot, then EpubReaderActivity consumes it via
// consumeReaderPostBootAction() on its first loop tick.
static ReaderPostBootAction g_pendingReaderPostBootAction = ReaderPostBootAction::None;
// Resume-at-spine target captured at boot (paired with ReaderPostBootAction::ResumeAtSpine).
static int g_pendingResumeSpine = -1;
// Process-lifetime flag indicating the current boot resumed from a silent
// restart. Lets activities skip cold-boot ceremony (e.g. the e-ink panel
// is still holding the pre-restart popup; don't repaint over it).
static bool g_continuingFromSilentReboot = false;
// v18.9.9.5: reader-facing snapshot of silentRebootDefragRetryMagic taken
// at boot BEFORE the RTC flag is cleared. True when this boot is the
// continuation of a Level 1 defrag silent-restart, so the reader knows
// it has already spent its one-shot defrag budget for the current book
// open and must escalate content (Level 2/3) on the next failure.
static bool g_defragRetryContinuation = false;
static bool g_xtcDefragRetryContinuation = false;
// CrumBLE 4.5: lean-boot OTA flag, set true in setup() when we're booting
// straight to OTA via silent-restart. Read in loop() to skip the BT singleton
// instantiation + HID activity processing -- OTA never enables BT and even
// the singleton's static state + first-call init burns heap that mbedtls'
// X.509 cert parsing needs.
static bool g_leanBootForOta = false;

void silentRestartToReaderWithAction(ReaderPostBootAction action) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(action);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // v18.9.5.1: reset the cover-heap-guard one-shot flag. It's meant to
  // prevent cover-guard boot loops, not to permanently disable the guard
  // across the device's uptime. When the *reason* for the restart is
  // unrelated to Home's cover load (BT defrag here, EnableBt continuation),
  // a subsequent Home entry may legitimately benefit from a guard restart.
  // Without this reset, users who did any BT session and then Back to Home
  // hit ~15-20 s renders with 7-8 KB maxAlloc because guard was already
  // "spent" in an earlier Home entry and never re-armed.
  coverHeapRestartFlag = 0;
  LOG_INF("MAIN", "Silent restart (target=reader, postAction=%u)", static_cast<unsigned>(action));
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

// v18.9.9.5: Level 1 defrag variant. Marks the RTC defrag-retry magic so
// the reader on boot 2 knows it's the continuation and has already spent
// its defrag budget for this book open. Everything else is identical to
// silentRestartToReaderWithAction -- the Level 1 marker is orthogonal to
// the post-boot action (typically EnableBt for the defrag path).
void silentRestartToReaderWithDefragRetry(ReaderPostBootAction action) {
  silentRebootDefragRetryMagic = SILENT_REBOOT_DEFRAG_RETRY_MAGIC;
  // v18.9.9.32: clear any stale spine target so the boot-side spine
  // consumer doesn't mistakenly resume at a spine set by a previous
  // silent-restart-to-defrag call. The AtSpine variant sets it below.
  silentRebootTargetSpine = 0xFFFFFFFFu;
  silentRestartToReaderWithAction(action);
}

void silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction action, int targetSpine) {
  silentRebootDefragRetryMagic = SILENT_REBOOT_DEFRAG_RETRY_MAGIC;
  // Sentinel 0xFFFFFFFF = "no spine override"; targetSpine < 0 is treated
  // the same (caller doesn't want to override the reader's saved position).
  if (targetSpine < 0) {
    silentRebootTargetSpine = 0xFFFFFFFFu;
    LOG_INF("MAIN", "Silent restart (target=reader, defragRetry+action=%u, no spine override)",
            static_cast<uint32_t>(action));
  } else {
    silentRebootTargetSpine = static_cast<uint32_t>(targetSpine) & 0xFFFF;
    LOG_INF("MAIN", "Silent restart (target=reader, defragRetry+action=%u, resumeSpine=%d)",
            static_cast<uint32_t>(action), targetSpine);
  }
  // v18.9.9.397: also stash APP_STATE.openEpubPath into RTC. If CPS is
  // corrupt on the next boot, the boot dispatcher can use this to know
  // which book to reopen. Field bug: SD write failures left CPS truncated
  // (IncompleteInput on parse), so post-restart APP_STATE was empty and
  // the chapter-jump restart landed on Home instead of the book.
  if (!APP_STATE.openEpubPath.empty() &&
      APP_STATE.openEpubPath.size() < sizeof(silentRebootEpubPath)) {
    strncpy(silentRebootEpubPath, APP_STATE.openEpubPath.c_str(),
            sizeof(silentRebootEpubPath) - 1);
    silentRebootEpubPath[sizeof(silentRebootEpubPath) - 1] = '\0';
    silentRebootEpubPathMagic = SILENT_REBOOT_EPUB_PATH_MAGIC;
  } else {
    silentRebootEpubPathMagic = 0;
    silentRebootEpubPath[0] = '\0';
  }
  silentRestartToReaderWithAction(action);
}

void silentRestartToXtcReaderWithDefragRetry(const char* xtcPath) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::None);
  // v18.9.9.40: use the XTC-scoped magic instead of the shared EPUB
  // reader magic. Prior version set the EPUB flag, which meant a fresh
  // XTC open in a session that had previously done an EPUB defrag hop
  // (or vice versa) inherited the wrong budget state and no-op'd the
  // restart. Independent budgets per reader type keep the semantics
  // straight.
  silentRebootDefragXtcMagic = SILENT_REBOOT_DEFRAG_XTC_MAGIC;
  silentRebootTargetSpine = 0xFFFFFFFFu;  // no spine override for XTC
  if (xtcPath) {
    strncpy(silentRebootXtcPath, xtcPath, sizeof(silentRebootXtcPath) - 1);
    silentRebootXtcPath[sizeof(silentRebootXtcPath) - 1] = '\0';
  } else {
    silentRebootXtcPath[0] = '\0';
  }
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=xtc-reader, defragXtc, path='%s')", silentRebootXtcPath);
  if (ensureFrameBufferForSilentRestart()) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), 0, false, HalDisplay::NO_REFRESH);
    snapshotFrameBufferForSilentRestart();
  }
  delay(50);
  ESP.restart();
}

// CrumBLE 4.4 post-bisect: OpenDefinition variant -- carries the word
// string across the reboot so the post-boot dispatch can land the user
// directly on the definition for the word they just tapped, rather than
// merely re-opening the word-select activity.
void silentRestartToReaderWithDefinition(const char* word) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::OpenDefinition);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  // Copy word into the fixed-size RTC slot, truncating if necessary.
  if (word) {
    strncpy(silentRebootDefinitionWord, word, sizeof(silentRebootDefinitionWord) - 1);
    silentRebootDefinitionWord[sizeof(silentRebootDefinitionWord) - 1] = '\0';
  } else {
    silentRebootDefinitionWord[0] = '\0';
  }
  // v18.9.9.249: this variant always starts the definition at chunk 0 --
  // reset the chunk-start slot so a leftover value from a prior aborted
  // restart doesn't leak into this OpenDefinition dispatch.
  silentRebootDefinitionChunkStart = DEFINITION_CHUNK_START_NONE;
  LOG_INF("MAIN", "Silent restart (target=reader, OpenDefinition='%s')", silentRebootDefinitionWord);
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

// v18.9.9.249: OpenDefinition variant that also carries a byte offset
// within the target entry to open on. Used by the chunked-reader's
// Down-at-boundary-refuse path so the post-boot dispatch lands the user
// directly on the chunk they were trying to page into, not back at
// chunk 0. The word carries the same 63-char cap; chunkStart is a raw
// byte offset within the .dict entry (up to entry.totalSize).
void silentRestartToReaderWithDefinitionAtChunk(const char* word, uint32_t chunkStart) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::OpenDefinition);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  if (word) {
    strncpy(silentRebootDefinitionWord, word, sizeof(silentRebootDefinitionWord) - 1);
    silentRebootDefinitionWord[sizeof(silentRebootDefinitionWord) - 1] = '\0';
  } else {
    silentRebootDefinitionWord[0] = '\0';
  }
  silentRebootDefinitionChunkStart = chunkStart;
  LOG_INF("MAIN", "Silent restart (target=reader, OpenDefinition='%s' chunkStart=%u)",
          silentRebootDefinitionWord, chunkStart);
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

void silentRestartToReaderWithCursorWord(const char* word) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::OpenLookupAtWord);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  if (word) {
    strncpy(silentRebootDefinitionWord, word, sizeof(silentRebootDefinitionWord) - 1);
    silentRebootDefinitionWord[sizeof(silentRebootDefinitionWord) - 1] = '\0';
  } else {
    silentRebootDefinitionWord[0] = '\0';
  }
  // v18.9.9.249: OpenLookupAtWord never jumps to a specific chunk, so
  // clear the chunk-start slot -- keeps a stale value from an aborted
  // v249 restart from bleeding into this dispatch.
  silentRebootDefinitionChunkStart = DEFINITION_CHUNK_START_NONE;
  LOG_INF("MAIN", "Silent restart (target=reader, OpenLookupAtWord='%s')", silentRebootDefinitionWord);
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

void silentRestartToReaderResumingAtSpine(int targetSpine) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::ResumeAtSpine);
  silentRebootTargetSpine = static_cast<uint32_t>(targetSpine < 0 ? 0 : targetSpine) & 0xFFFF;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=reader, ResumeAtSpine=%d)", targetSpine);
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

void silentRestartToReaderOpeningDrawerAt(uint8_t groupId) {
  if (deepSleepInProgress) return;
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootReaderPostAction = static_cast<uint32_t>(ReaderPostBootAction::OpenBookSettingsDrawer);
  silentRebootDrawerExpandGroup = groupId;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=reader, OpenBookSettingsDrawer, group=%u)", groupId);
  snapshotFrameBufferForSilentRestart();
  delay(50);
  ESP.restart();
}

ReaderPostBootAction consumeReaderPostBootAction() {
  const ReaderPostBootAction v = g_pendingReaderPostBootAction;
  g_pendingReaderPostBootAction = ReaderPostBootAction::None;
  return v;
}

// v18.9.5: read-only peek. Used by EpubReaderActivity::onEnter to decide
// whether to skip its initial page paint because a post-boot action is
// about to draw its own popup / trigger its own render. Does NOT clear the
// action -- the normal consumeReaderPostBootAction() still fires later.
ReaderPostBootAction peekReaderPostBootAction() {
  return g_pendingReaderPostBootAction;
}

int consumePendingResumeSpine() {
  const int v = g_pendingResumeSpine;
  g_pendingResumeSpine = -1;
  return v;
}

// v18.9.9.25: queued drawer group id for OpenBookSettingsDrawer boot action.
// Consumed once by EpubReaderActivity when it dispatches the action. Read
// straight out of the RTC slot (not double-buffered like the definition word)
// because the caller only needs the value once and there's no lifetime
// concern -- just an integer.
int consumePendingDrawerExpandGroup() {
  const uint32_t raw = silentRebootDrawerExpandGroup;
  silentRebootDrawerExpandGroup = 0xFFFFFFFFu;
  return raw == 0xFFFFFFFFu ? -1 : static_cast<int>(raw);
}

// CrumBLE 4.4 post-bisect: read-and-clear the queued definition word.
// The post-boot dispatcher calls this once and passes the string to the
// DictionaryDefinitionActivity. Returns a pointer to a static buffer
// (lives for the life of the process); nullptr if no word was queued.
static char g_pendingDefinitionWordBuf[64] = {0};
const char* consumePendingDefinitionWord() {
  if (silentRebootDefinitionWord[0] == '\0') return nullptr;
  std::strncpy(g_pendingDefinitionWordBuf, silentRebootDefinitionWord, sizeof(g_pendingDefinitionWordBuf) - 1);
  g_pendingDefinitionWordBuf[sizeof(g_pendingDefinitionWordBuf) - 1] = '\0';
  silentRebootDefinitionWord[0] = '\0';
  return g_pendingDefinitionWordBuf;
}

// v18.9.9.249: paired with consumePendingDefinitionWord. Returns 0 if
// no chunk offset was queued (post-boot should load chunk 0 as usual),
// otherwise the byte offset within the target entry to start on. Also
// clears the slot so a subsequent OpenDefinition dispatch without a
// paired chunk stays at 0. Safe to call once per boot; safe on cold
// boot because RTC NOINIT sentinel + magic pairing on set-side keeps
// stale garbage from being misread as a real offset.
uint32_t consumePendingDefinitionChunkStart() {
  const uint32_t stored = silentRebootDefinitionChunkStart;
  silentRebootDefinitionChunkStart = DEFINITION_CHUNK_START_NONE;
  return stored == DEFINITION_CHUNK_START_NONE ? 0u : stored;
}

// v18.9.9.58: stash reader's active render path so a subsequent silent
// restart can pick it up on the other side. Magic guards against a cold
// boot reading uninitialized RTC as a real value.
void stashReaderActivePathForNextBoot(uint8_t path) {
  silentRebootReaderActivePath = static_cast<uint32_t>(path);
  silentRebootReaderActivePathMagic = READER_ACTIVE_PATH_MAGIC;
}

// v18.9.9.58: read-and-clear the stashed active path. Returns true iff magic
// matches (i.e. a prior stash by us, not cold-boot garbage). Caller uses the
// value to skip the book-open determination and honor the pre-restart choice.
bool consumePendingReaderActivePath(uint8_t& out) {
  const uint32_t magic = silentRebootReaderActivePathMagic;
  const uint32_t val = silentRebootReaderActivePath;
  silentRebootReaderActivePathMagic = 0;
  silentRebootReaderActivePath = 0xFFFFFFFFu;
  if (magic != READER_ACTIVE_PATH_MAGIC) return false;
  out = static_cast<uint8_t>(val & 0xFF);
  return true;
}

// v18.9.9.59: arm the compat-toast flag so the next boot shows the
// "Compatibility Mode required" popup. Called from Layer 2 write-sidecar
// site only when the user had just manually disabled compat this session.
void armCompatReenabledToast() { silentRebootCompatToastMagic = COMPAT_TOAST_MAGIC; }

// v18.9.9.59: read-and-clear the compat-toast flag. Reader's onEnter uses
// the return value to decide whether to draw the toast popup.
bool consumePendingCompatReenabledToast() {
  const bool armed = silentRebootCompatToastMagic == COMPAT_TOAST_MAGIC;
  silentRebootCompatToastMagic = 0;
  return armed;
}

// v18.9.9.438: arm the chapter-heap-refuse toast for the next boot. Called
// only from the v437 escalation gate's heap-refuse branch. The failed
// spine index is packed into the low 16 bits so the toast can name the
// chapter the user tried to jump to.
void armChapterHeapRefuseToast(int failedSpine) {
  const uint16_t spine = failedSpine < 0 ? 0xFFFFu : static_cast<uint16_t>(failedSpine & 0xFFFF);
  silentRebootChapterHeapToast = CHAP_HEAP_TOAST_MAGIC_MASK | static_cast<uint32_t>(spine);
}

// v18.9.9.438: read-and-clear. Returns the failed spine index if the toast
// was armed, else -1. The reader shows the popup on the next post-restart
// render, then dismisses to normal.
int consumePendingChapterHeapRefuseToast() {
  const uint32_t val = silentRebootChapterHeapToast;
  silentRebootChapterHeapToast = 0;
  if ((val & 0xFFFFFF00u) != CHAP_HEAP_TOAST_MAGIC_MASK) return -1;
  return static_cast<int>(val & 0xFFFFu);
}

bool isContinuingFromSilentReboot() { return g_continuingFromSilentReboot; }
void clearSilentRebootContinuationFlag() { g_continuingFromSilentReboot = false; }
bool isDefragRetryContinuation() { return g_defragRetryContinuation; }
void clearDefragRetryContinuation() { g_defragRetryContinuation = false; }
bool isXtcDefragRetryContinuation() { return g_xtcDefragRetryContinuation; }
void clearXtcDefragRetryContinuation() { g_xtcDefragRetryContinuation = false; }
// v18.9.9.10: true when this boot is the continuation of a silent-restart
// whose post-boot action was EnableBt. The reader uses this at book open
// to decide whether the Simple Rendering sidecar (a "needs compat mode
// WHEN BT is on" hint) should activate compat mode from the start. When
// BT is neither currently enabled nor about to be enabled, the sidecar
// is ignored and the book renders in full-prebake mode.
bool isEnableBtContinuation() {
  return g_continuingFromSilentReboot &&
         g_pendingReaderPostBootAction == ReaderPostBootAction::EnableBt;
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

bool isGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  return isPowerButtonActionAvailableOutsideReader(action);
}

bool startGlobalSyncProgress() {
  if (!KOREADER_STORE.hasCredentials()) {
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  const std::string epubPath = APP_STATE.openEpubPath;
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath) || !Storage.exists(epubPath.c_str())) {
    LOG_DBG("MAIN", "No syncable EPUB open, opening KOReader settings instead");
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_ERR("MAIN", "Failed to load EPUB for global sync: %s", epubPath.c_str());
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  epub->setupCacheDir();

  int spineIndex = 0;
  int pageNumber = 0;
  int totalPagesInSpine = 1;
  FsFile progressFile;
  if (Storage.openFileForRead("MAIN", epub->getCachePath() + "/progress.bin", progressFile)) {
    uint8_t data[6];
    const int dataSize = progressFile.read(data, sizeof(data));
    if (dataSize >= 4) {
      spineIndex = data[0] | (data[1] << 8);
      pageNumber = data[2] | (data[3] << 8);
      if (pageNumber == UINT16_MAX) {
        pageNumber = 0;
      }
    }
    if (dataSize >= 6) {
      totalPagesInSpine = std::max(1, static_cast<int>(data[4] | (data[5] << 8)));
    }
    progressFile.close();
  }

  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    spineIndex = 0;
  }

  CrossPointPosition localPos = {spineIndex, pageNumber, totalPagesInSpine};
  KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(spineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";

  activityManager.pushActivity(
      std::make_unique<KOReaderSyncActivity>(renderer, mappedInputManager, epubPath, spineIndex, pageNumber,
                                             totalPagesInSpine, std::move(localKoPos), std::move(localChapterName)));
  return true;
}

CrossPointSettings::SHORT_PWRBTN getPowerButtonAction() {
  static bool longPowerButtonHandled = false;

  if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    if (longPowerButtonHandled) {
      longPowerButtonHandled = false;
      screenshotComboHandled = false;
      return CrossPointSettings::SHORT_PWRBTN::IGNORE;
    }

    if (screenshotComboHandled) {
      screenshotComboHandled = false;
      return CrossPointSettings::SHORT_PWRBTN::IGNORE;
    }

    return mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()
               ? static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)
               : static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  }

  if (longPowerButtonHandled || !mappedInputManager.isPressed(MappedInputManager::Button::Power) ||
      mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (!isGlobalPowerButtonAction(action)) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  longPowerButtonHandled = true;
  return action;
}

bool handleGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH: {
      LOG_DBG("MAIN", "Manual screen refresh triggered");
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT: {
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      return startGlobalSyncProgress();
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToFileTransfer();
      return true;
    default:
      return false;
  }
}

namespace {
constexpr uint16_t POST_SLEEP_SCREEN_SETTLE_MS = 500;
// In cycle mode, a press shorter than this is a tap (cycle); longer is a wake.
// Originally set equal to the wake-duration threshold (400 ms) so there was no
// dead zone, but user reports showed that releases in the 200-400 ms range --
// which usually mean "I tried to hold to wake but didn't quite make it" --
// were getting interpreted as taps and cycling the screensaver instead.
// Dropping to 200 ms keeps genuine deliberate taps (<150 ms) snappy while
// leaving anything 200 ms+ to fall through into the wake path. The dead zone
// between 200 ms and POWER_BUTTON_LONG_PRESS_MS (400 ms) is intentional:
// presses in that range are likely accidental, and waking is the less
// surprising outcome than cycling a screen the user wasn't asking for.
constexpr unsigned long SCREENSAVER_TAP_MAX_MS = 200;

constexpr uint8_t TILT_SLEEP_MAX_ATTEMPTS = 3;
constexpr uint16_t TILT_SLEEP_RETRY_DELAY_MS = 10;

void putTiltSensorToSleepForDeepSleep() {
  if (!halTiltSensor.isAvailable()) {
    return;
  }

  for (uint8_t attempt = 0; attempt < TILT_SLEEP_MAX_ATTEMPTS; ++attempt) {
    if (halTiltSensor.deepSleep()) {
      return;
    }
    delay(TILT_SLEEP_RETRY_DELAY_MS);
  }
  LOG_ERR("MAIN", "Tilt sensor did not confirm sleep before deep sleep");
}
}  // namespace

// Returns true if the wake-up press was a brief tap (released before SCREENSAVER_TAP_MAX_MS).
// Returns false if the button is still held past the tap window — in that case the caller
// should fall through to the existing wake-verification flow.
//
// We read GPIO directly rather than going through InputManager because its debounce can take
// ~500ms to register a press; any tap shorter than that would be invisible. The deep-sleep
// wake itself already proved the button went LOW, so we only need to determine "still held?"
// vs "already released?" right now.
#ifndef SIMULATOR
bool detectScreensaverCycleTap() {
  const unsigned long start = millis();
  while (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW && (millis() - start) < SCREENSAVER_TAP_MAX_MS) {
    delay(5);
  }
  const bool released = digitalRead(InputManager::POWER_BUTTON_PIN) == HIGH;
  LOG_INF("MAIN", "Cycle tap detect: %s (took %lu ms)", released ? "TAP" : "HELD", millis() - start);
  return released;
}

// Wait POST_SLEEP_SCREEN_SETTLE_MS for the e-ink panel to finish settling. While waiting,
// also poll the power-button GPIO so taps that land during this awake window cycle the
// screensaver instead of being lost (the chip has not entered deep sleep yet, so InputManager
// is dormant and GPIO wake is not armed). Returns true if a tap was observed; false on timeout.
extern volatile bool sleepEntryTapPending;  // fwd decl -- definition below
bool pollForCycleTapDuringSleepEntry() {
  const auto start = millis();
  while (millis() - start < POST_SLEEP_SCREEN_SETTLE_MS) {
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      const auto pressStart = millis();
      while (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW &&
             (millis() - pressStart) < SCREENSAVER_TAP_MAX_MS) {
        delay(5);
      }
      // Released within tap window = tap. Held past it = let the user wake (we will exit
      // to deep sleep with the button still LOW; GPIO wake will fire immediately on arm).
      const bool tapped = digitalRead(InputManager::POWER_BUTTON_PIN) == HIGH;
      // v18.9.9.346: clear the ISR flag before returning. The FALLING-edge
      // ISR set sleepEntryTapPending=true when the user pressed; the
      // post-render consumeCompletedSleepEntryTap() would otherwise see
      // that stale flag on the NEXT cycle and immediately advance --
      // symptom: cycle N flashes briefly, cycle N+1 renders, and cycle
      // N+2 fires without a real tap. Both paths (this poll and the ISR)
      // observed the same physical tap; whichever consumes first should
      // clear so the other doesn't double-count.
      noInterrupts();
      sleepEntryTapPending = false;
      interrupts();
      return tapped;
    }
    delay(10);
  }
  return false;
}

// Set by an ISR while a sleep-screen render is in progress. Lets us detect taps that land
// during the (uninterruptible) e-ink refresh + greyscale pass — the chip is awake the whole
// time, but no main-loop polling runs, so a flag-on-falling-edge ISR is the only way to
// notice them. Volatile because shared with the ISR; only set/cleared with interrupts off.
volatile bool sleepEntryTapPending = false;

void IRAM_ATTR onSleepEntryPowerEdge() { sleepEntryTapPending = true; }

void armSleepEntryTapIsr() {
  sleepEntryTapPending = false;
  attachInterrupt(InputManager::POWER_BUTTON_PIN, onSleepEntryPowerEdge, FALLING);
}

void disarmSleepEntryTapIsr() {
  detachInterrupt(InputManager::POWER_BUTTON_PIN);
  sleepEntryTapPending = false;
}

// Returns true if the ISR captured a press whose release has already happened
// (button currently HIGH) — that is, a complete tap during the render. If the button
// is still LOW, the press is unresolved (user might be tapping or holding); leave the
// flag set and let the poll-for-release path decide.
bool consumeCompletedSleepEntryTap() {
  if (!sleepEntryTapPending) return false;
  if (digitalRead(InputManager::POWER_BUTTON_PIN) != HIGH) return false;
  sleepEntryTapPending = false;
  return true;
}
#else
// Simulator: no power-button GPIO or ISRs — cycle-screensaver is on-device
// only. No-op stubs so callers compile (SETTINGS.cycleScreensaverOnTap stays
// off in the sim, so these are never actually reached).
bool detectScreensaverCycleTap() { return false; }
bool pollForCycleTapDuringSleepEntry() { return false; }
void armSleepEntryTapIsr() {}
void disarmSleepEntryTapIsr() {}
bool consumeCompletedSleepEntryTap() { return false; }
#endif

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

[[noreturn]] void cycleScreensaverThenDeepSleep() {
  APP_STATE.loadFromFile();

  // Display + renderer init only — fonts are not needed because the cycle path
  // only ever draws a BMP via SleepActivity::cycleScreensaverFromDeepSleep().
  display.begin();
  renderer.begin();

  armSleepEntryTapIsr();
  while (true) {
    SleepActivity::cycleScreensaverFromDeepSleep(renderer);
    if (consumeCompletedSleepEntryTap()) continue;
    if (pollForCycleTapDuringSleepEntry()) continue;
    break;
  }
  disarmSleepEntryTapIsr();

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Screensaver cycled — re-entering deep sleep");
  powerManager.startDeepSleep(gpio);

  // startDeepSleep does not return on hardware. The simulator stubs it as a
  // no-op; spin so [[noreturn]] holds and the simulator does not fall through.
  while (true) {
    delay(1000);
  }
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // v18.9.9.290: flush any in-flight reading session before we lose CPU.
  ReadingStats::onSleepEntry();

  // v18.9.9.456 (CrossInk parity): auto-daily backup of all-time stats to
  // /.crossink-stats-backup/. No-op when SETTINGS.autoBackupStats=0 or
  // clock is invalid (needs date-based filename to coalesce daily).
  StatsBackup::maybeWriteAutoBackup();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_ALWAYS ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // CrumBLE: give the current activity a chance to flush in-flight
  // state (most importantly: the reader's accumulated session time)
  // before the chip powers off. Without this hook, every minute spent
  // reading since the last natural activity-exit was lost on each
  // deep-sleep cycle. Ported in spirit from dawsonfi/aalu's
  // ReadingStatsManager::endSession deep-sleep wiring.
  activityManager.notifyBeforeDeepSleep();

  // Disable BLE before deep sleep so the NimBLE host shuts down cleanly and
  // the radio is released before the chip powers off. Idempotent if BLE was
  // already off (reader exit path) — defensive against the auto-sleep timer
  // firing while the user is still in a book with BLE on.
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    LOG_INF("SLP", "Disabling Bluetooth before deep sleep");
    btMgr.disable();
  }

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;

  // v18.9.9.363: flush debounced saves before entering deep sleep. Deep
  // sleep wipes system RAM; unflushed dirty settings/collections would
  // be lost on wake. This is the primary critical-exit path for the
  // batched-save mechanism.
  flushDeferredPersistenceForSilentRestart();

  if (SETTINGS.cycleScreensaverOnTap) {
    armSleepEntryTapIsr();
    activityManager.goToSleep(fromTimeout);
    while (true) {
      if (consumeCompletedSleepEntryTap()) {
        SleepActivity::cycleScreensaverFromDeepSleep(renderer);
        continue;
      }
      if (pollForCycleTapDuringSleepEntry()) {
        SleepActivity::cycleScreensaverFromDeepSleep(renderer);
        continue;
      }
      break;
    }
    disarmSleepEntryTapIsr();
  } else {
    activityManager.goToSleep(fromTimeout);
    if (isQuickResumeSleep) {
      saveSleepFrameBuffer();
    } else {
      delay(POST_SLEEP_SCREEN_SETTLE_MS);
    }
  }

  putTiltSensorToSleepForDeepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

// v18.9.9.376: leanForOta now also passed true on FILE_TRANSFER boots.
// WEBACT teardown releases the SD font seconds later anyway; skipping the
// load avoids the ~10 KB peak + fragmentation residue during WiFi init,
// which was measurably hurting FT reliability on tight-heap devices.
// Kept the parameter name for the existing log strings inside.
void setupDisplayAndFonts(bool seamless = false, bool leanForOta = false) {
#ifdef SIMULATOR
  (void)seamless;
  display.begin();
#else
  display.begin(seamless);
#endif
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");
  if (leanForOta) {
    LOG_INF("MEM", "Boot step setupDisplayAndFonts post-display: free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

#ifndef OMIT_CHAREINK_FONT
#ifndef OMIT_TEENSY_FONT
  renderer.insertFont(CHAREINK_8_FONT_ID, charein8FontFamily);
#endif
#ifndef OMIT_ITTY_BITTY_FONT
  renderer.insertFont(CHAREINK_9_FONT_ID, charein9FontFamily);
#endif
#ifndef OMIT_TINY_FONT
  renderer.insertFont(CHAREINK_10_FONT_ID, charein10FontFamily);
#endif
#ifndef OMIT_SMALL_FONT
  renderer.insertFont(CHAREINK_12_FONT_ID, charein12FontFamily);
#endif
#ifndef OMIT_MEDIUM_FONT
  renderer.insertFont(CHAREINK_14_FONT_ID, charein14FontFamily);
#endif
#ifndef OMIT_LARGE_FONT
  renderer.insertFont(CHAREINK_16_FONT_ID, charein16FontFamily);
#endif
#ifndef OMIT_XLARGE_FONT
  renderer.insertFont(CHAREINK_18_FONT_ID, charein18FontFamily);
#endif
#ifndef OMIT_HUGE_FONT
  renderer.insertFont(CHAREINK_20_FONT_ID, charein20FontFamily);
#endif
#endif  // OMIT_CHAREINK_FONT

#ifndef OMIT_LEXENDDECA_FONT
#ifndef OMIT_TEENSY_FONT
  renderer.insertFont(LEXENDDECA_8_FONT_ID, lexenddeca8FontFamily);
#endif
#ifndef OMIT_ITTY_BITTY_FONT
  renderer.insertFont(LEXENDDECA_9_FONT_ID, lexenddeca9FontFamily);
#endif
#ifndef OMIT_TINY_FONT
  renderer.insertFont(LEXENDDECA_10_FONT_ID, lexenddeca10FontFamily);
#endif
#ifndef OMIT_SMALL_FONT
  renderer.insertFont(LEXENDDECA_12_FONT_ID, lexenddeca12FontFamily);
#endif
#ifndef OMIT_MEDIUM_FONT
  renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14FontFamily);
#endif
#ifndef OMIT_LARGE_FONT
  renderer.insertFont(LEXENDDECA_16_FONT_ID, lexenddeca16FontFamily);
#endif
#ifndef OMIT_XLARGE_FONT
  renderer.insertFont(LEXENDDECA_18_FONT_ID, lexenddeca18FontFamily);
#endif
#ifndef OMIT_HUGE_FONT
  renderer.insertFont(LEXENDDECA_20_FONT_ID, lexenddeca20FontFamily);
#endif
#endif  // OMIT_LEXENDDECA_FONT

// CrumBLE 4.2.1: OMIT_BITTER_FONT drops the entire Bitter family (see all.h).
#ifndef OMIT_BITTER_FONT
#ifndef OMIT_TEENSY_FONT
  renderer.insertFont(BITTER_8_FONT_ID, bitter8FontFamily);
#endif
#ifndef OMIT_ITTY_BITTY_FONT
  renderer.insertFont(BITTER_9_FONT_ID, bitter9FontFamily);
#endif
#ifndef OMIT_TINY_FONT
  renderer.insertFont(BITTER_10_FONT_ID, bitter10FontFamily);
  // CrumBLE 4.5.4 task #5C+: register Bitter as the built-in last-resort
  // fallback for glyph misses. When the user picks an SD CJK font that
  // omits Latin/ASCII (e.g. their LXGW @20pt covers CJK only), book text
  // with English letters / digits / punctuation falls through to Bitter
  // and renders correctly instead of '?'. No new resident heap cost --
  // bitter10FontFamily is already loaded and stays loaded.
  EpdFontFamily::setBuiltInFallbackFamily(&bitter10FontFamily);
#endif
#ifndef OMIT_SMALL_FONT
  renderer.insertFont(BITTER_12_FONT_ID, bitter12FontFamily);
#endif
#ifndef OMIT_MEDIUM_FONT
  renderer.insertFont(BITTER_14_FONT_ID, bitter14FontFamily);
#endif
#ifndef OMIT_LARGE_FONT
  renderer.insertFont(BITTER_16_FONT_ID, bitter16FontFamily);
#endif
#ifndef OMIT_XLARGE_FONT
  renderer.insertFont(BITTER_18_FONT_ID, bitter18FontFamily);
#endif
#ifndef OMIT_HUGE_FONT
  renderer.insertFont(BITTER_20_FONT_ID, bitter20FontFamily);
#endif
#endif  // OMIT_BITTER_FONT
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  if (leanForOta) {
    LOG_INF("MEM", "Boot step setupDisplayAndFonts post-font-register: free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  // Discover and load SD card fonts. Skipped on lean-boot OTA path -- SD font
  // discovery scans the SD card and (if user has a saved sdFontFamilyName) the
  // load can allocate 20+ KB for glyph headers/cache. None of it is needed to
  // render the OTA progress screen, which uses only UI fonts.
  if (!leanForOta) {
    LOG_INF("MEM", "pre-sdFont.begin: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    sdFontSystem.begin(renderer);
    LOG_INF("MEM", "post-sdFont.begin (primary loaded): free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    // CrumBLE 4.5.4 task #5C: arm the UI fallback for LAZY load instead
    // of loading eagerly. Field report: the eager-loaded fallback (LXGW
    // @14pt for CJK users) held ~15-25 KB resident permanently, which
    // dominated heap-fragmentation on FT entry. Lazy-load defers the
    // resident cost until the first glyph miss in a UI render -- non-
    // CJK users never pay; CJK users pay at first CJK char render in
    // carousel / bookshelf. registerLazyFallback no-ops when the setting
    // is empty so the only visible effect for non-CJK users is recovered
    // boot heap.
    sdFontSystem.registerLazyFallback(renderer);
    LOG_INF("MEM", "post-sdFont.fallback (lazy armed): free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  } else {
    LOG_INF("MEM", "Boot step setupDisplayAndFonts skipped sdFontSystem.begin: free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

  const esp_reset_reason_t rawResetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t rawWakeupCause = esp_sleep_get_wakeup_cause();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#ifndef SIMULATOR
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  LOG_INF("BOOT", "Reset diagnostic: reset=%d(%s) sleepWake=%d(%s)", static_cast<int>(rawResetReason),
          resetReasonName(rawResetReason), static_cast<int>(rawWakeupCause), wakeupCauseName(rawWakeupCause));

  // CrumBLE 4.5.5: synchronous std::terminate trap. The periodic watchdog in
  // CrossPointWebServerActivity::loop catches CHRONIC low heap, but it polls
  // maxAlloc once per loop iteration and misses TRANSIENT spikes -- field log
  // showed Min Free dropping to 548 bytes between two 10-second MEM samples,
  // followed by abort(). Decoded crash chain:
  //   _parseForm (browser POST) -> bestEffortRemoveDir -> openNextFile ->
  //   make_unique<HalFile::Impl> -> bad_alloc -> __gxx_personality_v0 ->
  //   std::terminate -> abort() -> panic dump -> reboot.
  // Installing our own terminate handler intercepts BEFORE abort(). User
  // sees a clean silent-restart instead of a panic dump + reboot cycle.
  // In-flight work is still lost (same as the abort case) but the device
  // returns to a usable state in ~5s instead of ~10s.
  std::set_terminate([]() {
    // v18.9.9.440 + v18.9.9.446: pick the recovery target for hard-restart.
    // Two sources, tried in order:
    //   1. Real silent-restart already armed (silentRebootMagic + valid target)
    //      — this is a bad_alloc during flushIfDirtyNow that happens AFTER a
    //      silentRestartToX() call ran but before esp_restart. Preserve the
    //      caller's intended target.
    //   2. Activity-armed terminate-recovery target (terminateRecoveryMagic)
    //      — set by armSilentRestartTarget() from onEnter of terminate-prone
    //      activities. Only consulted if #1 didn't hit.
    //   3. Fallback: HOME.
    // Cold-boot RTC garbage can't pass both magic+bounded-target checks so
    // random noise still falls back to HOME safely.
    uint32_t recoveryTarget = SILENT_REBOOT_TARGET_HOME;
    const char* recoverySource = "home";
    if ((silentRebootMagic == SILENT_REBOOT_MAGIC) &&
        (silentRebootTarget <= SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC)) {
      recoveryTarget = silentRebootTarget;
      recoverySource = "pre-armed silent-restart target";
    } else if ((terminateRecoveryMagic == TERMINATE_RECOVERY_MAGIC) &&
               (terminateRecoveryTarget <= SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC)) {
      recoveryTarget = terminateRecoveryTarget;
      recoverySource = "activity-armed terminate-recovery target";
    }
    // Consume terminate-recovery arming regardless — post-restart is a fresh
    // activity session that must re-arm if it wants recovery.
    terminateRecoveryMagic = 0;
    terminateRecoveryTarget = 0;
    LOG_ERR("MAIN",
            "std::terminate invoked (likely bad_alloc from a throwing library path); "
            "hard-restart to %s",
            recoverySource);
    LOG_ERR("MAIN", "heap at terminate: free=%u maxAlloc=%u recoveryTarget=%u",
            static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(recoveryTarget));
    // v18.9.9.332: log the last known code-path checkpoint. Instrumented sites
    // call SET_CHECKPOINT("name") at entry so we can distinguish which
    // operation was in flight (BT enable, section load, drawer render, etc.).
    LOG_ERR("MAIN", "last checkpoint: %s", getLastCheckpoint());
    // CrumBLE 4.5.7: NEVER call silentRestart() from here.
    // silentRestart drawPopup allocates a std::string (translated
    // "Loading..."), and under bad_alloc conditions the fresh alloc
    // throws bad_alloc again -> std::terminate re-enters this handler
    // -> another silentRestart -> another bad_alloc -> ~35 recursions
    // -> stack overflow -> Guru Meditation panic. Set the RTC target
    // and hard-restart directly. No popup, no framebuffer snapshot --
    // the boot cold-refresh (~3.7 s) covers the missing "Loading..."
    // hint. Better a plain boot flash than a stack-corrupt hardfault.
    silentRebootTarget = recoveryTarget;
    silentRebootMagic = SILENT_REBOOT_MAGIC;
    // v18.9.9.1: signal boot path to skip seamless/HALF silent-resume paint
    // and do a full resync + FULL refresh, else ghosting from the mid-paint
    // panel state.
    silentRebootHardRestart = SILENT_REBOOT_HARD_RESTART_MAGIC;
    esp_restart();
  });

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  // v18.9.9.343: reset the home-boot-clock-sync attempted marker on cold
  // boot (magic doesn't match: uninitialized RTC or user-initiated
  // restart). Silent-reboots preserve it so a chain of restarts within
  // one boot session all count as "attempted this session".
  if (!isSilentReboot) {
    homeBootClockSyncAttemptedMagic = 0;
  }
  // v18.9.9.1: read-and-clear the hard-restart flag. Set only by the
  // std::terminate handler; forces the boot path to bypass the seamless
  // silent-resume paint and do a full resync + FULL refresh so a
  // mid-render panel doesn't ghost through into the next screen.
  const bool isHardRestart = isSilentReboot && (silentRebootHardRestart == SILENT_REBOOT_HARD_RESTART_MAGIC);
  silentRebootHardRestart = 0;
  if (isHardRestart) {
    LOG_INF("BOOT", "Hard restart from terminate handler: forcing panel resync + FULL refresh");
  }
  // v18.9.9.334: surface any checkpoint from the previous boot. RTC_NOINIT
  // survives ANY reboot cause (std::terminate, ESP-IDF heap-poison assert,
  // Guru Meditation, watchdog), so this is our best clue for what the
  // previous boot was doing when it died -- regardless of which panic
  // path was taken. Cold boot / no-checkpoint returns nullptr and we log
  // nothing.
  if (const char* prev = consumeCheckpointFromPrevBoot()) {
    LOG_INF("BOOT", "Previous boot's last checkpoint: %s", prev);
  }
  // v18.9.9.5: read+clear the Level 1 defrag-retry magic. Only honour it
  // when this boot is a confirmed silent reboot -- cold-boot RTC noise
  // should never be interpreted as "we already retried".
  g_defragRetryContinuation = isSilentReboot && (silentRebootDefragRetryMagic == SILENT_REBOOT_DEFRAG_RETRY_MAGIC);
  silentRebootDefragRetryMagic = 0;
  // v18.9.9.40: XTC defrag continuation, scoped independently from the
  // EPUB reader's flag above so cross-activity defrag hops don't
  // consume each other's one-shot budget.
  g_xtcDefragRetryContinuation = isSilentReboot && (silentRebootDefragXtcMagic == SILENT_REBOOT_DEFRAG_XTC_MAGIC);
  silentRebootDefragXtcMagic = 0;
  if (g_xtcDefragRetryContinuation) {
    LOG_INF("BOOT", "Continuing from XTC defrag retry -- XTC reader will skip further defrag hops for this file open");
  }
  if (g_defragRetryContinuation) {
    LOG_INF("BOOT", "Continuing from Level 1 defrag retry -- reader will skip further defrag hops for this book");
  }
  // CrumBLE 4.5.4 fix: bound was OTA_INSTALL (4) which snapped BT_SETTINGS (5)
  // to 0 -- silent-restart-from-BT landed on home instead of BT Settings.
  // Bumped again to OPDS_BROWSER (7) for the same reason: any new target we
  // add below the bound silently routes to home if we forget to widen it.
  // v18.9.9.342: bumped to CLOCK_SYNC (11) -- WIFI_SELECTION (10) and
  // CLOCK_SYNC (11) silent-restarts were clamped to Home by the previous
  // FONT_DOWNLOAD (9) bound, so the ClockSync heap pre-flight would kick
  // to Home mid-flash and reset the user's carousel focus.
  // v18.9.9.343: bumped again to HOME_CLOCK_SYNC (12).
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC) ? silentRebootTarget : 0;
  // Snapshot the FT mode hint into a normal variable before clearing
  // RTC state, so the FT activity's onEnter can read it via
  // consumeSilentRebootFtModeHint(). Only honour it on a confirmed
  // silent reboot to FT -- everything else gets zero (cold-boot RTC
  // garbage or stale state from a different silent-reboot target).
  g_pendingFtModeHintSnapshot =
      (isSilentReboot && snapshotTarget == SILENT_REBOOT_TARGET_FILE_TRANSFER) ? silentRebootFtModeHint : 0;

  // CrumBLE 4.4 post-bisect: snapshot the reader post-boot action and
  // resume-spine target before clearing RTC. Both honoured only when this
  // boot is a confirmed silent reboot whose target is the reader.
  // v18.9.9.1: hard restart looks like a cold boot to downstream activities.
  // HomeActivity uses g_continuingFromSilentReboot to pick FAST refresh
  // instead of HALF_REFRESH_DEEP; on a mid-render terminate that FAST
  // paint doesn't clear ghosting. Forcing false here routes Home through
  // its normal HALF_REFRESH_DEEP path.
  g_continuingFromSilentReboot = isSilentReboot && !isHardRestart;
  if (isSilentReboot && snapshotTarget == SILENT_REBOOT_TARGET_READER) {
    const uint32_t raw = silentRebootReaderPostAction;
    if (raw <= static_cast<uint32_t>(ReaderPostBootAction::OpenLookedUpWords)) {
      g_pendingReaderPostBootAction = static_cast<ReaderPostBootAction>(raw);
    } else {
      g_pendingReaderPostBootAction = ReaderPostBootAction::None;
    }
    // v18.9.9.32: honor a spine override whenever the RTC slot holds a real
    // value (not the 0xFFFFFFFF sentinel), regardless of action. Lets
    // silentRestartToReaderWithDefragRetryAtSpine chain EnableBt with a
    // resume-at-spine so a defrag restart triggered from a fresh chapter
    // navigation lands the user at the chapter they clicked, not their
    // previously-saved reading position.
    if (silentRebootTargetSpine != 0xFFFFFFFFu) {
      g_pendingResumeSpine = static_cast<int>(silentRebootTargetSpine & 0xFFFF);
    }
    if (g_pendingReaderPostBootAction == ReaderPostBootAction::OpenDefinition ||
        g_pendingReaderPostBootAction == ReaderPostBootAction::OpenLookupAtWord) {
      // Validate the word slot is null-terminated within bounds.
      silentRebootDefinitionWord[sizeof(silentRebootDefinitionWord) - 1] = '\0';
    }
    // v18.9.9.37: snapshot the XTC file path override. Populated by
    // silentRestartToXtcReaderWithDefragRetry when the XTC page buffer
    // alloc fails; the boot dispatch below prefers this over
    // APP_STATE.openEpubPath so we come back on the XTC file the user
    // was trying to open, not the previous EPUB.
    silentRebootXtcPath[sizeof(silentRebootXtcPath) - 1] = '\0';
    if (silentRebootXtcPath[0] != '\0') {
      strncpy(g_pendingXtcPath, silentRebootXtcPath, sizeof(g_pendingXtcPath) - 1);
      g_pendingXtcPath[sizeof(g_pendingXtcPath) - 1] = '\0';
    }
  }

  // CrumBLE 4.6: snapshot OTA install state from RTC. Module-scope globals
  // declared above; consumePendingOtaInstall() pulls them back when
  // OtaUpdateActivity's onEnter runs.
  g_pendingOtaInstallReady = false;
  g_pendingOtaUrl[0] = '\0';
  g_pendingOtaVersion[0] = '\0';
  g_pendingOtaSize = 0;
  if (isSilentReboot && snapshotTarget == SILENT_REBOOT_TARGET_OTA_INSTALL) {
    silentRebootOtaUrl[sizeof(silentRebootOtaUrl) - 1] = '\0';
    silentRebootOtaVersion[sizeof(silentRebootOtaVersion) - 1] = '\0';
    if (silentRebootOtaUrl[0] != '\0') {
      strncpy(g_pendingOtaUrl, silentRebootOtaUrl, sizeof(g_pendingOtaUrl) - 1);
      g_pendingOtaUrl[sizeof(g_pendingOtaUrl) - 1] = '\0';
      strncpy(g_pendingOtaVersion, silentRebootOtaVersion, sizeof(g_pendingOtaVersion) - 1);
      g_pendingOtaVersion[sizeof(g_pendingOtaVersion) - 1] = '\0';
      g_pendingOtaSize = silentRebootOtaSize;
      g_pendingOtaInstallReady = true;
    }
  }

  silentRebootMagic = 0;
  silentRebootTarget = 0;
  // v18.9.9.446: clear any stale terminate-recovery arming from previous
  // boot. On a real terminate the handler already cleared these (line ~2497);
  // this catches unrelated reboots (power cycle after user was in Settings)
  // so garbage doesn't accumulate.
  terminateRecoveryMagic = 0;
  terminateRecoveryTarget = 0;
  silentRebootFtModeHint = 0;
  silentRebootReaderPostAction = 0;
  // v18.9.9.67: use the 0xFFFFFFFFu sentinel instead of 0. Previously reset
  // to 0 meant that any subsequent silent-restart-without-spine (e.g.
  // silentRestartToReaderWithAction from BT pre-flight defrag) left RTC at
  // literal spine 0, and consumers that don't gate on the defrag-retry
  // marker would treat it as a real resume-at-spine=0 override.
  silentRebootTargetSpine = 0xFFFFFFFFu;
  silentRebootOtaUrl[0] = '\0';
  silentRebootOtaVersion[0] = '\0';
  silentRebootXtcPath[0] = '\0';
  silentRebootOtaSize = 0;

  // CrumBLE 4.5.4: auto-resume an interrupted FT WS upload. If the prior
  // boot's WS upload set the flag and we just panic-rebooted (cold/non-
  // silent boot), silent-restart back into FT so the browser's WS retry
  // naturally reconnects + the server's RESUME protocol picks up at the
  // saved byte offset. Counter caps consecutive auto-resumes so FT-mode-
  // itself crashes can't loop forever -- after MAX tries, fall through
  // to normal Home boot. Skip when this IS already a silent reboot to a
  // different target (OTA install / BT settings etc.) -- those take
  // precedence so the user isn't yanked away from their explicit choice.
  if (!isSilentReboot && ftUploadInProgressFlag == FT_UPLOAD_FLAG_MAGIC) {
    // v18.9.9.438: replace v403's clear-and-boot-to-Home with rate-limited
    // route-to-FT.
    //
    // v403 killed the earlier auto-recovery because it triggered a *fresh*
    // silent-restart from this early boot check -- and that inner restart
    // hit "Font UI_12 not found" (null-deref -> Load-access fault) because
    // this check runs at ~280 ms of boot, BEFORE setupDisplayAndFonts
    // registers UI_12 at ~520 ms. The ESP-IDF panic writer's diagnostic
    // draw call blew up trying to use an unregistered font. See v403 for
    // the full teardown.
    //
    // v438 sidesteps that: no silent-restart from here. Instead we set a
    // volatile flag consumed by the boot activity dispatcher AFTER setup-
    // DisplayAndFonts has run and every font is registered. Same route the
    // normal FT boot dispatch uses. Rate limit: at most 3 consecutive
    // panics before we give up and land at Home, so a crash-loop can't run
    // forever. The counter resets on any clean upload completion (see
    // setFtUploadInProgress(false)).
    const bool wasPanic = HalSystem::isRebootFromPanic();
    if (wasPanic) {
      const uint32_t attempts = ftPanicRecoveryAttempts + 1;
      if (attempts <= FT_PANIC_RECOVERY_MAX_ATTEMPTS) {
        ftPanicRecoveryAttempts = attempts;
        g_ftPanicRecoveryPendingToFt = true;
        LOG_INF("BOOT",
                "FT panic-recovery: attempt %u/%u -- routing to FT after setup; "
                "browser will auto-resume on WS reconnect",
                attempts, FT_PANIC_RECOVERY_MAX_ATTEMPTS);
      } else {
        LOG_ERR("BOOT",
                "FT panic-recovery: %u consecutive panics exceeds limit %u; "
                "giving up, landing at Home for manual re-entry",
                attempts, FT_PANIC_RECOVERY_MAX_ATTEMPTS);
        ftUploadInProgressFlag = 0;
        ftUploadResumeFailCount = 0;
        ftPanicRecoveryAttempts = 0;
      }
    } else {
      // Non-panic reboot with the FT flag set (unusual -- something else
      // rebooted us mid-upload). Clear and fall through as before.
      LOG_INF("BOOT",
              "FT upload flag set but reboot cause != panic; clearing flag, "
              "booting to Home");
      ftUploadInProgressFlag = 0;
      ftUploadResumeFailCount = 0;
      ftPanicRecoveryAttempts = 0;
    }
  }

  // CrumBLE 4.5: lean-boot path for silent-restart-to-OTA. The mbedtls SSL
  // handshake to api.github.com needs ~40-50 KB contiguous on top of WiFi's
  // ~58 KB share, and a normal boot's cover/library/recent/koreader/opds
  // loads + BT setup eat ~50 KB before OTA ever dispatches -- leaving the
  // post-WiFi MaxAlloc around ~12 KB (SSL setup -> -0x7F00). Skipping those
  // loads when we know the next activity is OTA hands the handshake the
  // full ~115 KB clean heap. Anything skipped here is reloaded by the
  // normal boot that follows OtaUpdateActivity::onExit's silentRestart().
  const bool isOtaSilentReboot = (isSilentReboot && (snapshotTarget == SILENT_REBOOT_TARGET_OTA_UPDATE ||
                                                     snapshotTarget == SILENT_REBOOT_TARGET_OTA_INSTALL));
  g_leanBootForOta = isOtaSilentReboot;
  // v18.9.9.376: also skip SD font on FT boot. User-verified field data:
  // running with Bitter+LXGW (~10 KB primary SD font) made FT api-files
  // time out at weak WiFi; switching to just Bitter (~7 KB) got api-files
  // to stream 33 rows in 2.2s. Skipping the SD-font load entirely on the
  // FT boot dispatch keeps the FT UI on built-in fonts only (which is
  // what FT rendering uses anyway) and returns ~10 KB + fragmentation to
  // the WiFi/webserver working budget. On FT exit the boot target flips
  // back and the reader/Home get their SD font on the next normal boot.
  const bool isFileTransferSilentReboot =
      (isSilentReboot && snapshotTarget == SILENT_REBOOT_TARGET_FILE_TRANSFER);

  // v18.9.9.458: extend the SD-font-skip to network-op silent-reboots. KOR
  // auth / OPDS browser / Font Download / Clock Sync / Wifi Selection all
  // render simple UI (labels, progress, list) that uses built-in fonts
  // only; the SD-font primary (~7 KB) is dead weight on those boots and
  // trims WiFi/mbedtls heap budget. Post-op exit → normal boot → SD font
  // loads as usual for reader/home.
  const bool isNetworkOpSilentReboot =
      (isSilentReboot && (snapshotTarget == SILENT_REBOOT_TARGET_KOREADER_AUTH ||
                          snapshotTarget == SILENT_REBOOT_TARGET_OPDS_BROWSER ||
                          snapshotTarget == SILENT_REBOOT_TARGET_FONT_DOWNLOAD ||
                          snapshotTarget == SILENT_REBOOT_TARGET_WIFI_SELECTION ||
                          snapshotTarget == SILENT_REBOOT_TARGET_CLOCK_SYNC ||
                          snapshotTarget == SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC));

  // CrumBLE 4.5.7 v18: skip the boot-time LibraryIndex/SeriesIndex/CollectionsStore
  // loads when this boot's target activity has nothing to do with the library
  // shelf (reader, BT settings, koreader auth, OPDS, file transfer). The three
  // stores are used only by Home and by the "Recently Added" / "All Books"
  // shelves; when we come up straight into the reader after a defrag restart
  // (silent-restart-to-reader with post-boot EnableBt), loading them at setup()
  // costs ~12 KB of contiguous heap that BT+reader on X4 can't spare. Reader
  // itself already releases them in onEnter (EpubReaderActivity.cpp:356), so
  // skipping the load is functionally equivalent -- and avoids the load-then-
  // release fragmentation churn. Home target silent-restart still loads eager
  // so the shelf is ready without a first-visit rebuild delay.
  const bool isLibraryLightBoot = isOtaSilentReboot ||
      (isSilentReboot && (snapshotTarget == SILENT_REBOOT_TARGET_READER ||
                          snapshotTarget == SILENT_REBOOT_TARGET_BT_SETTINGS ||
                          snapshotTarget == SILENT_REBOOT_TARGET_KOREADER_AUTH ||
                          snapshotTarget == SILENT_REBOOT_TARGET_OPDS_BROWSER ||
                          snapshotTarget == SILENT_REBOOT_TARGET_FONT_DOWNLOAD ||
                          snapshotTarget == SILENT_REBOOT_TARGET_WIFI_SELECTION ||
                          snapshotTarget == SILENT_REBOOT_TARGET_CLOCK_SYNC ||
                          snapshotTarget == SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC ||
                          snapshotTarget == SILENT_REBOOT_TARGET_FILE_TRANSFER));

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();
  ReadingStats::begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  LOG_INF("BOOT", "Post-GPIO diagnostic: device=%s usb=%d silentReboot=%d silentTarget=%lu",
          gpio.deviceIsX3() ? "X3" : "X4", gpio.isUsbConnected() ? 1 : 0, isSilentReboot ? 1 : 0,
          static_cast<unsigned long>(snapshotTarget));

  // CrumBLE 4.7.2: resolve which panel controller this unit carries, BEFORE the
  // SD card is mounted. Xteink switched newer X4/X3 runs from SSD1677/UC8253 to
  // the UltraChip UC8179/UC8279 on the same board, so the driver can only be
  // picked by asking the silicon -- but the probe bit-bangs the display pins as
  // raw GPIO, and SD shares SCLK/MOSI on this hardware. Running it after
  // Storage.begin() hung X4 boot (the disturbed card clamps the shared bus and
  // the panel never hears CMD_SOFT_RESET); XteinkDetect.h's contract is to call
  // it before SDCardManager::begin(), which is what this placement honors.
  //
  // The probe reads its pins from BoardConfig::ACTIVE, so the X3/X4 profile has
  // to be chosen first. ACTIVE defaults to XTEINK_X4; point it at the X3 sibling
  // when our own fingerprint says X3, so the promotion lands on UC8253->UC8279
  // rather than SSD1677->UC8179. On a confirmed X3 promotion, switch to the
  // XteinkX3Uc8279 profile: HalDisplay::begin()'s setDisplayX3() re-selects
  // XteinkX3 otherwise, which would reset displayController back to UC8253.
  //
  // Fail-safe: promotion needs two independent passes to both match the UC81xx
  // VER/FLG signature AND agree byte-for-byte. An original SSD1677/UC8253 does
  // not answer register 0x70 at all, so the bus floats, both passes fail, and
  // the profile default stands -- existing units keep today's driver.
#ifndef CRUMBLE_DISABLE_PANEL_PROBE
  // v4.7.2: assert the X4 battery-MOSFET latch (GPIO13) via holdPowerRails().
  // No-op on self-latching X4s; the revision that doesn't self-latch stays
  // powered only while the button is held without it.
  //
  // X4 only, and nothing here on X3 -- both learned on hardware. Probing the X4
  // display bus hung display init; calling selectDevice(XteinkX3) here switched
  // sd.powerEnable to GPIO13 and hung Storage.begin(). X3 must keep ACTIVE on the
  // XTEINK_X4 default until setDisplayX3(), as every release before this one.
  if (!gpio.deviceIsX3()) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
    LOG_INF("DISPLAY", "X4: SSD1677 (default); battery latch asserted");
  } else {
    LOG_INF("DISPLAY", "X3: UC8253 (default); profile deferred to setDisplayX3()");
  }
#endif

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();

  // v18.9.9.343 BT-off migration REVERTED in v18.9.9.344 -- field logs
  // showed esp_bt_mem_release only reclaimed ~1.6 KB, not the estimated
  // ~58 KB (NimBLE-Arduino's static allocations are baked into .bss and
  // don't participate in esp_bt_mem_release). The migration destroyed
  // users' bluetoothEnabled setting for no meaningful heap gain. Left
  // the hasAppliedBtOffMigration_v343 flag in the schema (harmless)
  // so users don't get a repeated reset if they downgrade+upgrade.

  // v18.9.9.245: BT-off-by-default at boot. SETTINGS.bluetoothEnabled has
  // always defaulted to 0 (fresh install = no BT), but the esp32-hal-bt-mem.h
  // constructor in BluetoothHIDManager.cpp reserves ~25 KB of controller
  // memory REGARDLESS of the setting so runtime enable() has a working
  // stack. That reservation is pure heap tax when the user isn't using
  // Bluetooth. Release it here now that we've loaded the setting: safe
  // because if the user later flips bluetoothEnabled=1, the enable()
  // failure path in BluetoothSettingsActivity silent-restarts back into
  // a boot where bluetoothEnabled=1 stays reserved (this branch skipped).
  // Same release sequence used by CrossPointWebServerActivity when
  // entering FT mode.
  //
  // v18.9.9.251: skip the release when a silent-restart-with-EnableBt is
  // pending this boot. Fixes a load-fault crash where the user tapped BT
  // Quick Connect while bluetoothEnabled=0, ERA armed EnableBt +
  // silent-restart, post-boot dispatched the EnableBt action against a
  // controller whose BLE memory was JUST released by this branch --
  // esp_bt_controller_init() then walked garbage internal queues inside
  // bt_controller_deinit_internal (RA 0x42195258 in the crash) and
  // vQueueDelete load-faulted at the uninitialised handle. On CrossPoint
  // upstream this never manifested because BT was init'd unconditionally
  // at boot. esp_bt_mem_release is one-way per boot -- once we've called
  // it, no path in this boot cycle can re-init the controller, so the
  // EnableBt action MUST run on a boot where the release didn't happen.
  //
  // v18.9.9.252: also set g_bleControllerMemReleased so any later
  // BluetoothHIDManager::enable() call in the SAME boot (e.g. user
  // navigates to BT Settings on a cold-boot BT-off session and toggles
  // BT on -- BluetoothSettingsActivity calls btMgr->enable() directly)
  // refuses cleanly instead of running the same crash path. The refuse
  // path silent-restarts to a fresh boot with bluetoothEnabled=1, where
  // this v245 branch is skipped and enable() succeeds naturally.
  #ifndef SIMULATOR
  const bool enableBtPending = g_pendingReaderPostBootAction == ReaderPostBootAction::EnableBt;
  if (!SETTINGS.bluetoothEnabled && !enableBtPending) {
    const uint32_t freeBefore = ESP.getFreeHeap();
    const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
    const esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();
    if (btStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
      esp_bt_controller_disable();
    }
    if (btStatus != ESP_BT_CONTROLLER_STATUS_IDLE) {
      esp_bt_controller_deinit();
    }
    esp_bt_mem_release(ESP_BT_MODE_BLE);
    extern bool g_bleControllerMemReleased;
    g_bleControllerMemReleased = true;
    LOG_INF("BOOT", "BT off (bluetoothEnabled=0): heap gained free=%d maxAlloc=%d",
            (int)(ESP.getFreeHeap()) - (int)freeBefore,
            (int)(ESP.getMaxAllocHeap()) - (int)maxAllocBefore);
  } else if (!SETTINGS.bluetoothEnabled && enableBtPending) {
    LOG_INF("BOOT", "BT off (bluetoothEnabled=0): SKIPPING release -- EnableBt action pending");
  }
  #endif

  // v18.9.9.50: first-boot settings-view-cache bootstrap. Consumer
  // activities (ReaderOptionsActivity, BookSettingsDrawerActivity) fall
  // back to the cache when heap is too tight to build the live
  // getSettingsList() -- a fresh install or an upgrade from pre-v50
  // firmware won't have written the cache yet, so seed it here while
  // heap is warm (~90 KB free / ~78 KB maxAlloc post-CollectionsStore).
  // Skipped when the cache is already present -- saveToFile() from any
  // subsequent settings write keeps it fresh.
  if (!settingsViewCacheExists()) {
    LOG_INF("BOOT", "Bootstrapping settings-view cache on first launch");
    saveSettingsViewCache(getSettingsList(&sdFontSystem.registry()));
  }

  // CrumBLE 4.2 one-shot migration: the slim-binary prebake crash that
  // forced optimizeChapterIndexing default=OFF in 4.1.0/4.1.1 is fixed
  // (dropped Lexend Deca instead of emoji, see release notes). Force ON
  // for users whose NVS still has the 4.1.x crisis-fix value, marked
  // via a small SD sentinel so we only run once per device.
  constexpr const char* kPrebake42MigrationFlag = "/.crosspoint/migrated_42_prebake";
  if (!Storage.exists(kPrebake42MigrationFlag)) {
    SETTINGS.optimizeChapterIndexing = 1;
    SETTINGS.saveToFile();
    FsFile mf;
    if (Storage.openFileForWrite("MAIN", kPrebake42MigrationFlag, mf)) {
      mf.print("4.2");
      mf.close();
    }
    LOG_INF("MAIN", "4.2 migration: optimizeChapterIndexing forced ON (prebake crash fixed)");
  }

  // CrumBLE 4.5.164 one-shot migration: flip showIndexingPageCount OFF for
  // existing users. Field feedback: the "Indexing page X of ~Y" popup makes
  // long indexes feel slower even though the wall-clock is unchanged. The
  // classic "Indexing..." + animated dots reads as patient rather than
  // laboring. Users who want the count can opt in from Settings. Marked
  // via SD sentinel so we only nudge once per device (a user who deliberately
  // turned it back on after this migration keeps their choice).
  constexpr const char* kIndexingPageCountMigrationFlag = "/.crosspoint/migrated_164_indexing";
  if (!Storage.exists(kIndexingPageCountMigrationFlag)) {
    SETTINGS.showIndexingPageCount = 0;
    SETTINGS.saveToFile();
    FsFile mf;
    if (Storage.openFileForWrite("MAIN", kIndexingPageCountMigrationFlag, mf)) {
      mf.print("4.5.164");
      mf.close();
    }
    LOG_INF("MAIN", "4.5.164 migration: showIndexingPageCount default OFF (indexing feels less like waiting)");
  }

  // CrumBLE 4.5.167 one-shot migration: the pre-Minimal-Stats "Reading Stats"
  // sleep screen (mode 7) is removed from the picker. Users who had it
  // selected get auto-upgraded to Minimal Stats (mode 10) — same "stats on
  // sleep" intent, better rendering. Marked via SD sentinel so we only
  // migrate once (users who later pick a different mode after this migration
  // keep their choice).
  constexpr const char* kReadingStatsSleepMigrationFlag = "/.crosspoint/migrated_167_stats_sleep";
  if (!Storage.exists(kReadingStatsSleepMigrationFlag)) {
    if (SETTINGS.sleepScreen == CrossPointSettings::READING_STATS_SLEEP) {
      SETTINGS.sleepScreen = CrossPointSettings::MINIMAL_STATS_SLEEP;
      SETTINGS.saveToFile();
      LOG_INF("MAIN", "4.5.167 migration: sleepScreen Reading Stats → Minimal Stats");
    }
    FsFile mf;
    if (Storage.openFileForWrite("MAIN", kReadingStatsSleepMigrationFlag, mf)) {
      mf.print("4.5.167");
      mf.close();
    }
  }

  // Always clamp the reader font family to an available built-in. The
  // settings.json on SD persists across firmware flavours, so a user
  // who downgraded LEXENDDECA -> slim (which OMITs Lexend) keeps a
  // stale fontFamily=LEXENDDECA value -- the picker's withEnumRawValues
  // gate filters that out, rendering the family slot blank. Falling
  // back to the variant's BUILTIN_DEFAULT_FONT_FAMILY gives the picker
  // something to draw and stops the "select Bitter -> mismatch prompt"
  // surprise. Runs every boot so re-installs / SD swaps self-heal.
  // 4.2.1: also clamp stale BITTER for the tiny-lexend / tiny-chareink
  // variant builds. Routing target is now BUILTIN_DEFAULT_FONT_FAMILY
  // (resolves at compile time per variant: BITTER on tiny-bitter,
  // LEXENDDECA on tiny-lexend, CHAREINK on tiny-chareink).
  bool fontFamilyClamped = false;
#ifdef OMIT_LEXENDDECA_FONT
  if (SETTINGS.fontFamily == CrossPointSettings::LEXENDDECA) {
    SETTINGS.fontFamily = CrossPointSettings::BUILTIN_DEFAULT_FONT_FAMILY;
    LOG_INF("MAIN", "Font family clamp: LEXENDDECA -> built-in default (Lexend not in this build)");
    fontFamilyClamped = true;
  }
#endif
#ifdef OMIT_CHAREINK_FONT
  if (SETTINGS.fontFamily == CrossPointSettings::CHAREINK) {
    SETTINGS.fontFamily = CrossPointSettings::BUILTIN_DEFAULT_FONT_FAMILY;
    LOG_INF("MAIN", "Font family clamp: CHAREINK -> built-in default (CharEink not in this build)");
    fontFamilyClamped = true;
  }
#endif
#ifdef OMIT_BITTER_FONT
  if (SETTINGS.fontFamily == CrossPointSettings::BITTER) {
    SETTINGS.fontFamily = CrossPointSettings::BUILTIN_DEFAULT_FONT_FAMILY;
    LOG_INF("MAIN", "Font family clamp: BITTER -> built-in default (Bitter not in this build)");
    fontFamilyClamped = true;
  }
#endif
  if (fontFamilyClamped) SETTINGS.saveToFile();
  APP_STATE.loadFromFile();

  // CrumBLE 4.4: post-firmware-update cover-thumb retry. The 4.4 EOCD scan
  // bump (1KB -> 4KB) lets the bookshelf decode covers from re-packaged
  // EPUBs (Anna's Archive etc.) that previously failed. Anyone who hit
  // that bug under earlier firmware now has thumb_failed_v3_*.marker
  // files poisoning every cover-gen retry; sweep them once per version
  // change so the fix actually takes effect without manual intervention.
  // Manual lever lives at Settings > System > Retry Failed Covers for
  // ad-hoc re-attempts (e.g. user freed heap, replaced a book file).
  // Lean-boot OTA path skips the sweep -- it'll run on the normal boot
  // that follows OTA exit.
  if (!isOtaSilentReboot && APP_STATE.lastCrumbleVersion != CRUMBLE_VERSION) {
    const int swept = CoverThumbStatus::sweepAllMarkers();
    LOG_INF("BOOT", "Firmware version changed (%s -> %s); swept %d cover-failed marker(s)",
            APP_STATE.lastCrumbleVersion.empty() ? "<none>" : APP_STATE.lastCrumbleVersion.c_str(),
            CRUMBLE_VERSION, swept);
    APP_STATE.lastCrumbleVersion = CRUMBLE_VERSION;
    APP_STATE.saveToFile();
  }

  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // v18.9.9.377: also skip these on FT boot. WEBACT teardown doesn't release
  // them (only LibraryIndex/Series/Collections), so on the pre-v377 path they
  // stayed pinned through the entire FT session -- 2-7 KB of heap depending on
  // user data (RecentBooks scales with history length, OPDS with server count).
  // FT UI reads none of these; the normal boot after FT exit reloads them.
  const bool skipHomeStores = isOtaSilentReboot || isFileTransferSilentReboot;
  if (!skipHomeStores) {
    RECENT_BOOKS.loadFromFile();
    KOREADER_STORE.loadFromFile();
    OPDS_STORE.loadFromFile();
  } else {
    LOG_INF("BOOT", "Lean-boot (%s): skipping RecentBooks/KOReader/OPDS/BT-callbacks/Library/Series/Collections",
            isOtaSilentReboot ? "OTA" : "FT");
  }

#ifndef SIMULATOR
  // v18.9.9.377: FT boot also doesn't need BT callbacks (WEBACT explicitly
  // disables BT). setBondedDevice below allocates a small std::string; the
  // std::function assignments hold captures. Not huge but no reason to pay
  // it when the callbacks will never fire this boot.
  if (!isOtaSilentReboot && !isFileTransferSilentReboot) {
    // OTA/FT do not enable BT; the callbacks aren't needed on this boot.
    auto& btMgr = BluetoothHIDManager::getInstance();
    btMgr.setButtonInjector([](uint8_t buttonIndex, bool pressed) {
      // CrumBLE 4.5.5: BTN_ACTION_REFRESH_SCREEN sentinel (0xFE) routes the
      // press to the same FORCE_REFRESH path the Power button uses (see
      // executeShortPowerButtonAction in this file). Out-of-range vs the
      // HalGPIO::BTN_* virtual-button enum (0-6), so no risk of colliding
      // with a real button. Release is a no-op -- a refresh is a one-shot
      // action and the renderer waveform already self-terminates.
      constexpr uint8_t kBtnActionRefreshScreen = 0xFE;
      if (buttonIndex == kBtnActionRefreshScreen) {
        if (pressed) {
          LOG_DBG("MAIN", "BLE-mapped Refresh Screen action triggered");
          RenderLock lock;
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
        return;
      }
      gpio.setVirtualButtonState(buttonIndex, pressed);
    });
    btMgr.setButtonActivityNotifier([](uint8_t buttonIndex) { gpio.updateVirtualButtonActivity(buttonIndex); });
    btMgr.setReaderContextCallback([]() { return gBluetoothReaderContext; });
    btMgr.setBondedDevice(SETTINGS.bleBondedDeviceAddr, SETTINGS.bleBondedDeviceName);
    // CrumBLE 4.5.5: wire the rich-map resolver. The HAL must not depend on
    // CrossPointSettings, so the lookup lives here and is called from
    // mapKeycodeToButton for every HID report. Linear 12-entry scan; trivial.
    btMgr.setBleKeyMapResolver([](uint8_t kind, uint8_t value) -> uint8_t {
      for (const auto& e : SETTINGS.bleKeyMap) {
        if (e.button == 0xFF || e.keyKind == 0xFF) continue;
        if (e.keyKind == kind && e.keyValue == value) return e.button;
      }
      return 0xFF;
    });
  }
#endif  // SIMULATOR: BLE injection uses ESP32 GPIO virtual buttons; no-op in sim.

  const auto wakeupReason = gpio.getWakeupReason();
  LOG_INF("BOOT", "Wake route: %s", wakeupRouteName(wakeupReason));
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      if (SETTINGS.cycleScreensaverOnTap) {
        if (detectScreensaverCycleTap()) {
          cycleScreensaverThenDeepSleep();
        }
        // Held past the tap window — proceed to wake. Our raw-GPIO detect already
        // confirmed the hold duration, so skip the InputManager-based verify which
        // would otherwise miss presses that release just after its debounce wakes up.
      } else {
        LOG_INF("BOOT", "Power-button wake: verifying duration required=%u shortAllowed=%d",
                SETTINGS.getPowerButtonWakeDuration(),
                SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
        gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonWakeDuration(),
                                     SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // TEMP: continue booting while diagnosing post-flash/reset behavior.
      // Normal behavior is to go back to sleep when USB power causes a cold boot.
      LOG_INF("BOOT", "AfterUSBPower route: TEMP continuing boot instead of deep sleep");
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
      LOG_INF("BOOT", "AfterFlash route: continuing boot");
      break;
    case HalGPIO::WakeupReason::Other:
    default:
      LOG_INF("BOOT", "Other wake route: continuing boot");
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrumBLE " CRUMBLE_VERSION " (CrossInk " CROSSINK_VERSION ")");

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  if (isOtaSilentReboot) {
    LOG_INF("MEM", "Boot step pre-setupDisplayAndFonts: free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }
  // v18.9.9.1: hard restart forces seamless=false so HalDisplay::begin
  // runs its wakeup-gated requestResync() and X3 initial-full-sync arming,
  // giving a clean panel state before the first paint.
  const bool seamlessBoot = (resume != BootResume::Splash) && !isHardRestart;
  // v18.9.9.376: lean-boot skip also for FT target (see above rationale).
  // v18.9.9.458: extended to KOR/OPDS/FontDL/WifiSel/ClockSync — network-op
  // paths that only need built-in fonts. Reclaims ~7 KB + fragmentation.
  setupDisplayAndFonts(seamlessBoot,
                       isOtaSilentReboot || isFileTransferSilentReboot || isNetworkOpSilentReboot);

  // CrumBLE 4.6 LAN-OTA re-anchor: if the device just booted from ota_1
  // because of a recent LAN-OTA install, flash the same bin into ota_0 so
  // future USB flashes via the CrossPoint web flasher (which writes ota_0
  // and doesn't touch otadata) take effect on first try. ~60-120s blocking.
  // See network/FirmwareFlasher.h:maybeRelocateLanOtaToOta0 for the full
  // rationale. Skip on lean-OTA silent reboots -- those are for the
  // LAN-OTA INSTALL pass, not the post-install boot; the install path
  // already reboots and lands here on the first non-lean boot.
  if (!isOtaSilentReboot && firmware_flash::relocateLanOtaPending()) {
    LOG_INF("MAIN", "LAN-OTA relocate-to-ota_0 needed -- rendering finalize screen");
    renderer.clearScreen();
    const int sw = renderer.getScreenWidth();
    const int sh = renderer.getScreenHeight();
    const int h = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, sh / 2 - h * 2, "Finalizing update");
    renderer.drawCenteredText(UI_10_FONT_ID, sh / 2, "Writing recovery copy to flash...");
    renderer.drawCenteredText(UI_10_FONT_ID, sh / 2 + h, "Device will restart automatically.");
    renderer.drawCenteredText(UI_10_FONT_ID, sh / 2 + h * 2, "Do not unplug.");
    renderer.displayBuffer();
    (void)sw;
    const auto reloc = firmware_flash::maybeRelocateLanOtaToOta0(nullptr, nullptr);
    if (reloc == firmware_flash::RelocateResult::RELOCATED) {
      LOG_INF("MAIN", "LAN-OTA relocate ok -- restarting into ota_0");
      delay(500);
      ESP.restart();  // does not return
    }
    LOG_ERR("MAIN", "LAN-OTA relocate skipped/failed -- continuing on current partition");
  }

  // v18.9.9.260: pending sleep-image bake, triggered by
  // silentRestartToBakeSleepImages() from the Settings action. Runs on
  // the freshly-booted heap (~93 KB free) which comfortably fits
  // PNGdec's 60 KB working set + BMP row buffers. Cleared unconditionally
  // so a crash mid-bake doesn't re-trigger on the recovery boot.
  if (hasPendingBakeSleepImages()) {
    clearPendingBakeSleepImages();
    LOG_INF("MAIN", "Post-boot: running deferred bake sleep images (free=%u maxAlloc=%u)",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const Rect popupRect = GUI.drawPopup(renderer, tr(STR_BAKING_SLEEP_IMAGES));
    static Rect s_pbPopupRect;
    static GfxRenderer* s_pbRenderer = nullptr;
    s_pbPopupRect = popupRect;
    s_pbRenderer = &renderer;
    auto onProgress = +[](int done, int total) {
      if (total <= 0 || !s_pbRenderer) return;
      GUI.fillPopupProgress(*s_pbRenderer, s_pbPopupRect, (done * 100) / total);
    };
    const SleepActivity::BakeResult result = SleepActivity::bakeAllSleepImages(renderer, onProgress);
    s_pbRenderer = nullptr;
    char msg[96];
    if (result.total == 0) {
      std::snprintf(msg, sizeof(msg), "%s", tr(STR_BAKE_SLEEP_NONE));
    } else {
      std::snprintf(msg, sizeof(msg), tr(STR_BAKE_SLEEP_DONE), result.baked, result.skipped);
    }
    GUI.drawPopup(renderer, msg);
    delay(1500);
    // Fall through to normal boot flow -- lands on Home per
    // silentRebootTarget == HOME.
  }

  // CrumBLE 4.3 option 3: page-heap reserve was acquired at boot here, but
  // that starves the File Transfer web server (HTML serve needs ~20 KB
  // contiguous; held 18 KB reserve drops MaxAlloc below 5 KB → serve fails
  // → silent-restart loop into FT). The reserve is now acquired lazily by
  // loadPageFromSectionFile()'s opportunistic-reacquire path on first
  // chapter open, so FT, Home, and other non-reader activities run with
  // the full heap.

  // LibraryIndex loads its cached JSON now (cheap); the heavy SD walk is
  // deferred until the user first accesses Recently Added / All Books on
  // the shelf so boot stays fast. Lean-boot OTA path skips this too --
  // OTA doesn't touch the library and the next normal boot will load it.
  //
  // CrumBLE 4.5.7 v18: extend the skip to any non-home silent-restart target.
  // Reader / BT settings / auth-wizard boots don't touch the library shelf,
  // and reader's onEnter releases the stores anyway -- loading them here just
  // costs ~12 KB + a fragmentation event before BT enable. Home-target
  // silent-restart still loads so the shelf is ready without a first-visit
  // rebuild.
  if (!isLibraryLightBoot) {
    LOG_INF("MEM", "pre-LibraryIndex.begin: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    LibraryIndex::getInstance().begin();
    LOG_INF("MEM", "post-LibraryIndex.begin: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    SeriesIndex::getInstance().begin();
    LOG_INF("MEM", "post-SeriesIndex.begin: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // CrumBLE: All Books / Recently Added are opt-in -- we no longer run the
    // whole-SD walk at boot (it was the cause of the long first-boot indexing
    // people complained about). On the first boot after this update, migrate
    // existing users (anyone who already has a library index) so those
    // collections stay visible; fresh installs leave them hidden until the user
    // opts in from the Home menu (which prompts before scanning). Must run before
    // CollectionsStore::begin() seeds the virtuals from these settings.
    if (SETTINGS.virtualCollectionsDefaultPending) {
      const bool existingUser = !LibraryIndex::getInstance().wasFreshFirstBoot();
      SETTINGS.showRecentlyAddedCollection = existingUser ? 1 : 0;
      SETTINGS.showAllBooksCollection = existingUser ? 1 : 0;
      SETTINGS.virtualCollectionsDefaultPending = false;
      SETTINGS.saveToFile();
    }

    CollectionsStore::getInstance().begin();
    LOG_INF("MEM", "post-CollectionsStore.begin: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  // Execute a pending deferred delete *before* the activity dispatch fires.
  // FileBrowserActivity stashed the path here when the user confirmed delete
  // but on-device heap was too fragmented to safely run BookActions::
  // clearFileMetadata + Storage.remove (typical after closing a heap-heavy
  // CJK book). Storage, LibraryIndex, SeriesIndex, CollectionsStore are all
  // up by this point and the heap is fresh (~85 KB MaxAlloc), so the
  // recursive metadata sweep + library forgetPath calls go through cleanly.
  // Clear the RTC slot BEFORE executing so any crash in the delete path
  // doesn't loop the user into a delete-restart-crash cycle on next boot.
  if (hasPendingDelete()) {
    const std::string deferredPath = getPendingDeletePath();
    const uint8_t deferredAction = getPendingDeleteAction();
    clearPendingDelete();
    const char* actionName = (deferredAction == 2) ? "clear-book-cache"
                              : (deferredAction == 1) ? "dir-delete"
                                                       : "file-delete";
    LOG_INF("BOOT", "Pending deferred op picked up: action=%s path=%s",
            actionName, deferredPath.c_str());
    BookActions::executeDeferredOperation(deferredPath, deferredAction);
    LOG_INF("MEM", "post-deferred-op: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  switch (resume) {
    case BootResume::Silent:
      // CrumBLE 4.4 post-bisect: paint the saved pre-restart framebuffer via
      // HALF refresh, mirroring the QuickResume sleep/wake pattern. The boot
      // HALF cycle is physically unavoidable (SDK power-init is bundled with
      // HALF for the first paint after begin()), so landing on the user's
      // previous content during that cycle turns a "cold-boot black/white
      // flash" into a "quick-resume flash" -- same technical refresh, very
      // different perceived UX. The activity's first render then FAST-refreshes
      // to the new content via ReaderUtils::displayWithRefreshCycle's
      // isContinuingFromSilentReboot branch. Falls through gracefully if the
      // snapshot is missing (first boot after this change, SD error, prior
      // restart's snapshot path didn't run).
      // v18.9.9.1: skip the HALF resume paint on hard restart -- the
      // sleep_frame is stale (terminate handler doesn't save one) and
      // the panel is in an unknown mid-render state; letting the activity's
      // first render do a FULL over a resync'd panel avoids ghosting.
      if (!isHardRestart && loadSleepFrameBuffer()) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageWidth = renderer.getScreenWidth();
        const auto pageHeight = renderer.getScreenHeight();
        // CrumBLE: brand cookie logo as the quick-resume status badge.
        // We draw a circular white halo first (fillRoundedRect with
        // cornerRadius = halfSide -> perfect circle) then layer the
        // square cookie on top. The 6 px halo gives the logo's edges
        // some breathing room from the sleep frame around it and keeps
        // the cookie silhouette legible on any background (cover, custom).
        constexpr int kLogoSize = 120;
        constexpr int kHaloPadding = 6;
        constexpr int kHaloSize = kLogoSize + kHaloPadding * 2;
        const int haloX = (pageWidth - kHaloSize) / 2;
        const int haloY = (pageHeight - kHaloSize) / 2;
        renderer.fillRoundedRect(haloX, haloY, kHaloSize, kHaloSize, kHaloSize / 2, Color::White);
        renderer.drawImage(Logo120, haloX + kHaloPadding, haloY + kHaloPadding, kLogoSize, kLogoSize);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // v18.9.9.349: boot-time invisible NTP sync. Prerequisites: cold-boot
  // splash path (never on silent-reboot/quick-resume), Home clock is
  // enabled, halClock has no valid time, WiFi creds exist, not going to
  // any override destination (recovery, panic, silent-restart target,
  // sleep-wake reader resume). We run the sync HERE so the "Syncing
  // time..." popup lands on top of the splash and the user perceives
  // one continuous boot instead of Home->Loading->boot->Home. Replaces
  // the v343 silent-restart-to-HomeClockSync flow that fired after
  // Home rendered.
  const bool eligibleBootNtp =
      resume == BootResume::Splash &&
      !recoveryFirmwareMode &&
      !HalSystem::isRebootFromPanic() &&
      snapshotTarget == SILENT_REBOOT_TARGET_HOME &&  // 0; also true on cold-boot when !isSilentReboot
      !isSilentReboot &&
      SETTINGS.homeClockShow &&
      !halClock.hasValidTime() &&
      !homeBootClockSyncAlreadyAttempted();
  if (eligibleBootNtp) {
    // Lazy-check credentials to skip the noisier "no WiFi" popup path
    // entirely on a fresh install with no networks configured.
    if (WIFI_STORE.getCredentials().empty()) WIFI_STORE.loadFromFile();
    if (!WIFI_STORE.getCredentials().empty()) {
      markHomeBootClockSyncAttempted();  // gate against repeat attempts on further silent-restarts this session
      runBootTimeNtpSyncOverBootScreen();
    }
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (g_ftPanicRecoveryPendingToFt) {
    // v18.9.9.438: FT panic auto-recovery. The early boot panic-check
    // detected an FT upload was in progress and set this flag; skip the
    // crash-report screen and route directly to FT so the browser's WS
    // retry loop reconnects and resumes the upload from the last fsync'd
    // 256 KB boundary. Consuming the flag here (as opposed to the RTC
    // counter) is what makes this idempotent across dispatch re-entries.
    g_ftPanicRecoveryPendingToFt = false;
    LOG_INF("BOOT", "FT panic-recovery: dispatching to FileTransfer");
    activityManager.goToFileTransfer();
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_FILE_TRANSFER) {
    // CrumBLE: heap-defrag restart triggered from CrossPointWebServer when
    // the FT serve guard fires. Route directly to FT so the user doesn't
    // see Home flash through before re-entering. They'll still pick mode
    // (Hotspot vs Join) again -- that screen is the FT activity's normal
    // first step on entry.
    activityManager.goToFileTransfer();
  } else if (resume == BootResume::Silent && (snapshotTarget == SILENT_REBOOT_TARGET_OTA_UPDATE ||
                                              snapshotTarget == SILENT_REBOOT_TARGET_OTA_INSTALL)) {
    // CrumBLE 4.5/4.6: heap-defrag restart from OtaUpdateActivity. Route
    // straight to OTA so the user doesn't see Home flash through.
    // OTA_UPDATE target = restart from the pre-flight bottom; the activity
    // starts in WIFI_SELECTION and runs check then install.
    // OTA_INSTALL target = restart after user confirmed install on a check
    // that already succeeded; consumePendingOtaInstall() in the activity
    // restores URL/size/version from RTC and the activity jumps straight
    // to install (skipping check) so the second handshake gets the same
    // fresh ~94 KB MaxAlloc the first one had.
    LOG_INF("BOOT", "Lean-boot OTA dispatch: target=%s heap=%u maxAlloc=%u",
            snapshotTarget == SILENT_REBOOT_TARGET_OTA_INSTALL ? "install" : "update", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    activityManager.goToOtaUpdate();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_BT_SETTINGS) {
    // CrumBLE 4.5.3: heap-defrag restart from BluetoothSettingsActivity.
    // Route straight back to BT settings on the freshly-recovered heap.
    // g_postBtSilentReboot flips so the activity's pre-flight knows not
    // to silent-restart again -- one attempt only, then real error.
    LOG_INF("BOOT", "Lean-boot BT dispatch: heap=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postBtSilentReboot = true;
    // v18.9: pass through scan-intent so the activity auto-enters scan view
    // instead of dropping the user on menu row 0.
    if (postBtSilentRebootScanIntentMagic == POST_BT_SILENT_REBOOT_SCAN_INTENT_MAGIC) {
      g_postBtSilentRebootScanIntent = true;
      postBtSilentRebootScanIntentMagic = 0;
    }
    activityManager.goToBluetoothSettings();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // v18.9.5: BT-menu -> Back with disableOnExit true lands here so the
    // user stays in the Settings root instead of getting dropped on Home.
    LOG_INF("BOOT", "Lean-boot Settings dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    activityManager.goToSettings();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_KOREADER_AUTH) {
    // CrumBLE 4.5.4: same pattern as BT, scoped to the KOReader auth flow.
    LOG_INF("BOOT", "Lean-boot KOReader auth dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postKoreaderSilentReboot = true;
    activityManager.goToKoreaderAuth();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_OPDS_BROWSER) {
    // CrumBLE 4.5.4: same pattern as BT, scoped to OPDS feed-fetch entry.
    // goToBrowser() handles the single-server-direct vs picker fork so
    // we don't have to persist which server was being accessed.
    LOG_INF("BOOT", "Lean-boot OPDS dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postOpdsSilentReboot = true;
    activityManager.goToBrowser();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_FONT_DOWNLOAD) {
    // v18.9.9.308: same pattern as KOReader/OPDS, scoped to Manage Fonts.
    // Lands the user back in the wizard on a fresh ~150 KB heap so
    // WiFi.mode(WIFI_STA)'s 4 RX buffer alloc + mbedtls handshake all
    // fit, instead of dumping to Home and forcing them to re-navigate
    // Settings > Reader > Font > Manage Fonts.
    LOG_INF("BOOT", "Lean-boot Font Download dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postFontDownloadSilentReboot = true;
    activityManager.goToFontDownload();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_WIFI_SELECTION) {
    // v18.9.9.336: WiFi selection direct-land. User hit Network -> WiFi from
    // Settings; that path called silentRestartToWifiSelection() to avoid
    // the wpa_supplicant crash observed on WiFi.mode(WIFI_STA) with a
    // fragmented in-book heap (checkpoint wifi:mode-STA). Land the user
    // back in WifiSelectionActivity on the ~150 KB fresh boot heap where
    // esp_wifi_init + supplicant setup fit cleanly.
    LOG_INF("BOOT", "Lean-boot WiFi Selection dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postWifiSelectionSilentReboot = true;
    activityManager.goToWifiSelection();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_CLOCK_SYNC) {
    // v18.9.9.337: same pattern for ClockSync. NTP sync goes through
    // WiFi.begin() -> same wpa_supplicant crash class. Fresh-heap land
    // avoids it.
    LOG_INF("BOOT", "Lean-boot Clock Sync dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postClockSyncSilentReboot = true;
    activityManager.goToClockSync();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_HOME_CLOCK_SYNC) {
    // v18.9.9.343: automatic boot-time sync so Home clock has time.
    // Same lean-boot as regular Clock Sync above, plus the return-to-Home
    // flag so ClockSyncActivity silent-restarts back to Home after the
    // sync finishes -- user never sees a "Sync Time" dialog they didn't
    // ask for.
    LOG_INF("BOOT", "Lean-boot Home Clock Sync dispatch: heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    g_postClockSyncSilentReboot = true;      // same heap-preflight bypass
    g_postHomeClockSyncSilentReboot = true;  // triggers auto-return to Home
    activityManager.goToClockSync();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             g_pendingXtcPath[0] != '\0') {
    // v18.9.9.37: XTC continuation of a silent-restart-with-defrag from
    // XtcReaderActivity's page-buffer alloc failure. Reopen via
    // ReaderActivity's normal file-dispatch (extension detection routes
    // to XtcReaderActivity again on the clean heap). Path snapshot is
    // consumed one-shot; clear so a subsequent unrelated silent-restart
    // doesn't accidentally reopen this file.
    const std::string xtcPath(g_pendingXtcPath);
    g_pendingXtcPath[0] = '\0';
    LOG_INF("BOOT", "Silent-restart XTC dispatch: reopening %s", xtcPath.c_str());
    activityManager.goToReader(xtcPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             silentRebootEpubPathMagic == SILENT_REBOOT_EPUB_PATH_MAGIC &&
             silentRebootEpubPath[0] != '\0') {
    // v18.9.9.397: CPS-independent fallback. APP_STATE was empty (likely
    // a corrupt CPS load from a bad SD -- see the strncpy site in
    // silentRestartToReaderWithDefragRetryAtSpine). Use the RTC-preserved
    // path so a chapter-jump restart doesn't strand the user at Home.
    silentRebootEpubPath[sizeof(silentRebootEpubPath) - 1] = '\0';
    const std::string rtcPath(silentRebootEpubPath);
    silentRebootEpubPathMagic = 0;
    silentRebootEpubPath[0] = '\0';
    LOG_INF("BOOT", "Silent-restart EPUB dispatch from RTC (CPS was empty): reopening %s",
            rtcPath.c_str());
    activityManager.goToReader(rtcPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  // CrumBLE: absorb the release edge from the wake-hold so the first
  // activity tick's wasReleased(POWER) doesn't fire. Without this, the
  // user's configured short/long-press action triggers IMMEDIATELY on
  // wake because getHeldTime() still reports the wake-hold duration
  // when the activity's first input read happens.
  //
  // Same pattern as the Silent-reboot path above (line ~1147): two
  // gpio.update() calls separated by > InputManager's DEBOUNCE_DELAY
  // transition the released-bit through lastDebounceTime without
  // setting releasedEvents, so the first loop()'s gpio.update() sees
  // state == currentState and emits no edge.
  gpio.update();
  delay(10);
  gpio.update();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  gBluetoothReaderContext = activityManager.isReaderActivity();
  // CrumBLE 4.5: skip the entire BT singleton + HID processing block when on a
  // lean-boot OTA path. Even just `getInstance()` instantiates the manager
  // (member state + first-call init logging "BluetoothHIDManager instance
  // created") and the auto-reconnect / disable-drain bookkeeping calls below
  // each allocate more. OTA never touches BT and the next normal boot (after
  // OtaUpdateActivity::onExit silentRestart to home) restores BT in the usual
  // loop tick. Skipping reclaims ~5-15 KB that mbedtls cert parsing needs.
  bool bleRecentActivity = false;
  if (!g_leanBootForOta) {
  auto& btMgr = BluetoothHIDManager::getInstance();
  const bool userInputDetectedForBt = gpio.wasAnyPressed() || gpio.wasAnyReleased();
  btMgr.updateActivity();
  // checkAutoReconnect can block for 2-3 s calling connectToDevice() when a
  // reconnect to the bonded remote is in flight. If the user impatiently
  // taps Back during that freeze, the release lands on the next iteration
  // and would kick them out of the book (and BLE auto-disables on book
  // exit). Time the call and swallow one Back release when it actually
  // blocked, so the hasty tap is treated as the no-op the user intended.
  const unsigned long btReconnectStart = millis();
  btMgr.checkAutoReconnect(userInputDetectedForBt);
  if (millis() - btReconnectStart > 500) {
    mappedInputManager.suppressNextBackRelease();
  }
  // Drain deferred disable from EpubReaderActivity::onExit. We can't call
  // disable() inline from onExit because the activity manager still holds
  // the render lock during the transition; doing it here, after loop()
  // returns, is safe.
  //
  // CrumBLE: serialize NimBLE deinit with the render task. The render task
  // drives the shared SPI bus (e-ink + SD cache I/O) under RenderLock. When the
  // reader drops BLE around a cold chapter build it re-renders in a tight loop
  // (cache-miss -> defer) while this main task tears the stack down; the deinit
  // races the render task's SD access and HANGS the indexer (frozen "Indexing",
  // needs a reboot). Holding RenderLock across the deinit keeps the render task
  // idle for the teardown. Only take the lock when a disable is actually pending
  // so we don't contend every loop. (requestUpdateAndWait gracefully rejects if
  // reached while we hold the lock, so this can't deadlock the teardown.)
  if (btMgr.isDisableLaterRequested()) {
    RenderLock lock;
    btMgr.tryDisableIfRequested();
  }
  // Companion drain: when the reader proactively drops BLE around a heavy
  // re-layout (e.g. font change from the Book Settings drawer), it asks
  // for BLE to come back up once the indexer finishes. checkAutoReconnect
  // then resumes the bonded-device link on the user's next button press.
  btMgr.tryEnableIfRequested();

  // CrumBLE: flush a settings save that was deferred because heap was too low to
  // build the JSON safely (e.g. while NimBLE held the heap). Cheap no-op when
  // nothing is pending; this lands the change once a BLE drop / chapter teardown
  // frees the heap back up, so a low-heap setting change isn't silently lost.
  SETTINGS.retryDeferredSaveIfNeeded();
  // v18.9.9.342: mirror the settings retry for collections. Without this,
  // a Show/Hide toggle whose save was deferred (heap too low on Home due
  // to CJK UI-fallback resident) is lost at the next silentRestart --
  // user's toggle appears to work but reverts after any heap-guard reboot.
  CollectionsStore::getInstance().retryDeferredSaveIfNeeded();

  // CrumBLE 4.4 post-bisect: one-shot auto-reconnect on early supervision-
  // timeout drop (HCI reason 520). The post-connect render's e-ink refresh
  // races the BLE event handler; if the link drops within ~3-10s of connect
  // and we haven't auto-retried this cycle, fire one silent re-connect
  // before going to the alert path. Spares the user a manual reconnect for
  // the common race-condition case.
  if (btMgr.takeAutoReconnectRequest() && btMgr.isEnabled()) {
    if (SETTINGS.bleBondedDeviceAddr[0] != '\0') {
      LOG_INF("MAIN", "BT auto-reconnect: re-attempting connect to %s after early drop",
              SETTINGS.bleBondedDeviceAddr);
      btMgr.connectToDevice(SETTINGS.bleBondedDeviceAddr);
    }
  }

  // CrumBLE: a Bluetooth link that dropped on its own seconds after connecting
  // is almost always heap starvation -- the connect spike craters free heap and
  // the controller times the link out (HCI 0x08). Surface a clear message
  // instead of failing silently, so the user understands why the remote
  // "didn't connect". Don't clobber an alert that's already queued.
  if (btMgr.takeConnectionLostAlert() && !APP_STATE.hasPendingAlert.load(std::memory_order_acquire)) {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_BT_CONNECT_FAILED_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_BT_CONNECT_FAILED_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
  }
  bleRecentActivity = btMgr.hasRecentActivity();
  // v18.9.3: sync the parser's BT-aware table guard. Tables buffer ~60 KB
  // parse-time; with NimBLE resident, that budget collides with the reader's
  // section load and books get OOM'd. When BT is on, force paragraph fallback
  // (matches CP/INX which never had structured table rendering). Cheap:
  // one bool write, no alloc.
  setChapterParserSuppressTables(btMgr.isEnabled());
  // v18.9.4/18.9.5.1: device-side BT auto-disconnect. When BT idle exceeds
  // the configured window (1-30 min, default 10), drop BT to reclaim ~58 KB
  // of heap. Doesn't touch the remote's own idle power-off timer (that's
  // controlled by the remote's firmware).
  if (btMgr.isEnabled()) {
    const unsigned long timeoutMs =
        static_cast<unsigned long>(SETTINGS.btAutoDisconnectMinutes) * 60UL * 1000UL;
    const unsigned long idleMs = btMgr.getMillisSinceLastActivity();
    if (idleMs != ULONG_MAX && idleMs >= timeoutMs) {
      LOG_INF("BT", "Auto-disconnect: idle %lu ms >= configured %lu ms; disabling",
              idleMs, timeoutMs);
      btMgr.disable();
    }
  }
  }  // end !g_leanBootForOta BT block

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setTextDarkness(SETTINGS.textDarkness);
  // CrumBLE 4.5.4 Shape 3: per-tick poll so a Settings change to the UI
  // glyph fallback family takes effect without a reboot. The function
  // is a strcmp + early-return when the active fallback already matches,
  // so the steady-state cost is near zero.
  sdFontSystem.ensureFallbackLoaded(renderer);
  // CrumBLE 4.6: Cover Tone Curve disabled before 4.5.0 ship -- per-tick
  // poll commented out so the converter coverTone defaults to 0 (Off /
  // identity) regardless of stale coverToneCurve in user settings.json.
  // Re-enable alongside the SettingsList.h Enum registration.
  // Epub::setCoverToneCurve(SETTINGS.coverToneCurve);

  // 4.5.5: adaptive sampling cadence -- every 2s when maxAlloc is below
  // 30 KB (the "interesting" heap-pressure window), 10s otherwise. The
  // 10s default is fine for steady-state but too coarse to track a leak
  // that runs 24K -> 5K over 130s of post-upload activity (only ~13
  // samples). 2s during pressure gives ~65 samples and is enough to
  // bisect which operation caused each drop.
  const uint32_t curMaxAlloc = ESP.getMaxAllocHeap();
  const uint32_t memLogInterval = (curMaxAlloc < 30u * 1024u) ? 2000u : 10000u;
  if (Serial && millis() - lastMemPrint >= memLogInterval) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep() || bleRecentActivity) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      screenshotComboHandled = true;
      mappedInputManager.suppressNextPowerConfirmRelease();
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

#ifdef SIMULATOR
  if (gpio.consumeSimulatorSleepRequest()) {
    enterDeepSleep();
    lastActivityTime = millis();
    return;
  }
#endif

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    // In the simulator, deep sleep is a no-op and returns — reset the timer so
    // the main loop does not immediately re-trigger auto-sleep.
    lastActivityTime = millis();
    return;
  }

  if (millis() >= allowSleepAt && handleGlobalPowerButtonAction(getPowerButtonAction())) {
    lastActivityTime = millis();
    return;
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  ReadingStats::tick();

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

#ifdef SIMULATOR
  runSimulatorSmokeTestTick();
#endif

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
      (void)activityDuration;
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
