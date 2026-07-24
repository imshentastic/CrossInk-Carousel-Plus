#include "BookSettingsDrawerActivity.h"

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "../../CrossPointState.h"
#include "../../SilentRestart.h"
#include "../../util/SettingsViewCache.h"
#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"  // prewarmReaderTextBuffer
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"  // sdFontSystem for SD-aware FONT_FAMILY / FONT_SIZE rows
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// v18.9.9.59: path-aware compat sidecar accessors for the drawer's compat
// toggle row. Mirror ROA's helpers, but keyed on APP_STATE.readerActivePath
// so the toggle acts on whichever render path this book is currently on
// (compat_prepared.flag vs compat_custom.flag).
constexpr const char* kDrawerCompatFlagPrepared = "/compat_prepared.flag";
constexpr const char* kDrawerCompatFlagCustom = "/compat_custom.flag";

const char* drawerActiveCompatFlagBasename() {
  return APP_STATE.readerActivePath == 0 ? kDrawerCompatFlagPrepared : kDrawerCompatFlagCustom;
}

std::string drawerActiveBookCachePath() {
  if (APP_STATE.openEpubPath.empty()) return {};
  return Epub::cachePathForFilePath(APP_STATE.openEpubPath, "/.crosspoint");
}

bool drawerReadCompatSidecar() {
  const auto cachePath = drawerActiveBookCachePath();
  if (cachePath.empty()) return false;
  return Storage.exists((cachePath + drawerActiveCompatFlagBasename()).c_str());
}

void drawerWriteCompatSidecar() {
  const auto cachePath = drawerActiveBookCachePath();
  if (cachePath.empty()) return;
  const std::string path = cachePath + drawerActiveCompatFlagBasename();
  if (Storage.exists(path.c_str())) return;
  HalFile f;
  if (Storage.openFileForWrite("DRW", path.c_str(), f)) {
    f.close();
    LOG_INF("DRW", "Compat sidecar written: %s", path.c_str());
  } else {
    LOG_ERR("DRW", "Failed to write compat sidecar: %s", path.c_str());
  }
}

void drawerClearCompatSidecar() {
  const auto cachePath = drawerActiveBookCachePath();
  if (cachePath.empty()) return;
  const std::string path = cachePath + drawerActiveCompatFlagBasename();
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    LOG_INF("DRW", "Compat sidecar cleared: %s", path.c_str());
  }
}

bool isLandscape(const GfxRenderer& r) {
  const auto o = r.getOrientation();
  return o == GfxRenderer::Orientation::LandscapeClockwise || o == GfxRenderer::Orientation::LandscapeCounterClockwise;
}

// v18.9.9.25: mirror SettingsActivity's isCompatLockedSetting so the drawer
// row-render decorates the same four/five settings compat forces (embedded
// style, images, tables, bionic reading, guide reading) with the "(Compat)"
// suffix. Kept identical to the SettingsActivity helper -- if either grows
// a setting, add to both.
bool isDrawerCompatLockedSetting(const SettingInfo& info) {
  switch (info.nameId) {
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

// Read the value text for a single SettingInfo-bound row.
std::string valueTextForSetting(const SettingInfo& info) {
  if (info.type == SettingType::TOGGLE && info.valuePtr != nullptr) {
    return SETTINGS.*(info.valuePtr) ? I18N.get(StrId::STR_STATE_ON) : I18N.get(StrId::STR_STATE_OFF);
  }
  if (info.type == SettingType::ENUM) {
    // CrumBLE: resolve the current option index. valueGetter wins -- it's
    // what FONT_FAMILY uses to combine built-in entries with SD card font
    // names (see buildFontFamilySetting in SettingsList.h). Fall back to
    // valuePtr when no getter is set (the common built-in path). Without
    // this branch, FONT_FAMILY always falls through to "" and the drawer
    // shows a blank value.
    uint8_t cur = 0;
    bool haveCur = false;
    if (info.valueGetter) {
      cur = info.valueGetter();
      haveCur = true;
    } else if (info.valuePtr != nullptr) {
      cur = SETTINGS.*(info.valuePtr);
      haveCur = true;
    }
    if (!haveCur) return std::string{};
    // Prefer enumStringValues over enumValues. SD-aware settings build
    // the runtime string list (built-in label + SD family names for
    // FONT_FAMILY, point-size labels for SD FONT_SIZE) and leave the
    // StrId enumValues empty. Built-in-only settings use enumValues.
    if (!info.enumStringValues.empty()) {
      if (cur < info.enumStringValues.size()) return info.enumStringValues[cur];
      return std::string{};
    }
    // CrumBLE 4.2: when the setting uses .withEnumRawValues({...}), the
    // raw stored value is NOT the display index -- map raw -> display
    // index first. Otherwise SETTINGS.fontFamily=1 (BITTER) silently
    // indexes off the end of a 1-element enumValues vector when
    // LEXENDDECA + CHAREINK are OMIT'd, and the in-book drawer renders a
    // blank font label.
    if (!info.enumRawValues.empty()) {
      for (size_t i = 0; i < info.enumRawValues.size() && i < info.enumValues.size(); i++) {
        if (info.enumRawValues[i] == cur) return I18N.get(info.enumValues[i]);
      }
      return std::string{};
    }
    if (cur < info.enumValues.size()) {
      return I18N.get(info.enumValues[cur]);
    }
    return std::string{};
  }
  if (info.type == SettingType::VALUE && info.valuePtr != nullptr) {
    return std::to_string(SETTINGS.*(info.valuePtr));
  }
  return std::string{};
}

void applyDeltaToSetting(const SettingInfo& info, int delta) {
  // CrumBLE: handle valueGetter/valueSetter (used by SD-aware settings like
  // FONT_FAMILY) BEFORE the valuePtr-only paths. Without this, left/right
  // on the FONT_FAMILY row would silently no-op when an SD font was
  // installed and the user expected to cycle through built-in + SD names.
  if (info.type == SettingType::ENUM && info.valueGetter && info.valueSetter) {
    int count = static_cast<int>(info.enumStringValues.size());
    if (count == 0) count = static_cast<int>(info.enumValues.size());
    if (count <= 0) return;
    int next = static_cast<int>(info.valueGetter()) + delta;
    next = ((next % count) + count) % count;
    info.valueSetter(static_cast<uint8_t>(next));
    return;
  }
  if (info.valuePtr == nullptr) return;
  if (info.type == SettingType::TOGGLE) {
    SETTINGS.*(info.valuePtr) = (SETTINGS.*(info.valuePtr) == 0) ? 1 : 0;
    return;
  }
  if (info.type == SettingType::ENUM) {
    // CrumBLE 4.2: cycle through enumRawValues when set -- the stored
    // value is raw, but the display order is encoded by enumRawValues
    // index. Treating raw value as display index (the old behaviour)
    // would land on raw values that aren't even in the displayed set
    // (e.g. cycling past Bitter would set fontFamily=0 == LEXENDDECA,
    // which the slim build no longer ships, and then the picker would
    // need the boot-time clamp to undo the damage).
    if (!info.enumRawValues.empty()) {
      const int count = static_cast<int>(info.enumRawValues.size());
      if (count <= 0) return;
      const uint8_t curRaw = SETTINGS.*(info.valuePtr);
      int curDisplay = 0;
      for (int i = 0; i < count; i++) {
        if (info.enumRawValues[i] == curRaw) {
          curDisplay = i;
          break;
        }
      }
      int next = curDisplay + delta;
      next = ((next % count) + count) % count;
      SETTINGS.*(info.valuePtr) = info.enumRawValues[next];
      return;
    }
    // Prefer enumStringValues count when present (SD-aware FONT_SIZE list
    // has its sizes there, not in enumValues).
    int count = static_cast<int>(info.enumStringValues.size());
    if (count == 0) count = static_cast<int>(info.enumValues.size());
    if (count <= 0) return;
    int next = static_cast<int>(SETTINGS.*(info.valuePtr)) + delta;
    next = ((next % count) + count) % count;
    SETTINGS.*(info.valuePtr) = static_cast<uint8_t>(next);
    return;
  }
  if (info.type == SettingType::VALUE) {
    const int step = std::max<int>(1, info.valueRange.step);
    int next = static_cast<int>(SETTINGS.*(info.valuePtr)) + delta * step;
    if (next < info.valueRange.min) next = info.valueRange.max;
    if (next > info.valueRange.max) next = info.valueRange.min;
    SETTINGS.*(info.valuePtr) = static_cast<uint8_t>(next);
  }
}

}  // namespace

BookSettingsDrawerActivity::BookSettingsDrawerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const std::vector<SettingInfo>* externalReaderSettings,
                                                       const std::optional<PxcManifest>* pxcManifest,
                                                       int initialExpandedGroupId)
    : Activity("BookSettingsDrawer", renderer, mappedInput),
      externalReaderSettings_(externalReaderSettings && !externalReaderSettings->empty() ? externalReaderSettings
                                                                                         : nullptr),
      pxcManifest_(pxcManifest),
      pendingInitialExpandedGroupId_(initialExpandedGroupId) {}

void BookSettingsDrawerActivity::onEnter() {
  Activity::onEnter();
  SET_CHECKPOINT("drawer:onEnter");

  // Remember whether BLE was on so onExit() can request its return after the
  // reader's re-layout drains. Settings toggles silently drop BLE (no prompt
  // anymore -- the disconnect is just a side-effect of the layout change).
  bleWasEnabledOnEntry_ = BluetoothHIDManager::getInstance().isEnabled();

  // Cache the underlying reader page so we can preserve it through every
  // partial-refresh redraw without re-rendering the EPUB. If the malloc
  // for the compressed backup fails (heap fragmentation under BLE
  // pressure is the usual cause), renderDrawer() falls back to either
  // the reader's existing backup or to whatever's already in the
  // framebuffer — both better than painting onto a cleared screen.
  readerBufferStored = renderer.storeBwBuffer();
  if (!readerBufferStored) {
    LOG_INF("BSD", "Failed to snapshot reader page; drawer will fall back to existing BW backup or in-place framebuffer");
  }

  // v18.9.9.25: OpenBookSettingsDrawer boot-continuation path -- the caller
  // asked us to open with a specific group pre-expanded. Build the settings
  // source now (we're on a fresh ~90 KB heap, so ensureSettingsSrcBuilt
  // should succeed) and set expandedGroupId_ before the first buildItems
  // pass so the expanded rows appear immediately.
  if (pendingInitialExpandedGroupId_ >= 0) {
    if (ensureSettingsSrcBuilt()) {
      expandedGroupId_ = pendingInitialExpandedGroupId_;
      LOG_INF("BSD", "Auto-expanded group %d after silent-restart continuation", expandedGroupId_);
    } else {
      LOG_INF("BSD", "OpenBookSettingsDrawer continuation: heap still tight; skipping auto-expand");
    }
    pendingInitialExpandedGroupId_ = -1;
  }

  buildItems();
  layoutDrawer();
  selectedIndex = 0;
  scrollOffset = 0;
  initialConfirmReleased = false;
  requestUpdate();
}

void BookSettingsDrawerActivity::onExit() {
  Activity::onExit();
  // Persist any changes the user made before the reader resumes.
  SETTINGS.saveToFile();
  // CrumBLE: if BLE was on at entry and we silently dropped it to apply a
  // layout change, request it back. Deferred so the drain fires AFTER the
  // reader's re-layout completes (main loop runs tryEnableIfRequested every
  // tick). User-initiated disconnect via the "BT Quick Disconnect" action
  // also clears bleWasEnabledOnEntry_-checked state correctly: the state
  // check is "was on AND is now off". If user explicitly disconnected, we
  // still bring it back -- but that's intentional; the disconnect action
  // closes the drawer immediately so this path won't fire (drawer onExit
  // happens before the disconnect lambda's finish() returns, but the
  // disconnect happens INSIDE the lambda before finish()). To be safe we
  // skip auto-reconnect when the disconnect action was taken (no settings
  // change to re-layout for); approximate via settingsChanged.
  auto& bt = BluetoothHIDManager::getInstance();
  if (bleWasEnabledOnEntry_ && !bt.isEnabled() && settingsChanged) {
    SET_CHECKPOINT("drawer:bt-re-enable-onexit");
    // CrumBLE Phase 1 fast-open: pre-grow the glyph buffer before the
    // deferred enable drains. The drawer dropped BLE on entry to apply
    // a layout change; the post-layout re-enable needs the buffer at
    // high-water mark before NimBLE eats heap.
    EpubReaderActivity::prewarmReaderTextBuffer(renderer);
    bt.requestEnableLater();
    SET_CHECKPOINT("drawer:bt-request-queued");
  }
}

// CrumBLE 4.5.6 (revision 2): mapping from a reader-category SettingInfo's
// nameId to its DrawerGroup. Settings not listed here are excluded from the
// drawer (deferred to Reader Options Main Settings). The 13-row simplified
// inventory was chosen with the INX drawer as a reference: high-frequency
// mid-read tweaks stay in the drawer (font, layout, reading aids, images);
// set-once defaults (EMBEDDED_STYLE, TEXT_AA, TEXT_DARKNESS, GUIDE_READING)
// drop out. Hidden meta-toggle (glyphAtlasEnabled, surfaced as STR_NONE_OPT)
// stays excluded as before.
//
// Returns -1 if the nameId is not in the drawer inventory at all.
static int drawerGroupForReaderSetting(StrId nameId) {
  switch (nameId) {
    // Font (2)
    case StrId::STR_FONT_FAMILY:
    case StrId::STR_FONT_SIZE:
      return BookSettingsDrawerActivity::kGroupFont;
    // Layout (6) -- order here is also the in-group render order
    case StrId::STR_LINE_SPACING:
    case StrId::STR_EXTRA_SPACING:
    case StrId::STR_FORCE_PARAGRAPH_INDENTS:
    case StrId::STR_PARA_ALIGNMENT:
    case StrId::STR_SCREEN_MARGIN:
    case StrId::STR_ORIENTATION:
      return BookSettingsDrawerActivity::kGroupLayout;
    // Reading aids (3)
    case StrId::STR_HYPHENATION:
    case StrId::STR_BIONIC_READING:
    case StrId::STR_READER_DARK_MODE:
      return BookSettingsDrawerActivity::kGroupReadingAids;
    // Images (1)
    case StrId::STR_IMAGES:
      return BookSettingsDrawerActivity::kGroupImages;
    // Everything else -- intentionally deferred to Main Settings or hidden:
    //   STR_EMBEDDED_STYLE, STR_TEXT_AA, STR_TEXT_DARKNESS, STR_GUIDE_READING,
    //   STR_NONE_OPT (the hidden glyphAtlasEnabled toggle).
    default:
      return -1;
  }
}

// Visible label for each DrawerGroup separator row. Hardcoded English to
// match the rest of the drawer's non-localized strings (BT rows, info row);
// localization can come later in a single sweep.
static const char* labelForGroup(int groupId) {
  switch (groupId) {
    case BookSettingsDrawerActivity::kGroupFont:
      return "Font";
    case BookSettingsDrawerActivity::kGroupLayout:
      return "Layout";
    case BookSettingsDrawerActivity::kGroupReadingAids:
      return "Reading aids";
    case BookSettingsDrawerActivity::kGroupImages:
      return "Images";
    default:
      return "";
  }
}

void BookSettingsDrawerActivity::buildItems() {
  items.clear();

  // CrumBLE: drawer item order was rearranged so the Bluetooth quick action
  // sits at the TOP of the drawer rather than appended after the Reader
  // settings. The BT row is the most common drawer action mid-read (drop
  // BLE to free heap for a chapter rebuild, then reconnect) and putting it
  // first reduces the number of d-pad steps to reach it. Reader settings
  // follow in their original declaration order.

  // 1) Bluetooth action row(s). Behavior depends on current link state:
  //
  //   - Not linked: show TWO connect rows -- "BT Quick Connect" (full images)
  //     and "BT No Images Quick Connect" (suppress image decode to keep a
  //     stable link on image-heavy books). Both enable BLE and connect to the
  //     bonded remote.
  //
  //   - Linked: show ONE disconnect row. Label reflects which mode is active:
  //     "BT No Images Disconnect" if suppressImages is armed, "BT Quick
  //     Disconnect" otherwise. Pressing it disables BLE (and clears image
  //     suppression on the way out). Avoids the confusing UX of offering a
  //     "Quick Connect" button while a remote is already connected.
  buildBluetoothItems();

  // 1b) v18.9.9.59: Compat Mode toggle row. See buildCompatModeItem for the
  // full rationale.
  buildCompatModeItem();

  // 2) Pull every Reader-category non-Action setting, in declaration order.
  //
  // PREFERRED PATH: when EpubReaderActivity built a settings cache at book
  // open (heap healthy, BLE not yet eating 58 KB), we iterate that vector
  // directly and item.settingIndex stays valid against it for the drawer's
  // lifetime. This means the drawer always shows the full settings list,
  // even mid-BLE-read with a fragmented heap -- the build that used to OOM
  // never happens here. Toggle-time BT prompt (attemptSettingChange) is the
  // gate, not list visibility.
  //
  // FALLBACK PATH (no external cache): rebuild locally with the original
  // heap-gate. `getSettingsList()` returns std::vector<SettingInfo> by value;
  // even one copy under a fragmented heap can bad_alloc -> terminate, so we
  // skip the build when heap is too tight and the drawer degrades to BT
  // actions only. This path is now rare -- only triggers when the parent
  // (EpubReaderActivity) couldn't build its own cache either.
  // CrumBLE 4.5.6 rev 3 (INX lazy pattern): drawer opens with BT actions + 4
  // group separators only. The ~6-10 KB settings source (getSettingsList
  // enumStringValues: font families, sizes, etc.) is built lazily on the
  // FIRST group expand -- users whose intent is BT Quick Connect never pay
  // the cost. Field test: post-BT free was 504 B with cache built, 3836 B
  // without -- delta unblocks BT + SD-font reading. rebuildDrawerItems()
  // handles both initial layout and post-expand rebuild; activateSelected
  // invokes ensureSettingsSrcBuilt() before flipping expandedGroupId_.
  rebuildDrawerItems();
}

// CrumBLE 4.5.6 rev 3: on-demand settings-source build. Runs when the user
// first expands a group. Idempotent: no-op after the first successful build.
// Returns true if the settings source is now available; false under low-heap.
bool BookSettingsDrawerActivity::ensureSettingsSrcBuilt() {
  if (externalReaderSettings_ != nullptr && !externalReaderSettings_->empty()) {
    return true;  // parent-supplied cache still supported (rare)
  }
  if (!settingsList_.empty()) {
    return true;  // built locally on a prior expand
  }
  // v18.9.9.63: v62 populates viewRows_ (not settingsList_) under viewMode_.
  // Without this check, a second group-expand tap on a different group would
  // re-enter the heap gate + clear-and-repopulate viewRows_ path, which under
  // BT-tight heap either refuses or churns. The user saw this as "can't
  // switch groups without closing/reopening the drawer" -- expandedGroupId_
  // never advanced because ensureSettingsSrcBuilt returned false the second
  // time around.
  if (viewMode_ && !viewRows_.empty()) {
    return true;
  }
  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, 28u * 1024u, 14u * 1024u)) {
    // v18.9.9.55 (task #40): fall back to the SD-cached settings-view
    // snapshot. If the cache loads, users can inspect their current
    // values under BT without disconnect; edits still redirect to a
    // silent-restart-with-OpenBookSettingsDrawer.
    if (tryPopulateFromViewCache()) {
      viewMode_ = true;
      LOG_INF("BSD",
              "ensureSettingsSrcBuilt: heap too tight (free=%u maxAlloc=%u); loaded %u rows from SD view cache "
              "(view-only mode, edits will silent-restart)",
              heap.freeHeap, heap.maxAllocHeap, static_cast<unsigned>(settingsList_.size()));
      return true;
    }
    LOG_INF("BSD",
            "ensureSettingsSrcBuilt: heap too tight (free=%u maxAlloc=%u); no view cache available, can't expand "
            "groups",
            heap.freeHeap, heap.maxAllocHeap);
    return false;
  }
  sdFontSystem.refreshIfDirty();
  settingsList_ = getSettingsList(&sdFontSystem.registry());
  LOG_INF("BSD", "ensureSettingsSrcBuilt: built lazily (heap free=%u maxAlloc=%u; %u entries)",
          heap.freeHeap, heap.maxAllocHeap, static_cast<unsigned>(settingsList_.size()));
  return true;
}

bool BookSettingsDrawerActivity::tryPopulateFromViewCache() {
  // v18.9.9.61: pre-flight heap gate. loadSettingsViewCache pulls the whole
  // cache file into a std::vector<SettingsViewRow>. Under -fno-exceptions
  // any single vector-grow failure calls std::terminate (bad_alloc can't
  // be caught). Refuse cleanly under super-tight heap.
  //
  // v18.9.9.62: drawer now stores viewRows_ directly (no SettingInfo
  // conversion), which cuts alloc pressure roughly in half vs pre-62 --
  // but the pre-flight stays because the initial load itself allocates a
  // ~5-8 KB vector even before we touch it.
  constexpr uint32_t kViewCacheMinMaxAlloc = 8u * 1024u;
  const auto heap = MemoryBudget::snapshot();
  if (heap.maxAllocHeap < kViewCacheMinMaxAlloc) {
    LOG_INF("BSD",
            "tryPopulateFromViewCache: maxAlloc=%u below floor=%u; refusing to load view cache "
            "(bad_alloc would hard-restart)",
            heap.maxAllocHeap, kViewCacheMinMaxAlloc);
    return false;
  }
  viewRows_.clear();
  if (!loadSettingsViewCache(viewRows_) || viewRows_.empty()) {
    viewRows_.clear();  // in case load partially populated then failed
    return false;
  }
  LOG_INF("BSD",
          "tryPopulateFromViewCache: loaded %u rows into viewRows_ (heap free=%u maxAlloc=%u)",
          static_cast<unsigned>(viewRows_.size()), heap.freeHeap, heap.maxAllocHeap);
  return true;
}

std::string BookSettingsDrawerActivity::viewRowValueText(const SettingsViewRow& row) const {
  // Mirrors ReaderOptionsActivity::viewRowValueText. Kept local to avoid a
  // cross-activity dependency for a 20-line helper.
  if (row.type == SettingType::TOGGLE) {
    return row.currentValue ? std::string(I18N.get(StrId::STR_STATE_ON))
                            : std::string(I18N.get(StrId::STR_STATE_OFF));
  }
  if (row.type == SettingType::ENUM) {
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
  return {};
}

// Constructor + post-expand item rebuild. Always adds BT actions + all group
// separators. If a settings source exists AND a group is expanded, appends
// that group's member items right after its separator.
void BookSettingsDrawerActivity::rebuildDrawerItems() {
  items.clear();
  buildBluetoothItems();
  // v18.9.9.59: keep the compat toggle in the same slot on rebuild so
  // group expand/collapse doesn't hide it. Layout intentionally mirrors
  // buildItems' sequencing (BT actions -> compat toggle -> group tree).
  buildCompatModeItem();

  // v18.9.9.26: under Compat mode AND while BT is up, the drawer omits every
  // reader settings group -- only the BT quick-connect / no-images /
  // disconnect items above stay. This is the tight-heap regime where the
  // group-expand path routinely refuses (~8 KB maxAlloc). When BT is off the
  // heap has ~30-40 KB maxAlloc back, so the settings groups render and
  // expand fine -- keep them visible so the user can still access reader
  // options through the drawer without going to the in-book Reader Options
  // screen. Compat mode itself (simpleRenderingActive_) only sticks when a
  // book was opened with the sidecar + BT; if BT drops mid-session, compat
  // stays on for this open but the heap is back so we relax the gate.
  if (APP_STATE.readerCompatModeActive && BluetoothHIDManager::getInstance().isEnabled()) {
    return;
  }

  const std::vector<SettingInfo>* settingsSrc = externalReaderSettings_;
  if ((settingsSrc == nullptr || settingsSrc->empty()) && !settingsList_.empty()) {
    settingsSrc = &settingsList_;
  }
  // v18.9.9.62: view-mode iterates viewRows_ (no SettingInfo copies) so the
  // Font group renders without the alloc storm that used to crash the drawer
  // under BT-tight heap. Item.settingIndex under viewMode_ points into
  // viewRows_ instead of currentSettings().
  const bool useViewRows = viewMode_ && !viewRows_.empty();

  for (int group = 0; group < kGroupCount; ++group) {
    Item sep;
    sep.isGroupSeparator = true;
    sep.groupId = group;
    sep.customName = labelForGroup(group);
    items.push_back(std::move(sep));

    if (group != expandedGroupId_) continue;
    if (!useViewRows && settingsSrc == nullptr) continue;

    if (useViewRows) {
      for (size_t i = 0; i < viewRows_.size(); ++i) {
        const auto& row = viewRows_[i];
        if (row.categoryId != StrId::STR_CAT_READER) continue;
        if (row.type == SettingType::ACTION || row.type == SettingType::SECTION_HEADER) continue;
        if (drawerGroupForReaderSetting(row.nameId) != group) continue;
        Item item;
        item.nameId = row.nameId;
        item.settingIndex = static_cast<int>(i);
        item.groupId = group;
        items.push_back(std::move(item));
      }
    } else {
      for (size_t i = 0; i < settingsSrc->size(); ++i) {
        const auto& info = (*settingsSrc)[i];
        if (info.category != StrId::STR_CAT_READER) continue;
        if (info.type == SettingType::ACTION || info.type == SettingType::SECTION_HEADER) continue;
        if (drawerGroupForReaderSetting(info.nameId) != group) continue;
        Item item;
        item.nameId = info.nameId;
        item.settingIndex = static_cast<int>(i);
        item.groupId = group;
        items.push_back(std::move(item));
      }
    }
  }
}


void BookSettingsDrawerActivity::buildCompatModeItem() {
  // Path-aware: writes/clears compat_prepared.flag or compat_custom.flag
  // depending on APP_STATE.readerActivePath. Setter mirrors ROA's compat
  // setter -- flips APP_STATE.readerCompatModeActive for immediate UI
  // decorations, signals compatModeChanged for the reader-menu return
  // handler, and tracks the manual-off intent for the auto-re-enable toast.
  const bool compatOn = drawerReadCompatSidecar();
  Item compat;
  compat.nameId = StrId::STR_COMPAT_MODE;  // theming id; label is set below
  compat.isAction = true;
  // v18.9.9.60: snprintf into a stack buffer + one final std::string
  // construction, instead of `string(a) + ": " + string(b)` which allocated
  // three intermediates. The drawer rebuilds on every group expand under
  // BT with maxAlloc as low as 44 B; every unnecessary alloc is a crash
  // vector.
  char labelBuf[48];
  snprintf(labelBuf, sizeof(labelBuf), "%s: %s", I18N.get(StrId::STR_COMPAT_MODE),
           compatOn ? I18N.get(StrId::STR_STATE_ON) : I18N.get(StrId::STR_STATE_OFF));
  compat.customName = labelBuf;
  compat.activate = [this, compatOn]() {
    if (compatOn) {
      drawerClearCompatSidecar();
      APP_STATE.readerCompatModeActive = false;
      APP_STATE.compatUserDisabledThisSession = true;
    } else {
      drawerWriteCompatSidecar();
      APP_STATE.readerCompatModeActive = true;
      APP_STATE.compatUserDisabledThisSession = false;
    }
    APP_STATE.compatModeChanged = true;
    MenuResult result;
    result.settingsChanged = true;
    setResult(ActivityResult{result});
    finish();
  };
  items.push_back(std::move(compat));
}

void BookSettingsDrawerActivity::buildBluetoothItems() {
  auto& btMgr = BluetoothHIDManager::getInstance();
  const bool stackUp = btMgr.isEnabled();
  const bool linked = stackUp && SETTINGS.bleBondedDeviceAddr[0] != '\0' &&
                      btMgr.isConnected(SETTINGS.bleBondedDeviceAddr);

  if (linked) {
    Item disc;
    disc.nameId = StrId::STR_BT_QUICK_CONNECT;  // base id for theming; customName overrides label
    disc.isAction = true;
    disc.customName = renderer.suppressImages() ? std::string("BT No Images Disconnect")
                                                : std::string("BT Quick Disconnect");
    disc.activate = [this]() {
      // Hardcoded popup -- sub-second op, not worth a 25-translation round-trip.
      GUI.drawPopup(renderer, "Disconnecting Bluetooth...");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      BluetoothHIDManager::getInstance().disable();
      // disable() doesn't clear the renderer's image-suppression flag (that's
      // owned by the renderer, not the BT manager). Clear it here so the next
      // render restores images without waiting for the loop()'s link-teardown
      // check to fire.
      renderer.setSuppressImages(false);
      MenuResult result;
      result.settingsChanged = settingsChanged;
      setResult(ActivityResult{result});
      finish();
    };
    items.push_back(std::move(disc));
    return;
  }

  // Not linked: surface BOTH connect rows (regular + no-images variant).
  // Regular Quick Connect is the primary action (most users want images
  // on); no-images variant is the fallback for image-heavy books that
  // starve renders under NimBLE pressure, so it sits below.

  // Bluetooth quick-action. Sets MenuResult flags so the reader can
  // sequence: (1) drain any pending re-layout first (settings just
  // toggled), (2) run the .pxc manifest-mismatch check and prompt if
  // needed, (3) finally enable BLE and connect. Doing this synchronously
  // here used to race the NimBLE handshake against a heap-heavy section
  // rebuild and brick the connect.
  {
    Item bt;
    bt.nameId = StrId::STR_BT_QUICK_CONNECT;
    bt.isAction = true;
    bt.activate = [this]() {
      const bool hasBonded = SETTINGS.bleBondedDeviceAddr[0] != '\0';
      MenuResult result;
      result.settingsChanged = settingsChanged;
      if (!hasBonded) {
        // No bonded remote -- bounce to the pairing UI. The user has to pair
        // first; the BT UI handles its own connect flow.
        result.requestBluetoothFlow = true;
      } else {
        result.bleConnectRequested = true;
        result.bleConnectNoImages = false;
      }
      setResult(ActivityResult{result});
      finish();
    };
    items.push_back(std::move(bt));
  }

  // Bluetooth quick-action, no-images variant. Same deferred flow as the
  // regular variant above, plus image suppression for books whose pages
  // can't survive the NimBLE handshake with images enabled.
  {
    Item btNoImg;
    btNoImg.nameId = StrId::STR_BT_NO_IMAGES_QUICK_CONNECT;
    btNoImg.isAction = true;
    btNoImg.activate = [this]() {
      const bool hasBonded = SETTINGS.bleBondedDeviceAddr[0] != '\0';
      MenuResult result;
      result.settingsChanged = settingsChanged;
      if (!hasBonded) {
        result.requestBluetoothFlow = true;
      } else {
        result.bleConnectRequested = true;
        result.bleConnectNoImages = true;
      }
      setResult(ActivityResult{result});
      finish();
    };
    items.push_back(std::move(btNoImg));
  }
}

void BookSettingsDrawerActivity::layoutDrawer() {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  if (isLandscape(renderer)) {
    drawerW = sw / 2;
    drawerX = sw - drawerW;
    drawerY = 0;
    drawerH = sh;
  } else {
    drawerX = 0;
    drawerW = sw;
    drawerH = (sh * 60) / 100;
    drawerY = sh - drawerH;
  }
  const int listRegion = drawerH - kTabOverlap - kListTopPad - hintsHeight;
  itemsVisible = std::max(1, listRegion / itemHeight);
}

// CrumBLE 4.5.6 (revision 2): visibility predicate for the INX-style
// collapsible drawer. Always-visible: BT actions (groupId<0), separators,
// info row. Setting-bound rows: visible only when their group is the
// currently-expanded one. clampSelection / render / navigation all walk
// items[] honoring this rule, so collapsed groups quietly disappear.
static bool isItemVisible(const BookSettingsDrawerActivity::Item& item, int expandedGroupId) {
  if (item.isAction || item.isInfo || item.isGroupSeparator) return true;
  if (item.groupId < 0) return true;  // top-level setting (none today, but be safe)
  return item.groupId == expandedGroupId;
}

void BookSettingsDrawerActivity::clampSelection() {
  if (items.empty()) {
    selectedIndex = 0;
    return;
  }
  const int n = static_cast<int>(items.size());
  if (selectedIndex < 0) selectedIndex = n - 1;
  if (selectedIndex >= n) selectedIndex = 0;
  // CrumBLE 4.5.6 (rev 2): if the cursor landed on a collapsed item (e.g.
  // user toggled a group closed, or buildItems just rebuilt), walk to the
  // next visible item. Wrap around at the ends. A drawer with at least
  // one BT row always has a visible item, so this terminates.
  if (!isItemVisible(items[selectedIndex], expandedGroupId_)) {
    int probe = selectedIndex;
    for (int steps = 0; steps < n; ++steps) {
      probe = (probe + 1) % n;
      if (isItemVisible(items[probe], expandedGroupId_)) {
        selectedIndex = probe;
        return;
      }
    }
  }
}

// CrumBLE 4.5.6 (rev 2): visible-item helpers for scroll/render math. With
// group collapse, scrollOffset must count *visible* items above the top of
// the rendered window, not raw item indices, or scroll math breaks when
// hidden items lie between scrollOffset and selectedIndex.
namespace {
int visibleCount(const std::vector<BookSettingsDrawerActivity::Item>& items, int expandedGroupId) {
  int n = 0;
  for (const auto& it : items) {
    if (isItemVisible(it, expandedGroupId)) n++;
  }
  return n;
}
// Returns the visible-index (0-based count of visible items strictly before
// rawIdx). Returns -1 if rawIdx is itself invisible (callers should not
// pass invisible items; clampSelection ensures selectedIndex is visible).
int visibleIndexOf(const std::vector<BookSettingsDrawerActivity::Item>& items, int expandedGroupId, int rawIdx) {
  if (rawIdx < 0 || rawIdx >= static_cast<int>(items.size())) return -1;
  if (!isItemVisible(items[rawIdx], expandedGroupId)) return -1;
  int vi = 0;
  for (int i = 0; i < rawIdx; ++i) {
    if (isItemVisible(items[i], expandedGroupId)) vi++;
  }
  return vi;
}
}  // namespace

void BookSettingsDrawerActivity::adjustScrollToSelection() {
  if (items.empty()) return;
  // Translate raw selectedIndex into the visible coordinate space. If the
  // selection isn't visible (shouldn't happen after clampSelection but be
  // safe), leave scrollOffset alone.
  const int selVi = visibleIndexOf(items, expandedGroupId_, selectedIndex);
  if (selVi < 0) return;
  if (selVi < scrollOffset) {
    scrollOffset = selVi;
  } else if (selVi >= scrollOffset + itemsVisible) {
    scrollOffset = selVi - itemsVisible + 1;
  }
  // CrumBLE 4.5.6 (rev 2): clamp uses visible-count, not items.size(), so
  // we don't reserve scroll positions for hidden rows that wouldn't render.
  const int visN = visibleCount(items, expandedGroupId_);
  scrollOffset = std::max(0, std::min(scrollOffset, std::max(0, visN - itemsVisible)));
}

void BookSettingsDrawerActivity::changeSelected(int delta) {
  if (items.empty()) return;
  auto& item = items[selectedIndex];
  if (item.isInfo) return;  // CrumBLE 4.5.6: info rows don't respond to value changes
  // CrumBLE 4.5.6 (rev 2): group-separator rows are toggle-only on Confirm;
  // Left/Right value adjust is a no-op (no "value" to cycle through).
  if (item.isGroupSeparator) return;
  if (item.isAction) {
    if (delta > 0 && item.activate) item.activate();
    return;
  }
  attemptSettingChange(selectedIndex, delta);
}

void BookSettingsDrawerActivity::activateSelected() {
  if (items.empty()) return;
  auto& item = items[selectedIndex];
  if (item.isInfo) return;  // CrumBLE 4.5.6: info rows don't respond to Confirm
  // CrumBLE 4.5.6 (rev 2): Confirm on a group separator toggles its
  // expansion. INX-style "one expanded at a time": expanding a new group
  // collapses the prior one. Confirm on the already-expanded group
  // collapses it (no group expanded). After toggling we re-clamp the
  // cursor so it doesn't end up sitting on an item that just disappeared.
  if (item.isGroupSeparator) {
    // v18.9.9.72c: remember the tapped group's ID BEFORE anything else. `item`
    // is a reference into items[selectedIndex]; rebuildDrawerItems() below
    // dangles it. We also can't rely on selectedIndex to still mean the same
    // thing post-rebuild: expanding Reading Aids while Layout was already
    // expanded shrinks Layout's rows out and pushes Reading Aids up by 6-8
    // slots, so the raw selectedIndex now points at some item further down
    // (or gets walked forward to BT Quick Disconnect by clampSelection when
    // it lands on a hidden row). Save the identity, then restore it.
    const int tappedGroupId = item.groupId;

    // v18.9.5.5: reconcile with the visual state before toggling. When the
    // lazy source hasn't been built yet, the indicator (see rebuildDrawerItems'
    // render) shows "+" for every group -- including the one expandedGroupId_
    // still points at from the header default (kGroupFont). Without this
    // reset, the user's first click on Font sees "expandedGroupId_ ==
    // item.groupId" and collapses to -1 (visually a no-op), forcing a second
    // click to actually expand. The visual truth is "nothing is expanded,"
    // so make the internal state agree before we branch.
    const bool hasSource =
        externalReaderSettings_ != nullptr || !settingsList_.empty() || (viewMode_ && !viewRows_.empty());
    if (!hasSource) {
      expandedGroupId_ = -1;
    }
    if (expandedGroupId_ == tappedGroupId) {
      expandedGroupId_ = -1;
    } else {
      // CrumBLE 4.5.6 rev 3: lazy settings-source build. First expand
      // triggers the ~6-10 KB getSettingsList() work. If heap is too tight,
      // refuse the expand (keeps drawer usable in BT-actions-only mode).
      // v18.9.9.26: under compat mode the group separators aren't even
      // rendered (see rebuildDrawerItems), so we can only reach here
      // outside compat -- refuses are rare on a normal-render session.
      if (!ensureSettingsSrcBuilt()) return;
      expandedGroupId_ = tappedGroupId;
    }
    rebuildDrawerItems();

    // Restore focus to the separator we just tapped. Walk items[] for the
    // group ID we saved; if it's gone for any reason (e.g. compat-mode
    // rebuild dropped every group separator), fall back to clampSelection.
    bool restored = false;
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].isGroupSeparator && items[i].groupId == tappedGroupId) {
        selectedIndex = static_cast<int>(i);
        restored = true;
        break;
      }
    }
    if (!restored) clampSelection();
    adjustScrollToSelection();
    return;
  }
  if (item.isAction) {
    if (item.activate) item.activate();
    return;
  }
  attemptSettingChange(selectedIndex, +1);
}

namespace {
// CrumBLE: Resolve a SettingInfo's enum value to a user-visible label string.
// Returns empty if the value is out of range. Honors enumRawValues when
// set, so settings that map a subset of the raw enum (e.g. fontFamily on
// slim builds with OMIT_LEXENDDECA / OMIT_CHAREINK) resolve correctly
// instead of indexing off the end of the trimmed enumValues vector.
std::string enumLabelOf(const SettingInfo& info, uint8_t value) {
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

// Find a reader-category SettingInfo by nameId; nullptr if absent.
const SettingInfo* findSetting(const std::vector<SettingInfo>& settings, StrId nameId) {
  for (const auto& s : settings) {
    if (s.nameId == nameId) return &s;
  }
  return nullptr;
}

// Format a font "Family (Size)" label from raw fields. SD font takes priority
// (its name string is what the user sees in the font picker); otherwise
// resolve the built-in family enum and the size enum (STR_TINY/SMALL/MEDIUM
// /etc. -- raw fontSize is an INDEX into the compiled-in size list, not a
// point size, so printing it as an integer reads as "1" or "2" rather than
// "14" -- we resolve via the live SettingInfo's enumValues).
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

// CrumBLE: build the side-by-side comparison body shown in the .pxc manifest
// mismatch prompt. Lists the four viewport-affecting fields with the prepared
// value first, the user's current value second. Used by both the toggle-time
// prompt (drawer) and the BLE-connect prompts (reader) -- but each owns its
// own copy of this helper because the dependencies (SettingInfo, I18N, etc.)
// would otherwise force a heavy include into PxcManifest.h. ~30 lines is OK
// to duplicate; the alternative is a new utility file for one function.
//
// Output is plain text with '\n' between lines; ConfirmationActivity respects
// hard newlines as section breaks (centered-per-line drop the indent visual,
// so we avoid leading spaces).
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

// CrumBLE: compute the would-be new value for a setting given an applyDelta
// call, WITHOUT mutating SETTINGS. Used to predict whether a toggle moves us
// off the .pxc manifest's layout before we commit. Mirrors the branch
// structure of applyDeltaToSetting (TOGGLE / ENUM / VALUE).
uint8_t previewDeltaValue(const SettingInfo& info, int delta) {
  if (info.valuePtr == nullptr) return 0;
  const uint8_t cur = SETTINGS.*(info.valuePtr);
  if (info.type == SettingType::TOGGLE) return cur == 0 ? 1 : 0;
  if (info.type == SettingType::ENUM) {
    if (info.enumValues.empty()) return cur;
    const int count = static_cast<int>(info.enumValues.size());
    int next = static_cast<int>(cur) + delta;
    next = ((next % count) + count) % count;
    return static_cast<uint8_t>(next);
  }
  if (info.type == SettingType::VALUE) {
    const int step = std::max<int>(1, info.valueRange.step);
    int next = static_cast<int>(cur) + delta * step;
    if (next < info.valueRange.min) next = info.valueRange.max;
    if (next > info.valueRange.max) next = info.valueRange.min;
    return static_cast<uint8_t>(next);
  }
  return cur;
}
}  // namespace

void BookSettingsDrawerActivity::attemptSettingChange(int itemIndex, int delta) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const auto& item = items[itemIndex];
  // v18.9.9.62: check view-mode BEFORE the currentSettings() bounds check.
  // Under viewMode_ we intentionally have currentSettings().empty()==true
  // (settingsList_ isn't populated -- viewRows_ owns the data). Pre-62 the
  // bounds check would return early and silently swallow the user's tap.
  if (viewMode_) {
    const uint8_t g = expandedGroupId_ >= 0 ? static_cast<uint8_t>(expandedGroupId_) : 0;
    LOG_INF("BSD",
            "View-mode edit tap (row=%d delta=%d group=%u); silent-restart with OpenBookSettingsDrawer "
            "to open editable drawer on fresh heap",
            itemIndex, delta, static_cast<unsigned>(g));
    silentRestartToReaderOpeningDrawerAt(g);
    // never returns
    return;
  }
  if (item.settingIndex < 0 || item.settingIndex >= static_cast<int>(currentSettings().size())) return;
  const SettingInfo& info = currentSettings()[item.settingIndex];

  // CrumBLE: silent layout-change flow. The previous "Turn off Bluetooth?"
  // prompt is gone -- the BLE disconnect is a side-effect of the layout
  // change, not a user choice, so we just do it. BLE comes back via
  // requestEnableLater() drained in onExit (if it was on at entry).
  //
  // The only prompt we keep is the .pxc manifest mismatch check: when the
  // user toggles one of the four viewport-affecting settings and the new
  // value would move them off the prepared layout, that's a meaningful
  // choice (images may render badly over BLE) and worth asking about.
  auto applyChangeSilent = [this, itemIndex, delta]() {
    auto& mgr = BluetoothHIDManager::getInstance();
    if (mgr.isEnabled()) {
      GUI.drawPopup(renderer, "Updating layout...");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      mgr.disable();
    }
    // Re-validate in case the drawer state mutated while the prompt activity
    // was open (e.g. a rebuild trimmed items).
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
    const auto& it = items[itemIndex];
    const auto& src = currentSettings();
    if (it.settingIndex < 0 || it.settingIndex >= static_cast<int>(src.size())) return;
    applyDeltaToSetting(src[it.settingIndex], delta);
    settingsChanged = true;
  };

  // CrumBLE: the drawer NEVER prompts on toggle. User must be free to scrub
  // through values to find what they want without a confirmation dialog
  // blocking each step. The .pxc manifest mismatch check only fires at BT
  // connect time -- the user finds out then whether they need to switch back
  // to the prepared layout for image rendering. Bluetooth-disable for layout
  // changes is also silent (no prompt); the disconnect is a forced
  // consequence of the heap pressure, not a user choice.
  applyChangeSilent();
  requestUpdate();
}

void BookSettingsDrawerActivity::loop() {
  const bool landscape = isLandscape(renderer);
  const auto& mi = mappedInput;
  using B = MappedInputManager::Button;

  // Back, navigation, and value-adjust are accepted immediately — the user
  // may hit Back to dismiss while still holding the long-press Confirm.
  if (mi.wasReleased(B::Back)) {
    // Pass the settings-changed flag back to the reader via MenuResult so its
    // result handler can skip the re-layout when nothing was modified.
    MenuResult result;
    result.settingsChanged = settingsChanged;
    setResult(ActivityResult{result});
    finish();
    return;
  }

  // List navigation: portrait uses Up/Down, landscape uses Right/Left.
  // CrumBLE 4.5.6 (rev 2): skip past invisible (collapsed-group) items so
  // one keypress always moves to the next visible row. Without this the
  // user would hit Up/Down repeatedly to traverse the cursor across hidden
  // settings, which would feel broken.
  const bool prevList = mi.wasReleased(landscape ? B::Right : B::Up);
  const bool nextList = mi.wasReleased(landscape ? B::Left : B::Down);
  if (prevList || nextList) {
    const int n = static_cast<int>(items.size());
    if (n > 0) {
      const int dir = prevList ? -1 : 1;
      int probe = selectedIndex;
      for (int steps = 0; steps < n; ++steps) {
        probe = (probe + dir + n) % n;
        if (isItemVisible(items[probe], expandedGroupId_)) {
          selectedIndex = probe;
          break;
        }
      }
    }
    adjustScrollToSelection();
    requestUpdate();
    return;
  }

  // Value adjust: portrait Left/Right, landscape Down/Up.
  const bool decrease = mi.wasReleased(landscape ? B::Down : B::Left);
  const bool increase = mi.wasReleased(landscape ? B::Up : B::Right);
  if (decrease) {
    changeSelected(-1);
    requestUpdate();
    return;
  }
  if (increase) {
    changeSelected(+1);
    requestUpdate();
    return;
  }

  // Confirm is special: the FIRST Confirm release we see is from the long
  // press that opened the drawer. Swallow it once, then accept Confirm
  // normally as "activate selected".
  if (mi.wasReleased(B::Confirm)) {
    if (!initialConfirmReleased) {
      initialConfirmReleased = true;
      return;
    }
    activateSelected();
    requestUpdate();
    return;
  }
}

void BookSettingsDrawerActivity::renderDrawer() {
  // Repaint reader page underneath, then draw the drawer panel on top.
  //
  // Three tiers of fallback for getting the reader page into the
  // framebuffer:
  //   1. Our own storeBwBuffer() snapshot from onEnter, if it succeeded.
  //   2. Otherwise, whatever backup the renderer already had — the
  //      EpubReader stores its own BW backup at the end of each page
  //      render (for the grayscale pass) and doesn't free it after
  //      restoring, so it usually sits there waiting for us.
  //   3. Last resort: do NOT clear the screen. Whatever was last
  //      rendered is what the e-ink is currently showing; clearing would
  //      diff every reader-page pixel against white on the next fast
  //      refresh and paint the panel onto a stark white background.
  //      Leaving the framebuffer alone matches whatever's already on
  //      the display so the fast-refresh diff is small.
  if (readerBufferStored) {
    renderer.restoreBwBuffer();
  } else if (renderer.hasStoredBwBuffer()) {
    renderer.restoreBwBuffer();
  }
  // else: intentionally no clearScreen() — see comment above.

  // CrumBLE 4.4: dark-mode-aware drawer. When the reader is in dark mode,
  // the drawer panel inverts (black fill, white borders, white text) so it
  // visually matches the dark page underneath instead of flashing as a
  // bright white panel. Selected-row highlight also flips: white fill +
  // black text in dark mode, mirroring the page-level reverse-video pattern
  // used for selection highlights and the lookup popup hints.
  const bool darkMode = ReaderUtils::readerDarkModeEnabled();
  const Color panelFill = darkMode ? Color::Black : Color::White;
  const bool panelStrokeInk = !darkMode;  // true=black stroke in light; false=white stroke in dark
  const bool panelTextBlack = !darkMode;

  // Panel body — filled, with the top two corners rounded so the panel
  // looks like it has rounded shoulders.
  const int panelTopY = drawerY + kTabOverlap;
  const int panelBodyH = drawerH - kTabOverlap;
  renderer.fillRoundedRect(drawerX, panelTopY, drawerW, panelBodyH, kPanelCornerRadius,
                           /*topL=*/true, /*topR=*/true, /*botL=*/false, /*botR=*/false, panelFill);

  // Panel top edge — a horizontal line between the rounded corners. The middle
  // section will be overpainted by the tab below so it appears to pass "behind"
  // the tab outline.
  renderer.drawLine(drawerX + kPanelCornerRadius, panelTopY,
                    drawerX + drawerW - kPanelCornerRadius - 1, panelTopY, 2, panelStrokeInk);
  // Quarter-circle outlines for the panel's top corners (matches the fill).
  renderer.drawArc(kPanelCornerRadius, drawerX + kPanelCornerRadius, panelTopY + kPanelCornerRadius,
                   -1, -1, 2, panelStrokeInk);
  renderer.drawArc(kPanelCornerRadius, drawerX + drawerW - kPanelCornerRadius - 1,
                   panelTopY + kPanelCornerRadius, 1, -1, 2, panelStrokeInk);

  // Tab — centred above the panel's top edge. Width auto-fits the header text
  // with horizontal padding; bottom of the tab extends kTabOverlap into the
  // panel so the panel's top line falls inside the tab.
  const char* headerText = "Global Book Settings";
  const int headerTextW = renderer.getTextWidth(UI_12_FONT_ID, headerText, EpdFontFamily::BOLD);
  const int tabPadX = 18;
  const int tabW = std::min(drawerW - 40, headerTextW + 2 * tabPadX);
  const int tabX = drawerX + (drawerW - tabW) / 2;
  const int tabY = drawerY;
  const int tabBottomY = tabY + kTabHeight;

  // 1) Erase the panel's top line where it would pass through the tab.
  renderer.fillRoundedRect(tabX, tabY, tabW, kTabHeight, kTabCornerRadius,
                           /*topL=*/true, /*topR=*/true, /*botL=*/false, /*botR=*/false, panelFill);

  // 2) Tab outline — top + rounded top corners + left and right sides only.
  // Bottom side intentionally omitted so the tab visually merges with the panel.
  renderer.drawLine(tabX + kTabCornerRadius, tabY, tabX + tabW - kTabCornerRadius - 1, tabY, 2, panelStrokeInk);
  renderer.drawArc(kTabCornerRadius, tabX + kTabCornerRadius, tabY + kTabCornerRadius, -1, -1, 2, panelStrokeInk);
  renderer.drawArc(kTabCornerRadius, tabX + tabW - kTabCornerRadius - 1, tabY + kTabCornerRadius, 1, -1, 2, panelStrokeInk);
  renderer.drawLine(tabX, tabY + kTabCornerRadius, tabX, tabBottomY - 1, 2, panelStrokeInk);
  renderer.drawLine(tabX + tabW - 1, tabY + kTabCornerRadius, tabX + tabW - 1, tabBottomY - 1, 2, panelStrokeInk);

  // Tab text — vertically centred in the upper portion of the tab so it sits
  // above the panel's top line.
  const int tabTextX = tabX + (tabW - headerTextW) / 2;
  const int tabTextY = tabY + 6;
  renderer.drawText(UI_12_FONT_ID, tabTextX, tabTextY, headerText, panelTextBlack, EpdFontFamily::BOLD);

  // Item list — starts a small pad below the panel's top line.
  const int listStartY = panelTopY + kListTopPad;
  const int leftPad = 12;
  const int rightPad = 12;
  // Renderer.drawText uses the same Y as the highlight rect top in the existing
  // reader-menu pattern; add a small top padding so the glyphs aren't flush
  // against the rect edge.
  const int rowTextY = 6;

  // CrumBLE 4.5.6 (rev 2): visible-aware row walk. Skip items whose group
  // is collapsed; render at most itemsVisible visible rows starting at the
  // scrollOffset-th visible row. Separator rows render with a +/-
  // expansion indicator on the right and a bold label on the left -- no
  // value column and no action arrow.
  int rowsDrawn = 0;
  int visiblesSeen = 0;
  for (int idx = 0; idx < static_cast<int>(items.size()) && rowsDrawn < itemsVisible; ++idx) {
    const Item& item = items[idx];
    if (!isItemVisible(item, expandedGroupId_)) continue;
    if (visiblesSeen < scrollOffset) {
      visiblesSeen++;
      continue;
    }
    visiblesSeen++;
    const int rowY = listStartY + rowsDrawn * itemHeight;
    const bool selected = (idx == selectedIndex);
    if (selected) {
      // Selection highlight uses inverse video relative to the panel.
      renderer.fillRect(drawerX + 1, rowY, drawerW - 2, itemHeight, panelStrokeInk);
    }
    const char* name = !item.customName.empty() ? item.customName.c_str() : I18N.get(item.nameId);
    const auto& src = currentSettings();
    // v18.9.9.62: dual-source name/value lookup. Under viewMode_ the
    // Item.settingIndex points into viewRows_ (raw cache rows, no
    // SettingInfo shells). Otherwise it points into currentSettings().
    // The compat suffix decorator still runs for both -- viewMode's
    // fake SettingInfo shell used to carry the info; now we drive it
    // off the row's nameId directly.
    const bool useViewRow = viewMode_ && item.settingIndex >= 0 &&
                            item.settingIndex < static_cast<int>(viewRows_.size());
    // Info + separator rows have no SettingInfo; skip value resolution.
    std::string value;
    if (!item.isInfo && !item.isGroupSeparator) {
      if (useViewRow) {
        value = viewRowValueText(viewRows_[item.settingIndex]);
      } else if (item.settingIndex >= 0 && item.settingIndex < static_cast<int>(src.size())) {
        value = valueTextForSetting(src[item.settingIndex]);
      }
    }
    // v18.9.9.25: append the compat marker on the drawer's setting rows too
    // -- same behaviour as SettingsActivity so the user sees the same
    // "(Compat)" hint whether they came in through the drawer or the global
    // Settings screen. v18.9.9.62: also route through viewRows_ for the
    // isDrawerCompatLockedSetting check when in view-mode.
    if (!value.empty() && APP_STATE.readerCompatModeActive && !item.isInfo && !item.isGroupSeparator) {
      if (useViewRow) {
        SettingInfo shim;  // stack-only shim, no vector allocations
        shim.nameId = viewRows_[item.settingIndex].nameId;
        if (isDrawerCompatLockedSetting(shim)) value += tr(STR_SETTING_COMPAT_SUFFIX);
      } else if (item.settingIndex >= 0 && item.settingIndex < static_cast<int>(src.size()) &&
                 isDrawerCompatLockedSetting(src[item.settingIndex])) {
        value += tr(STR_SETTING_COMPAT_SUFFIX);
      }
    }
    const bool textBlack = selected ? darkMode : panelTextBlack;
    if (item.isGroupSeparator) {
      // Bold label on the left; "+" if collapsed, "-" if expanded, on the right.
      renderer.drawText(UI_12_FONT_ID, drawerX + leftPad, rowY + rowTextY, name, textBlack, EpdFontFamily::BOLD);
      // v18.9.5.3: only show "-" when the group's items would actually appear
      // below. The default expandedGroupId_ = kGroupFont opens the Font group
      // conceptually, but the INX-style lazy settings source may not be
      // built yet (see ensureSettingsSrcBuilt). Without this guard the Font
      // row reads as expanded ("-") but shows no items underneath because
      // rebuildItems skipped them for lack of a source -- a visible lie.
      const bool hasSource = externalReaderSettings_ != nullptr || !settingsList_.empty() ||
                             (viewMode_ && !viewRows_.empty());
      const bool visuallyExpanded = hasSource && (expandedGroupId_ == item.groupId);
      const char* indicator = visuallyExpanded ? "-" : "+";
      const int iw = renderer.getTextWidth(UI_12_FONT_ID, indicator, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, drawerX + drawerW - rightPad - iw, rowY + rowTextY, indicator, textBlack,
                        EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_12_FONT_ID, drawerX + leftPad, rowY + rowTextY, name, textBlack);
      if (!value.empty()) {
        const int valueWidth = renderer.getTextWidth(UI_12_FONT_ID, value.c_str());
        renderer.drawText(UI_12_FONT_ID, drawerX + drawerW - rightPad - valueWidth, rowY + rowTextY, value.c_str(),
                           textBlack);
      } else if (item.isAction) {
        const char* arrow = "→";
        const int aw = renderer.getTextWidth(UI_12_FONT_ID, arrow);
        renderer.drawText(UI_12_FONT_ID, drawerX + drawerW - rightPad - aw, rowY + rowTextY, arrow, textBlack);
      }
    }
    rowsDrawn++;
  }

  // Scroll indicator: based on visible-count, not raw items.size().
  const int visN = visibleCount(items, expandedGroupId_);
  if (visN > itemsVisible) {
    const int trackH = itemsVisible * itemHeight;
    const int barH = std::max(8, (trackH * itemsVisible) / visN);
    const int barY = listStartY + (trackH - barH) * scrollOffset / std::max(1, visN - itemsVisible);
    renderer.fillRect(drawerX + drawerW - 4, barY, 2, barH, panelStrokeInk);
  }

  // Button hints.
  const auto& mi = mappedInput;
  const auto labels = mi.mapLabels(I18N.get(StrId::STR_BACK), I18N.get(StrId::STR_TOGGLE),
                                    I18N.get(StrId::STR_DIR_UP), I18N.get(StrId::STR_DIR_DOWN));
  const int hintY = drawerY + drawerH - hintsHeight + 16;
  // CrumBLE 4.2.1: hint order reversed to match the physical button layout:
  // Back sits to the LEFT of the dpad on every supported chassis, Toggle in
  // the middle, dpad Up/Down on the RIGHT. Reading the hint left-to-right
  // now mirrors the user's eye as it scans across the bottom of the device.
  std::string hintLine = std::string(labels.btn1) + " · " + labels.btn2 + " · " + labels.btn3 + "/" + labels.btn4;
  renderer.drawCenteredText(SMALL_FONT_ID, hintY, hintLine.c_str(), panelTextBlack);
}

void BookSettingsDrawerActivity::presentFastRefresh() {
  // Reverted from displayBufferRegion -- the windowed path's std::vector
  // allocation for the per-region BW+RED scratch (~19 KB combined under
  // dual-buffer mode) threw bad_alloc under tight heap, which the global
  // std::terminate handler caught and silent-restarted to FT, surprising
  // the user mid-drawer. The static frameBuffer path used by displayBuffer
  // avoids the runtime allocation entirely. Once partial-mode is actually
  // delivering a speedup AND the windowed-buffer alloc is bounded /
  // pre-checked, this can come back.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BookSettingsDrawerActivity::render(RenderLock&&) {
  SET_CHECKPOINT("drawer:render");
  renderDrawer();
  SET_CHECKPOINT("drawer:present");
  presentFastRefresh();
}
