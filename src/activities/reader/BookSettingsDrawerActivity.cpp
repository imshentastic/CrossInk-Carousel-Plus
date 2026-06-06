#include "BookSettingsDrawerActivity.h"

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"  // prewarmReaderTextBuffer
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"  // sdFontSystem for SD-aware FONT_FAMILY / FONT_SIZE rows
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool isLandscape(const GfxRenderer& r) {
  const auto o = r.getOrientation();
  return o == GfxRenderer::Orientation::LandscapeClockwise || o == GfxRenderer::Orientation::LandscapeCounterClockwise;
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
                                                       const std::optional<PxcManifest>* pxcManifest)
    : Activity("BookSettingsDrawer", renderer, mappedInput),
      externalReaderSettings_(externalReaderSettings && !externalReaderSettings->empty() ? externalReaderSettings
                                                                                         : nullptr),
      pxcManifest_(pxcManifest) {}

void BookSettingsDrawerActivity::onEnter() {
  Activity::onEnter();

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
    // CrumBLE Phase 1 fast-open: pre-grow the glyph buffer before the
    // deferred enable drains. The drawer dropped BLE on entry to apply
    // a layout change; the post-layout re-enable needs the buffer at
    // high-water mark before NimBLE eats heap.
    EpubReaderActivity::prewarmReaderTextBuffer(renderer);
    bt.requestEnableLater();
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
  if (externalReaderSettings_) {
    const auto& src = *externalReaderSettings_;
    for (size_t i = 0; i < src.size(); ++i) {
      const auto& info = src[i];
      if (info.category != StrId::STR_CAT_READER) continue;
      if (info.type == SettingType::ACTION || info.type == SettingType::SECTION_HEADER) continue;
      Item item;
      item.nameId = info.nameId;
      item.settingIndex = static_cast<int>(i);
      items.push_back(std::move(item));
    }
  } else {
    const auto heap = MemoryBudget::snapshot();
    if (MemoryBudget::hasHeap(heap, 28u * 1024u, 14u * 1024u)) {
      // CrumBLE: pass the SD font registry so FONT_FAMILY's enumStringValues
      // includes installed SD families AND FONT_SIZE uses the SD font's
      // size list when one is active. Without the registry, both rows
      // render blank when the user has an SD font selected: FONT_FAMILY
      // because its valueGetter looks for the active name in an empty
      // sdFamilyNames capture, and FONT_SIZE because the built-in size
      // enum has fewer entries than the SD font's index.
      sdFontSystem.refreshIfDirty();
      settingsList_ = getSettingsList(&sdFontSystem.registry());
      for (size_t i = 0; i < settingsList_.size(); ++i) {
        const auto& info = settingsList_[i];
        if (info.category != StrId::STR_CAT_READER) continue;
        if (info.type == SettingType::ACTION || info.type == SettingType::SECTION_HEADER) continue;
        Item item;
        item.nameId = info.nameId;
        item.settingIndex = static_cast<int>(i);
        items.push_back(std::move(item));
      }
    } else {
      LOG_INF("BSD", "Low heap (free=%u maxAlloc=%u); showing Bluetooth actions only", heap.freeHeap,
              heap.maxAllocHeap);
    }
  }

  // BT rows now live at the TOP of the drawer (built in step 1 above via
  // buildBluetoothItems()). The trailing reader-settings appender used to
  // run here; nothing else to do.
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
        // No bonded remote -- bounce to the pairing UI as before. Don't
        // bother flagging connect-after-relayout; the user has to pair
        // first and the BT UI handles its own connect flow.
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

void BookSettingsDrawerActivity::clampSelection() {
  if (items.empty()) {
    selectedIndex = 0;
    return;
  }
  const int n = static_cast<int>(items.size());
  if (selectedIndex < 0) selectedIndex = n - 1;
  if (selectedIndex >= n) selectedIndex = 0;
}

void BookSettingsDrawerActivity::adjustScrollToSelection() {
  if (items.empty()) return;
  if (selectedIndex < scrollOffset) {
    scrollOffset = selectedIndex;
  } else if (selectedIndex >= scrollOffset + itemsVisible) {
    scrollOffset = selectedIndex - itemsVisible + 1;
  }
  scrollOffset = std::max(0, std::min(scrollOffset, std::max(0, static_cast<int>(items.size()) - itemsVisible)));
}

void BookSettingsDrawerActivity::changeSelected(int delta) {
  if (items.empty()) return;
  auto& item = items[selectedIndex];
  if (item.isAction) {
    if (delta > 0 && item.activate) item.activate();
    return;
  }
  attemptSettingChange(selectedIndex, delta);
}

void BookSettingsDrawerActivity::activateSelected() {
  if (items.empty()) return;
  auto& item = items[selectedIndex];
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
  const bool prevList = mi.wasReleased(landscape ? B::Right : B::Up);
  const bool nextList = mi.wasReleased(landscape ? B::Left : B::Down);
  if (prevList) {
    selectedIndex--;
    clampSelection();
    adjustScrollToSelection();
    requestUpdate();
    return;
  }
  if (nextList) {
    selectedIndex++;
    clampSelection();
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

  // Panel body — filled white, with the top two corners rounded so the panel
  // looks like it has rounded shoulders.
  const int panelTopY = drawerY + kTabOverlap;
  const int panelBodyH = drawerH - kTabOverlap;
  renderer.fillRoundedRect(drawerX, panelTopY, drawerW, panelBodyH, kPanelCornerRadius,
                           /*topL=*/true, /*topR=*/true, /*botL=*/false, /*botR=*/false, Color::White);

  // Panel top edge — a horizontal line between the rounded corners. The middle
  // section will be overpainted by the tab below so it appears to pass "behind"
  // the tab outline.
  renderer.drawLine(drawerX + kPanelCornerRadius, panelTopY,
                    drawerX + drawerW - kPanelCornerRadius - 1, panelTopY, 2, true);
  // Quarter-circle outlines for the panel's top corners (matches the fill).
  renderer.drawArc(kPanelCornerRadius, drawerX + kPanelCornerRadius, panelTopY + kPanelCornerRadius,
                   -1, -1, 2, true);
  renderer.drawArc(kPanelCornerRadius, drawerX + drawerW - kPanelCornerRadius - 1,
                   panelTopY + kPanelCornerRadius, 1, -1, 2, true);

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
                           /*topL=*/true, /*topR=*/true, /*botL=*/false, /*botR=*/false, Color::White);

  // 2) Tab outline — top + rounded top corners + left and right sides only.
  // Bottom side intentionally omitted so the tab visually merges with the panel.
  renderer.drawLine(tabX + kTabCornerRadius, tabY, tabX + tabW - kTabCornerRadius - 1, tabY, 2, true);
  renderer.drawArc(kTabCornerRadius, tabX + kTabCornerRadius, tabY + kTabCornerRadius, -1, -1, 2, true);
  renderer.drawArc(kTabCornerRadius, tabX + tabW - kTabCornerRadius - 1, tabY + kTabCornerRadius, 1, -1, 2, true);
  renderer.drawLine(tabX, tabY + kTabCornerRadius, tabX, tabBottomY - 1, 2, true);
  renderer.drawLine(tabX + tabW - 1, tabY + kTabCornerRadius, tabX + tabW - 1, tabBottomY - 1, 2, true);

  // Tab text — vertically centred in the upper portion of the tab so it sits
  // above the panel's top line.
  const int tabTextX = tabX + (tabW - headerTextW) / 2;
  const int tabTextY = tabY + 6;
  renderer.drawText(UI_12_FONT_ID, tabTextX, tabTextY, headerText, true, EpdFontFamily::BOLD);

  // Item list — starts a small pad below the panel's top line.
  const int listStartY = panelTopY + kListTopPad;
  const int leftPad = 12;
  const int rightPad = 12;
  // Renderer.drawText uses the same Y as the highlight rect top in the existing
  // reader-menu pattern; add a small top padding so the glyphs aren't flush
  // against the rect edge.
  const int rowTextY = 6;

  for (int i = 0; i < itemsVisible; ++i) {
    const int idx = scrollOffset + i;
    if (idx >= static_cast<int>(items.size())) break;
    const Item& item = items[idx];
    const int rowY = listStartY + i * itemHeight;
    const bool selected = (idx == selectedIndex);
    if (selected) {
      renderer.fillRect(drawerX + 1, rowY, drawerW - 2, itemHeight, true);
    }
    const char* name = !item.customName.empty() ? item.customName.c_str() : I18N.get(item.nameId);
    const auto& src = currentSettings();
    const std::string value =
        (item.settingIndex >= 0 && item.settingIndex < static_cast<int>(src.size()))
            ? valueTextForSetting(src[item.settingIndex])
            : std::string{};
    const bool textBlack = !selected;
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

  // Scroll indicator: small block on the right edge if list overflows.
  if (static_cast<int>(items.size()) > itemsVisible) {
    const int trackH = itemsVisible * itemHeight;
    const int barH = std::max(8, (trackH * itemsVisible) / static_cast<int>(items.size()));
    const int barY = listStartY + (trackH - barH) * scrollOffset /
                                       std::max(1, static_cast<int>(items.size()) - itemsVisible);
    renderer.fillRect(drawerX + drawerW - 4, barY, 2, barH, true);
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
  renderer.drawCenteredText(SMALL_FONT_ID, hintY, hintLine.c_str(), true);
}

void BookSettingsDrawerActivity::presentFastRefresh() {
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BookSettingsDrawerActivity::render(RenderLock&&) {
  renderDrawer();
  presentFastRefresh();
}
