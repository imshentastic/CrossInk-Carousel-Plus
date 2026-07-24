#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <BluetoothHIDManager.h>

#include "../boot_sleep/SleepActivity.h"
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <SdCardFont.h>
#include <Serialization.h>
#include <esp_system.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>

#include "../../ReadingStats.h"
#include "../../SilentRestart.h"  // CrumBLE 4.4: silent-restart-before-BT pre-flight
#include "../settings/BluetoothSettingsActivity.h"
#include "../settings/KOReaderSettingsActivity.h"
#include "BookSettingsDrawerActivity.h"
// CrumBLE: dictionary feature (ported from SEEK reader sumegig/seek-reader).
#include "DictionaryDefinitionActivity.h"
#include "DictionaryIndexBuildActivity.h"
#include "DictionaryWordSelectActivity.h"
#include "fontIds.h"  // BITTER_12_FONT_ID for direct-launched definitions
#include "LookedUpWordsActivity.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"
#include "BookStatsActivity.h"
#include "CollectionsStore.h"
#include "SeriesIndex.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarkListActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "LibraryIndex.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include <ArduinoJson.h>  // for .pxc manifest parse

#include "SdCardFontSystem.h"
#include "SettingsList.h"  // for getSettingsList (drawer cache build)
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "activities/util/ChoicePromptActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

// v18.9.9.2: post-BT diagnostic MEM snapshot at a named reader step. Fires
// only while postBtDiagUntilMs_ is in the future (20s after BT link, armed
// in loop() at the linkedNow transition). Bounded log volume: ~1 render per
// second × ~7 steps × 20 s = ~140 lines per BT-linked session.
#define POST_BT_STEP(name)                                                                                 \
  do {                                                                                                     \
    if (millis() < postBtDiagUntilMs_) {                                                                   \
      LOG_INF("PBTD", "%s: free=%u maxAlloc=%u", name, ESP.getFreeHeap(), ESP.getMaxAllocHeap());          \
    }                                                                                                      \
  } while (0)

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long longPressMenuMs = 600;
constexpr uint16_t DEFAULT_AUTO_PAGE_TURN_INTERVAL_S = 30;
constexpr uint16_t MIN_AUTO_PAGE_TURN_INTERVAL_S = 5;
constexpr uint16_t MAX_AUTO_PAGE_TURN_INTERVAL_S = 120;
constexpr int MAX_PAGE_LOAD_RETRIES = 3;

// CrumBLE: .pxc manifest comparison body helper (duplicated from
// BookSettingsDrawerActivity.cpp because the alternative -- shared header --
// would force settings/I18n includes into PxcManifest.h, polluting every
// translation unit that touches the manifest struct). Keep the two copies
// behaviourally identical if you change one.
std::string enumLabelOf(const SettingInfo& info, uint8_t value) {
  // CrumBLE 4.2: honor enumRawValues when set. Settings that use
  // .withEnumRawValues({...}) (e.g. fontFamily on slim builds where the
  // displayed enum is a subset of the raw enum) need a raw-value -> display-
  // index lookup, otherwise the raw value silently indexes off the end of
  // the (now-shorter) enumValues vector and the label comes back blank.
  if (!info.enumRawValues.empty()) {
    for (size_t i = 0; i < info.enumRawValues.size() && i < info.enumValues.size(); i++) {
      if (info.enumRawValues[i] == value) {
        return std::string(I18N.get(info.enumValues[i]));
      }
    }
    return std::string{};
  }
  if (value < info.enumValues.size()) {
    return std::string(I18N.get(info.enumValues[value]));
  }
  return std::string{};
}
const SettingInfo* findSetting(const std::vector<SettingInfo>& settings, StrId nameId) {
  for (const auto& s : settings) {
    if (s.nameId == nameId) return &s;
  }
  return nullptr;
}
std::string fontLabel(const std::vector<SettingInfo>& settings, uint8_t fontFamily, uint8_t fontSize,
                      uint8_t sdSizeRange, const std::string& sdName) {
  if (!sdName.empty()) {
    static const char* range[] = {"S", "M", "L"};
    const char* r = sdSizeRange < 3 ? range[sdSizeRange] : "?";
    return sdName + " (" + r + ")";
  }
  std::string name = "Font " + std::to_string(static_cast<unsigned>(fontFamily));
  if (const auto* ff = findSetting(settings, StrId::STR_FONT_FAMILY)) {
    const auto label = enumLabelOf(*ff, fontFamily);
    if (!label.empty()) name = label;
  }
  std::string sizeStr;
  if (const auto* fs = findSetting(settings, StrId::STR_FONT_SIZE)) {
    sizeStr = enumLabelOf(*fs, fontSize);
  }
  if (sizeStr.empty()) sizeStr = std::to_string(static_cast<unsigned>(fontSize));
  return name + " (" + sizeStr + ")";
}
std::string buildManifestComparisonBody(const PxcManifest& m, const std::vector<SettingInfo>& settings,
                                         const std::string& leadIn) {
  const auto* oriInfo = findSetting(settings, StrId::STR_ORIENTATION);
  const auto* imgInfo = findSetting(settings, StrId::STR_IMAGES);
  std::string out = leadIn;
  if (!out.empty()) out += "\n\n";
  out += "Prepared:\n";
  out += "Font: " + fontLabel(settings, m.fontFamily, m.fontSize, m.sdFontSizeRange, m.sdFontFamilyName) + "\n";
  out += "Margin: " + std::to_string(static_cast<unsigned>(m.screenMargin)) + "\n";
  out += "Orientation: " + (oriInfo ? enumLabelOf(*oriInfo, m.orientation) : std::to_string(m.orientation)) + "\n";
  out += "Images: " + (imgInfo ? enumLabelOf(*imgInfo, m.imageRendering) : std::to_string(m.imageRendering)) + "\n";
  out += "\nYours:\n";
  out += "Font: " +
         fontLabel(settings, SETTINGS.fontFamily, SETTINGS.fontSize, SETTINGS.sdFontSizeRange,
                   SETTINGS.sdFontFamilyName) +
         "\n";
  out += "Margin: " + std::to_string(static_cast<unsigned>(SETTINGS.screenMargin)) + "\n";
  out += "Orientation: " +
         (oriInfo ? enumLabelOf(*oriInfo, SETTINGS.orientation) : std::to_string(SETTINGS.orientation)) + "\n";
  out += "Images: " +
         (imgInfo ? enumLabelOf(*imgInfo, SETTINGS.imageRendering) : std::to_string(SETTINGS.imageRendering));
  return out;
}

void drawToastBuffer(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  drawToastBuffer(renderer, msg);
  renderer.displayBuffer();
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

uint16_t clampAutoPageTurnIntervalSeconds(const uint16_t seconds) {
  return std::clamp(seconds, MIN_AUTO_PAGE_TURN_INTERVAL_S, MAX_AUTO_PAGE_TURN_INTERVAL_S);
}

// SD card folder finished books are moved into. Single source of truth for the path.
constexpr char READ_FOLDER[] = "/Read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// v18.9.9.70 (ported from crosspoint 05c1e9aa, adapted for our BluetoothHIDManager):
// RAII helper that lends the ~40 KB framebuffer allocation to the section cold-
// build for the duration of the loan. release() drops the framebuffer, restore()
// re-allocates it (white contents). Requires our freeink-sdk backport of upstream
// 059c1d1 which heap-allocates the framebuffer on non-PSRAM boards too. If
// restore fails, we try dropping BT before giving up, and hard-restart on total
// failure -- render state cannot proceed with a null framebuffer.
class FrameBufferBuildLoan {
 public:
  explicit FrameBufferBuildLoan(GfxRenderer& renderer) : renderer_(renderer) {}
  ~FrameBufferBuildLoan() {
    if (active_ && !restore()) {
      ESP.restart();
    }
  }

  void release() {
    if (active_ || !renderer_.hasFrameBuffer()) return;
    renderer_.releaseFrameBufferForBuild();
    active_ = true;
    LOG_DBG("ERS", "Framebuffer lent for section build (bt=%u heap=%u maxAlloc=%u)",
            BluetoothHIDManager::getInstance().isEnabled() ? 1 : 0,
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }

  bool restore() {
    if (!active_) return true;
    active_ = false;
    if (renderer_.restoreFrameBufferAfterBuild()) {
      LOG_DBG("ERS", "Framebuffer restored after section build");
      return true;
    }
    if (BluetoothHIDManager::getInstance().isEnabled()) {
      LOG_INF("ERS", "Framebuffer restore needs heap; freeing BT and retrying (heap=%u maxAlloc=%u)",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      BluetoothHIDManager::getInstance().disable();
      if (renderer_.restoreFrameBufferAfterBuild()) {
        LOG_DBG("ERS", "Framebuffer restored after freeing BT");
        return true;
      }
    }
    LOG_ERR("ERS", "Framebuffer restore failed after section build");
    return false;
  }

 private:
  GfxRenderer& renderer_;
  bool active_ = false;
};

// v18.9.6.1 / v18.9.9.58: Simple Rendering per-book sidecar, now split per
// render path. Compat is a "this path OOM'd once" hint scoped to whichever
// render path was active when the escalation fired:
//   - compat_prepared.flag: book was on the prepared-layout path
//   - compat_custom.flag:   book was on the user's own settings
// A book whose prepared path renders fine can still legitimately need compat
// under a settings drift the user hasn't cleaned up (or vice versa). Pre-v58
// legacy simple_rendering.flag is deleted on first sight (see
// clearLegacySimpleRenderingSidecar); a real OOM on either path re-writes
// within a page.
constexpr const char* kSimpleRenderingFlagLegacy = "/simple_rendering.flag";
constexpr const char* kSimpleRenderingFlagPrepared = "/compat_prepared.flag";
constexpr const char* kSimpleRenderingFlagCustom = "/compat_custom.flag";

const char* simpleRenderingSidecarBasename(ReaderPath path) {
  return path == ReaderPath::PreparedLayout
             ? kSimpleRenderingFlagPrepared
             : kSimpleRenderingFlagCustom;
}

bool simpleRenderingSidecarSet(const std::string& cachePath,
                               ReaderPath path) {
  const std::string full = cachePath + simpleRenderingSidecarBasename(path);
  return Storage.exists(full.c_str());
}

void writeSimpleRenderingSidecar(const std::string& cachePath,
                                 ReaderPath path) {
  const std::string full = cachePath + simpleRenderingSidecarBasename(path);
  if (Storage.exists(full.c_str())) return;
  HalFile f;
  if (Storage.openFileForWrite("ERS", full.c_str(), f)) {
    f.close();
    LOG_INF("ERS", "Simple Rendering sidecar written: %s", full.c_str());
  } else {
    LOG_ERR("ERS", "Failed to write Simple Rendering sidecar: %s", full.c_str());
  }
}

void clearSimpleRenderingSidecar(const std::string& cachePath,
                                 ReaderPath path) {
  const std::string full = cachePath + simpleRenderingSidecarBasename(path);
  if (Storage.exists(full.c_str())) {
    Storage.remove(full.c_str());
    LOG_INF("ERS", "Simple Rendering sidecar cleared: %s", full.c_str());
  }
}

// One-shot cleanup of the pre-v58 path-agnostic sidecar. Called from
// EpubReaderActivity::onEnter -- delete-if-present so books that had the
// legacy flag set (from pre-v58 firmware) don't stay stuck in compat forever
// once the reader switches to the split model. A real OOM will re-write the
// correct-path sidecar within a page.
void clearLegacySimpleRenderingSidecar(const std::string& cachePath) {
  const std::string full = cachePath + kSimpleRenderingFlagLegacy;
  if (Storage.exists(full.c_str())) {
    Storage.remove(full.c_str());
    LOG_INF("ERS", "Legacy simple_rendering.flag swept (v58 split): %s", full.c_str());
  }
}

// v18.9.9.6 Level 2: per-book sidecar for the "tables suppressed, images
// preserved" fallback. Written when Level 1 defrag didn't fit but full
// Simple Rendering feels heavy-handed for a book whose only problem is
// oversized table fragments. Images and embedded style stay on; only
// PageTableFragments are collapsed to paragraphs at parse time.
constexpr const char* kTablesSuppressedFlagFilename = "/tables_suppressed.flag";

bool tablesSuppressedSidecarSet(const std::string& cachePath) {
  const std::string path = cachePath + kTablesSuppressedFlagFilename;
  return Storage.exists(path.c_str());
}

void writeTablesSuppressedSidecar(const std::string& cachePath) {
  const std::string path = cachePath + kTablesSuppressedFlagFilename;
  if (Storage.exists(path.c_str())) return;
  HalFile f;
  if (Storage.openFileForWrite("ERS", path.c_str(), f)) {
    f.close();
    LOG_INF("ERS", "Tables-suppressed sidecar written: %s", path.c_str());
  } else {
    LOG_ERR("ERS", "Failed to write tables-suppressed sidecar: %s", path.c_str());
  }
}

void clearTablesSuppressedSidecar(const std::string& cachePath) {
  const std::string path = cachePath + kTablesSuppressedFlagFilename;
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    LOG_INF("ERS", "Tables-suppressed sidecar cleared: %s", path.c_str());
  }
}

// v18.9.9.35 (task #17) + v18.9.9.52 (task #37): per-book "user declined
// the prebake prompt at these settings" state, moved from an SD sidecar
// to RTC_NOINIT memory. The SD version persisted the decline across
// cold boots, which hid the fact that every chapter was cold-building
// and pushed image-heavy books past the "images suppressed under BT"
// gate silently. Corrected behavior: the decline is scoped to a single
// power-on session -- it survives silent-restart chains (which the
// original v35 justification was really about) but resets on cold boot
// so the next fresh open re-fires the prompt whenever current settings
// still don't match the prebake.
//
// The old kPrebakeDeclinedFilename SD files are cleaned up on first
// boot of the new firmware via clearLegacyPrebakeDeclinedSidecar().
constexpr const char* kLegacyPrebakeDeclinedFilename = "/prebake-declined.dat";
constexpr uint32_t kPrebakeDeclinedRtcMagic = 0x50444352;  // 'PDCR'

// RTC_NOINIT is uninitialized on cold boot and preserved across
// silent-restart / soft reset. We stamp a magic + book identity + a
// settings fingerprint. If any field on read doesn't match, treat as
// "no decline recorded" -- which for cold boot is exactly what we want.
RTC_NOINIT_ATTR uint32_t g_prebakeDeclinedMagic;
RTC_NOINIT_ATTR uint64_t g_prebakeDeclinedBookHash;
RTC_NOINIT_ATTR uint32_t g_prebakeDeclinedSettingsHash;

uint32_t hashPrebakeDeclineFingerprint() {
  // FNV-1a over the same fields the SD sidecar used to compare. Order
  // must be stable across firmware versions or the fingerprint check
  // becomes flakey after upgrades. Any field added here needs an
  // explicit migration story.
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 16777619u;
    }
  };
  mix(SETTINGS.orientation);
  mix(SETTINGS.screenMargin);
  mix(SETTINGS.imageRendering);
  mix(SETTINGS.fontFamily);
  mix(SETTINGS.fontSize);
  mix(SETTINGS.sdFontSizeRange);
  for (size_t i = 0; i < sizeof(SETTINGS.sdFontFamilyName); ++i) {
    mix(static_cast<uint8_t>(SETTINGS.sdFontFamilyName[i]));
  }
  mix(SETTINGS.lineSpacing);
  mix(SETTINGS.paragraphAlignment);
  mix(SETTINGS.extraParagraphSpacing);
  mix(SETTINGS.forceParagraphIndents);
  mix(SETTINGS.hyphenationEnabled);
  mix(SETTINGS.embeddedStyle);
  mix(SETTINGS.bionicReadingEnabled);
  mix(SETTINGS.guideReadingEnabled);
  return h;
}

uint64_t bookHashForCachePath(const std::string& cachePath) {
  // FNV-1a 64. The cache path itself is derived from the book's file
  // path hash (see Epub::cachePathForFilePath -- fnvHash64) so this is
  // effectively a stable per-book identifier for the session.
  uint64_t h = 14695981039346656037ULL;
  for (char c : cachePath) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

void writePrebakeDeclinedSidecar(const std::string& cachePath) {
  g_prebakeDeclinedMagic = kPrebakeDeclinedRtcMagic;
  g_prebakeDeclinedBookHash = bookHashForCachePath(cachePath);
  g_prebakeDeclinedSettingsHash = hashPrebakeDeclineFingerprint();
  LOG_INF("ERA", "Prebake decline recorded in RTC (bookHash=%08lx%08lx, settingsHash=%08x)",
          static_cast<unsigned long>(g_prebakeDeclinedBookHash >> 32),
          static_cast<unsigned long>(g_prebakeDeclinedBookHash & 0xFFFFFFFF), g_prebakeDeclinedSettingsHash);
}

void clearPrebakeDeclinedSidecar(const std::string& /*cachePath*/) {
  g_prebakeDeclinedMagic = 0;
  g_prebakeDeclinedBookHash = 0;
  g_prebakeDeclinedSettingsHash = 0;
  LOG_INF("ERA", "Prebake decline cleared from RTC");
}

// True only when the RTC slots (a) carry the magic sentinel, (b) match
// the current book's hash, and (c) match the current settings
// fingerprint. Any drift -> caller treats as "no decline" -> prompt
// fires.
bool prebakeDeclinedSidecarMatchesCurrent(const std::string& cachePath) {
  if (g_prebakeDeclinedMagic != kPrebakeDeclinedRtcMagic) return false;
  const uint64_t currentBookHash = bookHashForCachePath(cachePath);
  if (g_prebakeDeclinedBookHash != currentBookHash) return false;
  const uint32_t currentSettingsHash = hashPrebakeDeclineFingerprint();
  if (g_prebakeDeclinedSettingsHash != currentSettingsHash) {
    LOG_INF("ERA", "Prebake decline settings-hash drift (%08x -> %08x); clearing so prompt re-fires",
            g_prebakeDeclinedSettingsHash, currentSettingsHash);
    g_prebakeDeclinedMagic = 0;
    g_prebakeDeclinedBookHash = 0;
    g_prebakeDeclinedSettingsHash = 0;
    return false;
  }
  return true;
}

// v18.9.9.187: per-book "user already answered the pxc-manifest / prepared-
// layout prompt in this session" state, RTC-persisted so silent restarts
// (defrag-with-EnableBt, low-heap section rebuilds) don't re-fire the prompt
// on every boot's reader instance. Mirrors the prebake-declined RTC pattern
// -- book hash + magic sentinel, no settings hash (any answer, keep or use-
// prepared, means "don't ask again this session"). Cleared on user-exit so
// re-opening the book gets a fresh chance to accept prepared.
constexpr uint32_t kBtManifestAnsweredRtcMagic = 0x424D4152;  // 'BMAR'
RTC_NOINIT_ATTR uint32_t g_btManifestAnsweredMagic;
RTC_NOINIT_ATTR uint64_t g_btManifestAnsweredBookHash;

void writeBtManifestAnsweredSidecar(const std::string& cachePath) {
  g_btManifestAnsweredMagic = kBtManifestAnsweredRtcMagic;
  g_btManifestAnsweredBookHash = bookHashForCachePath(cachePath);
  LOG_INF("ERA", "BT manifest prompt answered recorded in RTC (bookHash=%08lx%08lx)",
          static_cast<unsigned long>(g_btManifestAnsweredBookHash >> 32),
          static_cast<unsigned long>(g_btManifestAnsweredBookHash & 0xFFFFFFFF));
}

void clearBtManifestAnsweredSidecar() {
  g_btManifestAnsweredMagic = 0;
  g_btManifestAnsweredBookHash = 0;
  LOG_INF("ERA", "BT manifest answered cleared from RTC");
}

bool btManifestAnsweredSidecarMatchesCurrent(const std::string& cachePath) {
  if (g_btManifestAnsweredMagic != kBtManifestAnsweredRtcMagic) return false;
  return g_btManifestAnsweredBookHash == bookHashForCachePath(cachePath);
}

// One-shot cleanup of the pre-v52 SD sidecar. Called on the first
// reader open per boot; a simple existence-based delete so we don't
// keep old "declined forever" state around after users upgrade.
void clearLegacyPrebakeDeclinedSidecar(const std::string& cachePath) {
  const std::string path = cachePath + kLegacyPrebakeDeclinedFilename;
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    LOG_INF("ERA", "Removed legacy SD prebake-declined sidecar: %s", path.c_str());
  }
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /Read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath, const std::string& title) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_MOVE_TO_READ_FAILED_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
             title.c_str());
    APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = Epub::cachePathForFilePath(dstPath, "/.crosspoint");
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

float EpubReaderActivity::getCurrentBookProgressPercent() const {
  if (!epub || !section || section->pageCount == 0 || epub->getBookSize() == 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
}

void EpubReaderActivity::initializeCompletionPromptTrigger() {
  completionTriggerSpineIndex = -1;
  completionTriggerSpineProgress = 1.0f;
  completionPromptQueued = false;
  completionPromptShown = stats.isCompleted;
  completionTriggerSeenBelow = false;
  lastAtOrPastCompletionTrigger = false;

  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  const int spineCount = epub->getSpineItemsCount();
  if (bookSize == 0 || spineCount <= 0) {
    return;
  }

  size_t targetSize = (bookSize / 100) * 99 + (bookSize % 100) * 99 / 100;
  if (targetSize >= bookSize) {
    targetSize = bookSize - 1;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;

  completionTriggerSpineIndex = targetSpineIndex;
  completionTriggerSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);

  if (completionTriggerSpineProgress < 0.0f) {
    completionTriggerSpineProgress = 0.0f;
  } else if (completionTriggerSpineProgress > 1.0f) {
    completionTriggerSpineProgress = 1.0f;
  }
}

bool EpubReaderActivity::isAtOrPastCompletionTrigger() const {
  if (!epub || !section || section->pageCount == 0 || completionTriggerSpineIndex < 0) {
    return false;
  }

  if (currentSpineIndex > completionTriggerSpineIndex) {
    return true;
  }
  if (currentSpineIndex < completionTriggerSpineIndex) {
    return false;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  return chapterProgress >= completionTriggerSpineProgress;
}

void EpubReaderActivity::queueCompletionPromptIfNeeded() {
  if (completionPromptShown || completionPromptQueued || stats.isCompleted || footnoteDepth > 0) {
    return;
  }

  const bool atOrPastTrigger = isAtOrPastCompletionTrigger();

  if (!atOrPastTrigger) {
    completionTriggerSeenBelow = true;
  }

  if (completionTriggerSeenBelow && !lastAtOrPastCompletionTrigger && atOrPastTrigger) {
    completionPromptQueued = true;
  }

  lastAtOrPastCompletionTrigger = atOrPastTrigger;
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  pageLoadRetryCount = 0;

  if (!epub) {
    return;
  }

  // v18.9.9.209: arm terminate-recovery to the READER. Field crash: a
  // bad_alloc terminate during book open (deferred settings save racing
  // the section build) dumped the user to Home (recoveryTarget=0). The
  // book path is already RTC-stashed while reading, so target=1 reopens
  // the book on the recovery boot. One-shot by construction — the
  // terminate handler zeroes the magic, so a repeat crash in the same
  // book falls back to Home instead of looping. Cleared in onExit.
  armSilentRestartTarget(/*SILENT_REBOOT_TARGET_READER=*/1);

  // CrumBLE: free the in-RAM library index for the duration of the reading
  // session. Recently Added / All Books keep it loaded -- tens of KB of scattered
  // string allocations for a large library -- and holding it through reading
  // erodes the contiguous heap that BLE glyph rendering needs. On the full feature
  // set that pushed the largest free block below the font glyph group, so text
  // starved and the remote dropped ("Bluetooth couldn't stay connected"). It
  // auto-rebuilds from the on-disk JSON on the next Recently Added / All Books
  // visit, so the only cost is a one-time rewalk back at Home.
  LibraryIndex::getInstance().releaseMemory();

  // CrumBLE 4.2: also free CollectionsStore and SeriesIndex string pools
  // for the reader session. Same reasoning as the FT entry release
  // (55dc28c2): the per-book metadata strings sit scattered across the
  // heap and prevent the CSS parser's rule-table grow from finding the
  // ~60 KB contiguous block a real-book stylesheet ends up needing. On
  // the X3 (which already has a tighter contiguous-heap budget thanks to
  // its larger 528x792 frame buffer + cover-pool blocks) this was the
  // difference between CSS parsing completing and bailing at ~30 KB
  // maxAlloc with "Skipping remaining CSS rules". Both stores rebuild
  // from JSON on the next Home visit.
  CollectionsStore::getInstance().releaseMemory();
  SeriesIndex::getInstance().releaseMemory();

  // CrumBLE 4.5.116: reader never needs the standalone UI glyph fallback
  // family (~10 KB resident tax fragmentation-scattered through the
  // reading session). Two paths:
  //   (a) primary SD font is loaded -- alias it as the UI fallback.
  //       Zero extra heap; any CJK glyph miss in reader UI (title,
  //       chapter label, progress bar, drawer, menu) resolves through
  //       the primary. Renders at primary's point size (typically
  //       14-20pt) which will look visibly larger than the surrounding
  //       10-12pt UI text but beats tofu boxes.
  //   (b) no primary loaded (user hasn't picked an SD font, or
  //       ReaderActivity's ensureLoaded hasn't run yet on this open
  //       path) -- fall through to suppress + release, matching the
  //       4.5.115 tofu-in-reader behavior.
  // Un-suppressed in onExit so per-tick poll restores the user's real
  // fallback on the return to home/settings.
  if (!sdFontSystem.aliasPrimaryAsFallback(renderer)) {
    sdFontSystem.setFallbackSuppressed(true);
    sdFontSystem.releaseFallback(renderer);
  }

  // v18.9.9.194: drop the in-RAM cover-bitmap cache on reader entry.
  // Reader has no use for cover thumbs, and v192's pinned-covers change
  // (Home/RBGA covers held resident to keep shelf scroll snappy) means
  // ~24 KB of pinned cover bytes now survive the Home-to-Reader transition
  // and eat into the contiguous heap wrapDefinition + section CSS need.
  // Field repro: post-RBGA reader entry -> dict lookup -> bad_alloc
  // terminate at maxAlloc=6388. Unpin + clear here restores the invariant
  // that the reader owns the full heap. Home rebuilds the cache lazily
  // on the next lookupCachedBitmapPinned.
  renderer.unpinAllCachedBitmaps();
  renderer.clearImageCache();

  // BT No Images Quick Connect is session-scoped: always start a freshly opened
  // (or reopened-after-reboot) book with images enabled. If the user picked the
  // no-images connect last session, rebooting and re-entering the book brings the
  // images back -- the flag is only re-armed when they explicitly choose that
  // drawer action again.
  renderer.setSuppressImages(false);
  btNoImgLinkSeen = false;
  pendingGraceReRender = false;

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // CrumBLE fast-open: clear deferred state. The font-buffer pre-grow
  // is no longer eagerly done here at all -- it's a BLE-only safety
  // net, so we run it inline at each BT-enable call site instead
  // (drawer Quick Connect, reader main menu BT toggle, manifest-prompt
  // accept, post-layout re-enable). Non-BT users never pay the cost;
  // BT users pay it inside the "Connecting Bluetooth..." popup window
  // where it's invisible.
  readerSettingsCache_.clear();
  pxcManifest_.reset();
  bookVisibleCharCount_ = 0;
  prebakeManifest_.reset();
  prebakeLastSnapshot_ = {};
  prebakePromptShowing_ = false;
  prebakePromptDiagLogged_ = false;
  deferredOnEnterPending_ = true;
  firstRenderCompleted_ = false;

  // CrumBLE 4.2 Option 2: switch SD-font prewarm to REGULAR-only iff this
  // user has actually paired a BT controller. They're trading per-glyph
  // SD reads for bold/italic for enough free heap to survive a BT enable
  // + post-connect page reload. Users with no bonded controller never pay
  // that cost and stay on the original eager-all-styles prewarm. Re-check
  // on every book open so a mid-session pair / unpair takes effect on the
  // next book.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->setSdFontLazyNonRegular(SETTINGS.bleBondedDeviceAddr[0] != '\0');
    LOG_DBG("ERA", "SD-font lazy non-REGULAR prewarm: %d (bonded=%s)", fcm->sdFontLazyNonRegular() ? 1 : 0,
            SETTINGS.bleBondedDeviceAddr[0] != '\0' ? SETTINGS.bleBondedDeviceAddr : "(none)");
  }
  // Eager prebake-manifest load: the switch-back prompt has to fire
  // BEFORE the first render's section.loadSectionFile path falls through
  // to createSectionFile (the "indexing" screen), otherwise the device
  // starts rebuilding the chapter even though the user is about to be
  // asked whether they want the prepared layout restored. The original
  // call lived in runDeferredOnEnter, which runs AFTER the first render,
  // so the prompt fired too late and the index work was already under
  // way. Manifest read is one ~250 byte JSON file -- safe to move into
  // the synchronous onEnter path.
  if (SETTINGS.optimizeChapterIndexing && epub) {
    PrebakeManifest pm;
    if (tryLoadPrebakeManifest(epub->getCachePath(), pm)) {
      prebakeManifest_ = pm;
      LOG_INF("ERA", "Eager-loaded prebake manifest: fontId=%ld viewport=%ux%u",
              static_cast<long>(pm.fontId), static_cast<unsigned>(pm.viewportWidth),
              static_cast<unsigned>(pm.viewportHeight));
    }
  }
  // Reset BLE-link edge state on every book open: a fresh book may have a
  // different manifest (or none), and any prior link tracking is stale.
  btWasLinked_ = false;
  btManifestPromptEarliestMs_ = 0UL;
  // v18.9.9.187: rehydrate the "answered this session" flag from RTC. A
  // silent-restart chain (defrag+EnableBt, low-heap section rebuild) tears
  // down this reader and rebuilds it, defaulting the flag to false. Before
  // v187 that caused the pxc-manifest prompt to re-fire on every boot in
  // the chain, asking the user "use prepared layout?" 2-3 times per QC.
  // RTC-persisted flag survives the restart; cleared in onExit so a fresh
  // open reasks.
  btManifestPromptAnsweredThisSession_ =
      (epub && btManifestAnsweredSidecarMatchesCurrent(epub->getCachePath()));
  if (btManifestPromptAnsweredThisSession_) {
    LOG_INF("ERA", "BT manifest prompt: honoring RTC-recorded prior answer, suppressing this session");
  }
  pendingBleQuickConnect_ = false;
  pendingBleQuickConnectNoImages_ = false;
  pendingBleQuickConnectSettingsChanged_ = false;
  pendingBleQuickConnectPromptStage_ = -1;
  bleConnectingPopupPainted_ = false;

  // v18.9.6: reset the per-session latches (BLE-drop retry, simple-retry
  // one-shots). These bound how many times a session escalates -- fresh
  // book, fresh attempts.
  layoutBleRetryAttempted = false;
  layoutSimpleRetryAttempted = false;
  // v18.9.9.36 Phase C2: fresh book -> no incremental build in flight.
  sectionBuildInProgress_ = false;
  sectionBuildSpine_ = -1;
  sectionBuildJustFailed_ = false;
  sectionBuildLayoutAbortedForLowMemory_ = false;
  sectionBuildImagesWereSuppressed_ = false;
  sectionBuildBleWasDroppedForFail_ = false;
  sectionBuildPopupLastMs_ = 0;
  sectionBuildPopupDotPhase_ = 0;
  // v18.9.9.58: sweep the pre-split legacy sidecar so any books stuck on
  // it from earlier firmware get a fresh chance. If a real OOM is still
  // reachable, the escalation cascade rewrites the correct-path flag
  // within a page.
  clearLegacySimpleRenderingSidecar(epub->getCachePath());

  // v18.9.9.58: determine which render path this book open is on. Priority:
  //   1. If continuing from silent-restart AND we stashed a path pre-restart,
  //      honor that (user's prompt answer / manual toggle carries across).
  //   2. Else if prebake manifest exists AND SETTINGS fully match its
  //      fingerprint, assume PreparedLayout. The mismatch prompt would just
  //      no-op here anyway.
  //   3. Else default to CustomSettings. If prebake exists but mismatches,
  //      the existing prebake prompt (checkAndShowPrebakePromptIfNeeded)
  //      still runs and can flip the path.
  {
    uint8_t stashed = 0;
    if (isContinuingFromSilentReboot() && consumePendingReaderActivePath(stashed) &&
        stashed <= static_cast<uint8_t>(ReaderPath::CustomSettings)) {
      readerActivePath_ = static_cast<ReaderPath>(stashed);
    } else if (prebakeManifest_.has_value()) {
      const auto& pm = *prebakeManifest_;
      const int32_t curFontId = SETTINGS.getReaderFontId();
      const uint16_t curW = renderer.getScreenWidth();
      const uint16_t curH = renderer.getScreenHeight();
      constexpr uint16_t kVpTolPx = 8;
      const uint16_t vpWDelta = pm.viewportWidth > curW ? pm.viewportWidth - curW : curW - pm.viewportWidth;
      const uint16_t vpHDelta = pm.viewportHeight > curH ? pm.viewportHeight - curH : curH - pm.viewportHeight;
      const bool matches =
          (pm.fontId == curFontId) &&
          (pm.lineCompression == SETTINGS.getReaderLineCompression()) &&
          (pm.extraParagraphSpacing == SETTINGS.extraParagraphSpacing) &&
          (pm.forceParagraphIndents == SETTINGS.forceParagraphIndents) &&
          (pm.paragraphAlignment == SETTINGS.paragraphAlignment) &&
          (vpWDelta <= kVpTolPx) && (vpHDelta <= kVpTolPx) &&
          (pm.hyphenationEnabled == SETTINGS.hyphenationEnabled) &&
          (pm.embeddedStyle == SETTINGS.embeddedStyle) &&
          (pm.imageRendering == SETTINGS.imageRendering) &&
          (pm.bionicReadingEnabled == SETTINGS.bionicReadingEnabled) &&
          (pm.guideReadingEnabled == SETTINGS.guideReadingEnabled);
      readerActivePath_ = matches ? ReaderPath::PreparedLayout : ReaderPath::CustomSettings;
    } else {
      readerActivePath_ = ReaderPath::CustomSettings;
    }
    APP_STATE.readerActivePath = static_cast<uint8_t>(readerActivePath_);
    stashReaderActivePathForNextBoot(APP_STATE.readerActivePath);
    LOG_INF("ERS", "Book open: readerActivePath=%s",
            readerActivePath_ == ReaderPath::PreparedLayout ? "PreparedLayout" : "CustomSettings");
  }

  // v18.9.9.59: consume the "compat auto-re-enabled" toast flag armed by
  // the prior Layer 2 write-sidecar site. When set, we briefly show a
  // popup explaining why compat is back on before dismissing to the
  // normal render.
  if (isContinuingFromSilentReboot() && consumePendingCompatReenabledToast()) {
    LOG_INF("ERS", "Compat re-enabled toast: user had just manually disabled compat; showing popup");
    GUI.drawPopup(renderer, "Compatibility Mode required", 0, false, HalDisplay::FAST_REFRESH);
    delay(1600);
  }
  // v18.9.9.438: consume the chapter-heap-refuse toast armed by the v437
  // escalation gate when a chapter jump refused on prebake+heap-tight.
  // Explains to the user why they're not at the chapter they clicked.
  if (isContinuingFromSilentReboot()) {
    const int refusedSpine = consumePendingChapterHeapRefuseToast();
    if (refusedSpine >= 0) {
      char msg[80];
      snprintf(msg, sizeof(msg),
               "Chapter %d needs more memory\nStayed on chapter %d",
               refusedSpine + 1, currentSpineIndex + 1);
      LOG_INF("ERS", "Chapter-heap-refuse toast: requested=%d, staying=%d",
              refusedSpine, currentSpineIndex);
      GUI.drawPopup(renderer, msg, 0, false, HalDisplay::FAST_REFRESH);
      delay(1800);
    }
  }
  // v18.9.9.10: sidecar is a "needs compat mode WHEN BT is on" hint, not
  // an always-on override. When BT is off (and no silent-restart is about
  // to enable it), respect the user's prebake experience and render full
  // even if the sidecar is set. When BT is on or being enabled via a
  // silent-restart continuation, honor the compat marker.
  // v18.9.9.58: sidecar is now path-scoped (compat_prepared / compat_custom).
  const bool sidecarSet = simpleRenderingSidecarSet(epub->getCachePath(), readerActivePath_);
  // v18.9.9.28: sidecar alone determines compat -- see the matching change
  // at the loadSectionFile preamble in loop(). The old "sidecar && BT" gate
  // meant a user toggling Compat On via ReaderOptions saw no effect until
  // BT was engaged, which didn't match "on / off" semantics.
  simpleRenderingActive_ = sidecarSet;
  if (simpleRenderingActive_) {
    LOG_INF("ERS", "Book opened with Simple Rendering active (sidecar set); compat mode ON");
  }
  // v18.9.9.23: broadcast compat state to Settings so the four compat-
  // overridden settings show a "· Compat" suffix and refuse to toggle.
  // Cleared on onExit.
  APP_STATE.readerCompatModeActive = simpleRenderingActive_;
  // v18.9.9.10: deprecated tables_suppressed.flag (v18.9.9.6-9). Sweep it
  // if present so it doesn't linger. Reader no longer reads or writes.
  clearTablesSuppressedSidecar(epub->getCachePath());
  // v18.9.9.52 (task #37): one-shot cleanup of the pre-v52 SD prebake-
  // declined sidecar. The state lives in RTC_NOINIT now (session-scoped);
  // any lingering SD file would silently keep suppressing the prompt on
  // cold boots. Cheap no-op after the first successful sweep per book.
  clearLegacyPrebakeDeclinedSidecar(epub->getCachePath());
  tableSuppressionActive_ = false;
  // v18.9.9.5: consume the Layer 1 defrag continuation signal. If this
  // boot is the result of a defrag silent-restart triggered by the same
  // book's last render attempt, we've already spent our defrag budget
  // for this book open -- the next failure jumps straight to Layer 2
  // (compat mode), no more defrag hops.
  if (isDefragRetryContinuation()) {
    layoutDefragRetryAttempted_ = true;
    // v18.9.9.168: seed the per-spine tracker so a cache-miss on the SAME
    // spine we just defragged for correctly falls through to Layer 2,
    // while a miss on a NEW spine is treated as a fresh defrag candidate.
    layoutDefragRetryChapterSpine_ = currentSpineIndex;
    LOG_INF("ERS", "Book opened continuing from Layer 1 defrag retry -- next failure -> compat mode");
    clearDefragRetryContinuation();
  }

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  epub->setupCacheDir();
  BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");

  // CrumBLE 4.2.1: book is already loaded with metadata at this point; cache
  // its author key into LibraryIndex so AuthorAlpha sort doesn't need to
  // load the EPUB again. Idempotent: a no-op when the key was already
  // identical (which is the common case for books that booted with v2
  // index). Persists to disk so the cache survives reboots.
  LibraryIndex::getInstance().setAuthorFromRaw(epub->getPath(), epub->getAuthor());

  if (APP_STATE.pendingBookmarkSpine != UINT16_MAX && APP_STATE.pendingBookmarkProgress >= 0.0f) {
    // Resume from a bookmark selected on the Home screen
    currentSpineIndex = APP_STATE.pendingBookmarkSpine;
    pendingSpineProgress = APP_STATE.pendingBookmarkProgress;
    pendingPercentJump = true;
    cachedSpineIndex = currentSpineIndex;

    // Clear the pending jump
    APP_STATE.pendingBookmarkSpine = UINT16_MAX;
    APP_STATE.pendingBookmarkProgress = -1.0f;
    APP_STATE.saveToFile();
  } else {
    FsFile f;
    // CrumBLE 4.4: quiet existence check first to silence the noisy
    // "[ERS] File does not exist" line when opening a never-read book.
    // Missing progress.bin is the normal "fresh book" state, not an error.
    const std::string progressPath = epub->getCachePath() + "/progress.bin";
    if (Storage.exists(progressPath.c_str()) && Storage.openFileForRead("ERS", progressPath, f)) {
      uint8_t data[6];
      int dataSize = f.read(data, 6);
      if (dataSize == 4 || dataSize == 6) {
        currentSpineIndex = data[0] + (data[1] << 8);
        nextPageNumber = data[2] + (data[3] << 8);
        if (nextPageNumber == UINT16_MAX) {
          // UINT16_MAX is an in-memory navigation sentinel for "open previous
          // chapter on its last page". It should never be treated as persisted
          // resume state after sleep or reopen.
          LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
          nextPageNumber = 0;
        }
        // v18.9.9.364: sanity-check the loaded spine index against the book's
        // actual spine count. Field-observed pattern: corrupt progress.bin
        // (mid-write crash, SD glitch) has spineIndex like 17237 while the
        // book has ~30 spines. The clamp at line 5082 then set
        // currentSpineIndex == spineCount → End of Book immediately on
        // open. Reset to 0 (restart from beginning) if wildly out of range;
        // preserves the "user genuinely finished" case where index equals
        // exactly spineCount (still lands on End of Book screen).
        const int spineCount = epub->getSpineItemsCount();
        if (spineCount > 0 && currentSpineIndex > spineCount) {
          LOG_ERR("ERS",
                  "Corrupt progress.bin: spineIndex=%d exceeds book spineCount=%d -- resetting to 0",
                  currentSpineIndex, spineCount);
          currentSpineIndex = 0;
          nextPageNumber = 0;
        }
        cachedSpineIndex = currentSpineIndex;
        LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
      }
      if (dataSize == 6) {
        cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      }
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0 && !pendingPercentJump) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // v18.9.9.66: apply resumeSpine from silent-restart. main.cpp captures the
  // RTC-stashed target spine (silentRebootTargetSpine, set by
  // silentRestartToReaderWithDefragRetryAtSpine and similar) into
  // g_pendingResumeSpine at boot; without this consume the reader lands on
  // progress.bin's last-committed spine instead of the one that triggered
  // the silent-restart.
  //
  // v18.9.9.67: gate the apply. main.cpp:1943 resets silentRebootTargetSpine
  // to 0 (v67 fix: to 0xFFFFFFFFu) after consuming, and
  // silentRestartToReaderWithAction (used by e.g. BT pre-flight defrag)
  // doesn't touch that slot. So a pre-v67 non-defrag silent-restart chain
  // could leave stale value 0 in RTC, and v66 (unconditionally) applied it
  // as spine 0 -- book jumped to cover/text-reference start and screen
  // turned white when BT reconnected. The defrag-retry silent-restart is
  // the ONLY path that sets silentRebootTargetSpine explicitly, so consuming
  // it makes sense only when the boot is a defrag-retry continuation.
  // Regular silent-restart-with-action continuations (drawer group, ROA,
  // OpenBt pre-flight, etc.) resume from progress.bin.
  //
  // v18.9.9.68: use layoutDefragRetryAttempted_ (set at line ~766 above from
  // isDefragRetryContinuation) instead of calling isDefragRetryContinuation
  // directly -- v67 was checking the raw flag which had already been cleared
  // by clearDefragRetryContinuation at line ~768. Net effect pre-68: the
  // else branch always ran, resumeSpine was NEVER applied even on a genuine
  // defrag-retry boot, so v64's chapter-boundary defrag-restart still landed
  // on the wrong chapter. layoutDefragRetryAttempted_ is the reader's own
  // copy of that state and stays true until book close.
  if (layoutDefragRetryAttempted_) {
    const int resumeSpine = consumePendingResumeSpine();
    if (resumeSpine >= 0 && resumeSpine < epub->getSpineItemsCount()) {
      LOG_INF("ERS", "Applying silent-restart resumeSpine=%d (was progress.bin=%d)",
              resumeSpine, currentSpineIndex);
      currentSpineIndex = resumeSpine;
      nextPageNumber = 0;
      cachedSpineIndex = currentSpineIndex;
      pendingPercentJump = false;
    }
  } else {
    // Drain the RTC slot so a later defrag-retry boot doesn't inherit
    // a stale value from a non-defrag silent-restart.
    (void)consumePendingResumeSpine();
  }

  // Load reading stats and record session start time.
  // Session count and reading time are committed on exit once thresholds are met.
  //
  // CrumBLE: also persist a zeroed stats.bin on FIRST open so the file exists
  // from this moment forward. The "Unopened" virtual collection uses
  // stats.bin presence as its membership gate -- any book the reader has been
  // entered into, even briefly, should drop out of Unopened immediately. The
  // periodic save would eventually create the file, but only after enough
  // reading time accumulates to enter the save path.
  const bool statsExistedAtOpen = BookReadingStats::exists(epub->getCachePath());
  stats = BookReadingStats::load(epub->getCachePath());
  if (!statsExistedAtOpen) {
    stats.save(epub->getCachePath());
    CollectionsStore::getInstance().invalidateScannedVirtuals();
  }
  sessionStartMs = millis();
  sessionSegmentStartMs = sessionStartMs;
  totalSessionMsThisOpen = 0UL;
  sessionCountedThisOpen = false;
  lastIncrementalSaveMs = sessionStartMs;

  globalStats = GlobalReadingStats::load();

  initializeCompletionPromptTrigger();

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  // v18.9.9.290: start a reading-stats session. Cheap no-op on X4 or
  // pre-clock-sync X3 (getStatus() != Ok inside noteBookOpened).
  ReadingStats::noteBookOpened(epub->getPath().c_str());
  SleepCoverAssets::prepareEpub(*epub);

  // v18.9.5 REVERTED in v18.9.5.4: previously skipped the initial page paint
  // when EnableBt was queued (to avoid a flash before the QC popup). That
  // caused a freeze -- the QC handler at line 1588 waits for `section` to
  // be built, and `section` is only built inside render(). Without an
  // initial render, section stayed null forever and the connect never
  // fired. Restoring the normal first-render trigger; the extra flash on
  // the defrag path is the cost of correctness. A follow-up (v18.9.5.5)
  // can look at making the initial paint use FAST_REFRESH when a post-boot
  // action is queued, to cut the visible impact.

  // Trigger first update
  requestUpdate();
}

// CrumBLE Phase 1 fast-open: pre-grow the glyph decompression buffer.
// Called inline at every BT-enable site so the buffer is ready before
// NimBLE eats heap. See header for the full rationale.
void EpubReaderActivity::prewarmReaderTextBuffer(GfxRenderer& renderer) {
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  // CrumBLE 4.2: skip the pre-grow for SD-card fonts. The pattern works
  // for compressed built-in fonts because the FontDecompressor's hot-
  // group buffer survives clearCache via invalidateHotGroup -- the
  // capacity grown here persists into NimBLE's 58 KB allocation pass.
  // SD-card fonts have no equivalent persistent buffer; SdCardFont::
  // clearCache fully frees the per-style miniData (intervals/glyphs/
  // bitmaps, ~40-60 KB across 4 styles). The prewarm-then-clear ends up
  // allocating that much, freeing it, and fragmenting the heap right
  // before NimBLE grabs 58 KB contiguous. Symptoms in the field: words
  // resolve to REPLACEMENT_GLYPH on the next page render because the
  // SD font's interval reallocation fails under pressure, and BT
  // disconnects shortly after.
  const int readerFontId = SETTINGS.getReaderFontId();
  if (renderer.isSdCardFont(readerFontId)) {
    return;
  }
  fcm->prewarmCache(readerFontId, "etaoinshrdlucmfwypvbgkjqxzETAOINSHRDLUCMFWYP.,;:'\"!?-0123456789", 0x0F);
  // Drop the prewarm's page-slot buffers immediately. We only need the
  // prewarm to grow the *shared glyph-group buffer* to its high-water
  // mark -- clearCache keeps that capacity via invalidateHotGroup.
  // Leaving the four sample-string slot buffers held (~10-15 KB total)
  // would fragment the heap right before NimBLE's allocation pass and
  // partially defeat the protection we're trying to add.
  fcm->clearCache();
}

void EpubReaderActivity::tryRecoverLowHeapForLookup() {
  // Drop the cheapest re-buildable allocations first: font caches and the
  // parsed section DOM. Together that's ~30-50 KB of mid-size blocks
  // that get reconstructed lazily on the next render. The section pointer
  // going null also forces renderContents to re-enter the scan-then-
  // prewarm-then-render cycle, which is when miniData / hot-group
  // buffers are repopulated from a freshly compacted heap.
  LOG_INF("ERS", "Heap recovery: before maxAlloc=%u free=%u", ESP.getMaxAllocHeap(), ESP.getFreeHeap());
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->clearCache();
  if (section) section.reset();
  // Yield twice -- once to let any pending render task drain, once for
  // the heap consolidator. 80 ms total is well under the perceived-
  // latency floor on the next user interaction.
  vTaskDelay(pdMS_TO_TICKS(40));
  vTaskDelay(pdMS_TO_TICKS(40));
  LOG_INF("ERS", "Heap recovery: after  maxAlloc=%u free=%u", ESP.getMaxAllocHeap(), ESP.getFreeHeap());
  // Trigger the re-render that fills section back in. The user's next
  // tap on Lookup / Highlight then runs against the freshly-allocated
  // section DOM + prewarmed font cache instead of the stale fragmented
  // state the previous attempt failed against.
  requestUpdate();
}

void EpubReaderActivity::warmPageCacheForBtTransition() {
  // Pre-condition checks mirror renderContents's guards: without a valid
  // section + in-bounds page, loadPageFromSectionFile would just refuse.
  if (!section || section->pageCount <= 0) {
    LOG_DBG("ERA", "warmPageCacheForBtTransition: no section/pages, skipping");
    return;
  }
  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERA", "warmPageCacheForBtTransition: page %d out of bounds (count %d), skipping",
            section->currentPage, section->pageCount);
    return;
  }

  // If the cache is already valid for the current (section, spine, page),
  // there's nothing to do. This also handles the case where renderContents
  // ran between the caller's last drop and this call.
  const bool cacheHit = cachedRenderPage_ && cachedRenderSection_ == static_cast<void*>(section.get()) &&
                        cachedRenderSpine_ == currentSpineIndex && cachedRenderPageIndex_ == section->currentPage;
  if (cacheHit) {
    LOG_DBG("ERA", "warmPageCacheForBtTransition: cache already valid for spine=%d page=%d", currentSpineIndex,
            section->currentPage);
    return;
  }

  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();

  // Mirror the assignment in renderContents (lines ~3491-3506). Done here
  // explicitly because the "Connecting" popup suppresses renderContents
  // for the entire 2.7 s connect + ~1.5 s subscribe window; by the time a
  // natural render happens the heap is fragmented to ~6 KB maxAlloc and
  // the load refuses.
  //
  // CrumBLE 4.3: free-old-before-build-new. unique_ptr::operator= evaluates
  // the RHS (which builds a fresh Page DOM via TextBlock::deserialize)
  // BEFORE deleting the previously held page. So a naive `cachedRenderPage_
  // = section->loadPageFromSectionFile()` momentarily holds two complete
  // page DOMs at once -- the old one + the one being built. Under post-BT
  // heap (~12 KB MaxAlloc, ~15 KB free) the second one's TextBlock alloc
  // exceeds remaining contiguous and bad_alloc terminates. Explicit
  // reset() first guarantees only one DOM resident at the deserialize peak,
  // recovering ~5-15 KB of contiguous heap for the rebuild to land in.
  cachedRenderPage_.reset();
  cachedRenderPage_ = section->loadPageFromSectionFile();
  if (cachedRenderPage_) {
    cachedRenderSection_ = static_cast<void*>(section.get());
    cachedRenderSpine_ = currentSpineIndex;
    cachedRenderPageIndex_ = section->currentPage;
    currentPageFootnotes = cachedRenderPage_->footnotes;
    LOG_INF("ERA", "warmPageCacheForBtTransition: loaded spine=%d page=%d (free %u->%u, maxAlloc %u->%u)",
            currentSpineIndex, section->currentPage, freeBefore, ESP.getFreeHeap(), maxAllocBefore,
            ESP.getMaxAllocHeap());
  } else {
    // Load refused (loadPageFromSectionFile's own 25 KB pre-flight, or
    // the section binary is missing). Cache stays empty -- the post-BT
    // render will fail the same way the old code did, but we tried.
    cachedRenderSection_ = nullptr;
    cachedRenderSpine_ = -1;
    cachedRenderPageIndex_ = -1;
    LOG_ERR("ERA", "warmPageCacheForBtTransition: load refused (free=%u maxAlloc=%u); post-BT render may fail",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}

// CrumBLE Phase 1 fast-open: ran from loop() once the first render has
// completed. Holds the (non-BLE-critical) work that used to sit in onEnter
// and added latency to tap-to-first-pixel. The font-buffer pre-grow used
// to live here -- it now runs only at the actual BT-enable call sites
// (drawer Quick Connect, reader main menu BT toggle, etc.) so the cost
// is paid inside the "Connecting Bluetooth..." popup window where it's
// invisible, instead of at every book open.
void EpubReaderActivity::runDeferredOnEnter() {
  if (!epub) return;

  // CrumBLE: build the reader-category settings list once now, while heap is
  // unfragmented (same window the prewarm depends on -- BLE not yet eating
  // 58 KB, no chapter decoded yet). The drawer references this cache instead
  // of rebuilding under BLE pressure, which used to OOM-crash on a fragmented
  // heap.
  //
  // CrumBLE 4.5.5+: lowered the build threshold from (40 KB free / 20 KB
  // maxAlloc) to (20 KB free / 10 KB maxAlloc). The actual cost of
  // getSettingsList + the readerSettingsCache_ copy is ~6-10 KB depending on
  // settings count -- the old 20 KB MaxAlloc gate was 2x over-cautious. On
  // CJK books in particular, MaxAlloc spends most of the book lifetime in
  // the 13-15 KB range from the streaming glyph atlas + per-page metadata,
  // so the old gate was a hard miss every open and the drawer perpetually
  // fell back to its Bluetooth-only emergency view (user pain: "I opened
  // settings and got 3 BT items"). The new gate clears 99% of normal
  // post-load heap states and only skips when heap is genuinely too tight
  // for even the actual ~6 KB build.
  // CrumBLE 4.5.6: eager readerSettingsCache_ build removed. It was ~6-10 KB
  // permanently reserved every book open, whether or not the user ever visited
  // the drawer. Field test: that heap was what pushed BT + SD-font reading
  // over the floor on X4 English books -- post-BT free 504 B with cache built
  // vs 3836 B without. The drawer now builds its own settings list lazily on
  // first group expand (BookSettingsDrawerActivity::activateSelected), so
  // users who tap "BT Quick Connect" as the first action never pay the cost.

  // CrumBLE: parse the optimizer's .pxc manifest if the book has one. Path is
  // META-INF/crumble-pxc.json (standard EPUB metadata directory). Contents
  // mirror the /api/reader-render-info snapshot the optimizer used at bake
  // time. We hold the parsed fields until book close so the BLE-link edge
  // detector in loop() can prompt the user to switch layouts when needed.
  {
    const std::string manifestPath = "META-INF/crumble-pxc.json";
    size_t manifestSize = 0;
    if (epub->getItemSize(manifestPath, &manifestSize) && manifestSize > 0 && manifestSize < 2048) {
      uint8_t* manifestBytes = epub->readItemContentsToBytes(manifestPath, &manifestSize);
      if (manifestBytes) {
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, manifestBytes, manifestSize);
        if (!err) {
          PxcManifest m;
          m.orientation = doc["orientation"] | 0;
          m.screenMargin = doc["screenMargin"] | 0;
          m.imageRendering = doc["imageRendering"] | 0;
          m.fontId = doc["fontId"] | 0;
          m.viewportW = doc["viewportWidth"] | 0;
          m.viewportH = doc["viewportHeight"] | 0;
          m.screenW = doc["screenWidth"] | 0;
          m.screenH = doc["screenHeight"] | 0;
          m.pxcCount = doc["pxcCount"] | 0;
          m.fontFamily = doc["fontFamily"] | 0;
          m.fontSize = doc["fontSize"] | 0;
          m.sdFontSizeRange = doc["sdFontSizeRange"] | 0;
          if (doc["sdFontFamilyName"].is<const char*>()) {
            m.sdFontFamilyName = doc["sdFontFamilyName"].as<const char*>();
          }
          pxcManifest_ = m;
          LOG_INF("ERA", "Loaded .pxc manifest: %u images, viewport %ux%u, fontId=%ld",
                  static_cast<unsigned>(m.pxcCount), static_cast<unsigned>(m.viewportW),
                  static_cast<unsigned>(m.viewportH), static_cast<long>(m.fontId));
        } else {
          LOG_INF("ERA", "Failed to parse .pxc manifest: %s", err.c_str());
        }
        free(manifestBytes);
      }
    }
  }

  // v18.9.9.298: parse crumble-stats.json for real Stable Page Numbers.
  // Written by the optimizer (v298+) with the total visible-text character
  // count across all XHTML files. Reader uses this instead of the byte-size
  // approximation from getBookSize(), which over-counts HTML markup.
  // Missing manifest is fine -- byte-size fallback preserves v78 behavior
  // for un-optimized books.
  {
    const std::string statsPath = "META-INF/crumble-stats.json";
    size_t statsSize = 0;
    if (epub->getItemSize(statsPath, &statsSize) && statsSize > 0 && statsSize < 512) {
      uint8_t* statsBytes = epub->readItemContentsToBytes(statsPath, &statsSize);
      if (statsBytes) {
        JsonDocument sdoc;
        const DeserializationError serr = deserializeJson(sdoc, statsBytes, statsSize);
        if (!serr) {
          const uint32_t chars = sdoc["totalChars"] | 0u;
          if (chars > 0) {
            bookVisibleCharCount_ = chars;
            LOG_INF("ERA", "Loaded stats manifest: totalChars=%u", chars);
          }
        } else {
          LOG_INF("ERA", "Failed to parse stats manifest: %s", serr.c_str());
        }
        free(statsBytes);
      }
    }
  }

  // Manifest load was hoisted to onEnter() so the switch-back prompt
  // can fire BEFORE the first render starts indexing. Keep a fallback
  // here for the unlikely case where the toggle was flipped on
  // mid-session between onEnter and the deferred tick -- otherwise
  // the prompt would never see the manifest until the next book open.
  if (SETTINGS.optimizeChapterIndexing && !prebakeManifest_.has_value()) {
    PrebakeManifest pm;
    if (tryLoadPrebakeManifest(epub->getCachePath(), pm)) {
      prebakeManifest_ = pm;
      LOG_INF("ERA", "Loaded prebake manifest in deferred onEnter: fontId=%ld viewport=%ux%u",
              static_cast<long>(pm.fontId), static_cast<unsigned>(pm.viewportWidth),
              static_cast<unsigned>(pm.viewportHeight));
    }
  }
}

bool EpubReaderActivity::checkAndFirePrebakePromptIfNeeded() {
  // SETTINGS gate first so flipping the toggle off mid-session quiets the
  // prompt path even though the manifest is still parsed in memory.
  if (!SETTINGS.optimizeChapterIndexing) {
    if (!prebakePromptDiagLogged_) {
      LOG_DBG("ERA", "Prebake prompt skipped: optimizeChapterIndexing=false");
      prebakePromptDiagLogged_ = true;
    }
    return false;
  }
  if (!prebakeManifest_.has_value()) {
    if (!prebakePromptDiagLogged_) {
      LOG_DBG("ERA", "Prebake prompt skipped: no manifest loaded (book has no prebake)");
      prebakePromptDiagLogged_ = true;
    }
    return false;
  }
  if (prebakePromptShowing_) return false;
  const PrebakeManifest& pm = *prebakeManifest_;
  const int32_t curFontId = SETTINGS.getReaderFontId();
  const float curLineComp = SETTINGS.getReaderLineCompression();

  // Compute current viewport (same logic as handleReaderRenderInfo / render).
  int omt, omr, omb, oml;
  renderer.getOrientedViewableTRBL(&omt, &omr, &omb, &oml);
  omt += SETTINGS.screenMargin;
  oml += SETTINGS.screenMargin;
  omr += SETTINGS.screenMargin;
  const uint8_t statusBarH = UITheme::getInstance().getStatusBarHeight();
  constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;
  omb += std::max<uint8_t>(SETTINGS.screenMargin, static_cast<uint8_t>(statusBarH + STATUS_BAR_TEXT_PADDING));
  const uint16_t curViewportW = static_cast<uint16_t>(renderer.getScreenWidth() - oml - omr);
  const uint16_t curViewportH = static_cast<uint16_t>(renderer.getScreenHeight() - omt - omb);

  auto snapshotCurrent = [this]() {
    prebakeLastSnapshot_.orientation = SETTINGS.orientation;
    prebakeLastSnapshot_.screenMargin = SETTINGS.screenMargin;
    prebakeLastSnapshot_.imageRendering = SETTINGS.imageRendering;
    prebakeLastSnapshot_.fontFamily = SETTINGS.fontFamily;
    prebakeLastSnapshot_.fontSize = SETTINGS.fontSize;
    prebakeLastSnapshot_.sdFontSizeRange = SETTINGS.sdFontSizeRange;
    strncpy(prebakeLastSnapshot_.sdFontFamilyName, SETTINGS.sdFontFamilyName,
            sizeof(prebakeLastSnapshot_.sdFontFamilyName) - 1);
    prebakeLastSnapshot_.sdFontFamilyName[sizeof(prebakeLastSnapshot_.sdFontFamilyName) - 1] = '\0';
    prebakeLastSnapshot_.lineSpacing = SETTINGS.lineSpacing;
    prebakeLastSnapshot_.paragraphAlignment = SETTINGS.paragraphAlignment;
    prebakeLastSnapshot_.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
    prebakeLastSnapshot_.forceParagraphIndents = SETTINGS.forceParagraphIndents;
    prebakeLastSnapshot_.hyphenationEnabled = SETTINGS.hyphenationEnabled;
    prebakeLastSnapshot_.embeddedStyle = SETTINGS.embeddedStyle;
    prebakeLastSnapshot_.bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
    prebakeLastSnapshot_.guideReadingEnabled = SETTINGS.guideReadingEnabled;
    prebakeLastSnapshot_.initialised = true;
  };

  const bool snapChanged = !prebakeLastSnapshot_.initialised ||
      prebakeLastSnapshot_.orientation != SETTINGS.orientation ||
      prebakeLastSnapshot_.screenMargin != SETTINGS.screenMargin ||
      prebakeLastSnapshot_.imageRendering != SETTINGS.imageRendering ||
      prebakeLastSnapshot_.fontFamily != SETTINGS.fontFamily ||
      prebakeLastSnapshot_.fontSize != SETTINGS.fontSize ||
      prebakeLastSnapshot_.sdFontSizeRange != SETTINGS.sdFontSizeRange ||
      strncmp(prebakeLastSnapshot_.sdFontFamilyName, SETTINGS.sdFontFamilyName,
              sizeof(prebakeLastSnapshot_.sdFontFamilyName)) != 0 ||
      prebakeLastSnapshot_.lineSpacing != SETTINGS.lineSpacing ||
      prebakeLastSnapshot_.paragraphAlignment != SETTINGS.paragraphAlignment ||
      prebakeLastSnapshot_.extraParagraphSpacing != SETTINGS.extraParagraphSpacing ||
      prebakeLastSnapshot_.forceParagraphIndents != SETTINGS.forceParagraphIndents ||
      prebakeLastSnapshot_.hyphenationEnabled != SETTINGS.hyphenationEnabled ||
      prebakeLastSnapshot_.embeddedStyle != SETTINGS.embeddedStyle ||
      prebakeLastSnapshot_.bionicReadingEnabled != SETTINGS.bionicReadingEnabled ||
      prebakeLastSnapshot_.guideReadingEnabled != SETTINGS.guideReadingEnabled;

  if (!prebakeLastSnapshot_.initialised) {
    // v18.9.9.35 (task #17): if the user previously declined the prompt
    // for this book at exactly these SETTINGS, honor that across silent
    // restarts / power cycles. Populating the snapshot from current
    // SETTINGS below is what the snapChanged gate uses to skip the
    // prompt on the next tick; we short-circuit the fresh-open path here
    // to reach that same steady state.
    if (epub && prebakeDeclinedSidecarMatchesCurrent(epub->getCachePath())) {
      LOG_INF("ERA", "Prebake prompt suppressed: user previously declined for this book at these settings");
      snapshotCurrent();
      prebakePromptDiagLogged_ = true;
      // v18.9.9.455: also mark this book as skip-prebake-fallback so the
      // section loader stops trying sections-prebake/N.bin per chapter.
      prebakeDeclinedForThisBook_ = true;
      return false;
    }
    snapshotCurrent();
  }
  if (!snapChanged) {
    if (!prebakePromptDiagLogged_) {
      LOG_DBG("ERA", "Prebake prompt skipped: snapshot unchanged since last call (no settings drift)");
      prebakePromptDiagLogged_ = true;
    }
    return false;
  }

  // CrumBLE 4.5.7: viewport tolerance matches Section::tryLoadFromPath.
  // freeink-sdk migration shifted computed viewport by ~5px; don't nag
  // users with a prompt they can't resolve (settings are already correct;
  // only the theme metric moved).
  constexpr uint16_t kViewportTolerancePx = 8;
  const uint16_t vpWDelta = (pm.viewportWidth > curViewportW) ? (pm.viewportWidth - curViewportW)
                                                              : (curViewportW - pm.viewportWidth);
  const uint16_t vpHDelta = (pm.viewportHeight > curViewportH) ? (pm.viewportHeight - curViewportH)
                                                               : (curViewportH - pm.viewportHeight);
  // v18.9.9.308: cross-check the SECTION file's fontId against the manifest's
  // claim. Field bug: some prebake pipelines (notably crosspointreader.com's
  // browser optimizer) write manifest.json with the device's CURRENT fontId
  // rather than the fontId actually used to bake the sections. Result: user
  // baked with LXGWWenKai (fontId=-2037991310), manifest recorded Bitter
  // (417158117) because the browser tab had Bitter selected at that moment,
  // sections were correctly baked at -2037991310. Device compares
  // manifest(417158117) vs device(417158117) at prompt-gate -> match -> no
  // prompt -> loads section, sees section-level fingerprint mismatch, drops
  // to indexing, EOCD errors under tight heap. Cross-checking section 0's
  // fontId (the ground truth) catches manifests that lie.
  int32_t sectionFontId = pm.fontId;  // fallback: trust the manifest
  sectionFontIdFromPeek_ = 0;  // v18.9.9.311: reset per-check; populated below on successful peek
  if (epub) {
    // Mirror of SECTION_CACHE_MAGIC in Section.cpp (`0xFF C X S`). Kept
    // as a local constexpr because Section's version is a compilation-unit
    // static -- exposing it via Section.h just for one peek would grow the
    // public surface of a hot header. If the on-disk format changes, the
    // magic check below fails and we fall through to trusting the
    // manifest, which is the correct conservative behaviour.
    constexpr uint32_t kSectionCacheMagicMirror = 0x535843FF;
    const std::string sectionPath = epub->getCachePath() + "/sections-prebake/0.bin";
    FsFile sfile;
    if (Storage.openFileForRead("ERA", sectionPath, sfile)) {
      uint32_t magic = 0;
      uint8_t version = 0;
      int32_t peekFontId = 0;
      // Section header layout: magic(4B) + version(1B) + fontId(4B int).
      if (serialization::tryReadPod(sfile, magic) && magic == kSectionCacheMagicMirror &&
          serialization::tryReadPod(sfile, version) && serialization::tryReadPod(sfile, peekFontId)) {
        sectionFontId = peekFontId;
        sectionFontIdFromPeek_ = peekFontId;  // v18.9.9.311: cached for rescue path
        if (peekFontId != pm.fontId) {
          LOG_INF("ERA",
                  "Prebake fingerprint: manifest claims fontId=%ld but sections-prebake/0.bin says fontId=%ld -- "
                  "trusting section (manifest is stale)",
                  static_cast<long>(pm.fontId), static_cast<long>(peekFontId));
        }
      }
      sfile.close();
    }
  }

  const bool mismatch =
      (sectionFontId != curFontId) ||
      (pm.lineCompression != curLineComp) ||
      (pm.extraParagraphSpacing != SETTINGS.extraParagraphSpacing) ||
      (pm.forceParagraphIndents != SETTINGS.forceParagraphIndents) ||
      (pm.paragraphAlignment != SETTINGS.paragraphAlignment) ||
      (vpWDelta > kViewportTolerancePx) ||
      (vpHDelta > kViewportTolerancePx) ||
      (pm.hyphenationEnabled != SETTINGS.hyphenationEnabled) ||
      (pm.embeddedStyle != SETTINGS.embeddedStyle) ||
      (pm.imageRendering != SETTINGS.imageRendering) ||
      (pm.bionicReadingEnabled != SETTINGS.bionicReadingEnabled) ||
      (pm.guideReadingEnabled != SETTINGS.guideReadingEnabled);
  // v18.9.9.58 gap: tableRendering is in the section-file fingerprint (v43)
  // but not in the PrebakeManifest JSON, so the prompt can't preemptively
  // catch a tables drift. Section load falls through the file-level
  // fingerprint mismatch and rebuilds cold. Fix requires prebake CLI schema
  // bump + tryLoadPrebakeManifest parse -- deferred out of v58 scope.

  if (!mismatch) {
    if (!prebakePromptDiagLogged_) {
      LOG_INF("ERA",
              "Prebake prompt skipped: full fingerprint matches (fontId=%ld viewport=%ux%u lineComp=%.3f "
              "ePS=%d fPI=%d pA=%u hyph=%d embed=%d imgR=%u bionic=%d guide=%d) -- no prompt needed",
              static_cast<long>(curFontId), static_cast<unsigned>(curViewportW),
              static_cast<unsigned>(curViewportH), static_cast<double>(curLineComp),
              SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
              static_cast<unsigned>(SETTINGS.paragraphAlignment),
              SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
              static_cast<unsigned>(SETTINGS.imageRendering),
              SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled);
      prebakePromptDiagLogged_ = true;
    }
    snapshotCurrent();  // user-driven drift but still valid; accept silently
    return false;
  }

  // v18.9.9.41 (task #26): mismatch confirmed and we're about to fire the
  // prompt. Atomically claim the "showing" slot so a concurrent caller
  // (loop() vs render() run on separate FreeRTOS tasks) can't also fall
  // through and double-push. If someone beat us to it, silently no-op.
  bool expected = false;
  if (!prebakePromptShowing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }

  LOG_INF("ERA",
          "Prebake fingerprint mismatch: cur fontId=%ld lineComp=%.3f ePS=%d fPI=%d pA=%u "
          "vp=%ux%u hyph=%d embed=%d imgR=%u bionic=%d guide=%d vs prebake fontId=%ld "
          "lineComp=%.3f ePS=%d fPI=%d pA=%u vp=%ux%u hyph=%d embed=%d imgR=%u bionic=%d guide=%d",
          static_cast<long>(curFontId), static_cast<double>(curLineComp),
          SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
          static_cast<unsigned>(SETTINGS.paragraphAlignment),
          static_cast<unsigned>(curViewportW), static_cast<unsigned>(curViewportH),
          SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
          static_cast<unsigned>(SETTINGS.imageRendering),
          SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
          static_cast<long>(pm.fontId), static_cast<double>(pm.lineCompression),
          pm.extraParagraphSpacing, pm.forceParagraphIndents,
          static_cast<unsigned>(pm.paragraphAlignment),
          static_cast<unsigned>(pm.viewportWidth), static_cast<unsigned>(pm.viewportHeight),
          pm.hyphenationEnabled, pm.embeddedStyle,
          static_cast<unsigned>(pm.imageRendering),
          pm.bionicReadingEnabled, pm.guideReadingEnabled);

  // Action-labeled two-option prompt. The previous confirm/cancel polarity
  // (confirm = restore prepared) tricked users who had just edited a
  // setting in the drawer: they expected "Confirm" to mean "apply what I
  // typed," and instead saw their edit get reverted by what they thought
  // was the affirmative button. Action labels remove the ambiguity --
  // the user clicks the verb that names what they want to happen.
  // Option 0 (default) keeps the user's current settings; Back/Cancel
  // maps to that same outcome because "do nothing destructive" is the
  // less surprising fallback when someone backs out of the prompt.
  //
  // CrumBLE 4.5.4: also list WHICH fields differ in the prompt body. Users
  // hit this prompt without knowing what they did wrong; the generic
  // "different reader settings" text made it impossible to align the
  // device's settings to the prebake without trial-and-error toggling.
  // Now the prompt names each drifted field (Font Size: 14pt -> 18pt,
  // Hyphenation: off -> on, etc.) so the user can decide informed.
  auto onoff = [](bool b) -> const char* { return b ? "on" : "off"; };
  std::string diffLines;
  auto append = [&diffLines](const std::string& line) {
    if (!diffLines.empty()) diffLines += "\n";
    diffLines += "  - ";
    diffLines += line;
  };
  if (pm.fontId != curFontId) {
    // CrumBLE 4.5.4 follow-up: show the human-readable font name (e.g.
    // "Bitter 14pt" / "LXGWWenKai 18pt") instead of the raw uint32 hash
    // -- the prebake manifest already carries fontFamily / fontSize /
    // sdFontFamilyName for exactly this display path, and the BT-path
    // prompt has used fontLabel() since 4.5.4 for the same reason.
    append("Font: " +
           fontLabel(readerSettingsCache_, SETTINGS.fontFamily, SETTINGS.fontSize, SETTINGS.sdFontSizeRange,
                     SETTINGS.sdFontFamilyName) +
           " -> " +
           fontLabel(readerSettingsCache_, pm.fontFamily, pm.fontSize, pm.sdFontSizeRange,
                     std::string(pm.sdFontFamilyName)));
  }
  if (vpWDelta > kViewportTolerancePx || vpHDelta > kViewportTolerancePx) {
    append("Viewport: " + std::to_string(curViewportW) + "x" + std::to_string(curViewportH) +
           " (device) vs " + std::to_string(pm.viewportWidth) + "x" + std::to_string(pm.viewportHeight) + " (prebake)");
  }
  if (pm.lineCompression != curLineComp) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Line spacing: %.2f -> %.2f", static_cast<double>(curLineComp),
             static_cast<double>(pm.lineCompression));
    append(buf);
  }
  if (pm.extraParagraphSpacing != SETTINGS.extraParagraphSpacing) {
    append(std::string("Paragraph spacing: ") + onoff(SETTINGS.extraParagraphSpacing) + " -> " + onoff(pm.extraParagraphSpacing));
  }
  if (pm.forceParagraphIndents != SETTINGS.forceParagraphIndents) {
    append(std::string("Force indents: ") + onoff(SETTINGS.forceParagraphIndents) + " -> " + onoff(pm.forceParagraphIndents));
  }
  if (pm.paragraphAlignment != SETTINGS.paragraphAlignment) {
    append("Alignment: " + std::to_string(SETTINGS.paragraphAlignment) + " -> " + std::to_string(pm.paragraphAlignment));
  }
  if (pm.hyphenationEnabled != SETTINGS.hyphenationEnabled) {
    append(std::string("Hyphenation: ") + onoff(SETTINGS.hyphenationEnabled) + " -> " + onoff(pm.hyphenationEnabled));
  }
  if (pm.embeddedStyle != SETTINGS.embeddedStyle) {
    append(std::string("Embedded CSS: ") + onoff(SETTINGS.embeddedStyle) + " -> " + onoff(pm.embeddedStyle));
  }
  if (pm.imageRendering != SETTINGS.imageRendering) {
    append("Images: " + std::to_string(SETTINGS.imageRendering) + " -> " + std::to_string(pm.imageRendering));
  }
  if (pm.bionicReadingEnabled != SETTINGS.bionicReadingEnabled) {
    append(std::string("Bionic reading: ") + onoff(SETTINGS.bionicReadingEnabled) + " -> " + onoff(pm.bionicReadingEnabled));
  }
  if (pm.guideReadingEnabled != SETTINGS.guideReadingEnabled) {
    append(std::string("Guide reading: ") + onoff(SETTINGS.guideReadingEnabled) + " -> " + onoff(pm.guideReadingEnabled));
  }
  std::string promptBody =
      "This book's chapter cache was prepared with different reader settings:\n\n" +
      diffLines +
      "\n\nKeep your current settings (rebuild chapters on demand), or restore the prepared layout (apply the book's prepared settings to your device)?";
  // prebakePromptShowing_ was atomically claimed above (task #26) before
  // the LOG_INF fired; no need to set again here.
  startActivityForResult(
      std::make_unique<ChoicePromptActivity>(
          renderer, mappedInput, "Settings differ from prepared layout",
          promptBody,
          std::vector<std::string>{"Keep my current settings", "Restore prepared layout"},
          /*ignoreInitialConfirmRelease=*/true),
      [this](const ActivityResult& result) {
        prebakePromptShowing_ = false;
        // ChoicePromptResult lives inside result.data. choice: 0 = "Keep
        // my current settings", 1 = "Restore prepared layout", -1 = user
        // hit the prompt's Cancel/back button.
        //
        // CrumBLE 4.4 follow-up: treat Cancel as "back to where I was
        // before the prompt fired". The prompt fires immediately on book
        // open when prebake settings drift from the current SETTINGS, so
        // "before the prompt" = the library carousel. Previously back-out
        // silently fell through to the "Keep current settings" path,
        // which then indexed + entered the book with mismatched settings
        // -- the opposite of what a Cancel button should do. finish()
        // pops the reader activity off the stack so the library renders
        // again immediately.
        int chosen = -1;
        if (const auto* cp = std::get_if<ChoicePromptResult>(&result.data)) {
          chosen = cp->choice;
        }
        if (result.isCancelled) {
          LOG_INF("ERA", "Prebake prompt: cancelled, returning to library");
          finish();
          return;
        }
        // CrumBLE 4.5.4 follow-up: suppress the BT-path PxcManifest prompt
        // for this session -- the user just answered the open-book
        // PrebakeManifest prompt, and the BT prompt would show nearly the
        // same mismatch in slightly different wording. The 'Restore'
        // branch below applies fontFamily/fontSize/sdFontFamilyName, so
        // any residual fontId difference the BT prompt would still surface
        // comes from a missing SD font (BT prompt can't fix that either).
        btManifestPromptAnsweredThisSession_ = true;
        // v18.9.9.187: RTC-persist so silent-restart chains honor the answer.
        if (epub) writeBtManifestAnsweredSidecar(epub->getCachePath());
        const bool keepCurrent = chosen != 1;
        if (keepCurrent) {
          // User declined -- keep their current settings. Don't delete the
          // prebake (Section.cpp's clearCache only ever touches sections/,
          // never sections-prebake/), so if they later open the book again
          // and revert their settings, the cache is still there. Update
          // the snapshot to current SETTINGS so we don't keep firing the
          // prompt on every render tick.
          LOG_INF("ERA", "Prebake prompt: user declined, keeping current settings");
          // v18.9.9.58: the user actively chose "keep my settings" -- lock
          // in CustomSettings as the render path so per-path compat state
          // routes to compat_custom.flag. Stash for silent-restart continuity.
          readerActivePath_ = ReaderPath::CustomSettings;
          APP_STATE.readerActivePath = static_cast<uint8_t>(readerActivePath_);
          stashReaderActivePathForNextBoot(APP_STATE.readerActivePath);
          prebakeLastSnapshot_.orientation = SETTINGS.orientation;
          prebakeLastSnapshot_.screenMargin = SETTINGS.screenMargin;
          prebakeLastSnapshot_.imageRendering = SETTINGS.imageRendering;
          prebakeLastSnapshot_.fontFamily = SETTINGS.fontFamily;
          prebakeLastSnapshot_.fontSize = SETTINGS.fontSize;
          prebakeLastSnapshot_.sdFontSizeRange = SETTINGS.sdFontSizeRange;
          strncpy(prebakeLastSnapshot_.sdFontFamilyName, SETTINGS.sdFontFamilyName,
                  sizeof(prebakeLastSnapshot_.sdFontFamilyName) - 1);
          prebakeLastSnapshot_.sdFontFamilyName[sizeof(prebakeLastSnapshot_.sdFontFamilyName) - 1] = '\0';
          prebakeLastSnapshot_.lineSpacing = SETTINGS.lineSpacing;
          prebakeLastSnapshot_.paragraphAlignment = SETTINGS.paragraphAlignment;
          prebakeLastSnapshot_.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
          prebakeLastSnapshot_.forceParagraphIndents = SETTINGS.forceParagraphIndents;
          prebakeLastSnapshot_.hyphenationEnabled = SETTINGS.hyphenationEnabled;
          prebakeLastSnapshot_.embeddedStyle = SETTINGS.embeddedStyle;
          prebakeLastSnapshot_.bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
          prebakeLastSnapshot_.guideReadingEnabled = SETTINGS.guideReadingEnabled;
          // v18.9.9.35 (task #17): persist the decision so the same
          // fingerprint isn't asked about again on subsequent silent
          // restarts or book reopens.
          if (epub) writePrebakeDeclinedSidecar(epub->getCachePath());
          // v18.9.9.455: session flag so the section loader skips prebake
          // fallback for this book from here on. No more per-section
          // fingerprint-mismatch SD reads.
          prebakeDeclinedForThisBook_ = true;
          requestUpdate();
          return;
        }
        // User confirmed -- apply the prebake's prepared layout. With the
        // reversion fields in the manifest we can fully restore the raw
        // SETTINGS the prebake was made against, so the next fingerprint
        // check against sections-prebake/ matches exactly.
        if (!prebakeManifest_.has_value()) return;
        const PrebakeManifest& pm2 = *prebakeManifest_;
        LOG_INF("ERA", "Prebake prompt: user accepted, restoring prepared layout");
        // v18.9.9.58: user accepted -- flip the render path so per-path
        // compat routes to compat_prepared.flag from here on. Stash for
        // silent-restart continuity.
        readerActivePath_ = ReaderPath::PreparedLayout;
        APP_STATE.readerActivePath = static_cast<uint8_t>(readerActivePath_);
        stashReaderActivePathForNextBoot(APP_STATE.readerActivePath);
        SETTINGS.orientation = pm2.orientation;
        SETTINGS.screenMargin = pm2.screenMargin;
        SETTINGS.imageRendering = pm2.imageRendering;
        SETTINGS.fontFamily = pm2.fontFamily;
        SETTINGS.fontSize = pm2.fontSize;
        SETTINGS.sdFontSizeRange = pm2.sdFontSizeRange;
        strncpy(SETTINGS.sdFontFamilyName, pm2.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
        SETTINGS.lineSpacing = pm2.lineSpacing;
        SETTINGS.paragraphAlignment = pm2.paragraphAlignment;
        SETTINGS.extraParagraphSpacing = pm2.extraParagraphSpacing;
        SETTINGS.forceParagraphIndents = pm2.forceParagraphIndents;
        SETTINGS.hyphenationEnabled = pm2.hyphenationEnabled;
        SETTINGS.embeddedStyle = pm2.embeddedStyle;
        SETTINGS.bionicReadingEnabled = pm2.bionicReadingEnabled;
        SETTINGS.guideReadingEnabled = pm2.guideReadingEnabled;
        SETTINGS.saveToFile();
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        // CrumBLE 4.2 fix: reload the SD-font manager so getReaderFontId()
        // recomputes the SD font hash against the JUST-restored sdFontFamilyName /
        // sdFontSizeRange / fontSize. Without this the manager keeps whatever
        // SD font it loaded at boot, and the per-section fingerprint check
        // (SETTINGS.getReaderFontId() vs sections-prebake/N.bin) keeps failing
        // because the live fontId still comes from the stale loaded family
        // -- producing the "Use prepared layout, but indexing every chapter"
        // symptom even though the user accepted the prepared layout. The
        // built-in-font path doesn't need this because its fontId is purely
        // a switch on (fontFamily, fontSize), no runtime side-state.
        sdFontSystem.ensureLoaded(renderer);

        // v18.9.9.311: prebake-fontId rescue. If the manifest was baked by a
        // broken pipeline (crosspointreader.com writes sdFontFamilyName=""
        // + fontId=Bitter's even when sections were actually rendered with
        // an SD font), the settings-restore above only recovered the LIE.
        // Cross-check: after the reload, does SETTINGS.getReaderFontId()
        // actually match the section-file fingerprint? If not, iterate
        // installed SD fonts to find the one whose fontId matches the
        // section, and swap SETTINGS to that. Then reload again. Costs
        // 1 tiny SD read per installed .cpfont; only fires when the
        // manifest is lying, so happy path pays nothing.
        {
          const int32_t restoredFontId = SETTINGS.getReaderFontId();
          if (sectionFontIdFromPeek_ != 0 && restoredFontId != sectionFontIdFromPeek_) {
            std::string rescuedFamily;
            uint8_t rescuedPointSize = 0;
            if (sdFontSystem.findFamilyByFontId(sectionFontIdFromPeek_, rescuedFamily, rescuedPointSize)) {
              LOG_INF("ERA",
                      "Prebake rescue: manifest restored to fontId=%ld but section wants %ld; "
                      "found installed family '%s' @%upt with matching fontId -- swapping",
                      static_cast<long>(restoredFontId), static_cast<long>(sectionFontIdFromPeek_),
                      rescuedFamily.c_str(), rescuedPointSize);
              strncpy(SETTINGS.sdFontFamilyName, rescuedFamily.c_str(),
                      sizeof(SETTINGS.sdFontFamilyName) - 1);
              SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
              // ensureLoaded picks the file matching (family, sdFontSizeRange,
              // fontSize) -- as long as the family has the target size, we get
              // the same fontId back. If the point-size enum still doesn't
              // resolve to rescuedPointSize (e.g. family has multiple sizes),
              // reader will report a fresh mismatch and we accept the fall-
              // through since we can't safely reverse-map pointSize -> enum.
              SETTINGS.saveToFile();
              sdFontSystem.ensureLoaded(renderer);
            } else {
              // v18.9.9.323: distinguish "family present but content differs"
              // from "family not installed at all". The old pessimistic log
              // claimed layout will fail regardless -- but if the manifest's
              // sdFontFamilyName IS installed (just a different build/version
              // with different content bytes hashing differently), the cold
              // rebuild path will succeed using the currently-installed font.
              // The user's on-screen layout may drift slightly from what was
              // originally baked (glyph metrics can differ across builds of
              // the same family) but the book renders. Only genuinely
              // uninstalled family names hit the hard failure.
              const bool familyInstalled =
                  SETTINGS.sdFontFamilyName[0] != '\0' &&
                  sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName) != nullptr;
              if (familyInstalled) {
                LOG_INF("ERA",
                        "Prebake rescue: family '%s' installed but content hash %ld (device) != %ld (baked); "
                        "sections will be rebuilt cold from EPUB using currently-installed font. Layout "
                        "may drift slightly if glyph metrics differ across font builds.",
                        SETTINGS.sdFontFamilyName, static_cast<long>(restoredFontId),
                        static_cast<long>(sectionFontIdFromPeek_));
              } else {
                LOG_ERR("ERA",
                        "Prebake rescue: family '%s' NOT installed AND no font with matching fontId=%ld -- "
                        "prebake layout unusable. Install the original bake font on SD card.",
                        SETTINGS.sdFontFamilyName, static_cast<long>(sectionFontIdFromPeek_));
              }
            }
          }
        }
        // Snapshot the now-reverted SETTINGS as the new baseline.
        prebakeLastSnapshot_.orientation = SETTINGS.orientation;
        prebakeLastSnapshot_.screenMargin = SETTINGS.screenMargin;
        prebakeLastSnapshot_.imageRendering = SETTINGS.imageRendering;
        prebakeLastSnapshot_.fontFamily = SETTINGS.fontFamily;
        prebakeLastSnapshot_.fontSize = SETTINGS.fontSize;
        prebakeLastSnapshot_.sdFontSizeRange = SETTINGS.sdFontSizeRange;
        strncpy(prebakeLastSnapshot_.sdFontFamilyName, SETTINGS.sdFontFamilyName,
                sizeof(prebakeLastSnapshot_.sdFontFamilyName) - 1);
        prebakeLastSnapshot_.sdFontFamilyName[sizeof(prebakeLastSnapshot_.sdFontFamilyName) - 1] = '\0';
        prebakeLastSnapshot_.lineSpacing = SETTINGS.lineSpacing;
        prebakeLastSnapshot_.paragraphAlignment = SETTINGS.paragraphAlignment;
        prebakeLastSnapshot_.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
        prebakeLastSnapshot_.forceParagraphIndents = SETTINGS.forceParagraphIndents;
        prebakeLastSnapshot_.hyphenationEnabled = SETTINGS.hyphenationEnabled;
        prebakeLastSnapshot_.embeddedStyle = SETTINGS.embeddedStyle;
        prebakeLastSnapshot_.bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
        prebakeLastSnapshot_.guideReadingEnabled = SETTINGS.guideReadingEnabled;
        // v18.9.9.35 (task #17): user aligned with prebake; drop any
        // stale "declined" record so a future settings drift asks fresh
        // instead of comparing against the pre-accept snapshot.
        if (epub) clearPrebakeDeclinedSidecar(epub->getCachePath());
        if (section) section.reset();
        requestUpdate();
      });
  return true;
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // CrumBLE 4.5.116: tear down the reader's fallback state (either the
  // primary-alias or the suppression flag, depending on which onEnter
  // branch fired). releaseFallback covers both -- it clears the alias
  // flag AND the fallback pointer, so the per-tick poll's next tick
  // sees a clean slate and loads the user's real fallback family.
  // Silent-restart bypasses onExit; the flag resets on the fresh boot.
  sdFontSystem.releaseFallback(renderer);
  sdFontSystem.setFallbackSuppressed(false);

  // v18.9.9.290: flush the reading-stats session. Silent-restart bypasses
  // onExit; ReadingStats::tick() also flushes every 5 min so at most that
  // window is lost across an unclean exit.
  ReadingStats::onBookClose();

  // v18.9.9.54 (task #39): clear the RTC-scoped prebake decline. Silent
  // restart goes through ESP.restart() which bypasses onExit entirely,
  // so this only fires on user-initiated exit (Back to home). Matches
  // the user's mental model: closing the book and reopening it is a
  // "fresh chance" to accept the prepared layout; only silent-restart
  // chains within the same session keep the decline active.
  if (epub) clearPrebakeDeclinedSidecar(epub->getCachePath());
  // v18.9.9.187: same user-exit reset for the BT manifest answered flag.
  // Silent-restart bypasses onExit; only real book-close reaches here.
  clearBtManifestAnsweredSidecar();

  // v18.9.9.23: clear the Settings-visible compat state -- the user is
  // leaving the reader, and Settings shouldn't keep locking values on the
  // book they just closed.
  APP_STATE.readerCompatModeActive = false;
  // v18.9.9.174: signal HomeActivity to force a full refresh if the last
  // rendered page had images. Prevents cover/illustration ghosting on the
  // shelf paint that immediately follows this exit.
  // v18.9.9.191: widened to include pages with highlights. Field report
  // (v188): exiting from a highlighted-but-image-free page produced a
  // 2/3-black, 1/3-white split ghost on the Home paint at tight heap.
  // prevPageHadHighlights is set by renderSavedHighlightsOverlay after
  // each render, so it accurately reflects the last-rendered state.
  APP_STATE.readerExitedFromImagePage = lastRenderedPageHadImages_ || prevPageHadHighlights;
  // v18.9.9.25: reset the drawer's silent-restart one-shot latch so the next
  // book open gets a fresh budget.
  APP_STATE.drawerHeapRestartTriedThisBook = false;
  // v18.9.9.59: clear the manual-off marker on book close. A subsequent
  // book open starts fresh -- Layer 2 auto-writes in the new session
  // aren't credited to a disable done in a previous book.
  APP_STATE.compatUserDisabledThisSession = false;

  // NOTE: the deep-sleep cycle cache (last_reader_page.bin) is no longer
  // snapshotted here. onExit runs AFTER the "Going home..." popup
  // (exitToHomeWithPopup) has been drawn, which baked that popup into the
  // cached background that transparent sleep PNGs show through. The clean page
  // is now captured at the two real exit points before their popups:
  // exitToHomeWithPopup() (go home) and SleepActivity::onEnter() (sleep).

  // BLE is a reader-session-only feature: turn it off whenever the user leaves
  // a book. The actual disable() is deferred to the next main-loop tick because
  // we're holding the render lock here and NimBLE teardown can call back into
  // the activity manager.
  BluetoothHIDManager::getInstance().requestDisableLater();

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Commit any remaining session time. Idempotent — if a deep-sleep
  // commit or incremental save already banked the current segment,
  // commitReadingSession returns without double-counting. (It also
  // guards `if (!epub) return;`, subsuming 1.3's null-check on save.)
  commitReadingSession();

  // CrumBLE 4.4: defensive save-on-exit. Progress is already saved on
  // every page render (line ~4131), so the typical exit has nothing new
  // to write. But if the render-time save FAILED under heap fragmentation
  // (e.g. mid-page interaction left MaxAlloc too low for the FsFile
  // alloc), the book gets stuck on its last successfully-saved page --
  // user-fatal because only deleting/re-uploading the book clears it.
  // Here at onExit the heap is usually cleaner (page DOM released,
  // BT teardown queued), so one more attempt with a heap pre-flight
  // catches the common case at near-zero cost.
  // v18.9.9.471: always save at onExit (previously gated behind
  // pendingSyncSaveError). Progress is now debounced during page turns,
  // so unsaved deltas can exist even on a healthy SD — need to flush
  // them here. Cheap when it's a no-op re-save; guaranteed on-exit
  // freshness when it's not.
  if (epub && section) {
    constexpr uint32_t kExitSaveMinMaxAlloc = 4 * 1024;
    const uint32_t exitMaxAlloc = ESP.getMaxAllocHeap();
    if (exitMaxAlloc >= kExitSaveMinMaxAlloc) {
      if (saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
        pagesSinceProgressSave_ = 0;
        pendingSyncSaveError = false;
      } else {
        LOG_INF("ERS", "Save-on-exit failed (maxAlloc=%u) — SD may need repair", exitMaxAlloc);
      }
    } else {
      LOG_INF("ERS", "Save-on-exit skipped: heap too low (maxAlloc=%u < %u)",
              exitMaxAlloc, kExitSaveMinMaxAlloc);
    }
  }

  // v18.9.9.209: paired with the onEnter arming — a terminate after this
  // point should route wherever the NEXT activity arms, not back into
  // the book we just left.
  clearArmedSilentRestartTarget();

  // v18.9.9.202: persist the current chapter title so Home's Dashboard
  // theme can show it under the book title without loading the EPUB.
  writeChapterTitleSidecar();

  BOOKMARKS.unload();

  // The renderer holds raw pointers into section->glyphAtlasSlots_[*].fontData
  // (and into the embedded-subset slots) that were handed over via the
  // per-page setEmbeddedGlyphData() call. Once section.reset() runs below,
  // those vectors are freed and the renderer's stored pointers dangle. The
  // GfxRenderer.h contract on setEmbeddedGlyphData explicitly puts the burden
  // of clearing on the reader. Skipping this step had been benign until the
  // CrumBLE 4.5.5 streaming-atlas changes started leaving more state inside
  // slot.fontData (glyphBitmapCtx = section_ptr) -- now any post-reset render
  // (home transition popup, screensaver redraw, idle repaint) dereferences
  // the freed Section and reads heap-poison bytes (0xabba1234), crashing with
  // load access fault. Clearing here closes the window.
  renderer.clearEmbeddedGlyphData(SETTINGS.getReaderFontId());

  section.reset();

  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string title = epub->getTitle();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath, title);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::commitReadingSession() {
  if (!epub) return;
  // Bank elapsed time from the current segment. sessionSegmentStartMs
  // is reset every time we commit so successive commits (incremental
  // save, deep-sleep, onExit) don't double-add the same milliseconds.
  const unsigned long now = millis();
  const unsigned long segmentMs = now - sessionSegmentStartMs;
  if (segmentMs == 0UL) return;
  sessionSegmentStartMs = now;
  totalSessionMsThisOpen += segmentMs;

  // Session count: incremented at most once per open (when cumulative
  // time crosses the 60s threshold). A book briefly tapped open
  // doesn't bump the count; a long read commits exactly one +1 even
  // if it spans multiple deep-sleep commits.
  if (!sessionCountedThisOpen && totalSessionMsThisOpen >= 60000UL) {
    stats.sessionCount++;
    globalStats.totalSessions++;
    sessionCountedThisOpen = true;
  }

  // Reading time: no longer floor-gated. Every banked ms adds to the
  // lifetime totals (was previously gated at 10 s, which silently
  // discarded short reads — particularly bad for users who do
  // many <10s sessions, e.g. mid-session deep-sleep cycles).
  const uint32_t elapsedSecs = static_cast<uint32_t>(segmentMs / 1000UL);
  if (elapsedSecs > 0) {
    stats.totalReadingSeconds += elapsedSecs;
    globalStats.totalReadingSeconds += elapsedSecs;

    // v18.9.9.441 (CrossInk parity): fold segment into 730-day streak
    // bitfield + ToD/DoW buckets. localStart = now - elapsed. Safe
    // no-op when clock is invalid (returns false, span not recorded).
    // v18.9.9.443 also folds into per-book ToD/DoW and updates pace.
    ReadingStatsDateTime nowLocal;
    const bool clockValid = getCurrentLocalReadingStatsDateTime(nowLocal);
    ReadingStatsDateTime localStart = nowLocal;
    if (clockValid) {
      int32_t sodSigned = static_cast<int32_t>(localStart.hour) * 3600 +
                          static_cast<int32_t>(localStart.minute) * 60 +
                          static_cast<int32_t>(localStart.second) - static_cast<int32_t>(elapsedSecs);
      while (sodSigned < 0) {
        addDaysToReadingStatsDate(localStart.date, -1);
        sodSigned += 24 * 3600;
      }
      localStart.hour = static_cast<uint8_t>(sodSigned / 3600);
      localStart.minute = static_cast<uint8_t>((sodSigned % 3600) / 60);
      localStart.second = static_cast<uint8_t>(sodSigned % 60);
      globalStats.recordReadingSpan(localStart, elapsedSecs);

      // v18.9.9.443: auto-populate startDate on first-seen session,
      // unless user has manually set it.
      if (!stats.startDate.isValid() && !(stats.flags & BookReadingStats::FLAG_START_DATE_MANUAL)) {
        stats.startDate = nowLocal.date;
      }
    }
    // Per-book pace / bucket recorder (pace runs even without clock).
    stats.recordReadingSpan(clockValid ? localStart : ReadingStatsDateTime{}, elapsedSecs,
                            /*pagesTurnedForward=*/0);
  }

  stats.save(epub->getCachePath());
  globalStats.save();
}

void EpubReaderActivity::onBeforeDeepSleep() {
  // Same commit path as onExit, but the activity STAYS alive (just
  // gets put to sleep alongside the chip). When the device wakes,
  // session-resume continues from the saved progress.bin position
  // and a fresh session segment begins.
  commitReadingSession();
  // v18.9.9.471: also flush progress.bin unconditionally. Page-turn saves
  // are debounced (every 3rd render), so a deep-sleep entry with 1-2
  // unsaved page turns would otherwise reopen on the older page. Cheap
  // when it's a no-op re-save.
  if (epub && section) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
      pagesSinceProgressSave_ = 0;
      pendingSyncSaveError = false;
    }
  }
  // v18.9.9.202: Home may render next (wake-to-home); keep its Dashboard
  // chapter line fresh.
  writeChapterTitleSidecar();
}

void EpubReaderActivity::writeChapterTitleSidecar() {
  if (!epub) return;
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  const std::string title = tocIdx >= 0 ? epub->getTocItem(tocIdx).title : std::string();
  const std::string path = epub->getCachePath() + "/chapter_title.txt";
  FsFile f;
  if (!Storage.openFileForWrite("ERS", path, f)) return;
  // Cap defensively — Dashboard truncates to two lines anyway.
  f.write(reinterpret_cast<const uint8_t*>(title.c_str()), std::min<size_t>(title.size(), 120));
  f.close();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // v18.9.6c: BT tryEnableIfRequested has burned its retry budget without
  // fitting NimBLE into heap. If we entered Simple Rendering earlier this
  // session because of a page-load conflict, the section is still holding
  // the trimmed layout that BT can't recover behind -- fire a silent-restart
  // with EnableBt to reset both the section cache (fingerprint mismatch
  // will fall back to prebake) AND get a fresh heap for NimBLE. Matches
  // the manual "drawer BT quick-connect" recovery a user would try anyway.
  //
  // v18.9.9.33 (task #16): also fire when BT gave up outside Simple
  // Rendering -- the common case is post-rebuild fragmentation. After a
  // chapter cache miss the reader drops BLE, builds the section, then
  // asks BT back on. Free heap is fine (~65 KB) but maxAlloc is scattered
  // to ~30 KB while NimBLE controller_init needs ~40 KB contiguous, so
  // the 6 s refuse-loop bails and the user is left with a silently-off
  // BLE. Silent-restart-to-defrag-with-EnableBt lands on a clean ~90 KB
  // contiguous heap and reconnects the bonded remote automatically. Gated
  // on the same layoutDefragRetryAttempted_ one-shot so a genuinely
  // NimBLE-incompatible book doesn't loop. currentSpineIndex resumes the
  // user on the chapter they were reading.
  if (BluetoothHIDManager::getInstance().takeEnableGaveUpAlert()) {
    if (simpleRenderingActive_) {
      LOG_INF("ERS",
              "BT enable gave up after Simple Rendering was active; silent-restart-with-EnableBt "
              "to reset section cache and reclaim heap for NimBLE");
      silentRestartToReaderWithAction(ReaderPostBootAction::EnableBt);
      // never returns
    }
    if (!layoutDefragRetryAttempted_) {
      LOG_INF("ERS",
              "BT enable gave up (likely post-rebuild fragmentation); silent-restart-to-defrag "
              "with EnableBt to reclaim contiguous heap and reconnect bonded remote (spine=%d)",
              currentSpineIndex);
      layoutDefragRetryAttempted_ = true;
      silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction::EnableBt, currentSpineIndex);
      // never returns
    }
    // Budget spent -- give up quietly; the takeEnableGaveUpAlert has
    // already cleared, and the user can hit BT quick-connect manually
    // from the drawer once they notice.
  }

  // CrumBLE 4.5.6: BT-cycle recovery (atlas / page load) sits idle waiting
  // for the deferred BT disable to actually drain. render() won't re-fire
  // without requestUpdate(). Kick one the tick after BT flips off so the
  // retry render runs on the freed heap.
  if ((atlasRetryPendingBtDrop_ || pageLoadRetryPendingBtDrop_) &&
      !BluetoothHIDManager::getInstance().isEnabled()) {
    requestUpdate();
  }

  // v18.9.9.36 Phase C2: drive the incremental Section build one chunk
  // per tick. Runs entirely outside the RenderLock so input polling,
  // sleep timer, battery snapshots, BT drain all continue during the
  // multi-second parse. Cancel semantics: if the section pointer went
  // null (chapter-nav, book exit) or currentSpineIndex changed
  // (user tapped prev/next mid-build), we abandon the tmp build and let
  // the next render construct + build for the new spine.
  if (sectionBuildInProgress_) {
    if (!section) {
      LOG_INF("ERS", "Section build cancelled: section pointer released mid-build");
      sectionBuildInProgress_ = false;
      return;
    }
    if (currentSpineIndex != sectionBuildSpine_) {
      LOG_INF("ERS", "Section build cancelled: user navigated from spine=%d to %d mid-build",
              sectionBuildSpine_, currentSpineIndex);
      section->abandonBuild();
      section.reset();
      sectionBuildInProgress_ = false;
      requestUpdate();
      return;
    }
    // Animate the popup on wall-clock (250 ms cadence). Painted under a
    // brief RenderLock so we don't collide with the render task.
    const unsigned long nowMs = millis();
    if (sectionBuildPopupLastMs_ != 0 && (nowMs - sectionBuildPopupLastMs_) >= 250) {
      sectionBuildPopupLastMs_ = nowMs;
      sectionBuildPopupDotPhase_ = (sectionBuildPopupDotPhase_ + 1) % 4;
      static constexpr const char* kDots[4] = {"", ".", "..", "..."};
      char buf[64];
      // v18.9.9.76: show "Indexing… page X of ~Y" once the byte-ratio estimate
      // has enough signal to be meaningful (pageCount > 0 AND estimate > pageCount).
      // Early ticks with no estimate fall back to the classic animated-dots form.
      const uint16_t pc = section ? section->pageCount : 0;
      const uint16_t est = section ? section->estimatedTotalPages() : 0;
      if (SETTINGS.showIndexingPageCount && pc > 0 && est > pc) {
        snprintf(buf, sizeof(buf), "%s page %u of ~%u", tr(STR_INDEXING), pc, est);
      } else {
        snprintf(buf, sizeof(buf), "%s%s", tr(STR_INDEXING), kDots[sectionBuildPopupDotPhase_]);
      }
      RenderLock lock(*this);
      GUI.drawPopup(renderer, buf, sectionBuildPopupMinWidth_, /*leftAlignText=*/true);
    }
    // v18.9.9.262: pre-flight heap-floor gate ported from CrossPoint
    // feat-bluetooth 74a0969c. Layout allocations inside parseStep abort()
    // on OOM under -fno-exceptions, so a section build entered on tight
    // heap can wipe the panel with a hard restart. If free heap is below
    // the floor, drop BT (if resident) BEFORE starting the tick so the
    // next iteration runs with NimBLE's ~58 KB reclaimed. bleAutoReEnable
    // brings it back after the build completes.
    constexpr uint32_t kBuildMinFreeHeap = 40u * 1024u;
    if (ESP.getFreeHeap() < kBuildMinFreeHeap && BluetoothHIDManager::getInstance().isEnabled()) {
      LOG_INF("ERS",
              "buildSomeMore: pre-flight below floor (free=%u < %u) with BT resident; requesting BT drop",
              ESP.getFreeHeap(), (unsigned)kBuildMinFreeHeap);
      BluetoothHIDManager::getInstance().requestDisableLater();
      bleAutoReEnableAfterReindex = true;
      // Skip this tick; next tick after BT drain runs with reclaimed heap.
      return;
    }
    // Drive a small chunk of build work. kBuildPagesPerTick is small so
    // each loop tick stays well under the ~250 ms window BT input
    // polling needs -- individual parseStep is ~10-30 ms for pure-ASCII
    // pages, but jumps to seconds-per-glyph on CJK+SD-fallback pages.
    // v18.9.9.70: lend the framebuffer's ~40 KB for the build chunk. Restored
    // at end-of-tick so subsequent renders see a valid buffer.
    // v18.9.9.451: 4 -> 1 for the animation cadence win. Field feedback:
    // on CJK books using an SD fallback the 4-page tick meant the
    // "Indexing..." dots updated every 20-40 s, reading as a frozen
    // device. 1 page per tick lets the popup animation fire every
    // ~5-10 s worst case, giving the user a clear "still working"
    // signal without changing total wall-clock build time.
    constexpr int kBuildPagesPerTick = 1;
    FrameBufferBuildLoan buildLoan(renderer);
    buildLoan.release();
    const bool ok = section->buildSomeMore(kBuildPagesPerTick);
    if (!buildLoan.restore()) { ESP.restart(); }
    if (!ok) {
      sectionBuildLayoutAbortedForLowMemory_ = section->lastBuildLayoutAbortedForLowMemory();
      sectionBuildImagesWereSuppressed_ = section->lastBuildImagesWereSuppressed();
      sectionBuildBleWasDroppedForFail_ = bleAutoReEnableAfterReindex;
      sectionBuildInProgress_ = false;
      sectionBuildJustFailed_ = true;
      // Do NOT reset section here -- the render() failure block does
      // section.reset() itself; leave the mid-build object alive so
      // getters (already snapshotted above) stay valid until the tail.
      // Actually snapshotted above so we can safely reset.
      section.reset();
      requestUpdate();
      return;
    }
    if (section->isBuildComplete()) {
      const bool imagesWereSuppressed = section->lastBuildImagesWereSuppressed();
      LOG_DBG("ERS", "Cache build complete (C2): pages=%u free=%u maxAlloc=%u",
              section->pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      // Post-build bookkeeping that used to live inline after the
      // createSectionFile call in render().
      layoutBleRetryAttempted = false;
      if (bleAutoReEnableAfterReindex) {
        bleAutoReEnableAfterReindex = false;
        bleReEnableHeldForImagePage = true;
        LOG_INF("ERS", "Section build done; holding BLE re-enable until a clean image-free render");
      }
      // v18.9.9.38 (task #23): if the build succeeded but had to suppress
      // images on the way (heap couldn't fit the ~57 KB decode window
      // for the largest inline image), silent-restart-to-defrag once so
      // the rebuild lands on the clean boot heap and images fit. Same
      // one-shot gate as the build-failure defrag path; if the retry
      // build ALSO suppresses images, we've hit a real ceiling and the
      // pending alert below (BT_IMAGES_HIDDEN / LOW_MEMORY_IMAGES title+body)
      // tells the user honestly. Only fires when at least one image was
      // dropped -- clean builds don't burn the budget.
      if (imagesWereSuppressed && !layoutDefragRetryAttempted_) {
        LOG_INF("ERS",
                "Build completed with images suppressed; silent-restart-to-defrag at spine=%d "
                "so images fit on the retry (one-shot per book)",
                currentSpineIndex);
        layoutDefragRetryAttempted_ = true;
        // Preserve BT if it was up; None otherwise. bleAutoReEnableAfterReindex
        // was already cleared above so read the manager directly.
        const bool bleUp = BluetoothHIDManager::getInstance().isEnabled();
        silentRestartToReaderWithDefragRetryAtSpine(
            bleUp ? ReaderPostBootAction::EnableBt : ReaderPostBootAction::None,
            currentSpineIndex);
        // never returns
      }
      if (imagesWereSuppressed) {
        const bool bleConnected = BluetoothHIDManager::getInstance().isEnabled();
        const StrId titleId = bleConnected ? StrId::STR_BT_IMAGES_HIDDEN_TITLE : StrId::STR_LOW_MEMORY_IMAGES_TITLE;
        const StrId bodyId = bleConnected ? StrId::STR_BT_IMAGES_HIDDEN_BODY : StrId::STR_LOW_MEMORY_IMAGES_BODY;
        snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
                 I18n::getInstance().get(titleId));
        snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s",
                 I18n::getInstance().get(bodyId));
        APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      }
      sectionBuildInProgress_ = false;
      // Drop the section so the next render() reconstructs a fresh one
      // and loadSectionFile picks up the just-committed .bin.
      section.reset();
      requestUpdate();
      return;
    }
    // Still building; loop() will tick again on its natural cadence.
    return;
  }

  // CrumBLE Phase 1 fast-open: deferred init runs the first time loop()
  // ticks AFTER the first render lands. Pays the font-buffer pre-grow +
  // settings cache + .pxc manifest parse here (~30-50 ms) so they don't
  // delay tap-to-first-pixel. Idempotent guard: deferredOnEnterPending_
  // flips false on the first run so subsequent ticks skip.
  if (deferredOnEnterPending_ && firstRenderCompleted_) {
    deferredOnEnterPending_ = false;
    runDeferredOnEnter();
  }

  // CrumBLE 4.4 post-bisect: dispatch the post-boot action queued by the
  // previous boot's silentRestartToReaderWithAction(). Runs once after the
  // first render has landed AND deferred init has finished -- so the heap
  // is in its stable post-boot shape, not mid-fast-open.
  //   EnableBt   -> set pendingBleQuickConnect_ (existing BT-connect SM)
  //   OpenLookup -> re-fire the LOOKUP menu action on a fresh heap
  //   OpenHighlight -> re-fire ADD_HIGHLIGHT
  {
    static bool postBootActionDispatched = false;
    if (!postBootActionDispatched && firstRenderCompleted_ && !deferredOnEnterPending_) {
      postBootActionDispatched = true;
      const ReaderPostBootAction action = consumeReaderPostBootAction();
      if (action == ReaderPostBootAction::EnableBt) {
        LOG_INF("ERA", "post-boot dispatch: EnableBt -> pendingBleQuickConnect_ (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        pendingBleQuickConnect_ = true;
        // v18.1: mark this QC as coming from the defrag boot dispatch so the
        // pre-flight knows to trust the fresh heap and skip its check. QCs
        // from other origins (drawer, reader menu) get the normal heap check.
        pendingBleQuickConnectFromBootDispatch_ = true;
        pendingBleQuickConnectNoImages_ = false;
        pendingBleQuickConnectSettingsChanged_ = false;
        pendingBleQuickConnectPromptStage_ = -1;

        // v18.9.9.81: paint the "Connecting Bluetooth..." popup NOW and kick
        // off BleHid.begin() so NimBLE controller init runs during the panel-
        // refresh window that the popup HALF_REFRESH is about to trigger. The
        // QC handler on the next loop tick sees btMgr.isEnabled() == true and
        // skips straight to connectToDevice — user sees the popup ~2 s earlier
        // and BleHid.begin's ~40 ms is fully hidden behind the panel refresh.
        //
        // Also release the page heap reserve here (mirrors what the QC handler
        // would do at line ~2486) so NimBLE's controller_init has ~20 KB more
        // contiguous heap. Safe to release early: the reader's initial page
        // render is already complete or completing behind this popup.
        if (Section::pageHeapReserveHeld()) {
          const uint32_t freeBefore = ESP.getFreeHeap();
          Section::releasePageHeapReserveForBtEnable();
          LOG_INF("ERA", "Boot-dispatch early: released page heap reserve (free %u->%u)",
                  freeBefore, ESP.getFreeHeap());
        }
        // v18.9.9.138: mirror the caches that the pendingBleQuickConnect_
        // handler (line ~2469) drops before its BT enable. On boot dispatch
        // the first reader render has already run, so cachedRenderPage_ holds
        // ~30 KB of DOM that NimBLE's controller_init would otherwise fight
        // for contiguous heap. Field repro: NimBLE malloc failed silently
        // during enable, connect timed out, disable crashed on null-deref.
        // Dropping cachedRenderPage_ costs ~100 ms on the next post-BT
        // render (one-time re-deserialize) -- imperceptible next to the
        // 3 sec BT connect window. storedBwBuffer + readerSettingsCache_
        // are typically empty on boot dispatch (drawer never opened), so
        // those calls are safe no-ops when empty.
        const uint32_t freePreCacheFlush = ESP.getFreeHeap();
        cachedRenderPage_.reset();
        cachedRenderSection_ = nullptr;
        cachedRenderSpine_ = -1;
        cachedRenderPageIndex_ = -1;
        if (renderer.hasStoredBwBuffer()) {
          renderer.discardStoredBwBuffer();
        }
        readerSettingsCache_.clear();
        readerSettingsCache_.shrink_to_fit();
        if (ESP.getFreeHeap() > freePreCacheFlush + 1024) {
          LOG_INF("ERA", "Boot-dispatch early: flushed reader caches (free %u->%u)",
                  freePreCacheFlush, ESP.getFreeHeap());
        }
        {
          RenderLock lock(*this);
          GUI.drawPopup(renderer, tr(STR_BT_CONNECTING), 0, false, HalDisplay::HALF_REFRESH);
        }
        // v18.9.9.145: pre-BT reserve REMOVED. Field test showed NimBLE
        // truly needs the full ~73 KB budget for enable + connect. The
        // 5 KB reserve starved NimBLE at 68 KB -- enable succeeded but
        // connect timed out because late-stage GATT allocations failed.
        // Rely instead on v141's raised streamed-render floor (1600) +
        // the "drop BT for render, retry" path at line ~5410 when
        // post-BT renders can't fit.
        // BT enable is synchronous and takes ~40 ms; kick it off now so the
        // ~1.7 s panel refresh (triggered by the popup above) overlaps with
        // NimBLE init. Panel refresh is SPI-idle; CPU is free for NimBLE.
        auto& btMgrEarly = BluetoothHIDManager::getInstance();
        if (!btMgrEarly.isEnabled()) btMgrEarly.enable();
      } else if (action == ReaderPostBootAction::OpenLookup) {
        LOG_INF("ERA", "post-boot dispatch: OpenLookup (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::LOOKUP);
      } else if (action == ReaderPostBootAction::OpenHighlight) {
        LOG_INF("ERA", "post-boot dispatch: OpenHighlight (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::ADD_HIGHLIGHT);
      } else if (action == ReaderPostBootAction::OpenDefinition) {
        const char* word = consumePendingDefinitionWord();
        // v18.9.9.249: also consume a paired chunk-start offset. Non-zero
        // means the restart was armed by the chunked reader's boundary
        // refuse path -- open the definition at that chunk directly
        // instead of chunk 0.
        const uint32_t chunkStart = consumePendingDefinitionChunkStart();
        if (word && word[0] != '\0' && epub) {
          LOG_INF("ERA", "post-boot dispatch: OpenDefinition('%s') chunkStart=%u (free=%u maxAlloc=%u)",
                  word, chunkStart, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          // CrumBLE 4.4 post-bisect: thread the word into the LOOKUP flow
          // so the word-select activity opens with cursor on the word AND
          // auto-opens the definition overlay. Replaces the prior
          // DictionaryDefinitionActivity push path -- now the overlay is
          // the SINGLE rendering path for definitions, so post-boot dispatch
          // routes through LOOKUP regardless of where the user was when
          // the silent-restart fired.
          pendingLookupDefinitionWord_ = word;
          pendingLookupDefinitionChunkStart_ = chunkStart;
          onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::LOOKUP);
        } else {
          LOG_INF("ERA", "post-boot dispatch: OpenDefinition but no word queued; falling back to OpenLookup");
          onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::LOOKUP);
        }
      } else if (action == ReaderPostBootAction::OpenReadingStats) {
        LOG_INF("ERA", "post-boot dispatch: OpenReadingStats (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::READING_STATS);
      } else if (action == ReaderPostBootAction::OpenKoSync) {
        LOG_INF("ERA", "post-boot dispatch: OpenKoSync (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SYNC);
      } else if (action == ReaderPostBootAction::OpenReaderOptions) {
        LOG_INF("ERA", "post-boot dispatch: OpenReaderOptions (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::READER_OPTIONS);
      } else if (action == ReaderPostBootAction::OpenLookedUpWords) {
        LOG_INF("ERA", "post-boot dispatch: OpenLookedUpWords (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::LOOKED_UP_WORDS);
      } else if (action == ReaderPostBootAction::OpenLookupAtWord) {
        const char* word = consumePendingDefinitionWord();
        if (word && word[0] != '\0' && epub) {
          LOG_INF("ERA", "post-boot dispatch: OpenLookupAtWord('%s') (free=%u maxAlloc=%u)",
                  word, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          pendingLookupDefinitionWord_ = word;
          pendingLookupCursorOnly_ = true;
          onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::LOOKUP);
        } else {
          LOG_INF("ERA", "post-boot dispatch: OpenLookupAtWord but no word queued; falling back to OpenLookup");
          onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::LOOKUP);
        }
      }
      // v18.9.9.26: ReaderPostBootAction::OpenBookSettingsDrawer removed.
      // v18.9.9.25's silent-restart-on-refuse path was reverted because BT
      // can't cleanly reconnect while the drawer is on the activity stack.
      // Compat mode now hides settings groups from the drawer entirely
      // (see BookSettingsDrawerActivity::rebuildDrawerItems), so the tight-
      // heap refuse the silent-restart was meant to work around doesn't
      // trigger anymore.
      //
      // v18.9.9.55 (task #40): dispatch restored. The drawer's view-mode
      // (v55) fires this action when the user taps to edit while in
      // view-only mode; post-boot heap is clean (~90 KB), BT is cold,
      // and the drawer opens editable with the previously-expanded
      // group pre-selected via consumePendingDrawerExpandGroup().
      // The 4.5.6 BT-can't-reconnect concern doesn't apply here: BT is
      // still off post-boot, and requestEnableLater on drawer close
      // schedules the reconnect for after the drawer teardown finishes.
      else if (action == ReaderPostBootAction::OpenBookSettingsDrawer) {
        const int group = consumePendingDrawerExpandGroup();
        LOG_INF("ERA", "post-boot dispatch: OpenBookSettingsDrawer (group=%d free=%u maxAlloc=%u)", group,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        startActivityForResult(std::make_unique<BookSettingsDrawerActivity>(
                                   renderer, mappedInput, &readerSettingsCache_, &pxcManifest_, group),
                               [this](const ActivityResult&) { requestUpdate(); });
      }
    }
  }

  // A chapter layout aborted under BLE pressure and we requested a BLE disable.
  // Now that the main loop has actually torn the stack down (full heap), fire the
  // one retry build -- so it sees the freed heap instead of racing the deferred
  // disable on the render task.
  if (pendingLayoutRetryAfterBleOff && !BluetoothHIDManager::getInstance().isEnabled()) {
    pendingLayoutRetryAfterBleOff = false;
    requestUpdate();
    return;
  }

  // CrumBLE: prebake-cache mismatch prompt. Delegated to a helper so
  // render() can call the same check at the top of its body -- catching
  // the mismatch BEFORE any chapter parse runs (which is what was OOM-ing
  // in the "switched to margin=10 then hit cancel" trace: the rebuild had
  // already consumed enough heap that the post-cancel re-layout couldn't
  // complete). Tick() still calls it as a safety net.
  if (checkAndFirePrebakePromptIfNeeded()) return;

  // CrumBLE: prebake artifacts now arrive via the file manager's /upload
  // endpoint, file-by-file, dropped straight into the final cache layout
  // by the JS-side optimizer (or a curl loop). No reader-side extraction
  // step needed -- Section's dual-path read picks them up automatically.

  // BT No Images Quick Connect auto-restore. The no-images flag exists only to
  // keep the contiguous heap free for NimBLE's ~58 KB, so restore images the
  // moment Bluetooth stops holding that heap. Two distinct drop signals:
  //   (1) stack disabled  -- user toggled BT off from the menu, or text starved
  //       and the auto-drop disabled it: isEnabled() goes false.
  //   (2) link dropped     -- controller powered off / out of range: the stack
  //       stays enabled (auto-reconnect armed) but the client reports
  //       disconnected, so isConnected(bonded) goes false. This is the case that
  //       also raises "Bluetooth couldn't stay connected".
  // We latch btNoImgLinkSeen once the remote actually links so the brief
  // pre-link connect handshake isn't mistaken for a drop.
  if (renderer.suppressImages()) {
    auto& btMgr = BluetoothHIDManager::getInstance();
    const bool stackUp = btMgr.isEnabled();
    const bool linked = stackUp && SETTINGS.bleBondedDeviceAddr[0] != '\0' &&
                        btMgr.isConnected(SETTINGS.bleBondedDeviceAddr);
    if (linked) btNoImgLinkSeen = true;
    if (!stackUp || (btNoImgLinkSeen && !linked)) {
      LOG_INF("ERS", "BLE link gone; restoring images for BT no-images mode");
      renderer.setSuppressImages(false);
      btNoImgLinkSeen = false;
      requestUpdate();
      return;
    }
  }

  // CrumBLE: .pxc-manifest mismatch prompt on Bluetooth connect.
  //
  // When a remote actually links AND the book has a .pxc manifest AND the four
  // viewport-affecting fields (fontId, orientation, screenMargin, imageRendering)
  // don't match what the optimizer baked against, the user's images won't render
  // over the link (the device's renderFromCache rejects mismatched dims). Prompt
  // the user to switch to the baked layout. Wait ~3s after first observing the
  // link to dodge NimBLE's connect-handshake transient, and skip if we've
  // already prompted this link.
  {
    auto& btMgr = BluetoothHIDManager::getInstance();
    const bool stackUp = btMgr.isEnabled();
    const bool linkedNow = stackUp && SETTINGS.bleBondedDeviceAddr[0] != '\0' &&
                           btMgr.isConnected(SETTINGS.bleBondedDeviceAddr);
    constexpr unsigned long kManifestPromptStabilityMs = 3000;
    if (linkedNow && !btWasLinked_) {
      // Fresh link. Arm the prompt; we'll fire once the stability window passes.
      btManifestPromptEarliestMs_ = millis() + kManifestPromptStabilityMs;
      // v18.9.9.2: arm 20s of dense MEM instrumentation to catch the
      // post-connect throw seen in the field log (Free 6944 -> terminate
      // in 30ms after "Successfully connected"). Log at named steps of
      // loop()/render() so the sub-alloc that swallows the last ~6.8KB
      // is nameable in the next repro.
      postBtDiagUntilMs_ = millis() + 20000UL;
      LOG_INF("PBTD", "post-BT diag ARMED for 20s (link established, free=%u maxAlloc=%u)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    } else if (!linkedNow && btWasLinked_) {
      // Link dropped. Don't clear btManifestPromptAnsweredThisSession_ here --
      // if the user already answered, brief controller drops shouldn't re-prompt.
      btManifestPromptEarliestMs_ = 0UL;
    }
    btWasLinked_ = linkedNow;

    if (linkedNow && pxcManifest_.has_value() && !btManifestPromptAnsweredThisSession_ &&
        btManifestPromptEarliestMs_ != 0UL && millis() >= btManifestPromptEarliestMs_) {
      const PxcManifest& m = *pxcManifest_;
      const int32_t curFontId = SETTINGS.getReaderFontId();
      const bool mismatch = (m.fontId != curFontId) ||
                            (m.orientation != SETTINGS.orientation) ||
                            (m.screenMargin != SETTINGS.screenMargin) ||
                            (m.imageRendering != SETTINGS.imageRendering);
      btManifestPromptAnsweredThisSession_ = true;  // one-shot per book, regardless of branch below
      if (epub) writeBtManifestAnsweredSidecar(epub->getCachePath());  // v187
      if (mismatch) {
        LOG_INF("ERA",
                ".pxc manifest mismatch on BLE connect: cur fontId=%ld ori=%u marg=%u img=%u vs mfst fontId=%ld ori=%u marg=%u img=%u",
                static_cast<long>(curFontId), static_cast<unsigned>(SETTINGS.orientation),
                static_cast<unsigned>(SETTINGS.screenMargin), static_cast<unsigned>(SETTINGS.imageRendering),
                static_cast<long>(m.fontId), static_cast<unsigned>(m.orientation),
                static_cast<unsigned>(m.screenMargin), static_cast<unsigned>(m.imageRendering));
        // Field-by-field comparison body so the user sees exactly which
        // settings differ. Hardcoded English -- rare prompt, layman wording.
        const std::string promptBody = buildManifestComparisonBody(
            *pxcManifest_, readerSettingsCache_,
            "This book was prepared for clearer images over Bluetooth. Your current layout doesn't "
            "match. Switch to the prepared layout?");
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(
                renderer, mappedInput, "Use prepared layout?", promptBody,
                /*ignoreInitialConfirmRelease=*/true),
            [this](const ActivityResult& result) {
              if (result.isCancelled) {
                requestUpdate();
                return;
              }
              // Apply the manifest's viewport-affecting settings, save, and trigger
              // a re-layout. Font is the trickiest -- SETTINGS doesn't store fontId
              // directly; it derives it from fontFamily + fontSize. We can't fully
              // invert that here without the registry, so for now we only apply the
              // three raw settings and accept that fontId may still differ if the
              // bake used a font we can't currently recreate (e.g. an SD font that's
              // since been deleted). The renderFromCache fallback then re-decodes
              // the JPEG -- not free over BLE, but not a crash either. Reverse
              // fontId->family mapping is a follow-up.
              if (pxcManifest_.has_value()) {
                const PxcManifest& mm = *pxcManifest_;
                SETTINGS.orientation = mm.orientation;
                SETTINGS.screenMargin = mm.screenMargin;
                SETTINGS.imageRendering = mm.imageRendering;
                SETTINGS.saveToFile();
                // Force a fresh layout next render -- screen orientation may have
                // changed, so re-apply at the renderer level too.
                ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
                if (section) {
                  section.reset();  // drop cached section so render() rebuilds
                }
                requestUpdate();
              }
            });
        return;
      }
    }
  }

  // CrumBLE: drain the drawer's deferred BT Quick Connect. The drawer left
  // pending flags; we sequence the operations:
  //
  //   1. If the prompt hasn't been shown yet (stage == -1) AND there's a
  //      manifest mismatch (current SETTINGS differ from the .pxc bake),
  //      push the 3-option ChoicePromptActivity. Options:
  //        0 = Use my settings    -> stage=0
  //        1 = Use prepared        -> stage=1 (also reverts SETTINGS to manifest)
  //        Back (cancel)           -> clear all pending; no re-layout, no
  //                                   connect
  //      The prompt fires BEFORE any re-layout: the previous design
  //      indexed first then prompted, which (a) wasted the indexing if
  //      the user picked "Use prepared" and (b) confused the user about
  //      sequencing. Stage flag survives across loop ticks.
  //
  //   2. If no mismatch OR the prompt has been answered (stage >= 0):
  //        - If pendingBleQuickConnectSettingsChanged_, drop the section
  //          (triggers re-layout next render). Wait for it to rebuild.
  //        - Once section is non-null (or never needed dropping), connect.
  //
  //   3. Connecting: enable() + connectToDevice(). Latch the session flag so
  //      the edge-detect prompt doesn't fire a duplicate later.
  if (pendingBleQuickConnect_) {
    auto& btMgr = BluetoothHIDManager::getInstance();
    const bool mismatch = pxcManifest_.has_value() &&
                          ((pxcManifest_->fontId != SETTINGS.getReaderFontId()) ||
                           (pxcManifest_->orientation != SETTINGS.orientation) ||
                           (pxcManifest_->screenMargin != SETTINGS.screenMargin) ||
                           (pxcManifest_->imageRendering != SETTINGS.imageRendering));

    // Step 1: prompt if needed and not yet shown.
    if (mismatch && pendingBleQuickConnectPromptStage_ == -1) {
      const std::string promptBody = buildManifestComparisonBody(
          *pxcManifest_, readerSettingsCache_,
          "This book was prepared for clearer images over Bluetooth.");
      std::vector<std::string> options = {"Use my settings", "Use prepared"};
      startActivityForResult(
          std::make_unique<ChoicePromptActivity>(renderer, mappedInput, "Use prepared layout?", promptBody,
                                                 std::move(options),
                                                 /*ignoreInitialConfirmRelease=*/true),
          [this](const ActivityResult& result) {
            // Back/Cancel -> drop everything. No re-layout, no connect. The
            // user's earlier setting toggle stays in SETTINGS but the next
            // natural re-layout (page turn, chapter boundary) will apply it.
            if (result.isCancelled) {
              pendingBleQuickConnect_ = false;
              pendingBleQuickConnectNoImages_ = false;
              pendingBleQuickConnectSettingsChanged_ = false;
              pendingBleQuickConnectPromptStage_ = -1;
              requestUpdate();
              return;
            }
            const auto* cr = std::get_if<ChoicePromptResult>(&result.data);
            const int pick = cr ? cr->choice : 0;
            pendingBleQuickConnectPromptStage_ = pick;
            btManifestPromptAnsweredThisSession_ = true;  // suppress edge-detect prompt later
            if (epub) writeBtManifestAnsweredSidecar(epub->getCachePath());  // v187: survive silent-restart
            if (pick == 1 && pxcManifest_.has_value()) {
              // Use prepared: revert SETTINGS to manifest values. If those
              // already match the section's built layout (e.g. user toggled
              // away from prepared and is now reverting), the section drop
              // below may not be strictly needed -- but we always drop when
              // settingsChanged was true to keep the bookkeeping simple.
              const PxcManifest& mm = *pxcManifest_;
              SETTINGS.orientation = mm.orientation;
              SETTINGS.screenMargin = mm.screenMargin;
              SETTINGS.imageRendering = mm.imageRendering;
              // CrumBLE 4.2 fix: also restore font selection when the
              // manifest carries the new font fields. Older manifests left
              // these at default (fontId == 0), so we gate on a real fontId
              // to avoid clobbering the user's font with all-zero values
              // when an old manifest is loaded. Mirrors the prebake-restore
              // handler -- without restoring font + reloading the SD-font
              // system, getReaderFontId() returns the live (pre-restore)
              // fontId and every section fingerprint check fails on the
              // restored layout. See the prebake-prompt restore at line
              // ~786 for the longer-form comment.
              if (mm.fontId != 0) {
                SETTINGS.fontFamily = mm.fontFamily;
                SETTINGS.fontSize = mm.fontSize;
                SETTINGS.sdFontSizeRange = mm.sdFontSizeRange;
                strncpy(SETTINGS.sdFontFamilyName, mm.sdFontFamilyName.c_str(),
                        sizeof(SETTINGS.sdFontFamilyName) - 1);
                SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
              }
              SETTINGS.saveToFile();
              ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
              if (mm.fontId != 0) {
                // Reload the SD-font manager so getReaderFontId() recomputes
                // from the freshly-restored sdFontFamilyName / sdFontSizeRange
                // / fontSize triple. Without this the manager keeps whatever
                // family it loaded at boot and the per-section fingerprint
                // check would keep failing on the restored manifest's fontId.
                sdFontSystem.ensureLoaded(renderer);
              }
              // Force a re-layout: SETTINGS may now differ from what the
              // section was built with.
              pendingBleQuickConnectSettingsChanged_ = true;
            }
            requestUpdate();
          });
      return;
    }

    // Step 2: drop section if a re-layout is needed, then wait for it.
    if (pendingBleQuickConnectSettingsChanged_ && section) {
      RenderLock lock(*this);
      if (section) {
        cachedSpineIndex = currentSpineIndex;
        cachedChapterTotalPageCount = section->pageCount;
        nextPageNumber = section->currentPage;
      }
      section.reset();
      pendingBleQuickConnectSettingsChanged_ = false;
      requestUpdate();  // kick the render task to rebuild
      return;
    }
    // If section is still null (mid re-layout), wait. Loop will re-enter
    // next tick once the render task has built the new section.
    //
    // v18.9.9.64: also wait when the section is constructed but has no
    // pages yet (Phase C2 build in progress, or pending first-load).
    // Under a defrag-restart-with-EnableBt sequence, we boot on a fresh
    // ~85 KB heap; if BT enables NOW, NimBLE takes ~70 KB of that before
    // the section's cold build has run -- the build then attempts at 12 KB
    // maxAlloc and fails, defeating the purpose of the defrag hop. Waiting
    // for pageCount>0 keeps the fresh heap available for the build first.
    if (!section || sectionBuildInProgress_ || section->pageCount <= 0) {
      return;
    }

    // Step 3: section is ready. Connect.
    const bool noImages = pendingBleQuickConnectNoImages_;
    pendingBleQuickConnect_ = false;
    pendingBleQuickConnectNoImages_ = false;
    pendingBleQuickConnectPromptStage_ = -1;
    if (noImages) renderer.setSuppressImages(true);

    // CrumBLE 4.4 post-bisect: BT enable pre-flight. If the heap is degraded
    // (typical: user opened the book, browsed a bit, did lookup/highlight,
    // accumulated fragmentation), silent-restart first so NimBLE inits into
    // a clean ~115 KB heap and post-BT MaxAlloc lands ~12-15 KB instead of
    // ~6 KB. Skips if this enable is already the result of a prior silent
    // restart (avoid restart loop on the same boot's recovery dispatch).
    //
    // The continuation flag is CLEARED after this check runs, so a LATER
    // BT enable in the same session (e.g. after lookup/highlight dragged
    // heap down) can still trigger another silent restart.
    //
    // Threshold history: raised from 40 KB -> 55 KB after a field crash
    // where SD-card-font mode (plus dark mode, both holding extra resident
    // state) left the pre-BT MaxAlloc at ~43 KB. The pre-flight passed,
    // but NimBLE init + connect + 6 HID report-char subscriptions then
    // consumed ~66 KB of free heap, leaving MaxAlloc at 92 bytes and
    // killing the next page deserialize. SD-card-font path has a higher
    // resident baseline than built-in fonts (font registry buffers, atlas,
    // per-section subset), so the safe floor for the post-NimBLE residual
    // has to be correspondingly higher. 55 KB leaves ~10-15 KB MaxAlloc
    // post-connect even in the SD-font case; degraded sessions below that
    // just take the silent-restart path and reconnect from a fresh heap.
    {
      const uint32_t preBtFree = ESP.getFreeHeap();
      const uint32_t preBtMaxAlloc = ESP.getMaxAllocHeap();
      // v18.9.9.281: was 55 KB. The 55 KB floor assumed pre-v280 NimBLE
      // consumed ~66 KB of free heap + left MaxAlloc at ~10-15 KB. With
      // the v280 shrink (BT_CTRL_BLE_MAX_ACT 6->1, MSYS/ACL/EVT/PREP
      // pools all trimmed), post-NimBLE MaxAlloc is roughly +20 KB
      // better -- a 35 KB pre-BT MaxAlloc now leaves ~15 KB post-connect,
      // which was the safety target. Symptom of a too-high floor: the
      // reader silent-restart-loops here trying to reach a MaxAlloc it
      // will never see because the SD-font + settings baseline itself
      // sits below the old threshold.
      constexpr uint32_t kPreBtFreshMaxAllocThreshold = 35 * 1024;
      // v18.9.9.252: BLE controller memory was released this boot by the
      // v245 BT-off boot branch (or the FT-enter release). enable() would
      // load-fault inside NimBLEDevice::init on the freed controller
      // memory (see v251 crash class). Silent-restart-with-EnableBt so
      // the boot lands with bluetoothEnabled=1 and the v245 release
      // skipped, giving NimBLE a fresh controller to init.
      extern bool g_bleControllerMemReleased;
      if (g_bleControllerMemReleased) {
        LOG_INF("ERA",
                "BT enable requested but BLE controller mem was released this boot -- "
                "silent-restart-with-EnableBt (free=%u maxAlloc=%u)",
                preBtFree, preBtMaxAlloc);
        {
          RenderLock lock(*this);
          silentRestartToReaderWithAction(ReaderPostBootAction::EnableBt);
        }
        return;
      }
      // v18.9.9.163: BleHid port reverted. v48 skip-deinit path is live again --
      // if the previous disable() skipped NimBLE deinit (crash mitigation),
      // NimBLE stack state is stale in RAM. Force a silent-restart-with-EnableBt
      // so the boot lands on a clean NimBLE state.
      if (BluetoothHIDManager::getInstance().nimbleStateSkippedTeardown()) {
        LOG_INF("ERA",
                "BT enable requested but a prior disable() skipped NimBLE deinit -- silent-restart-with-EnableBt "
                "to re-initialise on clean stack (free=%u maxAlloc=%u)",
                preBtFree, preBtMaxAlloc);
        {
          RenderLock lock(*this);
          silentRestartToReaderWithAction(ReaderPostBootAction::EnableBt);
        }
        return;
      }
      // v18.1: only skip the pre-flight when the QC actually came from a
      // ReaderPostBootAction::EnableBt dispatch (defrag path just brought us
      // here with a fresh heap). Skipping for ANY silent-restart continuation
      // was too broad -- e.g. a cover-heap-guard restart-to-home followed by
      // the user opening a book and tapping BT connect was letting BT enable
      // proceed at ~24 KB maxAlloc, crashing after connect.
      if (isContinuingFromSilentReboot() && pendingBleQuickConnectFromBootDispatch_) {
        LOG_INF("ERA",
                "BT enable pre-flight: skipped (defrag boot dispatch; this enable is the result of "
                "a prior silent-restart-with-EnableBt, free=%u maxAlloc=%u)",
                preBtFree, preBtMaxAlloc);
        // v18.9.9.159: DO NOT pre-alloc failsafe here. This branch runs AFTER
        // BleHid.begin (NimBLE controller already grabbed ~70 KB) and BEFORE
        // BleHid.connect (needs another few KB for connection state). The
        // 4 KB failsafe alloc here starved connect -> BLE_INIT malloc failed
        // -> inconsistent NimBLE state -> disable() panic. Boot-dispatch keeps
        // slow pass 2 as trade-off; safer than crashing.
        // v18.9.5.1: DON'T clear pendingBleQuickConnectFromBootDispatch_ here.
        // The popup draw at line 1687 checks this flag to decide NO_REFRESH
        // vs HALF_REFRESH; clearing here made every boot-dispatch QC still
        // HALF-flash the popup a second time. Clear happens after the popup
        // draw instead.
        clearSilentRebootContinuationFlag();
      } else if (preBtMaxAlloc < kPreBtFreshMaxAllocThreshold) {
        // CrumBLE 4.5.7 v17: revert to v12's defrag path. The v15.3 detour
        // through bt-settings + returnToReaderAfterBtMagic added a second
        // silent-restart hop and made the reader re-enable BT while its
        // render loop was still running, which races NimBLE init and lets
        // it consume ~9 KB more than a lean-boot enable (76 KB vs 67 KB).
        // The reader-with-EnableBt path lets reader load state fully FIRST,
        // then a queued post-boot EnableBt fires against a settled render
        // loop -- historically the "BT and page turns work" flow.
        LOG_INF("ERA",
                "BT enable pre-flight: pre-BT heap is degraded "
                "(free=%u maxAlloc=%u < %u); triggering silent restart with EnableBt to defrag heap",
                preBtFree, preBtMaxAlloc, kPreBtFreshMaxAllocThreshold);
        // v18.9.4 opt#1: take RenderLock and paint the "Connecting..." popup
        // to the framebuffer BEFORE calling silent-restart. Inside the
        // silent-restart, snapshotFrameBufferForSilentRestart sees
        // heldByCurrentTask() and takes the fast path -- no 300 ms
        // tryLockFor timeout, no "skipping sleep-frame snapshot" bailout.
        // Result: the popup pixels survive the reboot via the sleep-frame,
        // so the user sees "Connecting Bluetooth..." continuously through
        // the defrag restart instead of a blank period + double flash.
        // v18.9.9.45 (task #30): don't composite a "Connecting..." popup
        // into the framebuffer before the silent-restart. The old design
        // captured the popup pixels in the snapshot so the boot-restore
        // repainted them ("continuous UI through the reboot"), but that
        // produces a visible cascade: boot restore paints the POPUP
        // (flash 1), reader loads and repaints the actual page erasing
        // the popup (flash 2), then the post-BT-connect refresh paints
        // the page again (flash 3). Three flashes total, reported in
        // field logs.
        //
        // Skipping the popup composite means the snapshot captures the
        // reader page as-is. Post-boot restore paints the same pixels
        // the user was already looking at -- no perceived flash. Only
        // the reader's post-BT-connect refresh remains, so the whole
        // silent-restart collapses to one visible transition.
        {
          RenderLock lock(*this);
          silentRestartToReaderWithAction(ReaderPostBootAction::EnableBt);
        }
        return;
      } else {
        LOG_INF("ERA",
                "BT enable pre-flight: heap acceptable (free=%u maxAlloc=%u); proceeding",
                preBtFree, preBtMaxAlloc);
        // v18.9.9.161: SDCF failsafe pre-alloc reverted. Even at 3400 B it
        // starved NimBLE during BT connect and triggered std::terminate
        // (v160 log: pre-BT maxAlloc=61428 healthy -> post-connect
        // free=136 maxAlloc=16). BT reliability wins over prewarm speed.
      }
    }
    // Persistent "Connecting Bluetooth..." popup spanning the blocking
    // NimBLE init + GATT handshake (~2-3 s total -- enable() initializes
    // the controller and host, connectToDevice() establishes the link and
    // subscribes to HID reports). Without this, the user sees the page
    // sit unchanged for several seconds with no feedback that QC is
    // actively working.
    //
    // RenderLock pattern (mirrors reindexCurrentSection): drawPopup paints
    // directly to the buffer + displayBuffer() pushes it. Holding the lock
    // across enable()/connectToDevice() blocks the render task from
    // repainting the page until the connect call returns -- which is
    // exactly what we want, since any concurrent repaint would race the
    // popup. requestUpdate() after the lock releases lets the next render
    // paint the page back on top once the connect is done.
    {
      RenderLock lock(*this);
      // CrumBLE 4.5.7 v17: HALF_REFRESH instead of FAST_REFRESH. On
      // freeink-sdk, FAST_REFRESH is a visible whole-panel flash (worse
      // than open-x4-sdk's old custom-LUT path). HALF_REFRESH is subtler
      // and completes within the ~2-3 s BT init window anyway, so the
      // user still sees the "Connecting Bluetooth..." popup without a
      // jarring flash on top of a page they were just reading.
      //
      // v18.9.9.132: skip the second HALF_REFRESH when boot-dispatch already
      // painted the popup at line ~1958.
      // v18.9.9.164: also skip when drawer-close painted at :4287. Prior fix
      // only covered the boot-dispatch flag, so the drawer-tap path still
      // double-flashed the same popup here.
      const HalDisplay::RefreshMode refresh =
          (pendingBleQuickConnectFromBootDispatch_ || bleConnectingPopupPainted_)
              ? HalDisplay::NO_REFRESH
              : HalDisplay::HALF_REFRESH;
      GUI.drawPopup(renderer, tr(STR_BT_CONNECTING), 0, false, refresh);
      pendingBleQuickConnectFromBootDispatch_ = false;
      bleConnectingPopupPainted_ = false;
      // CrumBLE Phase 1 fast-open: pre-grow the glyph buffer NOW, before
      // NimBLE eats heap. Cost (~20 ms) hides inside the Connecting
      // popup window we just drew.
      prewarmReaderTextBuffer(renderer);
      // CrumBLE 4.2: drop the cached page DOM right before BT enable.
      // The cache normally holds ~30 KB across renders so post-BT renders
      // skip the deserialize, but the cache ALSO keeps that 30 KB out of
      // BT's pre-flight budget. Without this release, NimBLE's enable()
      // refuses when free heap dips below 67.5 KB (observed: free=62 KB
      // with cache held, would be ~92 KB with cache dropped). Releasing
      // here lets BT pass its own pre-flight; the first post-connect
      // render takes a one-time cache miss (page reloads against the
      // post-BT heap, which is now stable rather than mid-fragmentation)
      // and subsequent renders are cache hits again.
      cachedRenderPage_.reset();
      cachedRenderSection_ = nullptr;
      cachedRenderSpine_ = -1;
      cachedRenderPageIndex_ = -1;
      // CrumBLE 4.5.7 v18.7: drop the stored BW compressed backup. This is
      // the drawer's onEnter snapshot (up to ~32 KB for dense pages, ~2-5
      // KB for text). Owned by the renderer, not the drawer -- so it
      // survives drawer close and stays pinned through BT enable if we
      // don't explicitly discard. Field observation: user opens drawer +
      // clicks BT Quick Connect -> ~20-30 KB of dead snapshot was
      // competing with NimBLE's ~75 KB budget. Discarding here saves that
      // for the reader's post-connect deserialization budget.
      if (renderer.hasStoredBwBuffer()) {
        LOG_INF("ERA", "BT connect: discarding stored BW buffer to free heap for NimBLE");
        renderer.discardStoredBwBuffer();
      }
      // Also release the reader-settings cache (~25 KB of SettingInfo
      // entries the drawer reads). It's rebuilt lazily the next time the
      // user opens the drawer; the rebuild is ~50-100 ms inline at drawer
      // open. Dropping it here turns out to be the difference between
      // "BT connects + page reload succeeds" and "BT connects + Page
      // Load error". Observed: post-BT heap stays at ~14 KB without this
      // release; freeing the settings cache brings it back to ~40 KB
      // which fits the 25 KB Section deserialize budget.
      const size_t prevSettingsCacheCount = readerSettingsCache_.size();
      readerSettingsCache_.clear();
      readerSettingsCache_.shrink_to_fit();
      if (prevSettingsCacheCount > 0) {
        LOG_INF("ERA", "BT connect: dropped reader-settings cache (%zu entries) to free heap for NimBLE",
                prevSettingsCacheCount);
      }
      // CrumBLE 4.3: release the 18 KB page-DOM heap reserve right
      // before NimBLE's enable() runs.
      if (Section::pageHeapReserveHeld()) {
        const uint32_t freeBefore = ESP.getFreeHeap();
        Section::releasePageHeapReserveForBtEnable();
        LOG_INF("ERA", "BT connect: released page heap reserve (free %u->%u)", freeBefore, ESP.getFreeHeap());
      }
      // CrumBLE 4.3 Option 1: KEEP the embedded glyph subset resident
      // through BT enable. The subset is ~5-7 KB; previously we dropped
      // it to give NimBLE more pre-flight headroom, but the post-BT
      // lazy reload would then fail under tight MaxAlloc and all glyphs
      // rendered as '?'. Keeping it resident costs ~5 KB during BT
      // (NimBLE has ~80 KB instead of ~85 KB) but the subset stays
      // available for post-BT glyph lookup -- pages render correctly
      // from the embedded subset's in-RAM glyphs without needing any
      // SD-font miniData. Trade-off: NimBLE's enable pre-flight has less
      // margin; chapter transitions still require BT disconnect because
      // installing the next chapter's subset (another 5 KB) won't fit
      // under post-NimBLE MaxAlloc.
      // v18.9.9.145: pre-BT reserve REMOVED. See boot-dispatch comment.
      if (!btMgr.isEnabled()) btMgr.enable();
      btMgr.connectToDevice(SETTINGS.bleBondedDeviceAddr);
    }
    btManifestPromptAnsweredThisSession_ = true;  // we handled the manifest decision
    if (epub) writeBtManifestAnsweredSidecar(epub->getCachePath());  // v187: survive silent-restart
    requestUpdate();
    return;
  }

  // #48: a render starved inside the BLE connect grace window and we suppressed
  // the half-drawn frame. Fire exactly one re-render once the grace window has
  // expired (the connect spike has settled by then) or BLE has dropped. If the
  // page now renders clean it paints normally; if it's still starved the
  // past-grace auto-drop in render() takes over. One-shot, never a tight loop.
  if (pendingGraceReRender) {
    const bool btOn = BluetoothHIDManager::getInstance().isEnabled();
    const bool pastGrace = btOn && (millis() - btEnabledAtMs) > kBtConnectGraceMs;
    if (!btOn || pastGrace) {
      pendingGraceReRender = false;
      requestUpdate();
      return;
    }
  }

  // Incremental session save. Without this, a brown-out / hard crash
  // mid-reading loses ALL accumulated time since onEnter (or the last
  // commit). With it, worst-case loss is kIncrementalSaveMs. The cost
  // is small: one SD write per minute of reading.
  constexpr unsigned long kIncrementalSaveMs = 60000UL;  // 1 min
  if (millis() - lastIncrementalSaveMs >= kIncrementalSaveMs) {
    commitReadingSession();
    lastIncrementalSaveMs = millis();
  }

  if (completionPromptQueued) {
    completionPromptQueued = false;
    completionPromptShown = true;
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT_TITLE),
                                               tr(STR_MARK_FINISHED_PROMPT_BODY)),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            setBookCompleted(true);
            showCompletedFeedback(true);
          }
          requestUpdate();
        });
    return;
  }

  if (pendingBookmarkFeedback) {
    const bool timedOut = (millis() - bookmarkFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingBookmarkFeedback = false;
      requestUpdate();
      return;
    }
  }

  if (pendingCompletedFeedback) {
    const bool timedOut = (millis() - completedFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingCompletedFeedback = false;
      requestUpdate();
      return;
    }
  }
  if (pendingTiltPageTurnFeedback) {
    const bool timedOut = (millis() - tiltPageTurnFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingTiltPageTurnFeedback = false;
      requestUpdate();
      return;
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so any exit path relocates the book into /Read/.
  // setBookCompleted() also arms this when the user marks a book finished before
  // the End-of-Book screen.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else if (!stats.isCompleted) {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Long-press Confirm: execute the configured reader action without opening the menu
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressMenuHandled) {
      longPressMenuHandled = false;
      return;
    }
    if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF &&
        mappedInput.getHeldTime() >= longPressMenuMs) {
      executeLongPressMenuAction();
      return;
    }
  }
  if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF && !longPressMenuHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= longPressMenuMs) {
    longPressMenuHandled = true;
    executeLongPressMenuAction();
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int currentPage = 0;
    int totalPages = 0;
    float bookProgress = 0.0f;
    uint16_t bmSpine = static_cast<uint16_t>(currentSpineIndex);
    float bmProgress = 0.0f;
    int bookmarkPageCount = 1;
    bool isBookCompleted = stats.isCompleted;
    {
      // Serialize EPUB metadata/file access with the render task.
      RenderLock lock(*this);
      currentPage = section ? section->currentPage + 1 : 0;
      totalPages = section ? section->pageCount : 0;
      bmSpine = static_cast<uint16_t>(currentSpineIndex);
      bmProgress =
          (section && section->pageCount > 0) ? static_cast<float>(section->currentPage) / section->pageCount : 0.0f;
      bookmarkPageCount = (section && section->pageCount > 0) ? section->pageCount : 1;
      isBookCompleted = stats.isCompleted;
      bookProgress = getCurrentBookProgressPercent();
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

    // CrumBLE: dictionary availability gates the LOOKUP / LOOKED_UP_WORDS
    // menu entries (port of SEEK reader's feature). Dictionary::exists()
    // checks for the StarDict .idx/.dict files at the SD root; lookup
    // history is per-book in the cache dir, so we only surface "Looked Up
    // Words" when there's actually anything to show.
    const bool hasDictionary = Dictionary::exists();
    const bool hasLookupHistory = hasDictionary && LookupHistory::hasHistory(epub->getCachePath());
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty(), !BOOKMARKS.getBookmarks().empty(),
                               BOOKMARKS.hasBookmarkForPage(bmSpine, bmProgress, bookmarkPageCount), isBookCompleted,
                               automaticPageTurnActive, getAutoPageTurnIntervalSeconds(),
                               hasDictionary, hasLookupHistory, pendingHighlightStart_.has_value()),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             if (menu.settingsChanged) {
                               sdFontSystem.ensureLoaded(renderer);
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                               }
                               section.reset();  // Force re-layout with changed reader settings
                             }
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                             // CrumBLE 4.4: post-menu heap pre-flight. If the menu allocations + a
                             // BT auto-reconnect that fires while the menu was up squeezed MaxAlloc
                             // below the page-deserialize floor (~8 KB), the very next render's
                             // TextBlock alloc fails with "maxAlloc=116 < needed=231" and the
                             // section-cache-clear retry path reinstalls the atlas, pushing the
                             // heap below 1 KB and triggering an abort(). Silent-restart preserves
                             // the open book + page progress via progress.bin and resumes on a
                             // fresh ~115 KB heap. Threshold mirrors the chapter-transition
                             // pre-flight that already exists for the same class of failure.
                             constexpr uint32_t POST_MENU_MIN_MAX_ALLOC = 8u * 1024u;
                             const uint32_t maxAlloc = ESP.getMaxAllocHeap();
                             if (maxAlloc < POST_MENU_MIN_MAX_ALLOC) {
                               LOG_INF("ERA",
                                       "Post-menu heap pre-flight: maxAlloc=%u below %u; silent-restart to reader with OpenLookup",
                                       maxAlloc, POST_MENU_MIN_MAX_ALLOC);
                               // v18.9.9.176: carry OpenLookup postAction so the book reopens
                               // in Lookup mode. Without this, silent-restart drops the user
                               // back into normal reading mode and they'd have to re-open the
                               // menu and click Lookup again -- confusing UX.
                               silentRestartToReaderWithAction(ReaderPostBootAction::OpenLookup);
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    // CrumBLE: if the user arrived at the current page via View Bookmarks,
    // route Back to re-open the bookmark list instead of exiting to Home.
    // Consume the flag so subsequent Backs go to Home as normal.
    if (returnToBookmarkListOnBack_) {
      returnToBookmarkListOnBack_ = false;
      startActivityForResult(
          std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInput, BOOKMARKS.getBookmarks()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& bm = std::get<BookmarkResult>(result.data);
              RenderLock lock(*this);
              currentSpineIndex = bm.spineIndex;
              pendingSpineProgress = bm.progress;
              pendingPercentJump = true;
              section.reset();
              returnToBookmarkListOnBack_ = true;  // re-arm for the next pick
            }
            requestUpdate();
          });
      return;
    }
    exitToHomeWithPopup();
    return;
  }

  // Side button long-press actions use raw Up/Down so the direction stays
  // physical regardless of the Prev/Next side layout setting.
  const bool sideLongPressChangesFont =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_FONT_SIZE;
  const bool sideLongPressChangesOrientation =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE;
  if (sideLongPressChangesFont || sideLongPressChangesOrientation) {
    const bool topReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool bottomReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (sideButtonLongPressHandled && (topReleased || bottomReleased)) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool topLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Up) || topReleased);
    const bool bottomLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Down) || bottomReleased);

    if (!sideButtonLongPressHandled && topLongPressed) {
      sideButtonLongPressHandled = !topReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/true)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false));
        requestUpdate();
      }
      return;
    }
    if (!sideButtonLongPressHandled && bottomLongPressed) {
      sideButtonLongPressHandled = !bottomReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/false)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true));
        requestUpdate();
      }
      return;
    }
  }

  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executeShortPowerButtonAction()) {
    return;
  }
  if (executeLongPowerButtonAction()) {
    return;
  }

  const bool frontLongPressAction = SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP ||
                                    SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE;
  if (frontLongPressAction) {
    const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (frontButtonLongPressHandled && (leftReleased || rightReleased)) {
      frontButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!frontButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      frontButtonLongPressHandled = true;
      if (SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP) {
        if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
          if (nextLongPressed) {
            exitToHomeWithPopup();
          } else {
            currentSpineIndex = epub->getSpineItemsCount() - 1;
            nextPageNumber = 0;
            pendingPageJump = std::numeric_limits<uint16_t>::max();
            requestUpdate();
          }
          return;
        }

        {
          RenderLock lock(*this);
          nextPageNumber = 0;
          currentSpineIndex = nextLongPressed ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
        requestUpdate();
        return;
      }

      const uint8_t newOrientation = nextLongPressed
                                         ? ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false)
                                         : ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true);
      applyOrientation(newOrientation);
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && consumeLongPowerButtonHold()) {
    nextTriggered = true;
    fromSideBtn = false;
    fromTilt = false;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      exitToHomeWithPopup();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const bool skipChapter =
      longPress &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP);

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && !fromSideBtn && SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  pageLoadRetryCount = 0;
  if (!epub) {
    return;
  }

  // BookMetadataCache uses a shared seek-based FsFile for spine metadata lookups.
  // Hold the render/file mutex for the full jump calculation so menu-driven jumps
  // cannot race render/status-bar reads of the same cache file.
  RenderLock lock(*this);

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              const int targetSpine = std::get<ChapterResult>(result.data).spineIndex;
              // v18.9.9.397: chapter jump ALWAYS silent-restarts to the target
              // spine. Prior heap gate (v360 / v365 at 55K/40K) tried to run
              // inline when heap looked healthy, but field logs showed cases
              // just above the threshold that still crashed (page deserialize
              // + streaming atlas install + glyph queue eat 40-70 KB peak
              // together, and a jump right after a heavy render starts from
              // whatever the reader left behind). Simpler + always-reliable:
              // silent-restart every time. Cost is ~2 s of boot; win is a
              // guaranteed fresh ~85 KB heap for the target section load. Works
              // identically with or without prebake -- the boot reopens the
              // book, which uses the prebake if present. RTC-preserved EPUB
              // path (v397 in main.cpp) means even a corrupt CPS post-restart
              // still lands us back in the book at the target spine.
              LOG_INF("ERS",
                      "Chapter jump: silent-restart-to-reader at spine=%d for fresh-heap section load",
                      targetSpine);
              const bool bleUp = BluetoothHIDManager::getInstance().isEnabled();
              silentRestartToReaderWithDefragRetryAtSpine(
                  bleUp ? ReaderPostBootAction::EnableBt : ReaderPostBootAction::None,
                  targetSpine);
              // never returns
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP: {
      // CrumBLE 4.2: Lookup is always reachable from the menu now (the
      // gate was removed from EpubReaderMenuActivity::buildMainMenuItems).
      // First check: do the StarDict files actually exist? If not, show
      // an info screen with install instructions instead of running the
      // full BT-teardown + heap-preflight gauntlet for a flow that's
      // guaranteed to fail at the dictionary read.
      if (!Dictionary::exists()) {
        startActivityForResult(
            std::make_unique<ChoicePromptActivity>(
                renderer, mappedInput, tr(STR_DICT_NOT_FOUND_TITLE), tr(STR_DICT_NOT_FOUND_BODY),
                std::vector<std::string>{tr(STR_OK_BUTTON)},
                /*ignoreInitialConfirmRelease=*/true),
            [this](const ActivityResult& /*result*/) { requestUpdate(); });
        break;
      }
      // CrumBLE: auto-disable BT for the duration of the dictionary flow.
      // NimBLE holds ~58 KB of stack state while connected, which leaves
      // heap too fragmented for DictionaryWordSelect::extractWords to
      // allocate its WordInfo vector -- previously bad_alloc'd into
      // __cxxabiv1::__terminate and rebooted. User explicitly opted in
      // to "auto-disable BT on Lookup, reconnect on exit": the BT remote
      // is unusable inside the dictionary screens anyway (the crash is
      // 100% reproducible with BT on), so dropping it for the duration
      // is the right trade. User navigates dictionary UI with device
      // buttons during; requestEnableLater on each exit path brings the
      // bonded remote back on their next button press post-Lookup.
      auto& btMgr = BluetoothHIDManager::getInstance();
      const bool bleWasOnForLookup = btMgr.isEnabled();
      // CrumBLE 4.4 post-bisect: pre-BT-disable heap gate. NimBLE's host-stop
      // sequence (ble_hs_stop -> terminate connection -> free internal pools)
      // transiently allocates ~5-10 KB during teardown. Under a fragmented
      // heap where MaxAlloc is below that, the allocation lands in corrupted
      // memory and heap_caps_free aborts with "free() target outside heap
      // areas". Observed in the field: Lookup triggered with MaxAlloc=5364
      // -> btMgr.disable() -> ble_hs_stop_terminate_timeout -> heap canary
      // burnt -> panic. Catch the case here BEFORE entering NimBLE teardown:
      // silent-restart with OpenLookup so the post-boot flow runs with a
      // cold ~115 KB heap and BT already off (so the disable inside the
      // restarted LOOKUP path is a safe no-op).
      // v18.9.9.74 Phase 4: was v51's "widened gate" that pre-emptively
      // silent-restarted whenever BT was on, because pre-port disable() was
      // a no-op for heap purposes. Under BleHid, disable() truly reclaims
      // ~50 KB, so the fall-through to btMgr.disable() below is real. Only
      // silent-restart in the residual case: BT is OFF but the heap is
      // already too fragmented to lookup safely. That's the actual crash
      // guard — the BT-on case is now handled by disable + check.
      constexpr uint32_t LOOKUP_PRE_DISABLE_MIN_MAX_ALLOC = 12000;
      if (!bleWasOnForLookup && ESP.getMaxAllocHeap() < LOOKUP_PRE_DISABLE_MIN_MAX_ALLOC &&
          !isContinuingFromSilentReboot()) {
        LOG_INF("ERS",
                "Lookup: BT off + maxAlloc=%u below %u; silent-restart with OpenLookup to defrag",
                ESP.getMaxAllocHeap(), LOOKUP_PRE_DISABLE_MIN_MAX_ALLOC);
        silentRestartToReaderWithAction(ReaderPostBootAction::OpenLookup);
        break;
      }
      if (bleWasOnForLookup) {
        LOG_INF("ERS", "Lookup: disabling BT to free heap for word build (re-enabling on exit)");
        btMgr.disable();
        LOG_INF("ERS", "Lookup: heap after BT disable: free=%u maxAlloc=%u",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }

      // Pre-flight heap check AFTER the BT teardown. Even with NimBLE gone,
      // a long session on a fragmented heap can still bottom out below
      // the WordInfo reserve. User report: second Lookup attempt in a
      // single session crashed -- BT cycle leaves the heap slightly more
      // fragmented than before. Bump threshold from 25 KB to 32 KB so
      // the alert catches that second-attempt case before extractWords
      // bad_allocs. Cost: a few more "low memory" alerts in edge cases,
      // each of which is correct (better than a reboot).
      // CrumBLE 4.5.5: matched the highlight floor (32K -> 16K) since
      // WordInfo::lookupText is gone and both modes now share the same
      // ~16 KB vector footprint. See HIGHLIGHT_MIN_MAX_ALLOC comment for
      // the breakdown.
      constexpr uint32_t LOOKUP_MIN_MAX_ALLOC = 16000;
      if (ESP.getMaxAllocHeap() < LOOKUP_MIN_MAX_ALLOC) {
        // CrumBLE 4.4 post-bisect: silent-restart with OpenLookup queued
        // instead of the "low memory" dead-end. Post-boot dispatch
        // re-launches the activity. Skip if already post-recovery.
        if (!isContinuingFromSilentReboot()) {
          LOG_INF("ERS",
                  "Lookup pre-flight: maxAlloc=%u below %u; triggering silent restart with OpenLookup to defrag heap",
                  ESP.getMaxAllocHeap(), LOOKUP_MIN_MAX_ALLOC);
          silentRestartToReaderWithAction(ReaderPostBootAction::OpenLookup);
          break;
        }
        LOG_INF("ERS", "Lookup pre-flight: maxAlloc=%u below %u even after silent restart, showing alert",
                ESP.getMaxAllocHeap(), LOOKUP_MIN_MAX_ALLOC);
        clearSilentRebootContinuationFlag();  // future ops can restart again if needed
        if (bleWasOnForLookup) btMgr.requestEnableLater();
        tryRecoverLowHeapForLookup();
        strncpy(APP_STATE.pendingAlertTitle, tr(STR_LOW_MEMORY_LOOKUP_TITLE),
                sizeof(APP_STATE.pendingAlertTitle) - 1);
        strncpy(APP_STATE.pendingAlertBody, tr(STR_LOW_MEMORY_LOOKUP_BODY),
                sizeof(APP_STATE.pendingAlertBody) - 1);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
        break;
      }
      // Heap is acceptable. Clear continuation flag so future ops
      // (highlight, BT-reconnect) can silent-restart if their heap is
      // degraded after this lookup runs.
      clearSilentRebootContinuationFlag();
      // Port of SEEK reader's dictionary lookup. Compute the orientation-
      // adjusted margins from the current page so the word-select overlay
      // can hit-test taps against the rendered glyphs; also peek the FIRST
      // word of the next section so the overlay knows when the user has
      // selected the last word on the current page (for cursor navigation
      // wraparound). See sumegig/seek-reader commit b3074a2 -- this case
      // mirrors that integration point but adapted to our RenderLock /
      // margin-computation conventions.
      std::unique_ptr<Page> pageForLookup;
      std::string nextPageFirstWord;
      int orientedMarginTop = 0;
      int orientedMarginLeft = 0;
      {
        RenderLock lock(*this);
        if (!section) {
          requestUpdate();
          break;
        }
        int orientedMarginRight;
        int orientedMarginBottom;
        renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                         &orientedMarginLeft);
        orientedMarginTop += SETTINGS.screenMargin;
        orientedMarginLeft += SETTINGS.screenMargin;
        orientedMarginRight += SETTINGS.screenMargin;
        const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
        if (automaticPageTurnActive &&
            (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
          orientedMarginBottom += std::max(
              SETTINGS.screenMargin,
              static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
        } else {
          orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
        }
        pageForLookup = section->loadPageFromSectionFile();
        if (section->currentPage < section->pageCount - 1) {
          const int savedPage = section->currentPage;
          section->currentPage = savedPage + 1;
          auto nextPage = section->loadPageFromSectionFile();
          section->currentPage = savedPage;
          if (nextPage) {
            for (const auto& element : nextPage->elements) {
              if (!element || element->getTag() != TAG_PageLine) continue;
              const auto& line = static_cast<const PageLine&>(*element);
              auto block = line.getBlock();
              if (!block) continue;
              const auto& words = block->getWords();
              if (!words.empty()) {
                nextPageFirstWord = words.front();
                break;
              }
            }
          }
        }
      }
      if (!pageForLookup) break;

      // CrumBLE: before launching word select, make sure the dictionary
      // index is ready. Three paths:
      //   (a) already in RAM -> go straight to word select.
      //   (b) cache file on SD exists -> load it inline (~50ms) then go.
      //   (c) no cache -> ask the user to consent to a one-time ~10s
      //       scan, then run DictionaryIndexBuildActivity, then go.
      // Without this gate the first-ever lookup would freeze
      // DictionaryDefinitionActivity for ~10s with only "Looking up..."
      // on screen, with no warning to the user.
      const int readerFontId = SETTINGS.getReaderFontId();
      const auto orientation = SETTINGS.orientation;
      const auto cachePath = epub->getCachePath();
      auto pageShared = std::make_shared<std::unique_ptr<Page>>(std::move(pageForLookup));
      // CrumBLE: every exit path from the dictionary flow must re-enable
      // BT if we disabled it on entry, so the bonded remote reconnects
      // on the user's next press. requestEnableLater() defers to the
      // main loop's drain (next tick), which is the safe place to bring
      // NimBLE back up.
      auto reEnableBleIfNeeded = [bleWasOnForLookup]() {
        if (bleWasOnForLookup) {
          BluetoothHIDManager::getInstance().requestEnableLater();
        }
      };
      auto launchWordSelect = [this, pageShared, readerFontId, orientedMarginLeft, orientedMarginTop, cachePath,
                               orientation, nextPageFirstWord, reEnableBleIfNeeded]() {
        auto activity = std::make_unique<DictionaryWordSelectActivity>(
            renderer, mappedInput, std::move(*pageShared), readerFontId,
            orientedMarginLeft, orientedMarginTop, cachePath, orientation,
            nextPageFirstWord);
        // CrumBLE 4.4 post-bisect: thread the word + cursor-only flag from
        // the post-boot dispatch into the activity.
        //   OpenDefinition  -> cursorOnly = false: navigate cursor + auto-open popup
        //   OpenLookupAtWord-> cursorOnly = true:  navigate cursor only (dismiss path)
        if (!pendingLookupDefinitionWord_.empty()) {
          activity->setPendingDefinitionWord(std::move(pendingLookupDefinitionWord_),
                                              /*openOverlay=*/!pendingLookupCursorOnly_);
          // v18.9.9.249: forward the chunk-start offset if the post-boot
          // dispatch queued one. Word-select uses it as the initial
          // chunkStart in performDefinitionLookup so the definition
          // opens on the chunk the user was trying to page into.
          if (pendingLookupDefinitionChunkStart_ != 0) {
            activity->setPendingDefinitionChunkStart(pendingLookupDefinitionChunkStart_);
          }
          pendingLookupDefinitionWord_.clear();
          pendingLookupCursorOnly_ = false;
          pendingLookupDefinitionChunkStart_ = 0;
        }
        startActivityForResult(
            std::move(activity),
            [this, reEnableBleIfNeeded](const ActivityResult& result) {
              reEnableBleIfNeeded();
              requestUpdate();
            });
      };

      // v18.9.9.259: multi-dict entry. Discover all available dicts
      // (walks /, /dict/, /dictionary/). Path:
      //   0 dicts  -> Dictionary::exists() falls through, word-select
      //                shows "no dictionary found" as before.
      //   1 dict   -> auto-select it. Same UX as pre-v259 single-dict.
      //   2+ dicts -> if last-picked matches one, use it silently. Else
      //                push ChoicePromptActivity with dict display
      //                names. On pick: setActive + continue. On cancel:
      //                cleanup + return without word-select.
      // After the dict is chosen, the existing three-path index gate
      // (in-RAM / cached / build-needed) runs against THAT dict's
      // per-dict .qidx cache.
      auto proceedAfterDictChosen = [this, launchWordSelect, reEnableBleIfNeeded]() {
        if (Dictionary::isIndexReady()) {
          launchWordSelect();
        } else if (Dictionary::loadCachedIndex()) {
          launchWordSelect();
        } else {
          startActivityForResult(
              std::make_unique<ChoicePromptActivity>(
                  renderer, mappedInput, tr(STR_DICT_INDEX_PROMPT_TITLE), tr(STR_DICT_INDEX_PROMPT_BODY),
                  std::vector<std::string>{tr(STR_DICT_INDEX_PROMPT_BUILD), tr(STR_DICT_INDEX_PROMPT_CANCEL)},
                  /*ignoreInitialConfirmRelease=*/true),
              [this, launchWordSelect, reEnableBleIfNeeded](const ActivityResult& promptResult) {
                int chosen = -1;
                if (const auto* cp = std::get_if<ChoicePromptResult>(&promptResult.data)) {
                  chosen = cp->choice;
                }
                if (promptResult.isCancelled || chosen != 0) {
                  reEnableBleIfNeeded();
                  requestUpdate();
                  return;
                }
                startActivityForResult(std::make_unique<DictionaryIndexBuildActivity>(renderer, mappedInput),
                                        [this, launchWordSelect, reEnableBleIfNeeded](const ActivityResult& buildResult) {
                                          if (buildResult.isCancelled) {
                                            reEnableBleIfNeeded();
                                            requestUpdate();
                                            return;
                                          }
                                          launchWordSelect();
                                        });
              });
        }
      };

      const auto dicts = Dictionary::discoverAll();
      // v18.9.9.295: post-silent-restart continuation path -- if the
      // user already picked a dict before the restart (Dictionary::getActive
      // returns something in the discovered set), skip the picker entirely
      // and reuse that choice. The silent-restart is a heap-recovery
      // hop the user didn't ask for, so re-prompting them for the same
      // dict they were literally just using is bad UX. Only applies to
      // the OpenLookupAtWord continuation (pendingLookupCursorOnly_ is
      // the reliable signal for "we came from silent-restart"); the
      // normal fresh-lookup path still shows the picker for multi-dict
      // users to switch between dicts.
      bool skipPickerReusingActive = false;
      if (pendingLookupCursorOnly_ && dicts.size() >= 2) {
        const auto active = Dictionary::getActive();
        for (const auto& d : dicts) {
          if (d.dictPath == active.dictPath) { skipPickerReusingActive = true; break; }
        }
      }
      if (dicts.size() <= 1 || skipPickerReusingActive) {
        // Zero or one dict: no picker needed. If one, setActive it so
        // the per-dict cache path is derived correctly. If skipping
        // via reuse-active, Dictionary::getActive is already valid so
        // no setActive call needed.
        if (dicts.size() == 1) Dictionary::setActive(dicts[0]);
        proceedAfterDictChosen();
      } else {
        // v18.9.9.268: ALWAYS show picker when 2+ dicts (previously we
        // silently reused the last-picked). User feedback: "how do I
        // switch dicts?" -- this is the answer.
        // v18.9.9.270: cursor defaults to the last-picked dict when
        // it still exists in the discovered set, so the "keep using
        // same dict" flow is one tap (Confirm on the pre-selected
        // row). Only ~1-3 rows so arrow-nav is minor when switching.
        {
          std::vector<std::string> choices;
          choices.reserve(dicts.size());
          for (const auto& d : dicts) choices.push_back(d.displayName);
          const auto active = Dictionary::getActive();
          int defaultIdx = 0;
          for (size_t i = 0; i < dicts.size(); ++i) {
            if (dicts[i].dictPath == active.dictPath) { defaultIdx = static_cast<int>(i); break; }
          }
          startActivityForResult(
              std::make_unique<ChoicePromptActivity>(
                  renderer, mappedInput, tr(STR_DICT_PICKER_TITLE), tr(STR_DICT_PICKER_BODY),
                  std::move(choices),
                  /*ignoreInitialConfirmRelease=*/true,
                  /*defaultSelectedIndex=*/defaultIdx),
              [this, dicts, proceedAfterDictChosen, reEnableBleIfNeeded](const ActivityResult& r) {
                int chosen = -1;
                if (const auto* cp = std::get_if<ChoicePromptResult>(&r.data)) {
                  chosen = cp->choice;
                }
                if (r.isCancelled || chosen < 0 || chosen >= static_cast<int>(dicts.size())) {
                  reEnableBleIfNeeded();
                  requestUpdate();
                  return;
                }
                Dictionary::setActive(dicts[chosen]);
                proceedAfterDictChosen();
              });
        }
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKED_UP_WORDS: {
      // CrumBLE: same BT auto-disable / re-enable wrapper as the LOOKUP
      // case. LookedUpWordsActivity is just a history list, but tapping
      // an entry drills into DictionaryDefinitionActivity which calls
      // Dictionary::lookup -- same heap pressure that crashes Lookup
      // with NimBLE active. Dropping BT on entry keeps the saved-words
      // flow safe; requestEnableLater on activity finish brings the
      // bonded remote back on the user's next press.
      auto& btMgr = BluetoothHIDManager::getInstance();
      const bool bleWasOnForHistory = btMgr.isEnabled();
      if (bleWasOnForHistory) {
        LOG_INF("ERS", "LookedUpWords: disabling BT (re-enabling on exit)");
        btMgr.disable();
        LOG_INF("ERS", "LookedUpWords: heap after BT disable: free=%u maxAlloc=%u",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      // Pre-flight heap check: tapping an entry will eventually call
      // Dictionary::lookup and (if the user opens word select later from
      // there) hit the same WordInfo allocation pressure as LOOKUP. Same
      // 32 KB threshold.
      //
      // v18.9.9.268: on low heap, silent-restart-to-reader instead of
      // showing a dead-end "low memory" alert. Post-boot dispatch
      // re-fires the LOOKED_UP_WORDS menu action on a fresh ~90 KB heap
      // so the user lands exactly where they were trying to go.
      // v18.9.9.270: uses OpenLookedUpWords (new enum) instead of
      // OpenLookup so post-boot lands on the History screen directly,
      // not on Word Select. Loop-safety: isContinuingFromSilentReboot
      // check mirrors the LOOKUP pre-flight pattern.
      constexpr uint32_t LOOKED_UP_MIN_MAX_ALLOC = 32000;
      if (ESP.getMaxAllocHeap() < LOOKED_UP_MIN_MAX_ALLOC) {
        if (!isContinuingFromSilentReboot()) {
          LOG_INF("ERS",
                  "LookedUpWords pre-flight: maxAlloc=%u < %u -- silent-restart-to-reader-with-OpenLookedUpWords",
                  ESP.getMaxAllocHeap(), LOOKED_UP_MIN_MAX_ALLOC);
          silentRestartToReaderWithAction(ReaderPostBootAction::OpenLookedUpWords);
          // never returns
        }
        LOG_INF("ERS", "LookedUpWords pre-flight: post-restart maxAlloc=%u still below floor -- alert",
                ESP.getMaxAllocHeap());
        if (bleWasOnForHistory) btMgr.requestEnableLater();
        strncpy(APP_STATE.pendingAlertTitle, tr(STR_LOW_MEMORY_LOOKUP_TITLE),
                sizeof(APP_STATE.pendingAlertTitle) - 1);
        strncpy(APP_STATE.pendingAlertBody, tr(STR_LOW_MEMORY_LOOKUP_BODY),
                sizeof(APP_STATE.pendingAlertBody) - 1);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
        break;
      }
      startActivityForResult(std::make_unique<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this, bleWasOnForHistory](const ActivityResult& result) {
                               if (bleWasOnForHistory) {
                                 BluetoothHIDManager::getInstance().requestEnableLater();
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ADD_HIGHLIGHT: {
      // CrumBLE: phase 2/7 of the highlight UX. Reuses the dictionary
      // word-select activity in HighlightRange mode. Same BT teardown +
      // heap pre-flight as LOOKUP because the WordInfo vector allocation
      // is the same; bad_alloc here would still reboot the device.
      auto& btMgrH = BluetoothHIDManager::getInstance();
      const bool bleWasOnForHighlight = btMgrH.isEnabled();
      // v18.9.9.74 Phase 4: mirrors the LOOKUP simplification. Under BleHid
      // disable() truly frees, so we don't need to pre-empt when BT is on —
      // the disable below actually gets us the heap back. Only silent-restart
      // when BT is already off and heap is still too fragmented for the
      // WordInfo vector build.
      constexpr uint32_t HIGHLIGHT_MIN_MAX_ALLOC = 16000;
      if (!bleWasOnForHighlight && ESP.getMaxAllocHeap() < HIGHLIGHT_MIN_MAX_ALLOC &&
          !isContinuingFromSilentReboot()) {
        LOG_INF("ERS",
                "AddHighlight pre-flight: BT off + maxAlloc=%u below %u; silent-restart with OpenHighlight",
                ESP.getMaxAllocHeap(), HIGHLIGHT_MIN_MAX_ALLOC);
        silentRestartToReaderWithAction(ReaderPostBootAction::OpenHighlight);
        break;
      }
      if (bleWasOnForHighlight) {
        LOG_INF("ERS", "AddHighlight: disabling BT (re-enabling on exit)");
        btMgrH.disable();
        LOG_INF("ERS", "AddHighlight: heap after BT disable: free=%u maxAlloc=%u",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      // Post-silent-restart safety net: if we DID restart and heap is
      // still critically fragmented (rare -- means the boot itself is
      // struggling), show the alert path instead of pushing WordSelect
      // and blowing up mid-alloc.
      if (ESP.getMaxAllocHeap() < HIGHLIGHT_MIN_MAX_ALLOC) {
        LOG_INF("ERS", "AddHighlight pre-flight: maxAlloc=%u below %u even after silent restart, showing alert",
                ESP.getMaxAllocHeap(), HIGHLIGHT_MIN_MAX_ALLOC);
        clearSilentRebootContinuationFlag();
        if (bleWasOnForHighlight) btMgrH.requestEnableLater();
        tryRecoverLowHeapForLookup();
        strncpy(APP_STATE.pendingAlertTitle, tr(STR_LOW_MEMORY_LOOKUP_TITLE),
                sizeof(APP_STATE.pendingAlertTitle) - 1);
        strncpy(APP_STATE.pendingAlertBody, tr(STR_LOW_MEMORY_LOOKUP_BODY),
                sizeof(APP_STATE.pendingAlertBody) - 1);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
        break;
      }
      // Heap acceptable; clear continuation so future ops can restart.
      clearSilentRebootContinuationFlag();

      // Same page/margin extraction as LOOKUP -- the activity needs the
      // current Page to render and hit-test taps. Behind RenderLock so
      // we don't race the render task's frame in progress.
      std::unique_ptr<Page> pageForHighlight;
      int orientedMarginTopH = 0;
      int orientedMarginLeftH = 0;
      {
        RenderLock lock(*this);
        if (!section) {
          requestUpdate();
          if (bleWasOnForHighlight) btMgrH.requestEnableLater();
          break;
        }
        int orientedMarginRightH = 0;
        int orientedMarginBottomH = 0;
        renderer.getOrientedViewableTRBL(&orientedMarginTopH, &orientedMarginRightH, &orientedMarginBottomH,
                                         &orientedMarginLeftH);
        orientedMarginTopH += SETTINGS.screenMargin;
        orientedMarginLeftH += SETTINGS.screenMargin;
        orientedMarginRightH += SETTINGS.screenMargin;
        const uint8_t statusBarHeightH = UITheme::getInstance().getStatusBarHeight();
        if (automaticPageTurnActive &&
            (statusBarHeightH == 0 || statusBarHeightH == UITheme::getInstance().getProgressBarHeight())) {
          orientedMarginBottomH += std::max(
              SETTINGS.screenMargin,
              static_cast<uint8_t>(statusBarHeightH + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
        } else {
          orientedMarginBottomH += std::max(SETTINGS.screenMargin, statusBarHeightH);
        }
        pageForHighlight = section->loadPageFromSectionFile();
      }
      if (!pageForHighlight) {
        if (bleWasOnForHighlight) btMgrH.requestEnableLater();
        break;
      }

      // Snapshot reader state so the result handler can save the highlight
      // (we move pageForHighlight into the new activity, so we read
      // currentPage / pageCount / spineIndex up front).
      const uint16_t hlSpine = static_cast<uint16_t>(currentSpineIndex);
      const float hlProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      const int hlPageCount = section->pageCount;
      std::string hlChapterTitle;
      {
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        if (tocIdx != -1) hlChapterTitle = epub->getTocItem(tocIdx).title;
      }

      startActivityForResult(
          std::make_unique<DictionaryWordSelectActivity>(
              renderer, mappedInput, std::move(pageForHighlight), SETTINGS.getReaderFontId(), orientedMarginLeftH,
              orientedMarginTopH, epub->getCachePath(), SETTINGS.orientation, std::string{},
              DictionaryWordSelectActivity::Mode::HighlightRange),
          [this, bleWasOnForHighlight, hlSpine, hlProgress, hlPageCount,
           hlChapterTitle](const ActivityResult& result) {
            if (bleWasOnForHighlight) BluetoothHIDManager::getInstance().requestEnableLater();
            if (result.isCancelled) {
              requestUpdate();
              return;
            }
            const auto* hr = std::get_if<HighlightRangeResult>(&result.data);
            if (!hr || hr->startWordIndex < 0) {
              requestUpdate();
              return;
            }
            // Anchor-only result (end == -1): user pressed Hold after
            // placing start. Stash for the FINISH_HIGHLIGHT path.
            if (hr->endWordIndex < 0) {
              PendingHighlightStart hold;
              hold.spineIndex = hlSpine;
              hold.progress = hlProgress;
              hold.pageCount = static_cast<uint16_t>(hlPageCount);
              hold.wordIndex = static_cast<uint16_t>(hr->startWordIndex);
              hold.startWordText = hr->previewText;  // single word's raw text
              hold.chapterTitle = hlChapterTitle;
              pendingHighlightStart_ = std::move(hold);
              LOG_INF("ERS", "Highlight anchor held at spine=%u progress=%.3f word=%d",
                      hlSpine, hlProgress, hr->startWordIndex);
              // Show a brief banner so the user knows the start was saved.
              GUI.drawPopup(renderer, tr(STR_HIGHLIGHT_HELD));
              delay(900);
              requestUpdate();
              return;
            }
            // Full range (same-page): save directly.
            const auto addResult = BOOKMARKS.addHighlight(
                hlSpine, hlProgress, static_cast<uint16_t>(hr->startWordIndex), hlSpine, hlProgress,
                static_cast<uint16_t>(hr->endWordIndex), hlPageCount,
                hlChapterTitle.empty() ? nullptr : hlChapterTitle.c_str(), hr->previewText.c_str());
            if (addResult == BookmarkStore::AddResult::Added) {
              bookmarkFeedbackType = BookmarkFeedbackType::Added;
            } else {
              bookmarkFeedbackType = BookmarkFeedbackType::LimitReached;
            }
            pendingBookmarkFeedback = true;
            bookmarkFeedbackShowTime = millis();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::CANCEL_HIGHLIGHT: {
      pendingHighlightStart_.reset();
      GUI.drawPopup(renderer, tr(STR_HIGHLIGHT_CANCELLED));
      delay(700);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::EXPORT_BOOKMARKS: {
      // CrumBLE phase 7: dump every bookmark/highlight for the current
      // book to /highlights/<sanitized basename>.txt, in book order
      // (by spineIndex + progress). Each entry has a small header
      // (chapter + position) plus the preview text on its own line.
      // Empty-preview entries (page-only bookmarks / migrated v3
      // records) still get a header line so the file is a complete
      // record of the user's marks.
      const auto& bms = BOOKMARKS.getBookmarks();
      if (bms.empty()) {
        requestUpdate();
        break;
      }
      Storage.mkdir("/highlights");

      // Derive output filename from the book path (basename, sans extension).
      std::string outName = epub->getPath();
      const auto lastSlash = outName.find_last_of('/');
      if (lastSlash != std::string::npos) outName = outName.substr(lastSlash + 1);
      const auto lastDot = outName.find_last_of('.');
      if (lastDot != std::string::npos && lastDot > 0) outName = outName.substr(0, lastDot);
      // Replace any chars that would confuse the FAT filesystem.
      for (auto& c : outName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
          c = '_';
        }
      }
      const std::string outPath = "/highlights/" + outName + ".txt";

      // Snapshot + sort by (spineIndex, progress) so the output reads in
      // book order regardless of the order the user created the marks.
      std::vector<Bookmark> sorted(bms.begin(), bms.end());
      std::sort(sorted.begin(), sorted.end(), [](const Bookmark& a, const Bookmark& b) {
        if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
        return a.progress < b.progress;
      });

      FsFile out;
      if (!Storage.openFileForWrite("ERS", outPath, out)) {
        LOG_ERR("ERS", "Highlight export: openFileForWrite failed for %s", outPath.c_str());
        GUI.drawPopup(renderer, tr(STR_EXPORT_FAILED));
        delay(900);
        requestUpdate();
        break;
      }

      auto writeStr = [&out](const char* s) {
        if (!s || !*s) return;
        out.write(reinterpret_cast<const uint8_t*>(s), strlen(s));
      };
      auto writeLine = [&writeStr](const char* s) {
        writeStr(s);
        writeStr("\n");
      };

      // File header: book title + author + count.
      writeStr("# ");
      writeLine(epub->getTitle().c_str());
      if (!epub->getAuthor().empty()) {
        writeStr("# by ");
        writeLine(epub->getAuthor().c_str());
      }
      char countBuf[64];
      snprintf(countBuf, sizeof(countBuf), "# %u highlight%s\n\n",
               static_cast<unsigned>(sorted.size()), sorted.size() == 1 ? "" : "s");
      writeStr(countBuf);

      for (const auto& bm : sorted) {
        const char* chap = (bm.chapterTitle[0] != '\0') ? bm.chapterTitle : "(unknown chapter)";
        char header[160];
        snprintf(header, sizeof(header), "[%s, %d%%]\n", chap,
                 static_cast<int>(std::lround(bm.progress * 100.0)));
        writeStr(header);
        if (!bm.preview.empty()) {
          writeLine(bm.preview.c_str());
        } else {
          writeLine("(page bookmark)");
        }
        writeStr("\n");
      }
      out.close();
      LOG_INF("ERS", "Highlight export: wrote %zu entries to %s", sorted.size(), outPath.c_str());

      // Build a short toast: "Exported to /highlights/<name>.txt"
      std::string toast = tr(STR_HIGHLIGHTS_EXPORTED);
      toast += outName + ".txt";
      GUI.drawPopup(renderer, toast.c_str());
      delay(1200);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FINISH_HIGHLIGHT: {
      if (!pendingHighlightStart_.has_value()) break;
      // Same BT teardown + heap pre-flight + page extraction as ADD_HIGHLIGHT.
      auto& btMgrF = BluetoothHIDManager::getInstance();
      const bool bleWasOnForFinish = btMgrF.isEnabled();
      if (bleWasOnForFinish) {
        LOG_INF("ERS", "FinishHighlight: disabling BT (re-enabling on exit)");
        btMgrF.disable();
        LOG_INF("ERS", "FinishHighlight: heap after BT disable: free=%u maxAlloc=%u",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      constexpr uint32_t FINISH_MIN_MAX_ALLOC = 32000;
      if (ESP.getMaxAllocHeap() < FINISH_MIN_MAX_ALLOC) {
        LOG_INF("ERS", "FinishHighlight pre-flight: maxAlloc=%u below %u, attempting recovery",
                ESP.getMaxAllocHeap(), FINISH_MIN_MAX_ALLOC);
        if (bleWasOnForFinish) btMgrF.requestEnableLater();
        // CrumBLE 4.2: passive heap recovery -- see tryRecoverLowHeapForLookup
        // for the rationale and matching LOOKUP / ADD_HIGHLIGHT sites.
        tryRecoverLowHeapForLookup();
        strncpy(APP_STATE.pendingAlertTitle, tr(STR_LOW_MEMORY_LOOKUP_TITLE),
                sizeof(APP_STATE.pendingAlertTitle) - 1);
        strncpy(APP_STATE.pendingAlertBody, tr(STR_LOW_MEMORY_LOOKUP_BODY),
                sizeof(APP_STATE.pendingAlertBody) - 1);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
        break;
      }

      std::unique_ptr<Page> pageForFinish;
      int orientedMarginTopF = 0;
      int orientedMarginLeftF = 0;
      {
        RenderLock lock(*this);
        if (!section) {
          requestUpdate();
          if (bleWasOnForFinish) btMgrF.requestEnableLater();
          break;
        }
        int orientedMarginRightF = 0;
        int orientedMarginBottomF = 0;
        renderer.getOrientedViewableTRBL(&orientedMarginTopF, &orientedMarginRightF, &orientedMarginBottomF,
                                         &orientedMarginLeftF);
        orientedMarginTopF += SETTINGS.screenMargin;
        orientedMarginLeftF += SETTINGS.screenMargin;
        orientedMarginRightF += SETTINGS.screenMargin;
        const uint8_t statusBarHeightF = UITheme::getInstance().getStatusBarHeight();
        if (automaticPageTurnActive &&
            (statusBarHeightF == 0 || statusBarHeightF == UITheme::getInstance().getProgressBarHeight())) {
          orientedMarginBottomF += std::max(
              SETTINGS.screenMargin,
              static_cast<uint8_t>(statusBarHeightF + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
        } else {
          orientedMarginBottomF += std::max(SETTINGS.screenMargin, statusBarHeightF);
        }
        pageForFinish = section->loadPageFromSectionFile();
      }
      if (!pageForFinish) {
        if (bleWasOnForFinish) btMgrF.requestEnableLater();
        break;
      }

      // Snapshot END-side reader state -- the END page is wherever the
      // user navigated to between the Hold and now. (May be same spine,
      // may be a later spine for cross-chapter.)
      const uint16_t endSpine = static_cast<uint16_t>(currentSpineIndex);
      const float endProgressVal = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);

      startActivityForResult(
          std::make_unique<DictionaryWordSelectActivity>(
              renderer, mappedInput, std::move(pageForFinish), SETTINGS.getReaderFontId(), orientedMarginLeftF,
              orientedMarginTopF, epub->getCachePath(), SETTINGS.orientation, std::string{},
              DictionaryWordSelectActivity::Mode::HighlightSingleWord),
          [this, bleWasOnForFinish, endSpine, endProgressVal](const ActivityResult& result) {
            if (bleWasOnForFinish) BluetoothHIDManager::getInstance().requestEnableLater();
            if (result.isCancelled || !pendingHighlightStart_.has_value()) {
              requestUpdate();
              return;
            }
            const auto* hr = std::get_if<HighlightRangeResult>(&result.data);
            if (!hr || hr->startWordIndex < 0) {
              requestUpdate();
              return;
            }
            const auto& start = *pendingHighlightStart_;
            // CrumBLE 4.2: walk the actual pages of the highlight so the
            // bookmark preview captures the WHOLE quote, not just the
            // start anchor's 14-word trailing snippet + the end page's
            // selection. Previously a multi-page highlight surfaced as
            // "<words 0..13 from start anchor> ... <selection on end
            // page>" -- everything in between (entire pages of body
            // text) was dropped, which made the QuoteViewer mostly
            // useless for the long passages it was added for.
            //
            // Same-chapter (start.spineIndex == endSpine) is handled
            // inline by reusing the open Section and iterating each page
            // from startPage..endPage. We take RenderLock so the render
            // task doesn't race us mutating section->currentPage. Each
            // page load is ~50 ms; for a typical 3-5 page highlight
            // that's a 150-250 ms blip while finishing the highlight --
            // user-initiated action, no surprise.
            //
            // Cross-chapter (endSpine > start.spineIndex) would require
            // loading the other Section objects too. That's a bigger
            // change; for now we fall back to the old approximation and
            // log so it's visible in the field.
            std::string preview;
            const bool sameChapter = (start.spineIndex == endSpine);
            if (sameChapter && section && start.pageCount > 0) {
              RenderLock lock(*this);
              const int totalPages = static_cast<int>(start.pageCount);
              auto pageFromProgress = [&](float pr) {
                const int idx = static_cast<int>(pr * totalPages);
                return std::clamp(idx, 0, totalPages - 1);
              };
              const int startPage = pageFromProgress(start.progress);
              const int endPage = pageFromProgress(endProgressVal);
              const int savedPage = section->currentPage;
              const size_t kPreviewBudget = BOOKMARK_PREVIEW_MAX - 4;
              preview.reserve(kPreviewBudget);
              bool overflowed = false;
              for (int p = startPage; p <= endPage && !overflowed; p++) {
                section->currentPage = p;
                auto pg = section->loadPageFromSectionFile();
                if (!pg) continue;
                const size_t firstWord = (p == startPage) ? static_cast<size_t>(start.wordIndex) : 0;
                const size_t lastWord =
                    (p == endPage) ? static_cast<size_t>(hr->endWordIndex) : std::numeric_limits<size_t>::max();
                size_t wordIdx = 0;
                for (const auto& element : pg->elements) {
                  if (!element || element->getTag() != TAG_PageLine) continue;
                  const auto& line = static_cast<const PageLine&>(*element);
                  auto block = line.getBlock();
                  if (!block) continue;
                  const auto& lineWords = block->getWords();
                  for (const auto& w : lineWords) {
                    if (wordIdx >= firstWord && wordIdx <= lastWord) {
                      if (!preview.empty()) preview += ' ';
                      // Skip empty word slots (continuation rows, hyphen
                      // halves) so the output reads cleanly.
                      if (!w.empty()) preview += w;
                      if (preview.size() >= kPreviewBudget) {
                        preview.resize(kPreviewBudget);
                        preview += "...";
                        overflowed = true;
                        break;
                      }
                    }
                    wordIdx++;
                  }
                  if (overflowed) break;
                }
              }
              section->currentPage = savedPage;
            }
            // Fallback: cross-chapter highlight, or the same-chapter walk
            // produced nothing (e.g. all words were empty slots). Surface
            // both anchors so the bookmark isn't a blank.
            if (preview.empty()) {
              if (!sameChapter) {
                LOG_INF("ERS", "Cross-chapter highlight: preview falls back to anchor concatenation");
              }
              preview = start.startWordText;
              preview += "... ";
              preview += hr->previewText;
              if (preview.size() > BOOKMARK_PREVIEW_MAX - 1) {
                preview.resize(BOOKMARK_PREVIEW_MAX - 4);
                preview += "...";
              }
            }
            const auto addResult = BOOKMARKS.addHighlight(
                start.spineIndex, start.progress, start.wordIndex, endSpine, endProgressVal,
                static_cast<uint16_t>(hr->endWordIndex), start.pageCount,
                start.chapterTitle.empty() ? nullptr : start.chapterTitle.c_str(), preview.c_str());
            if (addResult == BookmarkStore::AddResult::Added) {
              bookmarkFeedbackType = BookmarkFeedbackType::Added;
            } else {
              bookmarkFeedbackType = BookmarkFeedbackType::LimitReached;
            }
            pendingHighlightStart_.reset();
            pendingBookmarkFeedback = true;
            bookmarkFeedbackShowTime = millis();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      {
        // Serialize EPUB metadata/file access with the render task.
        RenderLock lock(*this);
        bookProgress = getCurrentBookProgressPercent();
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      exitToHomeWithPopup();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_STATS: {
      // CrumBLE 4.4 (ported from CrossInk v1.3.3): delete just this book's
      // stats.bin. No cache clear, no exit -- the book stays open and
      // reading position is preserved.
      bool ok = false;
      if (epub) {
        ok = BookReadingStats::remove(epub->getCachePath());
      }
      drawToast(renderer, ok ? tr(STR_BOOK_STATS_DELETED) : tr(STR_CACHE_DELETE_FAILED));
      delay(ok ? 1000 : 1500);
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      bool cacheDeleted = false;
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          cacheDeleted = epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
          if (cacheDeleted) {
            drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
          } else {
            drawToast(renderer, tr(STR_CACHE_DELETE_FAILED));
          }
        }
      }
      delay(cacheDeleted ? 1000 : 1500);
      exitToHomeWithPopup();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      // CrumBLE 4.4 post-bisect: pre-flight under BT. Reading Stats pushes
      // a new activity (BookStatsActivity) that allocates for chart/text
      // rendering; under BT (~58 KB held by NimBLE) the available MaxAlloc
      // can be too low to safely both teardown NimBLE *and* construct the
      // new activity. Mirror the LOOKUP pattern: if BT is up and MaxAlloc
      // is below the pre-disable safe threshold, silent-restart with
      // OpenReadingStats so the post-boot path runs on a fresh heap with
      // BT already off. Field-observed crash: heap_caps_free assert
      // ("free() target outside heap areas") during NimBLE teardown when
      // Stats was opened with MaxAlloc ~5 KB.
      auto& btMgr = BluetoothHIDManager::getInstance();
      constexpr uint32_t STATS_PRE_DISABLE_MIN_MAX_ALLOC = 12000;
      if (btMgr.isEnabled() && ESP.getMaxAllocHeap() < STATS_PRE_DISABLE_MIN_MAX_ALLOC &&
          !isContinuingFromSilentReboot()) {
        LOG_INF("ERS",
                "Reading Stats: pre-BT-disable maxAlloc=%u below %u; silent-restart with OpenReadingStats",
                ESP.getMaxAllocHeap(), STATS_PRE_DISABLE_MIN_MAX_ALLOC);
        silentRestartToReaderWithAction(ReaderPostBootAction::OpenReadingStats);
        break;
      }
      // Auto-disable BT for the duration of Reading Stats, mirroring the
      // Lookup convention. The Stats screen is buttons-only (BT remote
      // would just compete for input), and freeing ~58 KB of NimBLE state
      // keeps the activity push within heap budget. requestEnableLater()
      // brings BT back on the user's next button press post-Stats.
      const bool bleWasOnForStats = btMgr.isEnabled();
      if (bleWasOnForStats) {
        LOG_INF("ERS", "Reading Stats: disabling BT to free heap (re-enabling on exit)");
        btMgr.disable();
      }
      clearSilentRebootContinuationFlag();
      // Include elapsed time from the CURRENT (uncommitted) session
      // segment on top of what's been banked into stats. Previously
      // banked segments are already in `stats.totalReadingSeconds`
      // because commitReadingSession persists them incrementally —
      // adding `millis() - sessionStartMs` would double-count.
      BookReadingStats displayStats = stats;
      displayStats.totalReadingSeconds += static_cast<uint32_t>((millis() - sessionSegmentStartMs) / 1000UL);
      startActivityForResult(
          std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getPath(), epub->getTitle(),
                                              epub->getThumbBmpPath(), displayStats, globalStats),
          [this, bleWasOnForStats](const ActivityResult&) {
            if (bleWasOnForStats) {
              BluetoothHIDManager::getInstance().requestEnableLater();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_COMPLETED: {
      const bool markCompleted = !stats.isCompleted;
      setBookCompleted(markCompleted);
      showCompletedFeedback(markCompleted);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        // CrumBLE 4.4: KOReader Sync's TLS handshake needs ~55 KB of free
        // heap (cert chain + mbedTLS scratch). Mid-reading the heap is
        // typically 16-19 KB free / 13-19 KB maxAlloc -- nowhere near
        // enough, and the user just gets "Not enough memory for sync --
        // please retry" with no path to actually recover. Pre-flight:
        // if free heap is below the TLS floor AND we're not already in
        // a post-restart attempt, silent-restart with OpenKoSync so the
        // sync runs against the fresh ~115 KB post-boot heap (BT cold).
        //
        // CrumBLE 4.5.4: raised 60 KB -> 95 KB. Field report showed TLS
        // failing at 18 KB free even though the pre-flight had passed --
        // wifi.connect + mbedtls cert-chain load + initial scratch
        // allocations consume ~40 KB BETWEEN the pre-flight click and
        // the actual TLS handshake. 95 KB at pre-flight leaves ~55 KB
        // for TLS after that intermediate consumption, matching
        // MIN_HEAP_FOR_TLS in KOReaderSyncClient.cpp.
        constexpr uint32_t KOSYNC_TLS_HEAP_FLOOR = 95u * 1024u;
        if (ESP.getFreeHeap() < KOSYNC_TLS_HEAP_FLOOR && !isContinuingFromSilentReboot()) {
          LOG_INF("KOSync",
                  "SYNC pre-flight: free=%u below %u; silent-restart with OpenKoSync",
                  ESP.getFreeHeap(), KOSYNC_TLS_HEAP_FLOOR);
          silentRestartToReaderWithAction(ReaderPostBootAction::OpenKoSync);
          break;
        }
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        // Pre-compute local KO position and chapter name while Epub is still in RAM.
        CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
        if (paragraphIndex.has_value()) {
          localPos.paragraphIndex = *paragraphIndex;
          localPos.hasParagraphIndex = true;
        }
        KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // Release the heavy Section now. Keep Epub alive until onExit(), which still
        // needs it for stats/cache cleanup before the sync activity starts.
        LOG_DBG("KOSync", "Releasing section for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
        }
        LOG_DBG("KOSync", "Section released for sync (heap after: %u)", (unsigned)ESP.getFreeHeap());

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      if (!section || section->pageCount == 0) break;
      const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);

      if (BOOKMARKS.hasBookmarkForPage(spine, progress, section->pageCount)) {
        BOOKMARKS.removeBookmarkForPage(spine, progress, section->pageCount);
        bookmarkFeedbackType = BookmarkFeedbackType::Removed;
      } else {
        const char* chapterTitle = nullptr;
        std::string titleStr;
        const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
        if (tocIndex != -1) {
          titleStr = epub->getTocItem(tocIndex).title;
          chapterTitle = titleStr.c_str();
        }
        const auto addResult = BOOKMARKS.addBookmark(spine, progress, section->pageCount, chapterTitle);
        bookmarkFeedbackType = (addResult == BookmarkStore::AddResult::Added) ? BookmarkFeedbackType::Added
                                                                              : BookmarkFeedbackType::LimitReached;
      }
      pendingBookmarkFeedback = true;
      bookmarkFeedbackShowTime = millis();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInput, BOOKMARKS.getBookmarks()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& bm = std::get<BookmarkResult>(result.data);
              RenderLock lock(*this);
              currentSpineIndex = bm.spineIndex;
              pendingSpineProgress = bm.progress;
              pendingPercentJump = true;
              section.reset();
              // CrumBLE: arm the back-button shortcut so the next Back
              // returns to the bookmark list instead of exiting Home.
              returnToBookmarkListOnBack_ = true;
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS: {
      BOOKMARKS.clearAll();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
      openAutoPageTurnIntervalPicker();
      break;
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::CONTROLS_OPTIONS:
      break;
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS: {
      // v18.9.9.49 + v18.9.9.50: post-boot dispatch after
      // silent-restart-with-OpenReaderOptions lands here. The menu-side
      // handler is bypassed on the post-boot path so we open the ROA
      // directly. Fresh boot heap; no BT to consider. Minimal callback:
      // on close, mark settings dirty so the section rebuilds if any
      // layout-affecting toggle changed, and requestUpdate for the next
      // paint. If a user came to Reader Options intending to just view,
      // they hit Back with no settings edits -- rebuild is a cheap no-op
      // in that case because the section-fingerprint check will find
      // everything matches.
      startActivityForResult(std::make_unique<ReaderOptionsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               if (APP_STATE.compatModeChanged) {
                                 APP_STATE.compatModeChanged = false;
                               }
                               requestUpdate();
                             });
      break;
    }
  }
}

void EpubReaderActivity::reindexCurrentSection() {
  SETTINGS.saveToFile();
  sdFontSystem.ensureLoaded(renderer);
  {
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::openFileTransfer() {
  if (epub && section) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }

  activityManager.goToFileTransfer(epub ? epub->getPath() : std::string{});
}

void EpubReaderActivity::openAutoPageTurnIntervalPicker(const bool ignoreInitialConfirmRelease) {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "EpubReaderAutoPageTurnInterval", StrId::STR_AUTO_TURN_INTERVAL_SECONDS,
          StrId::STR_AUTO_TURN_STEP_HINT, getAutoPageTurnIntervalSeconds(), MIN_AUTO_PAGE_TURN_INTERVAL_S,
          MAX_AUTO_PAGE_TURN_INTERVAL_S, 1, 5, StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, ignoreInitialConfirmRelease),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          setAutoPageTurnIntervalSeconds(static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
        }
        requestUpdate();
      });
}

void EpubReaderActivity::executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action) {
  switch (action) {
    case CrossPointSettings::LONG_MENU_SLEEP:
      enterDeepSleep();
      break;
    case CrossPointSettings::LONG_MENU_CHANGE_FONT:
      SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % CrossPointSettings::FONT_FAMILY_COUNT;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS:
      SETTINGS.guideReadingEnabled = !SETTINGS.guideReadingEnabled;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_BIONIC:
      SETTINGS.bionicReadingEnabled = !SETTINGS.bionicReadingEnabled;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE);
      break;
    case CrossPointSettings::LONG_MENU_REFRESH_SCREEN:
      pagesUntilFullRefresh = 1;  // Forces HALF_REFRESH on next render
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_SYNC_PROGRESS:
      if (KOREADER_STORE.hasCredentials()) {
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SYNC);
      } else {
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { SETTINGS.saveToFile(); });
      }
      break;
    case CrossPointSettings::LONG_MENU_MARK_FINISHED: {
      const bool newCompleted = !stats.isCompleted;
      setBookCompleted(newCompleted);
      showCompletedFeedback(newCompleted);
    }
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_READING_STATS:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::READING_STATS);
      break;
    case CrossPointSettings::LONG_MENU_SCREENSHOT:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SCREENSHOT);
      break;
    case CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN:
      openAutoPageTurnIntervalPicker(/*ignoreInitialConfirmRelease=*/true);
      break;
    case CrossPointSettings::LONG_MENU_FILE_TRANSFER:
      openFileTransfer();
      break;
    case CrossPointSettings::LONG_MENU_BOOK_SETTINGS:
      startActivityForResult(std::make_unique<BookSettingsDrawerActivity>(renderer, mappedInput,
                                                                          &readerSettingsCache_, &pxcManifest_),
                             [this](const ActivityResult& result) {
                               // Drawer consumed the Confirm release that closed it, so the reader's
                               // own long-press-handled cleanup (line ~391) never fired. Clear the
                               // flag here so the user's next short-press Confirm opens the regular
                               // menu instead of being silently swallowed.
                               longPressMenuHandled = false;

                               // Re-layout policy:
                               //   - If BT QC was requested, defer section.reset() until after
                               //     the manifest-mismatch prompt resolves (loop drain). The
                               //     prompt fires BEFORE any re-layout so the user can choose
                               //     to use the prepared layout instead -- and "Use prepared"
                               //     may end up not needing a re-layout at all (if current
                               //     section was built from the same settings the manifest
                               //     captures).
                               //   - Otherwise (no BT request), re-layout immediately on
                               //     settingsChanged as before.
                               const auto* menu = std::get_if<MenuResult>(&result.data);
                               const bool bleQc = menu && menu->bleConnectRequested;
                               if (menu && menu->settingsChanged && !bleQc) {
                                 // BLE handling for the re-layout is centralized in render()'s
                                 // cache-miss path (search for "Cache miss with BLE up"). We
                                 // don't drop BLE here because (a) inline disable() can
                                 // deadlock against NimBLE's host task if the remote is
                                 // actively sending events, and (b) the drawer is just one of
                                 // many section.reset() call sites — chapter boundaries hit
                                 // the same problem. Centralizing in render() handles all of
                                 // them uniformly.
                                 RenderLock lock(*this);
                                 if (section) {
                                   cachedSpineIndex = currentSpineIndex;
                                   cachedChapterTotalPageCount = section->pageCount;
                                   nextPageNumber = section->currentPage;
                                 }
                                 section.reset();
                               }
                               // CrumBLE: drawer asked to connect via QC. Stash the request +
                               // the settings-changed flag so loop() can run the manifest
                               // mismatch prompt FIRST (before any indexing), then sequence
                               // section.reset() + re-layout + connect based on the user's
                               // pick. Doing all of this inline used to (a) race the
                               // re-layout against the NimBLE handshake and (b) show the
                               // mismatch prompt AFTER the indexing already finished,
                               // wasting that indexing if the user picked "Use prepared".
                               if (bleQc) {
                                 pendingBleQuickConnect_ = true;
                                 pendingBleQuickConnectNoImages_ = menu->bleConnectNoImages;
                                 pendingBleQuickConnectSettingsChanged_ = menu->settingsChanged;
                                 pendingBleQuickConnectPromptStage_ = -1;  // not yet shown
                                 // CrumBLE 4.5.5: draw "Connecting Bluetooth..." here, the moment
                                 // QC is dispatched, instead of waiting until Step 3 (the existing
                                 // popup at ~line 1605 only renders AFTER section-reset settles and
                                 // AFTER any heap-recovery silent-restart cycle). Without this early
                                 // draw the user sees 3-10 s of normal page refreshes between the
                                 // drawer-close and the first popup, and the QC tap reads as "did
                                 // anything happen?" The popup will get clobbered by the next render
                                 // pass (drawer pop animation, section transition, etc.) -- the
                                 // existing late popup re-asserts it once we reach Step 3 -- but
                                 // even the brief flash on close gives the click immediate feedback.
                                 //
                                 // v18.9.5.1: skip this popup when the imminent BT enable pre-flight
                                 // will trip the silent-restart defrag path -- the pre-flight's own
                                 // popup (NO_REFRESH) + sleep-frame restore already gives the user
                                 // continuous feedback across the reboot. Flashing HERE would add a
                                 // wasted HALF_REFRESH (~1.7 s) right before we blank the panel for
                                 // the reboot. Match the pre-flight's 55 KB maxAlloc floor so we
                                 // only skip when a restart is genuinely coming.
                                 constexpr uint32_t kSkipDrawerPopupMaxAllocFloor = 55u * 1024u;
                                 if (ESP.getMaxAllocHeap() >= kSkipDrawerPopupMaxAllocFloor) {
                                   RenderLock lock(*this);
                                   GUI.drawPopup(renderer, tr(STR_BT_CONNECTING), 0, false, HalDisplay::HALF_REFRESH);
                                   bleConnectingPopupPainted_ = true;
                                 } else {
                                   LOG_INF("ERA",
                                           "Skipping drawer-close QC popup: maxAlloc=%u below pre-flight "
                                           "floor -- silent-restart will carry the popup across reboot",
                                           ESP.getMaxAllocHeap());
                                 }
                               }

                               // If the drawer's Bluetooth entry asked us to launch BT settings
                               // (user had no bonded remote), do it now via the same mechanism the
                               // reader menu's BLUETOOTH action uses — exit-on-success so the user
                               // lands back in the book after pairing.
#ifndef SIMULATOR
                               if (menu && menu->requestBluetoothFlow) {
                                 startActivityForResult(
                                     std::make_unique<BluetoothSettingsActivity>(
                                         renderer, mappedInput, [] { activityManager.popActivity(); },
                                         /*exitOnSuccessfulConnect=*/true,
                                         /*disableOnExit=*/false),
                                     [this](const ActivityResult&) { requestUpdate(); });
                                 return;
                               }
#endif  // SIMULATOR: BLE pairing UI needs NimBLE; no-op in the native simulator.
                               // No explicit requestUpdate() — ActivityManager's Pop path will
                               // automatically requestUpdateAndWait(), and adding our own here
                               // would cause the reader page to render twice (the dark→light
                               // greyscale pass would fire twice in succession).
                             });
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN:
      if (halTiltSensor.isAvailable()) {
        SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == CrossPointSettings::TILT_OFF ? CrossPointSettings::TILT_NORMAL
                                                                                      : CrossPointSettings::TILT_OFF;
        SETTINGS.saveToFile();
        halTiltSensor.clearPendingEvents();
        showTiltPageTurnFeedback(SETTINGS.tiltPageTurn != CrossPointSettings::TILT_OFF);
        requestUpdate();
      }
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE:
      SETTINGS.readerDarkMode = SETTINGS.readerDarkMode ? 0 : 1;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_OFF:
    default:
      break;
  }
}

bool EpubReaderActivity::executeShortPowerButtonAction() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  switch (SETTINGS.shortPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_READING_STATS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SCREENSHOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    default:
      return false;
  }
}

bool EpubReaderActivity::consumeLongPowerButtonRelease() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) || !longPowerButtonHandled) {
    return false;
  }

  longPowerButtonHandled = false;
  return true;
}

bool EpubReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  longPowerButtonHandled = true;
  return true;
}

bool EpubReaderActivity::executeLongPowerButtonAction() {
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }

  switch (SETTINGS.longPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_READING_STATS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SCREENSHOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    default:
      return false;
  }
}

void EpubReaderActivity::executeLongPressMenuAction() {
  executeReaderQuickAction(static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction));
}

void EpubReaderActivity::setBookCompleted(bool isCompleted) {
  if (stats.isCompleted == isCompleted) {
    return;
  }

  stats.isCompleted = isCompleted;
  if (isCompleted) {
    completionPromptShown = true;
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
      pendingReadFolderMove = true;
    }
    // v18.9.9.443 (CrossInk parity): auto-populate finishDate on
    // completion unless user has manually set it. Only when clock valid.
    if (!(stats.flags & BookReadingStats::FLAG_FINISH_DATE_MANUAL)) {
      ReadingStatsDateTime nowLocal;
      if (getCurrentLocalReadingStatsDateTime(nowLocal)) {
        stats.finishDate = nowLocal.date;
      }
    }
  } else {
    pendingReadFolderMove = false;
  }
  if (isCompleted) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(epub->getCachePath());
  globalStats.save();
}

void EpubReaderActivity::showCompletedFeedback(bool isCompleted) {
  completedFeedbackIsFinished = isCompleted;
  pendingCompletedFeedback = true;
  completedFeedbackShowTime = millis();
}

void EpubReaderActivity::showTiltPageTurnFeedback(bool enabled) {
  tiltPageTurnFeedbackEnabled = enabled;
  pendingTiltPageTurnFeedback = true;
  tiltPageTurnFeedbackShowTime = millis();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  const auto targetOrientation = ReaderUtils::toRendererOrientation(orientation);
  const bool settingsChanged = SETTINGS.orientation != orientation;
  const bool rendererChanged = renderer.getOrientation() != targetOrientation;

  // No-op only when both the persisted setting and the live renderer already match.
  if (!settingsChanged && !rendererChanged) {
    return;
  }

  {
    RenderLock lock(*this);

    // Preserve current reading position only when we need a live re-layout.
    if (rendererChanged && section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    if (settingsChanged) {
      // Persist the selection so the reader keeps the new orientation on next launch.
      SETTINGS.orientation = orientation;
      SETTINGS.saveToFile();
    }

    if (rendererChanged) {
      // Update renderer orientation to match the new logical coordinate system.
      renderer.setOrientation(targetOrientation);

      // Reset section to force re-layout in the new orientation.
      section.reset();
    }
  }
}

uint16_t EpubReaderActivity::getAutoPageTurnIntervalSeconds() const {
  const uint16_t seconds = static_cast<uint16_t>(pageTurnDuration / 1000UL);
  if (seconds == 0) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }
  return clampAutoPageTurnIntervalSeconds(seconds);
}

void EpubReaderActivity::setAutoPageTurnIntervalSeconds(uint16_t seconds) {
  if (seconds == 0) {
    automaticPageTurnActive = false;
    return;
  }

  seconds = clampAutoPageTurnIntervalSeconds(seconds);
  lastPageTurnTime = millis();
  pageTurnDuration = static_cast<unsigned long>(seconds) * 1000UL;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  pageLoadRetryCount = 0;
  // v18.9.9.290: count every forward AND backward turn towards the day's
  // page total. Overcounts a bit for user backtracks but stays intuitive
  // ("total pages read today" includes re-reads).
  ReadingStats::notePageTurn();
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // End-of-book detection: forward press on the last page of the
      // last spine. Used to advance into a stub "End of book" screen
      // that the user then had to back-button out of. Per upstream
      // PR #1425 / aalu d29b8ee2: just go home so finishing a book
      // closes it cleanly.
      if (currentSpineIndex >= epub->getSpineItemsCount() - 1) {
        exitToHomeWithPopup();
        return;
      }
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  stats.totalPagesTurned++;
  globalStats.totalPagesTurned++;
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  POST_BT_STEP("render enter");
  if (!epub) {
    return;
  }

  // v18.9.9.266 emergency BLE shed REVERTED in v18.9.9.269. Upstream's
  // 613371b7 shed calls bleinput::stop() which fully tears down NimBLE
  // and reclaims ~50 KB. CrumBLE's BluetoothHIDManager::disable() is
  // deliberately skip-deinit (v18.9.9.48 crash mitigation) -- it drops
  // the BT link but leaves NimBLE state in memory until the next
  // silent-restart. The shed therefore FAILED to recover heap in field
  // testing: BT link dropped 2 ms after connect, heap stayed stuck at
  // ~7 KB for 40 s, subsequent alloc failed -> terminate/crash. Without
  // a clean NimBLE deinit path the shed is counterproductive here.
  // Revisit if CrumBLE ever ports a safe deinit sequence.
  // v18.9.9.145: reserve release removed -- the reserve was starving
  // NimBLE. See boot-dispatch comment for full explanation.

  // CrumBLE: settings-drift check at the START of render -- before any
  // section.load / chapter parse runs. Tick()'s call to the same helper
  // already had a chance to fire; we double-check here because some entry
  // paths into render (post-settings-drawer-close in particular) can run
  // before tick() has had a chance to evaluate. If it fires, the prompt
  // gets pushed on top of the reader and we bail; the next render() call
  // (after the prompt closes) sees the reverted or accepted settings.
  if (checkAndFirePrebakePromptIfNeeded()) {
    return;
  }

  // v18.9.9.36 Phase C2 + v18.9.9.39 Phase C3: if loop() is driving an
  // incremental Section build, the section pointer is non-null but
  // holds a mid-build state. C3 lets render() proceed AS SOON AS the
  // target page (nextPageNumber) has been laid out -- Section's
  // loadPageFromSectionFile detects build_ and reads the page from the
  // tmp file via the in-memory LUT. Only paint the indexing popup when
  // the target page hasn't landed yet, or when we're briefly between
  // ticks (section null). Skipping the loadSectionFile / build-init
  // block below still applies (section is non-null so `if (!section)`
  // is false).
  if (sectionBuildInProgress_) {
    const int c3Target = nextPageNumber >= 0 ? nextPageNumber : 0;
    if (!section || c3Target >= section->pageCount) {
      static constexpr const char* kDots[4] = {"", ".", "..", "..."};
      char buf[64];
      // v18.9.9.76: same page-of-estimate treatment as the loop() animation path.
      const uint16_t pc = section ? section->pageCount : 0;
      const uint16_t est = section ? section->estimatedTotalPages() : 0;
      if (SETTINGS.showIndexingPageCount && pc > 0 && est > pc) {
        snprintf(buf, sizeof(buf), "%s page %u of ~%u", tr(STR_INDEXING), pc, est);
      } else {
        snprintf(buf, sizeof(buf), "%s%s", tr(STR_INDEXING), kDots[sectionBuildPopupDotPhase_ % 4]);
      }
      GUI.drawPopup(renderer, buf, sectionBuildPopupMinWidth_, /*leftAlignText=*/true);
      return;
    }
    // else: fall through -- read the just-laid-out page from the tmp file.
  }

  const auto showPendingSyncSaveError = [this]() {
    // v18.9.9.471: popup suppressed. Transient FAT-write hiccups happened
    // every page turn on affected users and drowned out the actual reading
    // experience with an unactionable toast. onExit and onBeforeDeepSleep
    // save unconditionally so at most 2 pages of drift can be lost even
    // on hard power-off — persistent SD failures still get diagnosed via
    // serial LOG_ERR from EpubReaderUtils::saveProgress. Kept the flag
    // reset so retry loops upstream still terminate.
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
  };

  const auto showLowMemoryLayoutError = [this]() {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_EPUB_LAYOUT_MEMORY_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(true, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    GUI.drawPopup(renderer, tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // Minimum padding between last line of text and the status bar
  static constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin +
                                      STATUS_BAR_TEXT_PADDING));
  } else {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin, static_cast<uint8_t>(statusBarHeight + STATUS_BAR_TEXT_PADDING));
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  // v18.9.9.439: X3-vs-X4 layout-overflow diagnostic. User reported same
  // book/prebake renders clean on X4 but shows overlapping Chinese text on
  // X3, with thousands of "[GFX] !! Outside range (528+, ...)" errors on
  // X3 (x=528 == X3's rotated logical panel width). This log dumps the
  // per-render geometry once per section load so X3 vs X4 numbers can be
  // compared side by side to nail whether the delta is in screenWidth,
  // margin-computation, or the SETTINGS.screenMargin add-on. One-shot per
  // section (guarded by !section below) so we don't flood the log.
  if (!section) {
    LOG_INF("ERS",
            "GEOM-DIAG device=%s panel=%ux%u screen=%ux%u marginLTRB=%d,%d,%d,%d "
            "screenMargin=%u viewport=%ux%u",
            gpio.deviceIsX3() ? "X3" : "X4",
            static_cast<unsigned>(renderer.getDisplayWidth()),
            static_cast<unsigned>(renderer.getDisplayHeight()),
            static_cast<unsigned>(renderer.getScreenWidth()),
            static_cast<unsigned>(renderer.getScreenHeight()),
            orientedMarginLeft, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
            static_cast<unsigned>(SETTINGS.screenMargin),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight));
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d (free=%u, maxAlloc=%u)", filepath.c_str(), currentSpineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    // v18.9.9.27: resync simpleRenderingActive_ from the sidecar so a mid-
    // session toggle in ReaderOptionsActivity takes effect on the next load.
    // v18.9.9.28: dropped the BT-on gate that was here originally. Auto-
    // escalation still works (Layer 2 OOM writes the sidecar, next rebuild
    // sees it), and the user-facing toggle now means "compat: on/off" the
    // way the user expects, not "compat when BT is on." Sidecar is the sole
    // source of truth for user intent.
    const bool sidecarNow = simpleRenderingSidecarSet(epub->getCachePath(), readerActivePath_);
    simpleRenderingActive_ = sidecarNow;
    APP_STATE.readerCompatModeActive = simpleRenderingActive_;

    // v18.9.9.18: reverted v18.9.9.17's "compat mode serves from prebake"
    // change. The runtime table/image skip on the streamed render path only
    // skips *drawing* -- the PageTableFragment still fully deserializes,
    // which allocates every cell TextBlock in one peak. Prebake bytes have
    // full-fat fragments (unlike a compat-mode cold rebuild which collapses
    // tables to paragraphs) so the deserialize peak blows the post-BT
    // maxAlloc ceiling on the first page turn under BT. Back to the safe
    // cold-rebuild path -- the v18.9.9.16 rename retry catches transient
    // SD glitches during that rebuild.
    //
    // Original v18.9.6a rationale for the gate: prebake artifact was built
    // with the user's real settings (tables + images + style) and re-loading
    // it would crash us again at PageTableFragment::deserialize -- still
    // true today because runtime-skip doesn't unwind the deserialize peak.
    // v18.9.9.455 REVERTED in v18.9.9.457: keeping the decline flag around
    // as a signal but NOT using it to force useprebakeFallback=false. Field
    // regression: books with prebake ONLY (no ever-built sections/) went
    // straight to live build after decline, which tries to write
    // sections/N.bin.tmp and fails with "book file couldn't be read". The
    // prebake fallback loading the mismatched file (then rejecting on
    // fingerprint) is wasteful but keeps sections/ dir seed logic alive
    // via a code path that IS correctly wired. Accept ~30-60 ms/section
    // of wasted SD I/O in exchange for books actually opening.
    const bool useprebakeFallback = SETTINGS.optimizeChapterIndexing != 0 && !simpleRenderingActive_;
    (void)prebakeDeclinedForThisBook_;  // flag intentionally unused post-revert
    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                  SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                  SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                  SETTINGS.tableRendering, useprebakeFallback,
                                  /*forceSimpleRendering=*/simpleRenderingActive_)) {
      // Cache miss with BLE up: NimBLE's ~58 KB share has historically
      // made the parser either return layoutAbortedForLowMemory (good —
      // the reactive retry path below handles it) or simply hang
      // mid-page when the malloc pattern fragments badly (bad — watchdog
      // territory). Drop BLE proactively and defer this build to the next
      // render pass after the main loop's tryDisableIfRequested() drain
      // runs. Cheaper than relying on the reactive retry, and works for
      // *every* cache-miss trigger: font/margin/etc. changes from the
      // drawer, chapter boundary advances, percent jumps, anything.
      //
      // We can't disable() inline here — we hold the RenderLock and
      // NimBLE teardown can fire callbacks that call requestUpdateAndWait,
      // which trips the lock-held assertion. Deferred + return is the
      // safe pattern; the next render iteration finds BLE off and builds
      // with full heap headroom. bleAutoReEnableAfterReindex brings it
      // back online + reconnects to the bonded remote after the build.
      auto& btMgr = BluetoothHIDManager::getInstance();
      if (btMgr.isEnabled()) {
        // CrumBLE: a STORED (Bluetooth-friendly optimized) chapter needs no
        // 32 KB DEFLATE window to read, so it can build in place with BLE still
        // connected -- text lays out above the 16/10 KB floor and images get
        // suppressed (the heap check before image decode skips them). That skips
        // the drop-build-re-enable cycle entirely; the re-enable was the real
        // problem, re-fragmenting the heap to ~3 KB contiguous and breaking
        // both font rendering and the bonded-remote reconnect. Only DEFLATE
        // chapters still pre-drop BLE, where the window allocation would hang
        // under NimBLE's fragmentation. If a STORED build still aborts for low
        // memory, the reactive path below drops BLE and retries -- same safety
        // net, just reached on demand instead of pre-emptively.
        const bool chapterStored = epub->isItemStored(epub->getSpineItem(currentSpineIndex).href);
        if (!chapterStored) {
          // v18.9.9.64: v48's skip-deinit means bt.disable() doesn't actually
          // free the ~58 KB NimBLE holds. requestDisableLater()'s deferred
          // build then runs at ~6-8 KB maxAlloc and fails; Layer 2 also
          // fails (same heap state) and the user dead-ends at "not enough
          // memory". Silent-restart-to-defrag with EnableBt is the only way
          // to actually reclaim that ~58 KB. Post-boot: clean ~90 KB heap
          // -> section build succeeds -> boot dispatch re-enables BT.
          //
          // Gated on layoutDefragRetryAttempted_ (isDefragRetryContinuation()
          // seeds it true on the boot after this restart), so if the fresh-
          // heap build ALSO fails we fall through to the pre-v64 defer-and-
          // retry path + Layer 2 escalation. Prevents boot-loops on genuinely
          // too-heavy chapters that even a clean boot can't parse.
          //
          // v18.9.9.69: this whole hack should go away once the crosspoint
          // NimBLE shrink (custom_sdkconfig in platformio.ini) can be
          // enabled -- it drops NimBLE from ~68 KB to ~52 KB, at which point
          // the pre-v64 requestDisableLater path just works. See the
          // platformio.ini comment for the path-move requirement.
          // v18.9.9.168: allow one defrag-restart per unique spine. A
          // session-wide one-shot was too aggressive under v48 skip-deinit --
          // requestDisableLater alone can't free NimBLE's ~58 KB, so the
          // fallback path crashed at framebuffer realloc on the second
          // boundary. Per-spine tracking gives us N defrags per session
          // while still bounding loops (the same spine failing twice
          // escalates as before).
          if (layoutDefragRetryChapterSpine_ != currentSpineIndex) {
            LOG_INF("ERS",
                    "Cache miss with BLE up (DEFLATE chapter); silent-restart-to-defrag with EnableBt "
                    "for spine %d (v48 skip-deinit means requestDisableLater alone can't free NimBLE's ~58 KB)",
                    currentSpineIndex);
            layoutDefragRetryAttempted_ = true;
            layoutDefragRetryChapterSpine_ = currentSpineIndex;
            silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction::EnableBt, currentSpineIndex);
            // never returns
          }
          LOG_INF("ERS",
                  "Cache miss with BLE up (DEFLATE chapter); already defragged for spine %d, "
                  "falling back to requestDisableLater + deferred build",
                  currentSpineIndex);
          btMgr.requestDisableLater();
          bleAutoReEnableAfterReindex = true;
          // Reset section back to null. We constructed it above (line ~1495)
          // and loadSectionFile failed, so it's holding an empty Section
          // shell. If we don't drop it, the next render iteration's
          // `if (!section)` short-circuits and the render proceeds with a
          // zero-page Section — user sees "empty chapter". Resetting ensures
          // we re-enter the construct+build path next time around.
          section.reset();
          requestUpdate();
          return;
        }
        LOG_INF("ERS", "Cache miss with BLE up (STORED chapter); building in place, BLE stays connected");
        // fall through to the in-place build below (BLE remains connected)
      }

      LOG_DBG("ERS", "Cache not found, building... (free=%u, maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

      // v18.9.9.36 Phase C2: pre-measure the widest popup frame
      // ("Indexing...") so the box stays anchored to the same pixels
      // when loop() animates the trailing dots.
      char widestBuf[40];
      snprintf(widestBuf, sizeof(widestBuf), "%s...", tr(STR_INDEXING));
      const int popupMinTextWidth =
          renderer.getTextWidth(UI_12_FONT_ID, widestBuf, EpdFontFamily::BOLD);

      bool imagesWereSuppressed = false;
      bool layoutAbortedForLowMemory = false;
      // v18.9.9.36 Phase C2: the section build is now driven from loop()
      // one buildSomeMore chunk at a time so render() doesn't hold the
      // RenderLock for the full multi-second parse. Two entry paths land
      // in this block:
      //   1. Kickoff -- loadSectionFile just missed, and sectionBuildJustFailed_
      //      is false. Call startBuild, seed the C2 state, draw the initial
      //      popup, return. loop() takes over next tick.
      //   2. Failure resume -- loop()'s buildSomeMore returned false last
      //      tick and set sectionBuildJustFailed_. Snapshot the outcome
      //      flags back into locals and fall through to the failure
      //      block below. Section was already reset by loop() before
      //      requesting this render; the branch below is safe with a
      //      null section (each branch either returns or resets again).
      if (sectionBuildJustFailed_) {
        sectionBuildJustFailed_ = false;
        imagesWereSuppressed = sectionBuildImagesWereSuppressed_;
        layoutAbortedForLowMemory = sectionBuildLayoutAbortedForLowMemory_;
        bleAutoReEnableAfterReindex = sectionBuildBleWasDroppedForFail_;
        // Fall through to the failure block below.
      } else {
        // Left-anchor so "Indexing" stays pinned at a fixed position
        // and the trailing dots cycle to its right without shifting.
        GUI.drawPopup(renderer, tr(STR_INDEXING), popupMinTextWidth, /*leftAlignText=*/true);
        // v18.9.9.70: lend the framebuffer's ~40 KB for the initial startBuild
        // (parser construction + first-page allocations). Restored before
        // return -- subsequent buildSomeMore ticks in loop() will lend/restore.
        FrameBufferBuildLoan startBuildLoan(renderer);
        startBuildLoan.release();
        const bool startedOk = section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                  SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                  SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                  SETTINGS.tableRendering, /*popupFn=*/nullptr,
                                  /*imagesWereSuppressed=*/nullptr,
                                  /*layoutAbortedForLowMemory=*/nullptr,
                                  /*forceSimpleRendering=*/simpleRenderingActive_,
                                  /*suppressTablesOnly=*/false);
        if (!startBuildLoan.restore()) { ESP.restart(); }
        if (!startedOk) {
          // v18.9.9.318: was `showPendingSyncSaveError()` no-op that left the
          // screen frozen. v18.9.9.322: replaced the "low memory" popup with
          // "book file couldn't be read" -- startBuild failing at initialise
          // (before any layout work) is essentially always a ZIP-side
          // problem, not memory: EOCD not found (truncated / Zip64 /
          // unsupported), inflate init failure (compressed stream broken),
          // or the section stream returned empty (missing spine file).
          // v320's streaming EOCD scan removed the fragmented-heap class of
          // failure so the message is now accurate. Genuine low-memory hits
          // the async cascade below, which shows the real low-memory error.
          LOG_ERR("ERS", "Section startBuild failed to initialise; showing book-file-unreadable error");
          section.reset();
          snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
                   tr(STR_EPUB_BOOK_FILE_UNREADABLE_TITLE));
          snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s",
                   tr(STR_EPUB_BOOK_FILE_UNREADABLE_BODY));
          APP_STATE.pendingAlertGoHomeOnBack.store(true, std::memory_order_relaxed);
          APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
          GUI.drawPopup(renderer, tr(STR_EPUB_BOOK_FILE_UNREADABLE_TITLE));
          return;
        }
        sectionBuildInProgress_ = true;
        sectionBuildSpine_ = currentSpineIndex;
        sectionBuildPopupMinWidth_ = popupMinTextWidth;
        sectionBuildPopupLastMs_ = millis();
        sectionBuildPopupDotPhase_ = 0;
        // v18.9.9.454: reset the "title-shown" flag so the first render
        // after this build completes leaves title empty (avoids blocking
        // first-page paint on title-glyph SD reads).
        postBuildFirstRenderShown_ = false;
        return;  // loop() drives buildSomeMore from here.
      }
      // Failure branch reachable only from the resume path above.
      {
        if (layoutAbortedForLowMemory) {
          LOG_ERR("ERS", "EPUB section layout aborted for low heap; chapter exceeds safe layout memory");
        }
        if (!layoutAbortedForLowMemory) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
        }
        section.reset();
        // v18.9.9.20 (task #6): if the failure was inflate-init-class (not a
        // parser abort) AND we already dropped BLE for this build AND we
        // haven't burned the defrag budget yet, silent-restart the reader to
        // reset the fragmented heap. Even with BLE dropped, mid-session heap
        // can leave the largest contiguous slot around 30 KB while the
        // DEFLATE window needs ~90 KB -- clean boot heap has ~90 KB contiguous
        // naturally. The alternative (returning here) leaves the user stuck
        // at "Failed to persist page data" and only the home-screen cover
        // heap-guard eventually recovers via its own silent-restart -- ugly
        // multi-hop path. Gated one-shot per book open via
        // layoutDefragRetryAttempted_ (isDefragRetryContinuation() seeds it
        // to true on the boot after this restart, so we never loop).
        // EnableBt as the post-boot action preserves the user's BLE session
        // -- the boot rebuilds first, then dispatches BT enable.
        // v18.9.9.22: also fire the defrag restart when BLE was never on but
        // maxAlloc is well below what the inflate window needs. Same class of
        // failure -- fragmented heap can't fit the ~90 KB contiguous slot --
        // just reached without a BLE-drop step. Threshold picked well below
        // the ~90 KB DEFLATE window need so we don't over-trigger; a clean
        // boot recovers with ~90 KB naturally.
        constexpr uint32_t COLD_INFLATE_DEFRAG_MAX_ALLOC_FLOOR = 60 * 1024;
        const bool droppedBleThisAttempt = bleAutoReEnableAfterReindex;
        const bool likelyFragmentationFailure =
            !droppedBleThisAttempt && ESP.getMaxAllocHeap() < COLD_INFLATE_DEFRAG_MAX_ALLOC_FLOOR;
        if (!layoutAbortedForLowMemory && !layoutDefragRetryAttempted_ &&
            (droppedBleThisAttempt || likelyFragmentationFailure)) {
          LOG_INF("ERS",
                  "Section build failed on inflate init (%s, maxAlloc=%u); "
                  "silent-restart-to-defrag with %s to retry on clean heap",
                  droppedBleThisAttempt ? "post-BLE-drop" : "no BLE this session",
                  ESP.getMaxAllocHeap(),
                  droppedBleThisAttempt ? "EnableBt" : "None");
          layoutDefragRetryAttempted_ = true;
          // v18.9.9.32: pass currentSpineIndex so post-boot resume lands
          // on the chapter the user was trying to open (e.g. a fresh
          // chapter clicked from ChapterSelect) rather than progress.bin's
          // last-committed position. If the user was already on this
          // spine, resuming at it is a no-op.
          silentRestartToReaderWithDefragRetryAtSpine(
              droppedBleThisAttempt ? ReaderPostBootAction::EnableBt : ReaderPostBootAction::None,
              currentSpineIndex);
          // never returns
        }
        // CrumBLE: if the layout aborted on heap pressure and BLE is still
        // hogging ~58 KB, drop BLE and retry once. requestDisableLater()
        // sets a flag that the next main-loop tick drains via
        // tryDisableIfRequested(), which runs BEFORE the next render() —
        // so on the retry, the parser sees the freed heap. BLE auto-
        // reconnects on the user's next button press, so the only visible
        // cost is a ~2-3 s delay during this one cold-cache parse.
        if (layoutAbortedForLowMemory && BluetoothHIDManager::getInstance().isEnabled() &&
            !layoutBleRetryAttempted) {
          LOG_INF("ERS", "Layout aborted under BLE pressure; dropping BLE, retrying once it's off");
          layoutBleRetryAttempted = true;
          BluetoothHIDManager::getInstance().requestDisableLater();
          // Same re-enable hook the drawer path uses — once this retry's
          // section build succeeds, bring BLE back up so the bonded
          // remote reconnects on the user's next press.
          bleAutoReEnableAfterReindex = true;
          // Do NOT requestUpdate() here. requestDisableLater() is drained on the
          // main task, but render() runs on the render task -- an inline
          // requestUpdate() re-attempts the build before the disable lands, so the
          // one-shot retry runs with NimBLE still holding ~58 KB (maxAlloc stays
          // tiny) and is wasted, even though the heap recovers seconds later.
          // Let loop() fire the retry once BLE is actually off (see
          // pendingLayoutRetryAfterBleOff), giving the chapter one genuine
          // full-heap build attempt before we'd show the low-memory error.
          pendingLayoutRetryAfterBleOff = true;
          return;
        }
        // v18.9.6: second-tier fallback. BLE-drop retry has already run (or
        // BLE wasn't the culprit) and the parse is still failing. Flip on
        // Simple Rendering (images off, style off, tables collapsed, bionic
        // + guide off) and retry once. Set the flag so subsequent chapters
        // in this session also render in simple mode -- prevents the same
        // crash on every chapter turn.
        if (layoutAbortedForLowMemory && !layoutSimpleRetryAttempted) {
          LOG_INF("ERS",
                  "Layout aborted after BLE-drop retry; escalating to Simple Rendering "
                  "(force images off, style off, tables collapsed, bionic+guide off) and retrying");
          layoutSimpleRetryAttempted = true;
          simpleRenderingActive_ = true; APP_STATE.readerCompatModeActive = true;
          // v18.9.6.1: persist so next open skips the crash-then-retry cycle.
          writeSimpleRenderingSidecar(epub->getCachePath(), readerActivePath_);
          if (APP_STATE.compatUserDisabledThisSession) { armCompatReenabledToast(); APP_STATE.compatUserDisabledThisSession = false; }
          section.reset();
          requestUpdate();
          return;
        }
        // Build failed and we already retried (or BLE wasn't the culprit).
        //
        // CrumBLE graceful fallback: a cold build that fails *after* we dropped
        // BLE for it is the fragmentation wall -- even with BLE gone, the heap
        // is too shattered to allocate the inflate window (no compaction on this
        // chip). Show the low-memory message (accurate) instead of a misleading
        // "save failed", and FLUSH it now so the panel shows the error during
        // the ~7 s BLE re-enable below -- otherwise the stale "Indexing" popup
        // stays frozen on screen the whole time (the reboot-needing hang the
        // user hit). Then bring BLE back so the remote isn't lost.
        const bool coldBuildLowMem = layoutAbortedForLowMemory || bleAutoReEnableAfterReindex;
        // Do NOT re-enable BLE on a failed cold build. The build failed because
        // the heap is too fragmented for the inflate window; re-enabling runs a
        // ~7 s BLOCKING connect right as the low-memory alert appears, and button
        // sampling is frozen for that whole window -- so the user's "Back" tap on
        // the alert is swallowed and it feels stuck. Leave the remote off: the
        // alert is immediately responsive, and the user can reconnect from Home
        // or a cached chapter. (On a *successful* cold build we still re-enable.)
        bleAutoReEnableAfterReindex = false;
        if (coldBuildLowMem) {
          showLowMemoryLayoutError();
        } else {
          showPendingSyncSaveError();
        }
        return;
      }
      LOG_DBG("ERS", "Cache build complete: pages=%u free=%u maxAlloc=%u", section->pageCount, ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      // Section parsed successfully — clear the BLE-retry latch so a future
      // failure on a different chapter can also use the retry path.
      layoutBleRetryAttempted = false;
      // If we dropped BLE around this build (drawer settings change, or the
      // reactive retry path above), the heap pressure is gone now -- but do NOT
      // re-enable BLE here. The page we're about to render may carry an inline
      // image, and re-enabling now would let NimBLE grab its ~58 KB right before
      // the JPEG/PNG decode, starving it and dropping BLE again -- the exact
      // connect/disconnect thrash seen at image-heavy chapter boundaries. Instead
      // latch the re-enable and let renderContents() fire it only after a clean,
      // image-free render, so we reconnect once we're past the un-decodable page.
      // (An image-free rebuilt chapter re-enables on this same render(), so a
      // text-only boundary still reconnects promptly.)
      if (bleAutoReEnableAfterReindex) {
        bleAutoReEnableAfterReindex = false;
        bleReEnableHeldForImagePage = true;
        LOG_INF("ERS", "Section build done; holding BLE re-enable until a clean image-free render");
      }

      if (imagesWereSuppressed) {
        // Be honest about *why* the images are gone. When a Bluetooth remote is
        // connected this is the expected trade-off (we kept BLE up and built the
        // chapter in place), so tell the user images come back if they
        // disconnect -- otherwise it's a plain low-memory notice.
        const bool bleConnected = BluetoothHIDManager::getInstance().isEnabled();
        const StrId titleId = bleConnected ? StrId::STR_BT_IMAGES_HIDDEN_TITLE : StrId::STR_LOW_MEMORY_IMAGES_TITLE;
        const StrId bodyId = bleConnected ? StrId::STR_BT_IMAGES_HIDDEN_BODY : StrId::STR_LOW_MEMORY_IMAGES_BODY;
        snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
                 I18n::getInstance().get(titleId));
        snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s",
                 I18n::getInstance().get(bodyId));
        APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build... (pages=%u, free=%u, maxAlloc=%u)", section->pageCount,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }

    // CrumBLE 4.3: install the embedded glyph subset block (when this is a
    // prebaked v39 section AND the loaded SD font's contentHash matches
    // what the section was baked against). After install -- successful or
    // not -- always thread the current per-style EpdFontData pointers
    // (nullptr for unprebaked styles, or all nullptr for sections without
    // any embedded subset) into the renderer's matching EpdFontFamily.
    // That overwrites any stale state left by the PRIOR section so a
    // chapter change away from a prebaked section automatically clears
    // the routing; no separate clear-on-exit hook needed.
    const int curFontId = SETTINGS.getReaderFontId();
    // CrumBLE 4.3 diagnostic: surface the two silent gates so a "no embedded
    // subset installed" failure is observable in the log instead of inferred
    // from a missing success line. Both gates are unconditional info logs
    // because the install path is rare (once per chapter open) and we need
    // to know which case fires for the SD-font + BT '?'-glyph regression.
    if (!section) {
      LOG_INF("SCT", "EGS gate: section null, skipping embedded-subset install");
    } else if (!section->hasEmbeddedGlyphSubset()) {
      LOG_INF("SCT",
              "EGS gate: section has no v39 embedded-subset trailer (fileVersion=%u) -- skipping install (curFontId=%d)",
              static_cast<unsigned>(section->fileVersion()), curFontId);
    } else {
      const auto& sdFontMap = renderer.getSdCardFonts();
      auto it = sdFontMap.find(curFontId);
      if (it == sdFontMap.end() || it->second == nullptr) {
        LOG_INF("SCT",
                "EGS gate: sdFontMap missing curFontId=%d (mapSize=%u) -- skipping install",
                curFontId, static_cast<unsigned>(sdFontMap.size()));
      } else {
        LOG_INF("SCT",
                "EGS gate: attempting install (curFontId=%d sdFontHash=0x%08x)",
                curFontId, it->second->contentHash());
        // CrumBLE 4.4 step 5: prefer the v40 glyph atlas when present.
        // If the section has both a v40 atlas and a v39 subset (which
        // is what the current prebake CLI emits when --emit-section-
        // glyph-subsets is set), the atlas wins -- it's smaller in
        // resident memory, has no eviction failure mode, and matches
        // the renderer's 1-bit blit path directly. The v39 subset stays
        // available as a fallback if the atlas install fails (e.g.
        // post-NimBLE heap too tight for the bitmap allocation).
        //
        // glyphAtlasEnabled toggle: when 0, atlas install is skipped
        // entirely and we go straight to the v39 subset path -- used to
        // A/B test whether the atlas integration is the source of the
        // FT upload heap regression.
        if (!SETTINGS.glyphAtlasEnabled && section->hasGlyphAtlas()) {
          LOG_INF("SCT", "EGS gate: atlas available but glyphAtlasEnabled=0, falling back to v39 subset");
        }
        // v18.9.9.436: lend the ~52 KB framebuffer to the atlas install for
        // its transient contiguous allocations. Chapter-jump into a CJK
        // section (1067+ glyphs) previously left maxAlloc ~10 KB
        // post-install because entries+synthesizedGlyphs+intervals pit the
        // heap around the framebuffer block; a fresh 52 KB hole for the
        // allocator to work in dramatically lowers post-install fragmen-
        // tation. Restored right after so the ensuing loadPageFromSection
        // File + render finds a valid framebuffer. Only applied when we
        // actually intend to install (atlas enabled + section has one).
        bool atlasOk = false;
        if (SETTINGS.glyphAtlasEnabled && section->hasGlyphAtlas()) {
          const uint32_t maxBefore = ESP.getMaxAllocHeap();
          FrameBufferBuildLoan atlasLoan(renderer);
          atlasLoan.release();
          LOG_INF("SCT",
                  "Atlas install: framebuffer lent (maxAlloc %u -> %u)",
                  maxBefore, ESP.getMaxAllocHeap());
          atlasOk = section->tryInstallGlyphAtlas(
              it->second->contentHash(),
              /*preferLowBitDepth=*/BluetoothHIDManager::getInstance().isEnabled());
          if (!atlasLoan.restore()) {
            LOG_ERR("SCT", "Atlas install: framebuffer restore failed; hard restart");
            ESP.restart();
          }
          LOG_INF("SCT",
                  "Atlas install: framebuffer restored (maxAlloc now=%u, atlasOk=%d)",
                  ESP.getMaxAllocHeap(), atlasOk ? 1 : 0);
        }
        // CrumBLE 4.5.6: CrossPoint-style pause/resume pattern
        // (crosspoint-reader@6305777b). If atlas refuses with BT enabled,
        // cycle NimBLE off, retry on next tick with ~15 KB freed, then
        // requestEnableLater so bonded remote auto-reconnects. In-process;
        // no reboot. Loop() drives the retry once BT actually drains.
        auto& btMgrForAtlas = BluetoothHIDManager::getInstance();
        const bool btEnabledAtlas = btMgrForAtlas.isEnabled();
        if (!atlasOk && SETTINGS.glyphAtlasEnabled && section->hasGlyphAtlas() &&
            btEnabledAtlas && !atlasRetryPendingBtDrop_) {
          LOG_INF("ERA",
                  "Atlas install refused (free=%u maxAlloc=%u); cycling BT off for retry",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          atlasRetryPendingBtDrop_ = true;
          btMgrForAtlas.requestDisableLater();
          // CrumBLE 4.5.7 v17: HALF_REFRESH avoids the freeink-sdk
          // FAST_REFRESH whole-panel flash while waiting for BT to drain.
          GUI.drawPopup(renderer, tr(STR_BT_CONNECTING), 0, false, HalDisplay::HALF_REFRESH);
          return;  // loop() will requestUpdate once BT drains
        }
        if (atlasOk && atlasRetryPendingBtDrop_) {
          LOG_INF("ERA",
                  "Atlas installed after BT cycle (free=%u); re-enabling BT",
                  ESP.getFreeHeap());
          btMgrForAtlas.requestEnableLater();
          atlasRetryPendingBtDrop_ = false;
        }
        if (!atlasOk) {
          // If BT was cycled + atlas still failed, subset install would also
          // fail (comment on skipSubset path: subset is often bigger than
          // atlas). Restore BT + rely on SD-font streaming.
          if (atlasRetryPendingBtDrop_) {
            LOG_INF("ERA",
                    "Atlas still refused after BT drop (free=%u); restoring BT, streaming glyphs",
                    ESP.getFreeHeap());
            btMgrForAtlas.requestEnableLater();
            atlasRetryPendingBtDrop_ = false;
          } else if (btEnabledAtlas) {
            LOG_INF("ERA",
                    "Atlas install refused (free=%u); streaming to preserve BT",
                    ESP.getFreeHeap());
          } else {
            section->tryInstallEmbeddedGlyphSubset(it->second->contentHash());
          }
        }
        // CrumBLE 4.4: one-time backward-compat write of prebake-cpfont.marker
        // for books that were re-uploaded with the optimizer.js atlas-emit
        // fix but missed the WASM CLI marker write (those came before the
        // CLI rebuild that emits this marker). Once the marker exists, the
        // badge tier upgrades to ✓IMG+CHAP+CP.FONT on the next FT listing
        // and on the next device long-press. Storage.exists is cheap;
        // gated on first-install success so we don't write for v40
        // sections that don't carry atlas.
        if (atlasOk && epub) {
          const std::string markerPath =
              Epub::cachePathForFilePath(epub->getPath(), "/.crosspoint") + "/prebake-cpfont.marker";
          if (!Storage.exists(markerPath.c_str())) {
            HalFile m;
            if (Storage.openFileForWrite("ERA", markerPath, m)) {
              m.close();
              LOG_INF("ERA", "Wrote backward-compat prebake-cpfont.marker for %s", epub->getPath().c_str());
            }
          }
        }
      }
    }
    // Atlas takes precedence when installed; the renderer slot is the
    // same for both paths (setEmbeddedGlyphData), so the existing draw
    // code doesn't need to know which source produced the data.
    auto glyphFontData = [&](uint8_t style) -> const EpdFontData* {
      if (!section) return nullptr;
      // glyphAtlasEnabled toggle gates the atlas read path too -- with
      // toggle OFF, the install above was skipped so glyphAtlasInstalled()
      // is false anyway, but checking the flag here makes it explicit and
      // catches the edge case where the atlas was already installed before
      // the user flipped the toggle.
      if (SETTINGS.glyphAtlasEnabled && section->glyphAtlasInstalled()) {
        if (const EpdFontData* atlas = section->glyphAtlasFontDataForStyle(style)) return atlas;
      }
      return section->embeddedFontDataForStyle(style);
    };
    renderer.setEmbeddedGlyphData(curFontId, glyphFontData(0), glyphFontData(1), glyphFontData(2), glyphFontData(3));

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }

    // Clamp the current page to ensure we stay within bounds if reader settings have
    // changed since the page number (e.g., via a bookmark) was saved.
    if (section->pageCount > 0) {
      if (section->currentPage >= section->pageCount) {
        section->currentPage = section->pageCount - 1;
      } else if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }
  }

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    // CrumBLE 4.2: serve the page DOM from the activity-level cache when
    // (section pointer, spine, page index) match the cached values.
    // Otherwise reload from disk. Caching trims the per-render
    // ~25-40 KB allocation churn that previously crashed under BT
    // heap pressure (TextBlock::deserialize -> vector<string>::resize
    // -> bad_alloc -> terminate). The first render after each page turn
    // or chapter change still pays the load cost (~50-80 ms); steady-
    // state re-renders (BT enable popup, focus changes, status bar
    // refreshes) reuse the cached page for free.
    // CrumBLE 4.3 heap-regression revert: force cache MISS on every render
    // and drop the page DOM at end of render. The cachedRenderPage_ feature
    // (introduced in 1ca7dfcc to spare BT-event re-renders the deserialize
    // cost) held ~25-40 KB of vector<string>-heavy state and added a 7 KB
    // permanent maxAlloc regression from the build/drop fragmentation
    // pattern around BT enable. Re-loading per render is the v3.7.3 behavior
    // and ~50-80 ms per call -- imperceptible compared to e-ink's 400 ms
    // refresh cycle.
    //
    // v18.9.9.4 (experiment): re-enable the cache scoped to BT-linked
    // sessions on !simpleRenderingActive_ books. Rationale: under BT the
    // per-tick deserialize churns 5-40 KB against a heap that also holds
    // NimBLE's 58 KB, and post-turn re-renders (BT connect popup, focus
    // change, header refresh) shouldn't re-pay the deserialize cost when
    // the DOM is already in RAM. Trade-off: the 4.3 fragmentation regression
    // may briefly reappear during the cache-hold window, but at post-BT
    // MaxAlloc of 2-8 KB the current unfragmented ceiling is already the
    // bottleneck. Simple Rendering books stay on the no-cache path -- their
    // per-page DOMs are already trimmed so the caching value is smaller
    // and the 4.3 regression cost is proportionally worse.
    const bool btLinkedNow = BluetoothHIDManager::getInstance().isEnabled() &&
                             SETTINGS.bleBondedDeviceAddr[0] != '\0' &&
                             BluetoothHIDManager::getInstance().isConnected(SETTINGS.bleBondedDeviceAddr);

    // v18.9.9.10 + v18.9.9.11: streamed line-by-line render whenever BT
    // is linked. Peak heap ~500 bytes vs ~10 KB for the whole-DOM path.
    // Under BT, images skip render and tables render via
    // renderContentOnly (cell text without borders) -- so the streamed
    // path is the "always fits" compat render for any BT session,
    // regardless of the sidecar / simpleRenderingActive_ state.
    // simpleRenderingActive_ is now a hint to the parser at section
    // rebuild time, not a runtime toggle.
    //
    // v18.9.9.17: also route through the streamed path when
    // simpleRenderingActive_ is on but BT isn't (rare: Layer 2 OOM
    // escalation on a heavy non-BT book). Compat-mode-without-BT would
    // otherwise use the whole-DOM path and re-OOM on the same content
    // that just escalated. Passing btLinked=true is a slight misnomer
    // here -- the flag really means "runtime-skip tables and images"
    // -- but that's exactly what compat mode wants regardless of BT.
    //
    // v18.9.9.57: revert v56's whole-DOM re-route. The whole-DOM path holds
    // ~10 KB Page DOM (one TextBlock per line) which -- combined with
    // NimBLE's ~58 KB -- drops post-render maxAlloc below the 4000 floor
    // and cascades into Layer 1 defrag -> Layer 2 compat mode. Instead,
    // stay on the streamed path under BT (peak ~500 B per element) but
    // pass a pxcImagesSafe flag so Section can blit images from the .pxc
    // cache when it's safe to do so. Cache blit peaks at ~64 B row buffer,
    // preserves the 4000 floor headroom, and unlocks images under BT.
    const bool pxcImagesSafe =
        pxcManifest_.has_value() && section && section->wasLoadedFromPrebake();
    const bool useReducedRender = btLinkedNow || simpleRenderingActive_;
    if (useReducedRender) {
      POST_BT_STEP("pre streamed render");
      const bool streamedOk = section->renderPageStreamed(
          renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop,
          ReaderUtils::readerForegroundBlack(), /*btLinked=*/true,
          /*pxcImagesSafe=*/pxcImagesSafe);
      POST_BT_STEP("post streamed render");
      if (streamedOk) {
        postRenderDrainedMaxAlloc_ = ESP.getMaxAllocHeap();
        // v18.9.9.11: renderStatusBar + displayBuffer here -- the streamed
        // path skips renderContents which normally does both, so without
        // this the framebuffer contents never get pushed to the panel
        // (bug in v18.9.9.10).
        // v18.9.9.12: use ReaderUtils::displayWithRefreshCycle instead of
        // a fixed HALF_REFRESH. HALF flashes the panel black/white every
        // page (~1.7 s) which is visually jarring on every turn. The
        // shared cycle helper picks FAST_REFRESH (~500 ms, no flash) for
        // most turns and HALF only every N pages to clear ghosting --
        // matching the whole-DOM path's cadence. Also honours the
        // continuation-from-silent-restart FAST-only rule.
        renderStatusBar();
        ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
        // Skip the whole-DOM cache/load/renderContents path below.
        goto streamedRenderDone;
      }
      LOG_ERR("ERS", "Streamed render refused; falling back to whole-DOM path (may OOM)");
    }

    const bool cachedPageStillValid =
        cachedRenderPage_ != nullptr && cachedRenderSection_ == static_cast<void*>(section.get()) &&
        cachedRenderSpine_ == currentSpineIndex && cachedRenderPageIndex_ == section->currentPage;
    const bool cacheHit = btLinkedNow && !simpleRenderingActive_ && cachedPageStillValid;
    if (cacheHit) {
      LOG_DBG("ERS", "Page DOM cache HIT: spine=%d page=%d (skip deserialize, BT linked)",
              currentSpineIndex, section->currentPage);
    }
    if (!cacheHit) {
      // CrumBLE 4.3: free-old-before-build-new. See warmPageCacheForBtTransition's
      // matching block for the full explanation. Briefly: unique_ptr::operator=
      // builds the RHS (a complete fresh Page DOM) before deleting the LHS, so
      // a doubled-DOM peak exists during deserialize. Under post-BT heap that
      // peak crashes TextBlock::deserialize -> bad_alloc -> terminate. Explicit
      // reset() ensures only the new DOM is being allocated at the peak --
      // recovers the previous page's footprint (~5-15 KB) for the deserialize.
      cachedRenderPage_.reset();
      cachedRenderSection_ = nullptr;
      cachedRenderSpine_ = -1;
      cachedRenderPageIndex_ = -1;

      // CrumBLE 4.3 known limitation: post-BT page-DOM deserialize peak
      // (~12-13 KB contiguous for 28+ element pages) exceeds the post-
      // NimBLE budget (~12.7 KB MaxAlloc) by a few hundred bytes for
      // text-heavy SD-font book chapters. Manifests as bad_alloc inside
      // Page::deserialize element loop. Tried a subset-drop escape valve
      // here -- worked mechanically but broke glyph rendering because
      // neither the lazy subset reinstall nor the SD-font fallback could
      // re-allocate after the page DOM ate the freed bytes. Permanent
      // fix is the page-DOM arena (task #24).
      POST_BT_STEP("pre loadPageFromSectionFile");
      cachedRenderPage_ = section->loadPageFromSectionFile();
      POST_BT_STEP("post loadPageFromSectionFile");
      if (cachedRenderPage_) {
        cachedRenderSection_ = static_cast<void*>(section.get());
        cachedRenderSpine_ = currentSpineIndex;
        cachedRenderPageIndex_ = section->currentPage;
        // Footnotes are owned by the page; copy (not move) so the cache
        // still has them on subsequent re-renders that read them.
        currentPageFootnotes = cachedRenderPage_->footnotes;
        POST_BT_STEP("post footnotes copy");
        // CrumBLE 4.5.6: page loaded after BT cycle → bring BT back.
        if (pageLoadRetryPendingBtDrop_) {
          BluetoothHIDManager::getInstance().requestEnableLater();
          pageLoadRetryPendingBtDrop_ = false;
          LOG_INF("ERS", "Page loaded after BT cycle; re-enabling BT (auto-reconnect follows)");
        }
      } else {
        cachedRenderSection_ = nullptr;
        cachedRenderSpine_ = -1;
        cachedRenderPageIndex_ = -1;
      }
    }

    if (!cachedRenderPage_) {
      // CrumBLE 4.5.6: CrossPoint-style pause/resume for page-load refuse.
      // Page::deserialize needs ~10-15 KB contiguous heap. With BT
      // connected, cycle NimBLE off (freeing ~15 KB + defragmenting),
      // retry on next tick, then requestEnableLater.
      auto& btMgrForPage = BluetoothHIDManager::getInstance();
      const bool btEnabledForPage = btMgrForPage.isEnabled();
      // Waiting for the deferred BT disable to drain? Don't clear cache or
      // bump retry -- just wait. loop() requestUpdate once BT is off.
      if (pageLoadRetryPendingBtDrop_ && btEnabledForPage) {
        return;
      }
      if (btEnabledForPage && !pageLoadRetryPendingBtDrop_) {
        LOG_INF("ERS", "Page load refused with BT on (free=%u maxAlloc=%u); cycling BT for retry",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        // v18.9.9.5 Level 1: before content escalation, spend a one-shot
        // silent-restart-with-EnableBt to defrag the heap while keeping
        // full render (tables + images) intact. This replicates the
        // "manual retry works" pattern users hit on heavy books: a fresh
        // ~110 KB boot heap fits the deserialize that a fragmented
        // mid-session heap couldn't. Gated so we don't loop: budget is
        // spent for the current book open, and boot 2's reader seeds
        // layoutDefragRetryAttempted_ from isDefragRetryContinuation()
        // so the NEXT failure escalates content (Level 2 or 3).
        if (!layoutDefragRetryAttempted_ && !layoutSimpleRetryAttempted && !simpleRenderingActive_) {
          LOG_INF("ERS",
                  "Level 1 defrag retry -- silent-restart-with-EnableBt to reset heap "
                  "fragmentation while keeping full render (one-shot per book open)");
          layoutDefragRetryAttempted_ = true;
          // v18.9.9.32: resume at currentSpineIndex so a defrag triggered
          // from a fresh-chapter navigation lands the user there, not on
          // progress.bin's stale spine.
          silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction::EnableBt, currentSpineIndex);
          // never returns
        }
        // v18.9.9.10 Layer 2: Layer 1 defrag already spent -- jump straight
        // to compat mode. No intermediate tables-only middle ground; the
        // streamed line-render path in Simple Rendering has a peak
        // footprint of ~500 bytes per page, guaranteed to fit any post-BT
        // heap state.
        if (!layoutSimpleRetryAttempted && !simpleRenderingActive_) {
          LOG_INF("ERS", "Layer 2 escalation -- Simple Rendering compat mode after defrag budget spent");
          layoutSimpleRetryAttempted = true;
          simpleRenderingActive_ = true; APP_STATE.readerCompatModeActive = true;
          writeSimpleRenderingSidecar(epub->getCachePath(), readerActivePath_);
          if (APP_STATE.compatUserDisabledThisSession) { armCompatReenabledToast(); APP_STATE.compatUserDisabledThisSession = false; }
          section->clearCache();
          section.reset();
        }
        pageLoadRetryPendingBtDrop_ = true;
        // v18.9.9.148: v137's guard REMOVED entirely. It was designed to
        // prevent a null-deref during NimBLE.disable() when the controller
        // partial-init'd (which happened only when we were starving NimBLE
        // via v142's reserve). With v145's reserve removal, NimBLE always
        // completes init cleanly when connect succeeds, so disable is safe.
        btMgrForPage.requestDisableLater();
        // CrumBLE 4.5.7 v17: HALF_REFRESH avoids the freeink-sdk FAST_REFRESH
        // whole-panel flash while cycling BT for the page-load retry.
        GUI.drawPopup(renderer, tr(STR_BT_CONNECTING), 0, false, HalDisplay::HALF_REFRESH);
        return;
      }
      if (pageLoadRetryPendingBtDrop_ && !btEnabledForPage) {
        // BT off, retry STILL failed. Restore BT + fall through to existing
        // clear-cache retry so the user isn't stranded with BT off.
        LOG_ERR("ERS", "Page load still refused after BT drop (free=%u); restoring BT + clearing section cache",
                ESP.getFreeHeap());
        btMgrForPage.requestEnableLater();
        pageLoadRetryPendingBtDrop_ = false;
      }

      // v18.9.6a: runtime deserialization crashes on prebaked books (the
      // parse-time escalation in v18.9.6 doesn't help because the parse
      // never runs -- Section loads sections-prebake/N.bin directly and
      // hits PageTableFragment::deserialize under low heap). Escalate to
      // Simple Rendering here BEFORE burning a normal clear-cache retry:
      // the next createSectionFile writes a fresh section.bin with tables
      // collapsed to paragraphs, so the failing fragment doesn't exist to
      // deserialize.
      //
      // v18.9.9.437: gate the escalation on WHY the load failed. The
      // v18.9.6a comment assumed the failure was a deserialize crash, but
      // loadPageFromSectionFile also returns nullptr when its pre-flight
      // refuses under maxAlloc < 12 KB -- a heap-budget refusal, NOT a
      // content problem. In that case the section is fine, we just don't
      // have room to allocate the Page struct on this pass. Escalating to
      // Simple Rendering here is actively harmful: Simple Rendering
      // abandons the prebake path and re-parses XHTML from the ZIP each
      // time, which cascades into the ZIP-read failure we saw with the
      // Harry Potter EPUB (bad SD cluster made every subsequent chapter
      // open fail as "book unreadable"). When we know the section loaded
      // cleanly from prebake AND maxAlloc is what refused the page load,
      // preserve the prebake path and surface an honest error instead.
      // The user can back out to a lighter chapter or retry after some
      // heap recovers.
      const bool sectionWasPrebakeLoad = section && section->wasLoadedFromPrebake();
      const bool pageLoadRefusedForHeap = section && ESP.getMaxAllocHeap() < 14u * 1024u;
      if (!layoutSimpleRetryAttempted && !simpleRenderingActive_ &&
          !(sectionWasPrebakeLoad && pageLoadRefusedForHeap)) {
        LOG_INF("ERS",
                "Page load still failing; escalating to Simple Rendering (free=%u maxAlloc=%u)",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        layoutSimpleRetryAttempted = true;
        simpleRenderingActive_ = true; APP_STATE.readerCompatModeActive = true;
        // v18.9.6.1: persist for future opens.
        writeSimpleRenderingSidecar(epub->getCachePath(), readerActivePath_);
          if (APP_STATE.compatUserDisabledThisSession) { armCompatReenabledToast(); APP_STATE.compatUserDisabledThisSession = false; }
        section->clearCache();
        section.reset();
        requestUpdate();
        automaticPageTurnActive = false;
        return;
      }
      if (sectionWasPrebakeLoad && pageLoadRefusedForHeap) {
        LOG_INF("ERS",
                "Page load failed on prebake path under heap pressure (free=%u maxAlloc=%u); "
                "NOT escalating to Simple Rendering. Silent-restart-to-reader dropping "
                "resumeSpine=%d, will fall back to progress.bin's last-committed spine "
                "and show a toast on arrival explaining why.",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), currentSpineIndex);
        // Arm the toast for the next boot's onEnter, then silent-restart.
        // silentRestartToReader() (no *Resuming* variant) doesn't set
        // silentRebootTargetSpine, so the boot-side consumer sees the
        // 0xFFFFFFFF sentinel and progress.bin's spine is used unchanged.
        // Every FT/heap-refuse path here already went through the Layer 1
        // defrag continuation once (that's WHY we're on the second attempt
        // that failed); using the plain restart avoids re-entering the
        // defrag-retry loop.
        armChapterHeapRefuseToast(currentSpineIndex);
        section->clearCache();
        section.reset();
        silentRestartToReader();  // never returns
        return;
      }

      pageLoadRetryCount++;
      if (pageLoadRetryCount < MAX_PAGE_LOAD_RETRIES) {
        LOG_ERR("ERS", "Failed to load page from SD (retry %d) - clearing section cache", pageLoadRetryCount);
        section->clearCache();
        section.reset();
        requestUpdate();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }

      LOG_ERR("ERS", "Failed to load page from SD after %d retries", pageLoadRetryCount);
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), ReaderUtils::readerForegroundBlack(),
                                EpdFontFamily::BOLD);
      // The auto-retry already tried clearing+rebuilding this chapter's cache. If
      // it still won't load, the SD filesystem is likely the problem (it can't be
      // self-healed on-device) -- point the user at the recovery options.
      renderer.drawCenteredText(UI_10_FONT_ID, 332, tr(STR_PAGE_LOAD_ERROR_HINT), ReaderUtils::readerForegroundBlack());
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    pageLoadRetryCount = 0;

    // CrumBLE 4.3: lazy embedded glyph subset reload. After the BT-enable
    // path dropped the v2 subset to free heap for NimBLE + page deserialize,
    // try to re-install it once the page DOM is loaded. The install has
    // its own preflight (skips if MaxAlloc < bitmap size + 1 KB) so it
    // silently bails on tight heap and the render falls back to SD-font
    // onGlyphMiss for everything (some Outside range drift, but readable).
    // CrumBLE 4.4 step 5: lazy reload prefers the v40 atlas over the v39
    // subset when both are present, mirroring the section-open path.
    // glyphAtlasEnabled toggle: when off, atlasNeedsReload stays false and
    // the lazy reload reaches only the v39 subset path.
    const bool atlasNeedsReload = SETTINGS.glyphAtlasEnabled && section &&
                                  section->hasGlyphAtlas() && !section->glyphAtlasInstalled();
    const bool subsetNeedsReload =
        section && section->hasEmbeddedGlyphSubset() && !section->embeddedSubsetInstalled();
    if (atlasNeedsReload || subsetNeedsReload) {
      const int curFontId = SETTINGS.getReaderFontId();
      const auto& sdFontMap = renderer.getSdCardFonts();
      auto it = sdFontMap.find(curFontId);
      if (it != sdFontMap.end() && it->second != nullptr) {
        const uint32_t freeBefore = ESP.getFreeHeap();
        const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
        bool atlasOk = false;
        if (atlasNeedsReload) {
          atlasOk = section->tryInstallGlyphAtlas(it->second->contentHash(),
                                                       /*preferLowBitDepth=*/BluetoothHIDManager::getInstance().isEnabled());
        }
        bool subsetOk = section->embeddedSubsetInstalled();
        // CrumBLE 4.4 v4.4.1: when the atlas is already providing data
        // (either freshly installed above OR installed at section-open and
        // still resident), the v39 subset is redundant -- both blocks are
        // emitted from the same prewarmed glyph working set, so they cover
        // identical codepoints with identical metrics. Installing the
        // subset alongside the atlas burns ~6 KB of MaxAlloc and ~5 KB of
        // free heap (see lazy-reload log line that motivated this change:
        // `atlas=0 subset=1 (free 56592->49364, maxAlloc 49140->42996)` --
        // the atlas was already installed at section-open, so the lazy
        // reload's subset install was pure overhead going into the BT
        // enable window). Only fall through to the subset when there's no
        // atlas data at all -- either because the section was baked
        // pre-v40 (no atlas block) OR the atlas install above failed under
        // tight heap.
        const bool atlasUsable = atlasOk || section->glyphAtlasInstalled();
        if (!atlasUsable && subsetNeedsReload) {
          // Atlas unavailable or install failed; fall back to v39 subset.
          subsetOk = section->tryInstallEmbeddedGlyphSubset(it->second->contentHash());
        }
        // Log skipped subset reloads distinctly so the trace shows
        // "atlas already covering" vs "both install failed" -- previously
        // both produced atlas=0 subset=0 with no heap delta.
        const bool subsetSkippedForAtlas = atlasUsable && subsetNeedsReload && !subsetOk;
        LOG_INF("ERA", "Lazy glyph data reload: atlas=%d subset=%d%s (free %u->%u, maxAlloc %u->%u)",
                atlasOk, subsetOk, subsetSkippedForAtlas ? " (subset skipped: atlas covering)" : "",
                freeBefore, ESP.getFreeHeap(), maxAllocBefore, ESP.getMaxAllocHeap());
        auto glyphFontData = [&](uint8_t style) -> const EpdFontData* {
          if (section->glyphAtlasInstalled()) {
            if (const EpdFontData* atlas = section->glyphAtlasFontDataForStyle(style)) return atlas;
          }
          return section->embeddedFontDataForStyle(style);
        };
        renderer.setEmbeddedGlyphData(curFontId, glyphFontData(0), glyphFontData(1), glyphFontData(2),
                                      glyphFontData(3));
      }
    }

    POST_BT_STEP("pre renderContents");
    const auto start = millis();
    renderContents(*cachedRenderPage_, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                   orientedMarginLeft);
    POST_BT_STEP("post renderContents");
    LOG_DBG("ERS", "Rendered page in %dms (cache %s)", millis() - start, cacheHit ? "hit" : "miss");
    // v18.9.9.9: capture the drained MaxAlloc BEFORE the cache-drop path
    // frees ~10 KB of page DOM back to the allocator. Without this snapshot,
    // the post-render escalation check further down sees the RECOVERED
    // heap and never fires, even though the just-run deserialize proved
    // the session heap can't fit another page-turn deserialize.
    postRenderDrainedMaxAlloc_ = ESP.getMaxAllocHeap();
    // CrumBLE 4.3 heap-regression revert: release the page DOM immediately
    // after render so it doesn't fragment the heap around BT events.
    // v18.9.9.4: keep the DOM alive when BT is linked on a non-simple book
    // so subsequent re-renders (BT popup, focus change, header refresh)
    // reuse it instead of re-deserializing under tight post-BT MaxAlloc.
    // Dropped on next page turn (cache-miss branch above) or on BT drop
    // (next render sees btLinkedNow=false, refreshes into no-cache mode).
    if (!btLinkedNow || simpleRenderingActive_) {
      cachedRenderPage_.reset();
      cachedRenderSection_ = nullptr;
      cachedRenderSpine_ = -1;
      cachedRenderPageIndex_ = -1;
    }
  }
streamedRenderDone:;
  // v18.9.9.134: BT re-enable path for BOTH streamed and DOM render paths.
  // My v132 fix put this check inside renderContents(), which the streamed
  // render path skips via `goto streamedRenderDone`. Result: force-simple
  // books (which ALWAYS take streamed) never fired the gate -- BT stayed
  // off after chapter-boundary section rebuild until user paged forward
  // several times. Now the check lives here at the shared join point.
  // Streamed render has no image blocks that could partial-decode, so we
  // don't need the imageRepaintUnsafeNow check for that path.
  if (bleReEnableHeldForImagePage &&
      (simpleRenderingActive_ || renderer.suppressImages())) {
    bleReEnableHeldForImagePage = false;
    LOG_INF("ERS", "Clean BLE-safe streamed render after rebuild (images suppressed); "
                   "re-enabling BLE for bonded remote reconnect");
    if (Section::pageHeapReserveHeld()) {
      const uint32_t freeBefore = ESP.getFreeHeap();
      Section::releasePageHeapReserveForBtEnable();
      LOG_INF("ERS", "Post-rebuild re-enable: released page heap reserve (free %u->%u)",
              freeBefore, ESP.getFreeHeap());
    }
    prewarmReaderTextBuffer(renderer);
    BluetoothHIDManager::getInstance().requestEnableLater();
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  // v18.9.9.471: debounce to every 3rd page turn. onExit and onBeforeDeepSleep
  // always save unconditionally, so a hard power-off can lose at most 2
  // pages. 3× fewer SD writes = 3× less FAT stress and correspondingly less
  // "Could not save progress" exposure. If a prior save is pending recovery
  // (pendingSyncSaveError set), force a save this turn to catch up.
  constexpr int kProgressSaveEveryNPages = 3;
  ++pagesSinceProgressSave_;
  const bool forceSave = pendingSyncSaveError || pagesSinceProgressSave_ >= kProgressSaveEveryNPages;
  if (forceSave) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
      pagesSinceProgressSave_ = 0;
      pendingSyncSaveError = false;
    } else {
      pendingSyncSaveError = true;
    }
  }
  queueCompletionPromptIfNeeded();

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  // CrumBLE Phase 1 fast-open: flip the gate so loop() picks up the
  // deferred init on the next tick. Only the FIRST render needs to set
  // this -- subsequent re-renders idempotently keep it true.
  firstRenderCompleted_ = true;

  // v18.9.9.8: post-render heap-floor escalation. If we're BT-linked and
  // the render just drained MaxAlloc below the sustainable-session floor,
  // any subsequent operation (BT re-subscribe on link blip, next page turn
  // deserialize, drawer open) will fail and the reader loop() will grind
  // to a halt with buttons registering but no response. Detect this here
  // and trigger Level 1 defrag (or Level 2/3 if defrag budget spent),
  // BEFORE the user tries to page-turn into a wall.
  //
  // Threshold sized to survive one more page deserialize on this book's
  // 7-8 KB heap-consumption profile: MaxAlloc 4000 leaves headroom for a
  // small page + BT event allocs. Books with lighter pages will rarely
  // trip this; books with heavy tables will escalate through the
  // graduated levels the same as page-load-refuse triggers.
  constexpr uint32_t POST_RENDER_MIN_MAX_ALLOC = 4000;
  const bool btLinkedForFloorCheck = BluetoothHIDManager::getInstance().isEnabled() &&
                                     SETTINGS.bleBondedDeviceAddr[0] != '\0' &&
                                     BluetoothHIDManager::getInstance().isConnected(SETTINGS.bleBondedDeviceAddr);
  // v18.9.9.9: use the maxAlloc captured immediately after renderContents
  // returned, BEFORE the cache-drop path freed the page DOM. That's the
  // ACTUAL drained heap floor; post-cache-drop value is misleadingly high.
  const uint32_t postRenderMaxAlloc =
      postRenderDrainedMaxAlloc_ != 0 ? postRenderDrainedMaxAlloc_ : ESP.getMaxAllocHeap();
  postRenderDrainedMaxAlloc_ = 0;
  if (btLinkedForFloorCheck && postRenderMaxAlloc < POST_RENDER_MIN_MAX_ALLOC) {
    if (!layoutDefragRetryAttempted_ && !layoutSimpleRetryAttempted && !simpleRenderingActive_) {
      LOG_INF("ERS",
              "Post-render floor breach (BT linked, drained maxAlloc=%u < %u); Layer 1 defrag "
              "silent-restart before next page-turn fails",
              postRenderMaxAlloc, POST_RENDER_MIN_MAX_ALLOC);
      layoutDefragRetryAttempted_ = true;
      // v18.9.9.32: resume where user is currently reading (which is
      // currentSpineIndex, already reflected in progress.bin because the
      // just-completed render committed it -- but pass explicitly for
      // consistency with the pre-render defrag sites).
      silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction::EnableBt, currentSpineIndex);
      // never returns
    } else if (!simpleRenderingActive_) {
      LOG_INF("ERS",
              "Post-render floor breach (BT linked, drained maxAlloc=%u); Layer 2 Simple Rendering "
              "compat mode + silent-restart (defrag budget already spent)",
              postRenderMaxAlloc);
      writeSimpleRenderingSidecar(epub->getCachePath(), readerActivePath_);
          if (APP_STATE.compatUserDisabledThisSession) { armCompatReenabledToast(); APP_STATE.compatUserDisabledThisSession = false; }
      section->clearCache();
      // v18.9.9.32: resume on the same chapter -- the user was mid-read
      // when the post-render heap floor breached, so landing them back
      // where they are is the right default.
      silentRestartToReaderWithDefragRetryAtSpine(ReaderPostBootAction::EnableBt, currentSpineIndex);
      // never returns
    }
    // else: Simple Rendering already active. The streamed render path
    // (Section::renderPageStreamed under simpleRenderingActive_ && BT
    // linked) has a peak footprint of ~500 bytes per page, so we should
    // never reach here with simpleRenderingActive_ = true. If we do, it's
    // a diagnostic edge case -- log and continue; the terminate handler
    // still catches a bad_alloc if one somehow slips through.
    else {
      LOG_ERR("ERS",
              "Post-render floor breach with Simple Rendering already active (drained maxAlloc=%u). "
              "Streamed render should have prevented this. Continuing without escalation.",
              postRenderMaxAlloc);
    }
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                  SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                  SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                  SETTINGS.tableRendering,
                                  // v18.9.9.455 REVERTED in v457: see main useprebakeFallback comment.
                                  SETTINGS.optimizeChapterIndexing != 0 && !simpleRenderingActive_,
                                  /*forceSimpleRendering=*/simpleRenderingActive_)) {
    return;
  }

  if (!MemoryBudget::hasHeapForOptionalEpubRebuild("ERS", "silent next-chapter indexing", nextSpineIndex)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d (free=%u, maxAlloc=%u)", nextSpineIndex, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                     SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                     SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                     SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                     SETTINGS.tableRendering, nullptr, nullptr, nullptr,
                                     /*forceSimpleRendering=*/simpleRenderingActive_,
                                     /*suppressTablesOnly=*/false)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  } else {
    LOG_DBG("ERS", "Silent indexing complete: chapter=%d pages=%u free=%u maxAlloc=%u", nextSpineIndex,
            nextSection.pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(const Page& page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  POST_BT_STEP("renderContents enter");
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  POST_BT_STEP("renderContents pre scan-pass");
  page.renderText(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  POST_BT_STEP("renderContents post scan-pass");
  scope.endScanAndPrewarm();
  POST_BT_STEP("renderContents post prewarm");
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);
  (void)heapBefore;
  (void)heapAfter;

  const bool pageHasImages = page.hasImages();
  lastRenderedPageHadImages_ = pageHasImages;
  // CrumBLE: NimBLE holds ~90 KB while the remote is up, leaving ~24 KB
  // contiguous -- not enough for the grayscale re-render pass (it would starve
  // glyphs, the "5% of characters" bug). Gate the AA re-render on the remote
  // being up. We gate on the stack being enabled rather than a mid-render heap
  // threshold: measured here (after the glyph prewarm) maxAlloc reads well below
  // the ~82 KB idle value even with the remote OFF, which wrongly skipped work.
  //
  // Images are NOT blanket-suppressed here: the image converters already apply
  // their own per-image heap check, so a light book keeps an image that fits
  // even with the remote on, and a heavy book drops images that don't fit. A
  // blanket "text-only under the remote" hid images that would have rendered
  // fine -- lost functionality -- so we leave per-image suppression to the
  // converters and only gate the (whole-page) AA re-render here.
  const bool bleConnected = BluetoothHIDManager::getInstance().isEnabled();
  const bool needsImageGrayscale = pageHasImages;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || needsImageGrayscale;

  renderer.takeRenderStarved();        // clear stale; capture only this render's failures
  renderer.takeImageRepaintUnsafe();   // clear stale; capture only this render's uncached images

  // 4.5.5+: highlight rendering moved from underline (solid 2-px bar) to
  // faux-bold via per-word overprint in renderSavedHighlightsOverlay. No
  // ghost-clear refresh strategy is needed for the bold path -- the dark
  // pixels in faux-bold are glyph shapes, not a continuous bar, and ghost
  // the same way regular text does (which is to say: handled cleanly by
  // FAST_REFRESH between pages). The old prevPageHadHighlights /
  // ghostClearOnNextRender_ machinery is now dead code; renderSavedHighlightsOverlay
  // leaves prevPageHadHighlights false so no special path ever fires.

  page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop,
              ReaderUtils::readerForegroundBlack());

  // 4.5.5: layer persistent highlights on top of the rendered text. Runs
  // after page.render so the solid underline draws beneath the glyphs (no
  // z-order conflict with the text raster). Also updates
  // prevPageHadHighlights so the NEXT render knows to promote refresh.
  renderSavedHighlightsOverlay(page, orientedMarginLeft, orientedMarginTop);

  // Note when the BLE remote came up. The connect handshake makes NimBLE grab
  // its ~58 KB and churn temporary buffers, which briefly spikes heap pressure
  // — enough to starve a single render even on books that otherwise read fine
  // with BLE. We ignore starvation during a short post-connect grace window so
  // we only drop Bluetooth for books that are *genuinely* unrenderable with it.
  const bool btOn = BluetoothHIDManager::getInstance().isEnabled();
  if (btOn && !btWasEnabled) btEnabledAtMs = millis();
  btWasEnabled = btOn;
  const bool pastBtConnectGrace = btOn && (millis() - btEnabledAtMs) > kBtConnectGraceMs;
  const bool renderStarvedNow = renderer.takeRenderStarved();
  const bool imageRepaintUnsafeNow = renderer.takeImageRepaintUnsafe();

  // #48: during the BLE connect grace window the handshake's heap spike can
  // transiently starve a single render (half-drawn glyphs) on a book that
  // otherwise reads fine with the remote. Don't paint that broken frame -- keep
  // the previous page on the panel and re-render once, after the grace window
  // settles (handled in loop()). We deliberately do NOT requestUpdate() here: a
  // tight in-grace retry loop previously forced a render right at the grace
  // boundary and tripped the auto-drop below on books that actually stay
  // connected.
  if (renderStarvedNow && btOn && !pastBtConnectGrace) {
    LOG_INF("ERS", "Render starved during BT connect grace; suppressing frame, retry after grace");
    pendingGraceReRender = true;
    return;  // skip displayBuffer: the half-drawn frame is never shown
  }

  // If this page still couldn't render with a BLE remote connected past the
  // grace window — an image failed to decode, or glyphs were starved (missing
  // text) — NimBLE's ~58 KB is the culprit and this book is genuinely
  // unrenderable with the remote. Silently hiding content (white gaps) is worse
  // than dropping the remote, so drop Bluetooth for the rest of this book, then
  // re-render this page cleanly (images AND text). The user reads with the
  // device buttons; re-enabling BLE from the reader menu will just starve
  // again. We return before any display so the broken frame is never shown —
  // the panel keeps the previous page until the re-render lands.
  if (pastBtConnectGrace && renderStarvedNow) {
    LOG_INF("ERS", "Page render starved with BLE up past grace; dropping Bluetooth for this book");
    BluetoothHIDManager::getInstance().requestDisableLater();
    if (!btDisabledForMemoryThisBook) {
      btDisabledForMemoryThisBook = true;
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_BT_LOWMEM_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_BT_LOWMEM_BODY));
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    }
    requestUpdate();
    return;
  }

  // This render is clean (not starved). Cancel any pending #48 grace re-render so
  // loop() doesn't fire a redundant repaint.
  pendingGraceReRender = false;

  // CrumBLE: a low-memory rebuild dropped BLE and latched a re-enable. Now that
  // this page rendered cleanly with BLE off, bring the remote back as soon as the
  // page is BLE-safe to *repaint* -- i.e. it has no images, or every image is now
  // in its .pxc cache (this BLE-off render decoded and cached them). A cached
  // image repaints via a tiny row-buffer blit with no decoder, so NimBLE's ~58 KB
  // no longer starves it. We only keep holding when the page decoded an image it
  // could NOT cache (partial/off-screen), since that one would re-decode and drop
  // BLE again. requestEnableLater() defers to the loop so NimBLE init doesn't
  // fight the next render; checkAutoReconnect() then relinks on the next press.
  // v18.9.9.132: when Simple Rendering is active (or suppressImages is on),
  // the section has no image blocks that could partial-decode -- the "unsafe
  // repaint" flag has nothing to gate against, and holding BT off just
  // strands the bonded remote until the user pages forward again. Field
  // repro: user crossed chapter boundary on force-simple book, BT dropped
  // to build section, section done, but bleReEnableHeldForImagePage stayed
  // true across renders (unclear why -- possibly a stale flag from before
  // Simple Rendering was set), and BT never came back.
  const bool imageSafeAlways = simpleRenderingActive_ || renderer.suppressImages();
  if (bleReEnableHeldForImagePage && (imageSafeAlways || !imageRepaintUnsafeNow)) {
    bleReEnableHeldForImagePage = false;
    LOG_INF("ERS", "Clean BLE-safe render after rebuild (images %s); re-enabling BLE for bonded remote reconnect",
            imageSafeAlways ? "suppressed" : "cached");
    // v18.9.9.74 Phase 4: release the page heap reserve BEFORE the deferred
    // enable drains. Post-rebuild heap sits ~3-8 KB below our free-heap floor
    // even after Phase 4's threshold drop; the reserve gives ~20 KB. Field
    // repro: chapter 15 build finished at free=63 KB, enable refused for 6 s
    // (crosspoint's 56 KB floor helps but pre-rebuild fragmentation kept us
    // just under). Releasing the reserve here mirrors the same call the
    // boot-dispatch EnableBt path does at line ~2477.
    if (Section::pageHeapReserveHeld()) {
      const uint32_t freeBefore = ESP.getFreeHeap();
      Section::releasePageHeapReserveForBtEnable();
      LOG_INF("ERS", "Post-rebuild re-enable: released page heap reserve (free %u->%u)",
              freeBefore, ESP.getFreeHeap());
    }
    // CrumBLE Phase 1 fast-open: pre-grow the glyph buffer before the
    // queued enable drains. NimBLE starts initializing on the next loop
    // tick; the buffer needs to be at high-water by then.
    prewarmReaderTextBuffer(renderer);
    BluetoothHIDManager::getInstance().requestEnableLater();
  }

  renderStatusBar();
  if (pendingBookmarkFeedback) {
    const char* msg = tr(STR_BOOKMARK_ADDED);
    switch (bookmarkFeedbackType) {
      case BookmarkFeedbackType::Added:
        msg = tr(STR_BOOKMARK_ADDED);
        break;
      case BookmarkFeedbackType::Removed:
        msg = tr(STR_BOOKMARK_REMOVED);
        break;
      case BookmarkFeedbackType::LimitReached:
        msg = tr(STR_BOOKMARK_LIMIT_REACHED);
        break;
    }
    drawToastBuffer(renderer, msg);
  }
  if (pendingCompletedFeedback) {
    const char* msg = completedFeedbackIsFinished ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED);
    drawToastBuffer(renderer, msg);
  }
  if (pendingTiltPageTurnFeedback) {
    const char* msg = tiltPageTurnFeedbackEnabled ? tr(STR_TILT_TO_TURN_ON) : tr(STR_TILT_TO_TURN_OFF);
    drawToastBuffer(renderer, msg);
  }
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  const auto logImagePageProfile = [](const uint32_t imageBlankDisplayMs, const uint32_t imageRestoreRenderMs,
                                      const uint32_t imageFinalDisplayMs) {
    LOG_DBG("ERS", "Image page profile: blank_display=%lums restore_render=%lums final_display=%lums",
            imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
  };

  // Only the toast's *dismiss* frame needs a clean half-refresh. Erasing the
  // white toast box on a fast refresh ghosts it ("mostly still visible"), so we
  // force HALF on the frame where a toast was shown last render but is gone now.
  // The appear frame can ride the normal fast cadence — drawing the box looks
  // fine; forcing HALF there too just added a second, jarring black flash.
  const bool toastShownThisRender =
      pendingBookmarkFeedback || pendingCompletedFeedback || pendingTiltPageTurnFeedback;
  const bool toastDismissedThisRender = toastShownLastRender && !toastShownThisRender;
  if (toastDismissedThisRender) {
    pagesUntilFullRefresh = 1;  // forces HALF_REFRESH to fully erase the toast box
  }
  toastShownLastRender = toastShownThisRender;

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique).
    // v18.9.9.167: also preclear the status-bar band when dynamic content is
    // shown, and re-renderStatusBar on step 2 so the page counter refreshes.
    // v18.9.9.411 REVERTED in v18.9.9.412: image-page singlePass with a single
    // HALF_REFRESH was worse than the original double-FAST -- HALF triggers
    // a full-panel wipe/repaint on image transitions on this X4 driver
    // (whereas on text-only content the wipe is subtle). Users reported a
    // pronounced full-page flash. Fell back to the ORIGINAL image-page flow
    // for both singlePass on and off; only text pages benefit from the toggle.
    int16_t imgX, imgY, imgW, imgH;
    if (page.getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // v18.9.9.207: back to re-render between the two flushes (v205's
      // snapshot/restore experiment spliced an extra store/restore cycle
      // into the BW-buffer machinery the grayscale AA pass also uses, and
      // image grays came out washed out). With clearStatusBarBand now
      // positioned off the BASE margins (it used to start a full text
      // reserve too high), the late-arriving region is just the thin
      // status row + the image rects themselves.
      //
      // v18.9.9.203: blank each image's own rect, not the union bounding
      // box. The union of two images far apart (or one tall figure)
      // swallowed a band of TEXT into the second flush.
      if (ReaderUtils::hasDynamicStatusBarContent()) {
        ReaderUtils::clearStatusBarBand(renderer, orientedMarginBottom);
      }
      page.blankImageRects(renderer, orientedMarginLeft, orientedMarginTop);
      const auto tImageBlankDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageBlankDisplayMs = millis() - tImageBlankDisplay;

      const auto tImageRestoreRender = millis();
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop,
                  ReaderUtils::readerForegroundBlack());
      renderStatusBar();
      const uint32_t imageRestoreRenderMs = millis() - tImageRestoreRender;
      const auto tImageFinalDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageFinalDisplayMs = millis() - tImageFinalDisplay;
      logImagePageProfile(imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
    } else {
      POST_BT_STEP("renderContents pre displayBuffer HALF");
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else if (ReaderUtils::shouldPreclearStatusBarBeforeFastRefresh(pagesUntilFullRefresh)) {
    // v18.9.9.167: double-FAST with status-bar preclear for text pages carrying
    // dynamic status-bar content. Kills counter ghosting under normal FAST_REFRESH.
    // v18.9.9.414: v413's skip-displayBuffer attempt made displayGrayBuffer
    // paint against stale panel state -> severe ghosting + slow-mo transitions.
    // Reverted to always running the FAST_REFRESH double-tap. singlePass now
    // relies solely on v412's restoreBwBuffer-before-displayGrayBuffer for
    // the status bar fix. Net effect for the user: same visual as baseline
    // (per user testing on v410), status bar no longer blanks for 500ms.
    POST_BT_STEP("renderContents pre displayBuffer status-preclear FAST");
    ReaderUtils::clearStatusBarBand(renderer, orientedMarginBottom);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop,
                ReaderUtils::readerForegroundBlack());
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  } else {
    POST_BT_STEP("renderContents pre displayWithRefreshCycle");
    // v18.9.9.414: reverted v413's skip-displayBuffer attempt. Always
    // display via displayWithRefreshCycle. singlePass now relies on
    // v412's restoreBwBuffer-before-displayGrayBuffer status bar fix.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();
  POST_BT_STEP("renderContents post displayBuffer");

  // Save bw buffer to reset buffer state after grayscale data sync
  const uint32_t bwStoreHeapBefore = esp_get_free_heap_size();
  const bool storedBwBuffer = renderer.storeBwBuffer();
  const uint32_t bwStoreHeapAfter = esp_get_free_heap_size();
  const auto tBwStore = millis();
  (void)bwStoreHeapBefore;
  (void)bwStoreHeapAfter;
  // Apply grayscale AA when the page wants it (text AA on, or the page has
  // images). Fast path: if we captured a BW backup, restore it after the gray
  // pass. Fallback: when there's no backup (a dense / picture-heavy page whose
  // PackBits backup exceeded the cap), RE-RENDER the BW page after the gray pass
  // -- the re-render needs no backup buffer, so AA survives on dense pages.
  //
  // BUT the re-render lays the whole page out a SECOND time, which needs real
  // contiguous heap. With the remote connected NimBLE holds ~90 KB and the
  // largest free block is only ~24 KB, so that extra pass starves glyph
  // rendering -- the "page shows only 5% of its characters" bug. So take the
  // re-render path only when the remote is OFF (ample heap); with it connected,
  // fall back to the original behavior and SKIP grayscale (render once in BW).
  // (We gate on the remote being up, not a mid-render heap threshold: measured
  // here -- after the prewarm -- maxAlloc reads well below idle even with the
  // remote off, which would wrongly skip AA on dense pages.) Net effect: AA
  // always works without a remote (full heap re-renders fine), light pages still
  // get AA via the backup path even with the remote on, and dense pages under
  // the remote render cleanly without AA instead of starving.
  const bool reRenderSafe = !bleConnected;
  const bool canApplyGrayscale = needsAnyGrayscale && (storedBwBuffer || reRenderSafe);
  // v18.9.9.346: publish for SleepActivity so a sleep overlay entering
  // from a text-only / highlight-only page skips the redundant
  // grayscale-plane rebuild passes that caused the triple-flash on
  // highlighted pages. canApplyGrayscale is the truest signal (accounts
  // for AA-off, heap-restricted images, etc.).
  APP_STATE.lastReaderPageNeededGrayscale = canApplyGrayscale;
  const bool grayscaleNeedsReRender = canApplyGrayscale && !storedBwBuffer;
  // Per-page AA status for diagnosis. DBG level (compiled out of the
  // production build). mode=re-render means we drew the page twice to avoid
  // the backup buffer; mode=backup is the fast snapshot/restore path.
  LOG_DBG("ERS", "AA: textAA=%s images=%s applied=%s mode=%s bwStore=%s freeHeap=%u",
          needsTextGrayscale ? "on" : "off", needsImageGrayscale ? "yes" : "no",
          canApplyGrayscale ? "YES" : "no", grayscaleNeedsReRender ? "re-render" : "backup",
          storedBwBuffer ? "ok" : "FAILED", esp_get_free_heap_size());

  // grayscale rendering
  if (canApplyGrayscale) {
    // CrumBLE 4.4: the LSB/MSB grayscale buffers are intentionally cleared to
    // 0x00 (their "no glyph here" state), separate from dark mode. The actual
    // foreground/background colour comes from the text draw paths below, so
    // pass readerForegroundBlack into page.render the same as the BW path.
    // v18.9.9.452: ALSO draw status bar into each grayscale plane. Without
    // this, displayGrayBuffer paints text but the status-bar band is empty,
    // so the bottom of the page reads as white until the follow-up
    // restoreBwBuffer + next refresh puts the status bar back. Field
    // symptom: bottom bar visibly "fills in" ~500 ms after each page turn.
    // (This was v410's fix which appears to have been reverted alongside
    // v415's singlePass rollback.)
    const bool fg = ReaderUtils::readerForegroundBlack();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    if (needsTextGrayscale) {
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop, fg);
    } else {
      page.renderImages(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    }
    renderStatusBar();
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    if (needsTextGrayscale) {
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop, fg);
    } else {
      page.renderImages(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    }
    renderStatusBar();
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    // Restore the BW framebuffer so the next partial refresh has the correct
    // base image. Fast path: blit it back from the compressed backup. Fallback
    // (BLE active, backup alloc failed): re-render the page in BW — one extra
    // render pass, but needs no backup buffer, which is the whole point: AA
    // works even when NimBLE has eaten the heap.
    if (storedBwBuffer) {
      renderer.restoreBwBuffer();
    } else {
      // Re-render fallback. Two parts mirror what restoreBwBuffer() does:
      // (1) put the BW page back in the framebuffer (here by redrawing it),
      // and (2) clean up the display controller's grayscale RAM state by
      // writing that BW framebuffer back to it. Skipping (2) was the cause
      // of the heavy ghosting — the panel kept the 4-level grayscale RAM
      // and the next refresh smeared against it.
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop,
                  ReaderUtils::readerForegroundBlack());
      renderStatusBar();
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_store_ok=%d "
            "bw_store_heap_before=%lu bw_store_heap_after=%lu bw_store_heap_delta=%ld "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, storedBwBuffer,
            bwStoreHeapBefore, bwStoreHeapAfter, (int32_t)bwStoreHeapAfter - (int32_t)bwStoreHeapBefore,
            tGrayLsb - tBwStore, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    if (storedBwBuffer) {
      // Restore the BW data when we skipped grayscale entirely.
      renderer.restoreBwBuffer();
    }
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_store_ok=%d "
            "bw_store_heap_before=%lu bw_store_heap_after=%lu bw_store_heap_delta=%ld "
            "bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, storedBwBuffer,
            bwStoreHeapBefore, bwStoreHeapAfter, (int32_t)bwStoreHeapAfter - (int32_t)bwStoreHeapBefore,
            tBwRestore - tBwStore, tEnd - t0);
  }
}

void EpubReaderActivity::renderSavedHighlightsOverlay(const Page& page, int marginLeft, int marginTop) {
  // Track whether THIS render drew anything; used at the end to set
  // prevPageHadHighlights for the next render's refresh-mode decision.
  bool drewAny = false;

  // 4.5.5: heap gate lowered 4 KB -> 1 KB. fillRect + zero-allocation
  // vector iteration here doesn't touch heap; the original 4K floor was
  // over-defensive. Field report: under BT-on heap pressure (maxAlloc
  // ~12-20 KB but periodic dips into single-digit KB during connect
  // grace windows) the 4K gate was tripping and the underline visually
  // disappeared. 1 KB is below anything fillRect's call-chain might
  // need and matches the existing api-files bailout-floor convention.
  if (ESP.getMaxAllocHeap() < 1024) { prevPageHadHighlights = false; return; }
  if (!section || section->pageCount <= 0) { prevPageHadHighlights = false; return; }

  const auto& bookmarks = BOOKMARKS.getBookmarks();
  if (bookmarks.empty()) { prevPageHadHighlights = false; return; }

  // Current page's progress slice [pageStart, pageEnd). Saved bookmarks
  // anchor on (spineIndex, progress) where progress is the page's start.
  // Matching a highlight to the current page means progress falls within
  // this half-open interval.
  const float pageStart = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  const float pageSlice = 1.0f / static_cast<float>(section->pageCount);
  const float pageEnd = pageStart + pageSlice;

  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);

  for (const auto& bm : bookmarks) {
    // Plain point bookmark (the bookmark icon, not a highlight). Identified
    // by an empty preview AND zero-length word range AND single-page-anchor.
    // We don't draw an underline for those -- the bookmark icon in the
    // status bar handles them.
    const bool hasWordRange = (bm.endWord > bm.startWord) || !bm.preview.empty();
    if (!hasWordRange) continue;

    // Which slice of the highlight lives on the current page? A single-page
    // highlight has both anchors on this page. Cross-page highlights are
    // rare today (HighlightRange is single-page; HighlightSingleWord is
    // start==end) but we handle them defensively: if the start anchors on
    // this page, highlight from startWord onward; if the end anchors here,
    // highlight up to endWord; else (full mid-range page) highlight all
    // words. INT16_MAX as a sentinel means "no upper bound on this page".
    const bool startOnPage = (bm.spineIndex == static_cast<uint16_t>(currentSpineIndex)) &&
                             (bm.progress >= pageStart) && (bm.progress < pageEnd);
    const bool endOnPage = (bm.endSpineIndex == static_cast<uint16_t>(currentSpineIndex)) &&
                           (bm.endProgress >= pageStart) && (bm.endProgress < pageEnd);
    if (!startOnPage && !endOnPage) {
      // Could still be a fully-spanning highlight (start before, end after
      // current page). Detect that: same-spine, start before this page,
      // end after. Treat as "underline every word on the page."
      const bool spansPage = (bm.spineIndex == static_cast<uint16_t>(currentSpineIndex)) &&
                             (bm.endSpineIndex == static_cast<uint16_t>(currentSpineIndex)) &&
                             (bm.progress < pageStart) && (bm.endProgress >= pageEnd);
      if (!spansPage) continue;
    }

    const int firstWord = startOnPage ? static_cast<int>(bm.startWord) : 0;
    const int lastWord = endOnPage ? static_cast<int>(bm.endWord) : INT16_MAX;

    // Walk page elements; count word index as we go (matches the
    // DictionaryWordSelectActivity::extractWords assignment). Note: that
    // function further splits a lineWords entry on internal em-dashes,
    // which we don't replicate here -- 1:1 with TextBlock::getWords() is
    // accurate for >99% of real text. Worst-case visual is one word off
    // for em-dash-containing lines; acceptable.
    int wordIdx = 0;
    for (const auto& element : page.elements) {
      if (!element || element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block) continue;
      const auto words = block->getWords();
      const auto wordXpos = block->getWordXpos();
      const auto wordStyles = block->getWordStyles();

      // CrumBLE 4.5.6 (ported from INX): highlight rendering pivoted from
      // faux-bold overprint to a sparse-ink lattice (every 2nd pixel on
      // every 2nd row painted black) drawn as an ADDITIVE overlay under
      // the word. Eye reads it as ~25% grey on the 1-bit panel. Wins over
      // the faux-bold path:
      //   1. Dot pattern (not contiguous mass) doesn't ghost on FAST
      //      refresh -- same anti-ghost property faux-bold had.
      //   2. Visually distinct from "this word is bold for emphasis" --
      //      bold is now back to meaning source-bold, not "highlighted."
      //      Faux-bold on LXGWWenKai (single-weight CJK) produced a
      //      false-bold reading.
      //   3. Reads as a single contiguous wash across multi-word
      //      highlights once we group adjacent in-range words on the
      //      same baseline into one rect.
      // The lattice is byte-aligned + ADDITIVE (pixels ANDed to black
      // rather than overwritten to a dither pattern), so text glyphs
      // underneath the highlight remain visible. Speed matches the
      // existing byte-aligned fillRect (~10x per-pixel drawPixel loop).
      const int lineBaseX = marginLeft + line.xPos;
      const int textY = marginTop + line.yPos;
      int runStartX = -1;
      int runEndX = -1;
      auto flushRun = [&]() {
        if (runStartX < 0) return;
        renderer.fillSparseInkLatticeInRect(runStartX, textY, runEndX - runStartX, lineHeight,
                                            /*step=*/2, /*state=*/true);
        drewAny = true;
        runStartX = -1;
        runEndX = -1;
      };
      for (size_t i = 0; i < words.size(); ++i) {
        const bool inRange = (wordIdx >= firstWord && wordIdx <= lastWord);
        if (inRange) {
          const auto wordView = words[i];
          const int baseX = lineBaseX + wordXpos[i];
          const int wordW = renderer.getTextWidth(fontId, wordView.c_str(), wordStyles[i]);
          if (runStartX < 0) {
            runStartX = baseX;
            runEndX = baseX + wordW;
          } else {
            runEndX = std::max(runEndX, baseX + wordW);
          }
        } else {
          flushRun();
        }
        ++wordIdx;
      }
      flushRun();
    }
  }

  // 4.5.5+: flag retained for any callers / tests that read it, but
  // intentionally set to false now -- bold-by-overprint doesn't leave a
  // ghost trail, so the post-highlight FULL_REFRESH dispatch (see
  // renderContents) is no longer needed.
  prevPageHadHighlights = false;
  (void)drewAny;
  (void)lineHeight;
}

void EpubReaderActivity::renderStatusBar() const {
  int currentPage = section->currentPage + 1;
  // v18.9.9.43 (task #28): during an in-progress build (C3 read-during-build),
  // section->pageCount is a partial-and-growing count. Sending it as-is would
  // paint "5/8" then "5/47" and read as if the book got longer between page
  // turns. Pass -1 as a sentinel so drawStatusBar renders "5/..." until
  // finalizeBuild commits the real total.
  float pageCount = section->isBuilding() ? -1.0f : static_cast<float>(section->pageCount);
  const float bookProgress = getCurrentBookProgressPercent();

  // v18.9.9.78: Stable Page Numbers (CrossInk parity, KOReader-style). When on,
  // the section-local "Page N of M" is replaced with a book-wide "Stable page X
  // of Y" derived from byte position + a configurable char-count divisor. Byte
  // position is what Epub::calculateProgress already computes; we reuse it so
  // there's no per-page char-offset table needed. Held stable across font /
  // margin changes because it's tied to source EPUB byte position, not the
  // rendered layout — that's the whole point of the mode.
  if (SETTINGS.showStablePageNumbers && epub && epub->getBookSize() > 0 && !section->isBuilding()) {
    const uint16_t divisor = SETTINGS.stablePageChars > 0 ? SETTINGS.stablePageChars : 1500;
    // v18.9.9.298: prefer the optimizer-provided visible-char count when
    // the book has a META-INF/crumble-stats.json manifest -- accurate to
    // actual text length. Fall back to getBookSize() for books not run
    // through the v298+ optimizer (which counts inflated HTML bytes and
    // over-reports for image-heavy or markup-heavy books).
    const size_t bookSize =
        (bookVisibleCharCount_ > 0) ? static_cast<size_t>(bookVisibleCharCount_) : epub->getBookSize();
    const int stableTotal = std::max(1, static_cast<int>((bookSize + divisor - 1) / divisor));
    // bookProgress is 0-100; convert to 0-1 for the multiplier.
    const float bookProgress01 = bookProgress / 100.0f;
    // v18.9.9.293: FLOOR + 1 (was ROUND). Old rounding jumped in both
    // directions -- sometimes two consecutive page turns rounded to the
    // same stable page (visual stall), sometimes one turn double-jumped
    // (5, 5, 7 instead of 5, 6, 7). Floor gives a monotonic sequence
    // where a single page-turn always shows the same or the next stable
    // page. Occasional stall is intentional -- it means "still on that
    // stable chunk"; user's real-page-turn feedback is still there.
    int stableCurrent = static_cast<int>(bookProgress01 * static_cast<float>(stableTotal)) + 1;
    if (stableCurrent < 1) stableCurrent = 1;
    if (stableCurrent > stableTotal) stableCurrent = stableTotal;
    currentPage = stableCurrent;
    pageCount = static_cast<float>(stableTotal);
  }

  std::string title;

  int textYOffset = 0;

  // v18.9.9.454: on the FIRST render after a section build completes, leave
  // the title empty so glyph loads for non-Latin titles don't block the
  // first-page paint. Title fills in from the second render onward
  // (typically the user's first page turn) — by then any needed SD reads
  // don't compete with the page-load critical path. Section-in-progress
  // renders are always title-less (Indexing popup covers the status bar
  // anyway; keeps the flag pattern simple). Automatic-turn banner is
  // exempt — it's user-triggered state, not book content.
  const bool suppressTitleForFirstRender =
      !automaticPageTurnActive && (section->isBuilding() || !postBuildFirstRenderShown_);

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(pageTurnDuration / 1000);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (suppressTitleForFirstRender) {
    // title stays empty this render; drawStatusBar's pre-clear covers the band.
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  // v18.9.9.454: mark the post-build first render as done so subsequent
  // renders include the title. Only flip once the build has actually
  // completed (isBuilding false) so the first non-Indexing render is the
  // one that pays the deferral cost.
  if (!section->isBuilding()) {
    postBuildFirstRenderShown_ = true;
  }

  const float rawProgress = (pageCount > 0) ? (static_cast<float>(section->currentPage) / pageCount) : 0.0f;
  const bool bookmarked = BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), rawProgress,
                                                       section->pageCount > 0 ? section->pageCount : 1);
  // v18.9.9.15 / v18.9.9.53 (task #38): reader-mode icon.
  //   - Compat mode (Simple Rendering) -> Reduced (flipped gauge). Takes
  //     precedence because prebake is deliberately bypassed under compat.
  //   - Prebake available AND user's current settings match the prebake
  //     fingerprint -> Prebake bolt. Sections load directly from
  //     sections-prebake/N.bin, no cold builds.
  //   - Prebake available AND user's settings DON'T match the prebake
  //     fingerprint (either drift since decline, or a fresh open where
  //     the user hasn't seen the prompt yet this session) -> PrebakeDeclined
  //     (bolt with slash). Every chapter is being cold-built. Signal to
  //     the user that they can visit the prompt or align settings to
  //     switch to the fast path.
  //   - No prebake manifest OR optimizeChapterIndexing disabled -> no
  //     icon; normal reader.
  BaseTheme::ReaderStatusIcon readerIcon = BaseTheme::ReaderStatusIcon::None;
  if (simpleRenderingActive_) {
    readerIcon = BaseTheme::ReaderStatusIcon::Reduced;
  } else if (prebakeManifest_.has_value() && SETTINGS.optimizeChapterIndexing != 0) {
    const PrebakeManifest& pm = *prebakeManifest_;
    const int32_t curFontId = SETTINGS.getReaderFontId();
    const float curLineComp = SETTINGS.getReaderLineCompression();
    const bool fingerprintMatch = pm.fontId == curFontId && pm.lineCompression == curLineComp &&
                                  pm.extraParagraphSpacing == SETTINGS.extraParagraphSpacing &&
                                  pm.forceParagraphIndents == SETTINGS.forceParagraphIndents &&
                                  pm.paragraphAlignment == SETTINGS.paragraphAlignment &&
                                  pm.hyphenationEnabled == SETTINGS.hyphenationEnabled &&
                                  pm.embeddedStyle == SETTINGS.embeddedStyle &&
                                  pm.imageRendering == SETTINGS.imageRendering &&
                                  pm.bionicReadingEnabled == SETTINGS.bionicReadingEnabled &&
                                  pm.guideReadingEnabled == SETTINGS.guideReadingEnabled;
    readerIcon = fingerprintMatch ? BaseTheme::ReaderStatusIcon::Prebake
                                  : BaseTheme::ReaderStatusIcon::PrebakeDeclined;
  }
  // v18.9.9.463: compute time-left from per-book pace × chapter-pages-remaining.
  // Simple within-chapter estimate — accurate when the user's pace is roughly
  // constant across chapters. 0 = insufficient pace data OR at chapter end,
  // in which case drawStatusBar suppresses the field.
  uint32_t timeLeftSeconds = 0;
  if (SETTINGS.statusBarTimeLeft && stats.avgSecondsPerForwardPage > 0 && !section->isBuilding() &&
      section->pageCount > 0 && section->currentPage < section->pageCount) {
    const uint32_t pagesRemaining =
        static_cast<uint32_t>(section->pageCount) - static_cast<uint32_t>(section->currentPage);
    timeLeftSeconds = pagesRemaining * static_cast<uint32_t>(stats.avgSecondsPerForwardPage);
  }
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, bookmarked,
                    ReaderUtils::readerDarkModeEnabled(), readerIcon, timeLeftSeconds);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  pageLoadRetryCount = 0;
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  pageLoadRetryCount = 0;
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}
bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    const int dataSize = f.read(data, 6);
    if (dataSize >= 4) {
      spineIndex = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      pageNumber = (int)((uint32_t)data[2] | ((uint32_t)data[3] << 8));
    }
    f.close();
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Compute margins exactly as render() does
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  marginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  // v18.9.9.455: this call site is static (sleep-page cache rebuild) and has
  // no instance flag access. Leaves per-book decline unenforced here — the
  // path is a rare sleep-time rebuild, not the hot in-book reading path, so
  // the perf impact is negligible.
  if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
                                SETTINGS.guideReadingEnabled, SETTINGS.tableRendering,
                                SETTINGS.optimizeChapterIndexing != 0)) {
    if (!MemoryBudget::hasHeapForOptionalEpubRebuild("SLP", "EPUB sleep-page cache rebuild", spineIndex)) {
      return false;
    }

    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding (free=%u, maxAlloc=%u)", spineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    // v18.9.6: static method has no access to member simpleRenderingActive_;
    // sleep-frame regeneration is a cold path (called by SleepActivity, not
    // during active reading), safe to fall back to normal rendering. If the
    // parse aborts here, the sleep cover just won't refresh -- the reader's
    // own path will hit the retry cycle on the user's next book-open.
    if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                    SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                    SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                    SETTINGS.tableRendering, []() {},
                                    nullptr, nullptr,
                                    /*forceSimpleRendering=*/false)) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
    LOG_DBG("SLP", "EPUB: section cache rebuilt for spine %d (pages=%u, free=%u, maxAlloc=%u)", spineIndex,
            section->pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
  // No displayBuffer call; caller (SleepActivity) handles that after compositing the overlay.
  return true;
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
