#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <BluetoothHIDManager.h>

#include "../boot_sleep/SleepActivity.h"
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <esp_system.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>

#include "../settings/BluetoothSettingsActivity.h"
#include "../settings/KOReaderSettingsActivity.h"
#include "BookSettingsDrawerActivity.h"
// CrumBLE: dictionary feature (ported from SEEK reader sumegig/seek-reader).
#include "DictionaryIndexBuildActivity.h"
#include "DictionaryWordSelectActivity.h"
#include "LookedUpWordsActivity.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"
#include "BookStatsActivity.h"
#include "CollectionsStore.h"
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

  // CrumBLE: free the in-RAM library index for the duration of the reading
  // session. Recently Added / All Books keep it loaded -- tens of KB of scattered
  // string allocations for a large library -- and holding it through reading
  // erodes the contiguous heap that BLE glyph rendering needs. On the full feature
  // set that pushed the largest free block below the font glyph group, so text
  // starved and the remote dropped ("Bluetooth couldn't stay connected"). It
  // auto-rebuilds from the on-disk JSON on the next Recently Added / All Books
  // visit, so the only cost is a one-time rewalk back at Home.
  LibraryIndex::getInstance().releaseMemory();

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
  prebakeManifest_.reset();
  prebakeLastSnapshot_ = {};
  prebakePromptShowing_ = false;
  deferredOnEnterPending_ = true;
  firstRenderCompleted_ = false;
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
  btManifestPromptAnsweredThisSession_ = false;
  pendingBleQuickConnect_ = false;
  pendingBleQuickConnectNoImages_ = false;
  pendingBleQuickConnectSettingsChanged_ = false;
  pendingBleQuickConnectPromptStage_ = -1;

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  epub->setupCacheDir();
  BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");

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
    if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
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
  SleepCoverAssets::prepareEpub(*epub);

  // Trigger first update
  requestUpdate();
}

// CrumBLE Phase 1 fast-open: pre-grow the glyph decompression buffer.
// Called inline at every BT-enable site so the buffer is ready before
// NimBLE eats heap. See header for the full rationale.
void EpubReaderActivity::prewarmReaderTextBuffer(GfxRenderer& renderer) {
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  fcm->prewarmCache(SETTINGS.getReaderFontId(),
                    "etaoinshrdlucmfwypvbgkjqxzETAOINSHRDLUCMFWYP.,;:'\"!?-0123456789", 0x0F);
  // Drop the prewarm's page-slot buffers immediately. We only need the
  // prewarm to grow the *shared glyph-group buffer* to its high-water
  // mark -- clearCache keeps that capacity via invalidateHotGroup.
  // Leaving the four sample-string slot buffers held (~10-15 KB total)
  // would fragment the heap right before NimBLE's allocation pass and
  // partially defeat the protection we're trying to add.
  fcm->clearCache();
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
  // heap. Skip if heap is already tight -- the drawer's own local gate will
  // fall back to its actions-only list, same as before. Tracking BLE state
  // too because mid-book Reader Activity re-entry (e.g. returning from a
  // settings sub-activity) can hit this with BLE already connected; in that
  // rare case we prefer the safe skip over a doomed build.
  {
    const auto heap = MemoryBudget::snapshot();
    if (MemoryBudget::hasHeap(heap, 40u * 1024u, 20u * 1024u)) {
      auto all = getSettingsList(&sdFontSystem.registry());
      readerSettingsCache_.reserve(20);
      for (auto& s : all) {
        if (s.category == StrId::STR_CAT_READER) {
          readerSettingsCache_.push_back(std::move(s));
        }
      }
      readerSettingsCache_.shrink_to_fit();
      LOG_INF("ERA", "Cached %u reader settings (heap free=%u maxAlloc=%u)",
              static_cast<unsigned>(readerSettingsCache_.size()), heap.freeHeap, heap.maxAllocHeap);
    } else {
      LOG_INF("ERA", "Skipping reader-settings cache build; heap too tight (free=%u maxAlloc=%u)",
              heap.freeHeap, heap.maxAllocHeap);
    }
  }

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
  if (!SETTINGS.optimizeChapterIndexing) return false;
  if (!prebakeManifest_.has_value() || prebakePromptShowing_) return false;
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

  if (!prebakeLastSnapshot_.initialised) snapshotCurrent();
  if (!snapChanged) return false;

  const bool mismatch =
      (pm.fontId != curFontId) ||
      (pm.lineCompression != curLineComp) ||
      (pm.extraParagraphSpacing != SETTINGS.extraParagraphSpacing) ||
      (pm.forceParagraphIndents != SETTINGS.forceParagraphIndents) ||
      (pm.paragraphAlignment != SETTINGS.paragraphAlignment) ||
      (pm.viewportWidth != curViewportW) ||
      (pm.viewportHeight != curViewportH) ||
      (pm.hyphenationEnabled != SETTINGS.hyphenationEnabled) ||
      (pm.embeddedStyle != SETTINGS.embeddedStyle) ||
      (pm.imageRendering != SETTINGS.imageRendering) ||
      (pm.bionicReadingEnabled != SETTINGS.bionicReadingEnabled) ||
      (pm.guideReadingEnabled != SETTINGS.guideReadingEnabled);

  if (!mismatch) {
    snapshotCurrent();  // user-driven drift but still valid; accept silently
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
  const std::string promptBody =
      "This book was prepared with different reader settings. Keep your current "
      "settings (chapters will rebuild against the live cache), or restore the "
      "prepared layout for instant chapter loads?";
  prebakePromptShowing_ = true;
  startActivityForResult(
      std::make_unique<ChoicePromptActivity>(
          renderer, mappedInput, "Settings differ from prepared layout",
          promptBody,
          std::vector<std::string>{"Keep my current settings", "Restore prepared layout"},
          /*ignoreInitialConfirmRelease=*/true),
      [this](const ActivityResult& result) {
        prebakePromptShowing_ = false;
        // ChoicePromptResult lives inside result.data. choice: 0 = "Keep
        // my current settings", 1 = "Restore prepared layout", -1 =
        // user backed out. Treat back-out as "Keep" -- the less
        // destructive default.
        int chosen = -1;
        if (const auto* cp = std::get_if<ChoicePromptResult>(&result.data)) {
          chosen = cp->choice;
        }
        const bool keepCurrent = result.isCancelled || chosen != 1;
        if (keepCurrent) {
          // User declined -- keep their current settings. Don't delete the
          // prebake (Section.cpp's clearCache only ever touches sections/,
          // never sections-prebake/), so if they later open the book again
          // and revert their settings, the cache is still there. Update
          // the snapshot to current SETTINGS so we don't keep firing the
          // prompt on every render tick.
          LOG_INF("ERA", "Prebake prompt: user declined, keeping current settings");
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
        if (section) section.reset();
        requestUpdate();
      });
  return true;
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

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

  BOOKMARKS.unload();
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
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
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
              SETTINGS.saveToFile();
              ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
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
    if (!section) {
      return;
    }

    // Step 3: section is ready. Connect.
    const bool noImages = pendingBleQuickConnectNoImages_;
    pendingBleQuickConnect_ = false;
    pendingBleQuickConnectNoImages_ = false;
    pendingBleQuickConnectPromptStage_ = -1;
    if (noImages) renderer.setSuppressImages(true);
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
      GUI.drawPopup(renderer, tr(STR_BT_CONNECTING));
      // CrumBLE Phase 1 fast-open: pre-grow the glyph buffer NOW, before
      // NimBLE eats heap. Cost (~20 ms) hides inside the Connecting
      // popup window we just drew.
      prewarmReaderTextBuffer(renderer);
      if (!btMgr.isEnabled()) btMgr.enable();
      btMgr.connectToDevice(SETTINGS.bleBondedDeviceAddr);
    }
    btManifestPromptAnsweredThisSession_ = true;  // we handled the manifest decision
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
                               hasDictionary, hasLookupHistory),
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
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
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
      // CrumBLE: pre-flight heap check. DictionaryWordSelect::extractWords
      // reserves a vector of WordInfo entries (~64 bytes each); with BLE
      // connected (~58 KB NimBLE state), the largest contiguous free block
      // can drop below the reserve size, and operator new throws
      // std::bad_alloc -> __cxxabiv1::__terminate -> reboot.
      // Bail out with an actionable alert before launching anything. The
      // LOOKUP menu entry only appears when Dictionary::exists(), so users
      // expect this to work; a clear message that names the BT cost is
      // friendlier than a silent crash. 25 KB threshold matches the worst
      // case of the reserve plus the index scan's working set.
      constexpr uint32_t LOOKUP_MIN_MAX_ALLOC = 25000;
      if (ESP.getMaxAllocHeap() < LOOKUP_MIN_MAX_ALLOC) {
        LOG_INF("ERS", "Lookup pre-flight: maxAlloc=%u below %u, raising alert",
                ESP.getMaxAllocHeap(), LOOKUP_MIN_MAX_ALLOC);
        strncpy(APP_STATE.pendingAlertTitle, tr(STR_LOW_MEMORY_LOOKUP_TITLE),
                sizeof(APP_STATE.pendingAlertTitle) - 1);
        strncpy(APP_STATE.pendingAlertBody, tr(STR_LOW_MEMORY_LOOKUP_BODY),
                sizeof(APP_STATE.pendingAlertBody) - 1);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
        break;
      }
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
      auto launchWordSelect = [this, pageShared, readerFontId, orientedMarginLeft, orientedMarginTop, cachePath,
                               orientation, nextPageFirstWord]() {
        startActivityForResult(
            std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(*pageShared), readerFontId,
                                                          orientedMarginLeft, orientedMarginTop, cachePath, orientation,
                                                          nextPageFirstWord),
            [this](const ActivityResult& result) { requestUpdate(); });
      };

      if (Dictionary::isIndexReady()) {
        launchWordSelect();
      } else if (Dictionary::loadCachedIndex()) {
        // Cache present AND loaded cleanly (~50ms). If the file existed
        // but was truncated/corrupt, loadCachedIndex returns false here
        // and we fall through to the prompt -- we'd rather ask the user
        // to consent to a rebuild than silently freeze inside the first
        // lookup() call (which is what happens if we proceed assuming
        // the cache loaded when it didn't).
        launchWordSelect();
      } else {
        startActivityForResult(
            std::make_unique<ChoicePromptActivity>(
                renderer, mappedInput, tr(STR_DICT_INDEX_PROMPT_TITLE), tr(STR_DICT_INDEX_PROMPT_BODY),
                std::vector<std::string>{tr(STR_DICT_INDEX_PROMPT_BUILD), tr(STR_DICT_INDEX_PROMPT_CANCEL)},
                /*ignoreInitialConfirmRelease=*/true),
            [this, launchWordSelect](const ActivityResult& promptResult) {
              int chosen = -1;
              if (const auto* cp = std::get_if<ChoicePromptResult>(&promptResult.data)) {
                chosen = cp->choice;
              }
              if (promptResult.isCancelled || chosen != 0) {
                requestUpdate();
                return;
              }
              startActivityForResult(std::make_unique<DictionaryIndexBuildActivity>(renderer, mappedInput),
                                     [this, launchWordSelect](const ActivityResult& buildResult) {
                                       if (buildResult.isCancelled) {
                                         requestUpdate();
                                         return;
                                       }
                                       launchWordSelect();
                                     });
            });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKED_UP_WORDS: {
      startActivityForResult(std::make_unique<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this](const ActivityResult& result) { requestUpdate(); });
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
          [this](const ActivityResult&) { requestUpdate(); });
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
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
    case EpubReaderMenuActivity::MenuAction::CONTROLS_OPTIONS:
      break;
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
                                         /*exitOnSuccessfulConnect=*/true),
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
  if (!epub) {
    return;
  }

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

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
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
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
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

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d (free=%u, maxAlloc=%u)", filepath.c_str(), currentSpineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                  SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                  SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                  SETTINGS.optimizeChapterIndexing != 0)) {
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
          LOG_INF("ERS", "Cache miss with BLE up (DEFLATE chapter); dropping BLE and deferring build to next render");
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

      // Animated indexing popup. The parser ticks the callback every
      // ~250 ms; we cycle the trailing dots so the user sees the system
      // is alive during the multi-second chapter build instead of
      // staring at a frozen popup.
      //
      // Pre-measure the widest frame ("Indexing...") and pass that as
      // minTextWidth to every drawPopup call. The box sizes itself once
      // off the widest frame and stays anchored on the same pixels for
      // every subsequent tick — without this floor, period vs space
      // glyph widths differ enough in Inter that the box visibly pulses
      // wider on the 3-dot frame.
      char widestBuf[40];
      snprintf(widestBuf, sizeof(widestBuf), "%s...", tr(STR_INDEXING));
      const int popupMinTextWidth =
          renderer.getTextWidth(UI_12_FONT_ID, widestBuf, EpdFontFamily::BOLD);

      // Left-anchor the text so "Indexing" stays pinned at a fixed
      // position and the trailing dots cycle to its right without
      // visibly shifting the word.
      GUI.drawPopup(renderer, tr(STR_INDEXING), popupMinTextWidth, /*leftAlignText=*/true);

      const auto popupFn = [this, popupMinTextWidth]() {
        static uint8_t dotPhase = 0;
        static constexpr const char* kDots[4] = {"", ".", "..", "..."};
        dotPhase = (dotPhase + 1) % 4;
        char buf[40];
        snprintf(buf, sizeof(buf), "%s%s", tr(STR_INDEXING), kDots[dotPhase]);
        GUI.drawPopup(renderer, buf, popupMinTextWidth, /*leftAlignText=*/true);
      };

      bool imagesWereSuppressed = false;
      bool layoutAbortedForLowMemory = false;
      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                      SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                      SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                      SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled, popupFn,
                                      &imagesWereSuppressed, &layoutAbortedForLowMemory)) {
        if (layoutAbortedForLowMemory) {
          LOG_ERR("ERS", "EPUB section layout aborted for low heap; chapter exceeds safe layout memory");
        }
        if (!layoutAbortedForLowMemory) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
        }
        section.reset();
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

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
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
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      // The auto-retry already tried clearing+rebuilding this chapter's cache. If
      // it still won't load, the SD filesystem is likely the problem (it can't be
      // self-healed on-device) -- point the user at the recovery options.
      renderer.drawCenteredText(UI_10_FONT_ID, 332, tr(STR_PAGE_LOAD_ERROR_HINT), true);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    pageLoadRetryCount = 0;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  if (!saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
    pendingSyncSaveError = true;
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
                                  SETTINGS.optimizeChapterIndexing != 0)) {
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
                                     SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  } else {
    LOG_DBG("ERS", "Silent indexing complete: chapter=%d pages=%u free=%u maxAlloc=%u", nextSpineIndex,
            nextSection.pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->renderText(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);
  (void)heapBefore;
  (void)heapAfter;

  const bool pageHasImages = page->hasImages();
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
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);

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
  if (bleReEnableHeldForImagePage && !imageRepaintUnsafeNow) {
    bleReEnableHeldForImagePage = false;
    LOG_INF("ERS", "Clean BLE-safe render after rebuild (images cached); re-enabling BLE for bonded remote reconnect");
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
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      const auto tImageBlankDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageBlankDisplayMs = millis() - tImageBlankDisplay;

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      const auto tImageRestoreRender = millis();
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      const uint32_t imageRestoreRenderMs = millis() - tImageRestoreRender;
      const auto tImageFinalDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageFinalDisplayMs = millis() - tImageFinalDisplay;
      logImagePageProfile(imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

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
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    if (needsTextGrayscale) {
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    }
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    if (needsTextGrayscale) {
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    }
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
      renderer.clearScreen();
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
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

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float bookProgress = getCurrentBookProgressPercent();

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(pageTurnDuration / 1000);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

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

  const float rawProgress = (pageCount > 0) ? (static_cast<float>(section->currentPage) / pageCount) : 0.0f;
  const bool bookmarked = BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), rawProgress,
                                                       section->pageCount > 0 ? section->pageCount : 1);
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, bookmarked);
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
  if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
                                SETTINGS.guideReadingEnabled, SETTINGS.optimizeChapterIndexing != 0)) {
    if (!MemoryBudget::hasHeapForOptionalEpubRebuild("SLP", "EPUB sleep-page cache rebuild", spineIndex)) {
      return false;
    }

    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding (free=%u, maxAlloc=%u)", spineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                    SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                    SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled, []() {})) {
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

  renderer.clearScreen();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
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
