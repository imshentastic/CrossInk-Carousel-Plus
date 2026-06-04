#pragma once

// Bottom-drawer-style quick settings menu shown in the reader on long-press of
// the menu button. Architecture and partial-refresh approach are adapted from
// inx (MIT, Copyright 2025 Dave Allie):
//   https://github.com/obijuankenobiii/inx — src/activity/reader/Epub/SettingsDrawer.{h,cpp}
//
// The drawer overlays the bottom ~60% of the screen (or right half in
// landscape) on top of the live reader page. It uses FAST_REFRESH so toggling
// items doesn't sweep the panel — only the changed pixels in the drawer
// region get repainted. The reader page underneath is preserved by storing
// the framebuffer on entry and restoring it before every drawer redraw.

#include <I18n.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "PxcManifest.h"

class BookSettingsDrawerActivity final : public Activity {
 public:
  // externalReaderSettings: optional pointer to a reader-category SettingInfo
  // vector owned by the parent activity (typically EpubReaderActivity, built
  // once at book open while heap is unfragmented). When non-null, the drawer
  // skips its own getSettingsList() build entirely -- which used to OOM-crash
  // on a BLE-fragmented heap. When null, falls back to the local heap-gated
  // build (drawer shows only BT actions if heap doesn't allow it).
  // pxcManifest: optional pointer to the parsed .pxc manifest (or empty
  // optional). When the user toggles a viewport-affecting setting (font /
  // orientation / margin / image-rendering) and the new value would mismatch
  // the manifest, the drawer prompts before applying so the user understands
  // they're moving off the prepared layout (images may render badly over BLE).
  // Null pointer (or empty optional) = no manifest = no mismatch prompt path.
  explicit BookSettingsDrawerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::vector<SettingInfo>* externalReaderSettings = nullptr,
                                      const std::optional<PxcManifest>* pxcManifest = nullptr);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  // A single row in the drawer.
  //   - Setting-bound rows have non-null change/getValueText callbacks; each
  //     closure carries its own SettingInfo copy (see buildItems for why).
  //   - The BLE quick-action rows are special and use the activate callback.
  struct Item {
    StrId nameId;
    bool isAction = false;  // true: Confirm activates; false: Confirm toggles/cycles value
    // Setting-bound rows store an index into settingsList_ and resolve the
    // SettingInfo on demand, instead of capturing a by-value SettingInfo copy
    // into per-row closures. Those copies (each with its own enumValues vector,
    // wrapped in std::function storage) were dozens of small heap allocations
    // that OOM-crashed the drawer when it was opened under a fragmented heap
    // (e.g. reconnecting BT mid-read). -1 for action rows.
    int settingIndex = -1;
    std::function<void()> activate;  // action rows only
    // CrumBLE: optional override for the row label. When non-empty, the
    // renderer uses this string instead of I18N.get(nameId). Used by the
    // BT action rows so they can show "BT Quick Disconnect" / "BT No Images
    // Disconnect" when the link is currently up, without needing a new
    // i18n key for every variant.
    std::string customName;
  };

  void buildItems();
  // CrumBLE: BT rows extracted so they can be inserted at the TOP of the
  // drawer list (most common drawer action mid-read).
  void buildBluetoothItems();
  void layoutDrawer();
  void renderDrawer();
  void presentFastRefresh();
  void clampSelection();
  void adjustScrollToSelection();
  void changeSelected(int delta);
  void activateSelected();
  // CrumBLE: apply a delta to a settings-bound row, gating on BLE state. If
  // BLE is on, push a confirmation prompt (turning off BLE frees the ~58 KB
  // the layout re-build needs); on confirm, disable BLE synchronously and
  // then apply the delta. If BLE is off, applies the delta immediately.
  void attemptSettingChange(int itemIndex, int delta);

  std::vector<Item> items;
  // Local copy of the reader settings list, used only when externalReaderSettings_
  // is null (no parent-cached source). Held for the drawer's lifetime. Items
  // index into this instead of each capturing its own SettingInfo copy -- the old
  // per-row copies were the heap churn that crashed the drawer under low memory.
  std::vector<SettingInfo> settingsList_;

  // CrumBLE: when non-null, points to a reader-settings cache built by the
  // parent activity (EpubReaderActivity) at book open. We use it directly
  // instead of rebuilding getSettingsList() here. Item.settingIndex always
  // refers to indices in *this* vector (currentSettings() resolves which).
  const std::vector<SettingInfo>* externalReaderSettings_ = nullptr;

  // Single access point for the settings source -- external when provided,
  // local fallback otherwise. Item.settingIndex is valid against whichever
  // this returns.
  const std::vector<SettingInfo>& currentSettings() const {
    return externalReaderSettings_ ? *externalReaderSettings_ : settingsList_;
  }

  // CrumBLE: parent's .pxc manifest, if any. Used by attemptSettingChange to
  // detect "this toggle moves us off the prepared layout" and surface the
  // confirmation prompt accordingly.
  const std::optional<PxcManifest>* pxcManifest_ = nullptr;
  int selectedIndex = 0;
  int scrollOffset = 0;

  // Long-press Confirm pushes this activity while the button is still held.
  // We must swallow that release before accepting user input.
  bool initialConfirmReleased = false;

  // Set when the user actually changes a setting-bound row, so the result
  // handler in EpubReaderActivity can skip the section.reset() (which
  // triggers a re-layout) when the user just glanced at the drawer.
  bool settingsChanged = false;

  // True once the entry framebuffer (live reader page) has been stashed in
  // the renderer's internal BW buffer via storeBwBuffer(). We re-blit that
  // buffer before each drawer redraw so the reader page survives partial
  // refreshes and dismiss.
  bool readerBufferStored = false;

  // CrumBLE: snapshot of BLE-enabled state at drawer entry. The drawer's
  // toggle path silently disables BLE (no prompt) when applying a layout
  // change; onExit() restores it via requestEnableLater() so the drain
  // happens AFTER the reader's re-layout completes. If BLE was already
  // off at entry, no auto-restore -- user explicitly wants it off.
  bool bleWasEnabledOnEntry_ = false;

  // Drawer geometry, recomputed in layoutDrawer() from current orientation.
  int drawerX = 0;
  int drawerY = 0;
  int drawerW = 0;
  int drawerH = 0;
  int itemHeight = 40;
  int itemsVisible = 8;
  // Reserve enough vertical space below the list for the button-hint line
  // to render fully — at 28 the SMALL_FONT baseline + descender was clipping
  // against the screen bottom.
  int hintsHeight = 44;

  // Tab + panel decoration constants. The tab sits centred above the panel's
  // top edge, with its bottom extending TAB_OVERLAP_PX into the panel so the
  // panel's top horizontal line appears to pass "behind" it.
  static constexpr int kPanelCornerRadius = 12;
  static constexpr int kTabHeight = 36;
  static constexpr int kTabOverlap = 16;          // tab bottom is at drawerY + kTabOverlap (panel top line)
  static constexpr int kTabCornerRadius = 8;
  // Space between the panel's top line and the first row, sized so the
  // selection highlight on the first row never reaches up into the tab.
  static constexpr int kListTopPad = 40;
};
