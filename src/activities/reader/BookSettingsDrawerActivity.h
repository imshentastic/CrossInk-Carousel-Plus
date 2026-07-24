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
#include "../../util/SettingsViewCache.h"
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
  // v18.9.9.25: initialExpandedGroupId opens the drawer with that group
  // pre-expanded. Used by the OpenBookSettingsDrawer boot dispatch after a
  // tight-heap silent-restart so the user's tap that got refused doesn't
  // need to be repeated. -1 = no auto-expand (normal open path).
  explicit BookSettingsDrawerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::vector<SettingInfo>* externalReaderSettings = nullptr,
                                      const std::optional<PxcManifest>* pxcManifest = nullptr,
                                      int initialExpandedGroupId = -1);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

  // CrumBLE 4.5.6 (rev 2): Item + DrawerGroup are public so the in-.cpp
  // visibility/grouping helpers (`isItemVisible`, `visibleCount`,
  // `visibleIndexOf`, `drawerGroupForReaderSetting`, `labelForGroup`) can
  // see them. They were previously private, which made the standalone
  // static helpers a private-access violation.
 public:
  // A single row in the drawer.
  //   - Setting-bound rows have non-null change/getValueText callbacks; each
  //     closure carries its own SettingInfo copy (see buildItems for why).
  //   - The BLE quick-action rows are special and use the activate callback.
  struct Item {
    StrId nameId;
    bool isAction = false;  // true: Confirm activates; false: Confirm toggles/cycles value
    // CrumBLE 4.5.6: non-interactive informational row. customName holds the
    // body text; the row paints across the full width with no value column
    // and no action arrow, and both Confirm and Left/Right become no-ops.
    // Used by buildItems' low-heap fallback path to surface "Reader settings
    // unavailable -- close and reopen the book" instead of silently pruning
    // every reader-bound row down to the two BT actions (which left users
    // with no idea why the drawer had degraded).
    bool isInfo = false;
    // CrumBLE 4.5.6 (revision 2): INX-style group separator row. Renders as
    // a header line ("Font", "Layout", etc.) with a +/- indicator on the
    // right showing whether its group is expanded or collapsed. Confirm
    // toggles its group's expansion; Left/Right are no-ops. Setting-bound
    // rows within a collapsed group are skipped by both render and
    // navigation, keeping the visible list short (5-7 rows when one group
    // is open vs. ~13 if everything was flat).
    bool isGroupSeparator = false;
    // -1 = top-level (always visible; used by BT rows + group separators
    // themselves). >=0 = setting-bound row that's a member of the group
    // with this id; visible only when expandedGroupId_ matches.
    int groupId = -1;
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

  // CrumBLE 4.5.6 (revision 2): group IDs for the INX-style collapsible
  // drawer. Order here is the visual order in the drawer body.
  enum DrawerGroup {
    kGroupFont = 0,
    kGroupLayout = 1,
    kGroupReadingAids = 2,
    kGroupImages = 3,
    kGroupCount = 4,
  };

 private:
  void buildItems();
  // CrumBLE: BT rows extracted so they can be inserted at the TOP of the
  // drawer list (most common drawer action mid-read).
  void buildBluetoothItems();
  // v18.9.9.59: Compat Mode toggle row. Sits right below BT actions in both
  // the initial build and the group-expand rebuild path. Writes/clears the
  // path-aware compat sidecar (compat_prepared / compat_custom) and closes
  // the drawer so the reader picks up the change via APP_STATE.compatModeChanged.
  void buildCompatModeItem();
  // CrumBLE 4.5.6 rev 3 (INX lazy pattern): compose BT actions + group
  // separators (+ expanded group's members if any). Called on ctor and
  // whenever the expanded group changes.
  void rebuildDrawerItems();
  // Lazily allocate settingsList_ via getSettingsList() on first group
  // expand. Returns false under low heap so the caller keeps the group
  // collapsed cleanly. v18.9.9.55: on low-heap refuse, attempts to fall
  // back to the SD-cached settings view snapshot (see SettingsViewCache
  // / v18.9.9.50). If the cache loads, populates settingsList_ with
  // view-only SettingInfo entries (valueGetter closures over cached
  // values, no valuePtr / valueSetter) and sets viewMode_ = true so
  // attemptSettingChange can redirect to silent-restart-with-
  // OpenBookSettingsDrawer instead of writing to SETTINGS.
  bool ensureSettingsSrcBuilt();
  // v18.9.9.55: populate settingsList_ from the SD cache for view-only
  // display when the live getSettingsList() build would OOM. Returns
  // true iff the cache existed and parsed; caller flips viewMode_.
  // v18.9.9.62: rewired -- populates viewRows_ directly (mirrors ROA's
  // v50 pattern). No SettingInfo conversion => no per-row vector copies
  // of enumStringLabels (the Font row's 15 SD-font names was the
  // dozens-of-small-allocs path that crashed the drawer under BT-tight
  // heap). Rendering + attemptSettingChange consult viewRows_ when
  // viewMode_ is true (see viewRowValueText / viewRowAt helpers).
  bool tryPopulateFromViewCache();
  // v18.9.9.62: read-only value string for a view-mode row. Mirrors
  // ReaderOptionsActivity::viewRowValueText -- resolves TOGGLE/ENUM/VALUE/
  // STRING types against the row's cached currentValue + enumStrIds +
  // enumStringLabels. No allocations beyond the returned std::string.
  std::string viewRowValueText(const SettingsViewRow& row) const;
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

  // CrumBLE 4.5.6 (revision 2): INX-style "one group expanded at a time"
  // state. -1 = all groups collapsed; otherwise a DrawerGroup id. Default
  // kGroupFont so the drawer opens with Font visible (the most common
  // mid-read tweak path). Confirm on a separator row toggles this; if the
  // user collapses the currently-open group, expandedGroupId_ goes to -1.
  int expandedGroupId_ = kGroupFont;

  // v18.9.9.25: caller-provided initial expand group, applied inside onEnter
  // once the settings source is safely buildable (usually because we're
  // running post-silent-restart on clean heap). -1 = no override, use the
  // default expandedGroupId_ = kGroupFont.
  int pendingInitialExpandedGroupId_ = -1;

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
  // v18.9.9.55 (task #40): drawer entered view-only mode because the
  // live getSettingsList() build refused on tight heap and the SD
  // cache was available. All setting-bound rows render from cached
  // valueGetter closures; attemptSettingChange redirects any edit
  // to a silent-restart-with-OpenBookSettingsDrawer preserving the
  // currently expanded group. User can still browse groups + values;
  // only the delta application path is blocked.
  bool viewMode_ = false;
  // v18.9.9.62: raw view-cache rows for view-mode rendering, replacing the
  // pre-v62 settingsList_-populated-with-SettingInfo-shells approach. Each
  // SettingsViewRow carries the nameId + type + currentValue + enum labels
  // needed to render name/value strings, without the SettingInfo's per-row
  // std::function slots and duplicated enum vectors. Item.settingIndex
  // under viewMode_ points into this vector.
  std::vector<SettingsViewRow> viewRows_;

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
