#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// CrumBLE submenu redesign: SUBMENU rows hold a child SettingInfo list and a
// title StrId. Confirm pushes the child list onto the SettingsActivity nav
// stack; Back pops. SECTION_HEADER is retained for short groups that stay
// inline rather than nesting.
enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING, SECTION_HEADER, SUBMENU };

enum class SettingAction {
  None,
  RemapFrontButtons,
  RemapFrontButtonsReader,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<uint8_t> enumRawValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  // CrumBLE submenu: when type == SUBMENU, `children` is the list shown after
  // the user opens this row. Empty for all other types.
  std::vector<SettingInfo> children;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  SettingInfo& withEnumRawValues(std::vector<uint8_t> values) {
    enumRawValues = std::move(values);
    return *this;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo SectionHeader(StrId nameId) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SECTION_HEADER;
    return s;
  }

  // CrumBLE: build a SUBMENU row. nameId is the row label, children is the
  // SettingInfo list rendered after the user opens it. The user navigates
  // back with the Back button, which pops the nav stack.
  static SettingInfo Submenu(StrId nameId, std::vector<SettingInfo> children) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SUBMENU;
    s.children = std::move(children);
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

inline size_t settingEnumOptionCount(const SettingInfo& setting) {
  return setting.enumStringValues.empty() ? setting.enumValues.size() : setting.enumStringValues.size();
}

inline std::string settingEnumOptionLabel(const SettingInfo& setting, const uint8_t displayIndex) {
  if (!setting.enumStringValues.empty()) {
    return displayIndex < setting.enumStringValues.size() ? setting.enumStringValues[displayIndex] : std::string();
  }
  return displayIndex < setting.enumValues.size() ? std::string(I18N.get(setting.enumValues[displayIndex]))
                                                  : std::string();
}

class SettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  // CrumBLE submenu redesign: replaces the old four-category-tabs top level.
  // rootSettings_ holds the six-entry top-level list (Display / Reader /
  // Controls / Library / Sync & Network / System). Opening a SUBMENU row
  // pushes a frame onto navStack_; Back pops it. selectedIndex is stored per
  // frame so the cursor restores when the user backs out of a sub-screen.
  std::vector<SettingInfo> rootSettings_;

  struct NavFrame {
    // Pointer-stable for the duration of the activity: rootSettings_ is the
    // base, and each submenu's `children` vector is owned by its parent
    // entry in rootSettings_, so the address stays valid as long as we
    // don't mutate the parent.
    const std::vector<SettingInfo>* settings = nullptr;
    StrId titleId = StrId::STR_SETTINGS_TITLE;
    int selectedIndex = 0;
  };
  std::vector<NavFrame> navStack_;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

  // Submenu nav helpers. enterSubmenu pushes the row's child list; goBack
  // pops; currentFrame returns the top of the stack (or rootSettings_ if
  // empty). selectableIndex skips SECTION_HEADER entries.
  NavFrame& currentFrame() { return navStack_.back(); }
  const NavFrame& currentFrame() const { return navStack_.back(); }
  const std::vector<SettingInfo>& currentSettings() const { return *currentFrame().settings; }
  int currentSettingsCount() const { return static_cast<int>(currentSettings().size()); }
  void enterSubmenu(const SettingInfo& row);
  void goBack();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Settings", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
