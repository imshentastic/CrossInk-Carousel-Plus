#pragma once

#include <HalClock.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// CrumBLE 4.4 (ported from upstream CrossInk v1.3.2): font sizes now display
// as "<pt> pt" instead of friendly names like "Tiny" / "Small" / "Medium".
// This lets SD-card font sizes interleave intuitively with built-in sizes
// (the user sees "10 pt / 12 pt / 14 pt" everywhere instead of a mix of
// "Tiny / Small / Medium" and bare "11 pt / 13 pt").
inline std::string fontSizePointLabel(const uint8_t pointSize) {
  return std::to_string(static_cast<int>(pointSize)) + " pt";
}

inline SettingInfo buildBuiltinFontSizeSetting() {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::fontSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;

  // Order mirrors the prior StrId list: TINY, SMALL, MEDIUM, LARGE, X_LARGE,
  // TEENSY, HUGE, ITTY_BITTY -- matches the user's current muscle memory of
  // where each size sits in the cycle.
  using S = CrossPointSettings;
  struct Entry { S::FONT_SIZE raw; bool include; };
  const Entry entries[] = {
#ifndef OMIT_TINY_FONT
      {S::TINY, true},
#else
      {S::TINY, false},
#endif
#ifndef OMIT_SMALL_FONT
      {S::SMALL, true},
#else
      {S::SMALL, false},
#endif
#ifndef OMIT_MEDIUM_FONT
      {S::MEDIUM, true},
#else
      {S::MEDIUM, false},
#endif
#ifndef OMIT_LARGE_FONT
      {S::LARGE, true},
#else
      {S::LARGE, false},
#endif
#ifndef OMIT_XLARGE_FONT
      {S::EXTRA_LARGE, true},
#else
      {S::EXTRA_LARGE, false},
#endif
#ifndef OMIT_TEENSY_FONT
      {S::TEENSY, true},
#else
      {S::TEENSY, false},
#endif
#ifndef OMIT_HUGE_FONT
      {S::HUGE_SIZE, true},
#else
      {S::HUGE_SIZE, false},
#endif
#ifndef OMIT_ITTY_BITTY_FONT
      {S::ITTY_BITTY, true},
#else
      {S::ITTY_BITTY, false},
#endif
  };

  for (const auto& e : entries) {
    if (!e.include) continue;
    s.enumStringValues.push_back(fontSizePointLabel(S::getReaderFontPointSize(e.raw)));
    s.enumRawValues.push_back(static_cast<uint8_t>(e.raw));
  }
  return s;
}

inline SettingInfo buildSdFontSizeSetting(const SdCardFontFamilyInfo& family) {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::fontSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;

  const std::vector<uint8_t> sizes = family.availableSizes();
  s.enumStringValues.reserve(sizes.size());
  s.enumRawValues.reserve(sizes.size());
  for (size_t i = 0; i < sizes.size(); i++) {
    // CrumBLE 4.4: same pt-label format as builtin sizes; no special-casing.
    s.enumStringValues.push_back(fontSizePointLabel(sizes[i]));
    s.enumRawValues.push_back(static_cast<uint8_t>(i));
  }
  return s;
}

inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  if (registry && SETTINGS.sdFontFamilyName[0] != '\0') {
    const SdCardFontFamilyInfo* family = registry->findFamily(SETTINGS.sdFontFamilyName);
    if (family && !family->files.empty()) {
      return buildSdFontSizeSetting(*family);
    }
  }
  return buildBuiltinFontSizeSetting();
}

inline uint8_t closestPointSizeIndex(const std::vector<uint8_t>& sizes, const uint8_t targetPointSize) {
  if (sizes.empty()) return 0;

  uint8_t bestIndex = 0;
  uint8_t bestDiff = UINT8_MAX;
  for (size_t i = 0; i < sizes.size(); i++) {
    const uint8_t size = sizes[i];
    const uint8_t diff = size > targetPointSize ? size - targetPointSize : targetPointSize - size;
    if (diff < bestDiff || (diff == bestDiff && size < sizes[bestIndex])) {
      bestIndex = static_cast<uint8_t>(i);
      bestDiff = diff;
    }
  }
  return bestIndex;
}

inline uint8_t closestBuiltinFontSizeIndex(const uint8_t targetPointSize) {
  uint8_t bestStored = 0;
  uint8_t bestPointSize = 0;
  uint8_t bestDiff = UINT8_MAX;

  for (uint8_t i = 0; i < CrossPointSettings::FONT_SIZE_COUNT; i++) {
    const auto size = static_cast<CrossPointSettings::FONT_SIZE>(i);
    const uint8_t stored = CrossPointSettings::getStoredReaderFontSize(size);
    if (stored == UINT8_MAX) continue;

    const uint8_t pointSize = CrossPointSettings::getReaderFontPointSize(size);
    const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
    if (diff < bestDiff || (diff == bestDiff && pointSize < bestPointSize)) {
      bestStored = stored;
      bestPointSize = pointSize;
      bestDiff = diff;
    }
  }
  return bestStored;
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
// CrumBLE 4.5.4 Shape 3: UI Font Fallback picker. Lists "None" + every
// SD-card family currently present on disk. Stores the chosen family
// name (or empty for "None") in SETTINGS.uiFontFallbackFamily. The
// per-tick poll in main.cpp loop() picks up the change without reboot.
inline SettingInfo buildUiFontFallbackSetting(const SdCardFontRegistry* registry) {
  SettingInfo s;
  s.nameId = StrId::STR_UI_FONT_FALLBACK;
  s.type = SettingType::ENUM;
  s.key = "uiFontFallbackFamily";
  s.category = StrId::STR_CAT_DISPLAY;

  // Index 0 is always "None" so the user always has a way back to the
  // disabled state regardless of what's on SD.
  std::vector<std::string> familyNames;
  familyNames.reserve(1);
  familyNames.push_back("");  // "None" sentinel -- empty string in storage
  if (registry) {
    for (const auto& f : registry->getFamilies()) {
      familyNames.push_back(f.name);
    }
  }
  // The render code prefers enumStringValues over enumValues when both
  // are present, so build display labels here.
  s.enumStringValues.reserve(familyNames.size());
  s.enumStringValues.push_back("None");
  for (size_t i = 1; i < familyNames.size(); ++i) {
    s.enumStringValues.push_back(familyNames[i]);
  }

  s.valueGetter = [familyNames]() -> uint8_t {
    const char* cur = SETTINGS.uiFontFallbackFamily;
    if (cur[0] == '\0') return 0;
    for (size_t i = 1; i < familyNames.size(); ++i) {
      if (familyNames[i] == cur) return static_cast<uint8_t>(i);
    }
    // Stored family is no longer on SD (deleted since last save). Show as
    // "None" but DON'T clear the saved name -- if the file comes back
    // (user re-uploads same .cpfont) we'll auto-rebind.
    return 0;
  };

  s.valueSetter = [familyNames](uint8_t v) {
    SETTINGS.uiFontFallbackFamily[0] = '\0';
    if (v == 0 || v >= familyNames.size()) return;
    const std::string& name = familyNames[v];
    strncpy(SETTINGS.uiFontFallbackFamily, name.c_str(), sizeof(SETTINGS.uiFontFallbackFamily) - 1);
    SETTINGS.uiFontFallbackFamily[sizeof(SETTINGS.uiFontFallbackFamily) - 1] = '\0';
    // Family change resets the picked size to "auto" -- the user's last
    // size was relative to the prior family, which may not have a
    // matching size in the new one. ensureFallbackLoaded honors 0 = auto.
    SETTINGS.uiFontFallbackPointSize = 0;
  };
  return s;
}

// CrumBLE 4.5.4: companion size picker for the fallback family. Lists
// "Auto (smallest)" + every size available in the currently-selected
// uiFontFallbackFamily. Stored as the literal point size (uint8_t);
// 0 = auto = legacy behavior (smallest available). The per-tick poll
// picks up the change without reboot.
inline SettingInfo buildUiFontFallbackSizeSetting(const SdCardFontRegistry* registry) {
  SettingInfo s;
  s.nameId = StrId::STR_UI_FONT_FALLBACK_SIZE;
  s.type = SettingType::ENUM;
  s.key = "uiFontFallbackPointSize";
  s.category = StrId::STR_CAT_DISPLAY;

  // sizes[0] reserved for 0 = auto sentinel. Remaining entries: literal pt
  // sizes from the currently-selected family. If no family is selected,
  // we still show the picker (with just "Auto"); it's a no-op until a
  // family is chosen.
  std::vector<uint8_t> sizes;
  sizes.reserve(8);
  sizes.push_back(0);  // auto
  if (registry && SETTINGS.uiFontFallbackFamily[0] != '\0') {
    const auto* fam = registry->findFamily(SETTINGS.uiFontFallbackFamily);
    if (fam) {
      for (const uint8_t pt : fam->availableSizes()) sizes.push_back(pt);
    }
  }

  s.enumStringValues.reserve(sizes.size());
  s.enumStringValues.push_back("Auto");
  for (size_t i = 1; i < sizes.size(); ++i) {
    s.enumStringValues.push_back(std::to_string(static_cast<int>(sizes[i])) + " pt");
  }

  s.valueGetter = [sizes]() -> uint8_t {
    const uint8_t cur = SETTINGS.uiFontFallbackPointSize;
    if (cur == 0) return 0;
    for (size_t i = 1; i < sizes.size(); ++i) {
      if (sizes[i] == cur) return static_cast<uint8_t>(i);
    }
    return 0;
  };

  s.valueSetter = [sizes](uint8_t v) {
    if (v == 0 || v >= sizes.size()) {
      SETTINGS.uiFontFallbackPointSize = 0;
      return;
    }
    SETTINGS.uiFontFallbackPointSize = sizes[v];
  };
  return s;
}

inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId). CrumBLE: OMIT_BITTER_FONT,
  // OMIT_CHAREINK_FONT and OMIT_LEXENDDECA_FONT drop their respective
  // family options from the picker so each tiny-* variant only shows
  // fonts that are embedded. Users can install any of the three as an
  // SD-card .cpfont and the picker appends them from the registry.
  std::vector<StrId> enumValues = {
#ifndef OMIT_LEXENDDECA_FONT
      StrId::STR_LEXEND_DECA,
#endif
#ifndef OMIT_BITTER_FONT
      StrId::STR_BITTER,
#endif
#ifndef OMIT_CHAREINK_FONT
      StrId::STR_CHAREINK,
#endif
  };
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
#ifndef OMIT_LEXENDDECA_FONT
    allStringValues.push_back(I18N.get(StrId::STR_LEXEND_DECA));
#endif
#ifndef OMIT_BITTER_FONT
    allStringValues.push_back(I18N.get(StrId::STR_BITTER));
#endif
#ifndef OMIT_CHAREINK_FONT
    allStringValues.push_back(I18N.get(StrId::STR_CHAREINK));
#endif
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  std::vector<std::vector<uint8_t>> sdFamilySizes;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    sdFamilySizes.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilySizes),
                   [](const SdCardFontFamilyInfo& f) { return f.availableSizes(); });
  }

  // CrumBLE 4.2: built-in slots are gated by OMIT_BITTER_FONT,
  // OMIT_LEXENDDECA_FONT and OMIT_CHAREINK_FONT. Each tiny-* variant's
  // display order is whichever families survive the OMITs (in
  // fontFamily-enum order) followed by SD families. The picker addresses
  // by DISPLAY INDEX, but SETTINGS.fontFamily stores the RAW enum value
  // -- so getter/setter need a mapping that respects OMIT. The unknown-
  // value fallback routes through BUILTIN_DEFAULT_FONT_FAMILY so each
  // variant (tiny-bitter, tiny-lexend, tiny-chareink) lands on whichever
  // family it ships with rather than a hardcoded Bitter that may be OMIT'd.
  constexpr uint8_t kAvailableBuiltinCount = 0
#ifndef OMIT_LEXENDDECA_FONT
                                             + 1
#endif
#ifndef OMIT_BITTER_FONT
                                             + 1
#endif
#ifndef OMIT_CHAREINK_FONT
                                             + 1
#endif
      ;
  auto fontFamilyRawToDisplay = [](uint8_t ff) -> uint8_t {
    uint8_t pos = 0;
#ifndef OMIT_LEXENDDECA_FONT
    if (ff == CrossPointSettings::LEXENDDECA) return pos;
    pos++;
#endif
#ifndef OMIT_BITTER_FONT
    if (ff == CrossPointSettings::BITTER) return pos;
    pos++;
#endif
#ifndef OMIT_CHAREINK_FONT
    if (ff == CrossPointSettings::CHAREINK) return pos;
#endif
    return 0;  // unknown raw value -- fall back to first built-in
  };
  auto displayToFontFamilyRaw = [](uint8_t d) -> uint8_t {
    uint8_t pos = 0;
#ifndef OMIT_LEXENDDECA_FONT
    if (d == pos++) return CrossPointSettings::LEXENDDECA;
#endif
#ifndef OMIT_BITTER_FONT
    if (d == pos++) return CrossPointSettings::BITTER;
#endif
#ifndef OMIT_CHAREINK_FONT
    if (d == pos++) return CrossPointSettings::CHAREINK;
#endif
    return CrossPointSettings::BUILTIN_DEFAULT_FONT_FAMILY;
  };

  s.valueGetter = [sdFamilyNames, fontFamilyRawToDisplay]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(kAvailableBuiltinCount + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return fontFamilyRawToDisplay(SETTINGS.fontFamily);
  };

  s.valueSetter = [sdFamilyNames, sdFamilySizes, displayToFontFamilyRaw](uint8_t v) {
    uint8_t targetPointSize = CrossPointSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (size_t i = 0; i < sdFamilyNames.size(); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName && SETTINGS.fontSize < sdFamilySizes[i].size()) {
          targetPointSize = sdFamilySizes[i][SETTINGS.fontSize];
          break;
        }
      }
    }

    if (v < kAvailableBuiltinCount) {
      SETTINGS.fontFamily = displayToFontFamilyRaw(v);
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.fontSize = closestBuiltinFontSizeIndex(targetPointSize);
    } else {
      int sdIdx = v - kAvailableBuiltinCount;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        SETTINGS.fontSize = closestPointSizeIndex(sdFamilySizes[sdIdx], targetPointSize);
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

inline SettingInfo buildSleepScreenSetting() {
  // v18.9.9.446: Reading Stats sleep screen (the pre-v445 BookStatsView-on-
  // sleep mode) removed from the picker — superseded by Minimal Stats which
  // has cleaner typography and consistent theming with the rest of Minimal.
  // Enum value + dispatch case are kept in code for source-compat but
  // legacy persisted saves auto-migrate at boot (see main.cpp migration).
  SettingInfo s = SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                                    {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER,
                                     StrId::STR_NONE_OPT, StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY,
                                     StrId::STR_THEME_MINIMAL, StrId::STR_QUICK_RESUME,
                                     StrId::STR_MINIMAL_STATS},
                                    "sleepScreen", StrId::STR_CAT_DISPLAY);
  s.withEnumRawValues({
      static_cast<uint8_t>(CrossPointSettings::DARK),
      static_cast<uint8_t>(CrossPointSettings::LIGHT),
      static_cast<uint8_t>(CrossPointSettings::CUSTOM),
      static_cast<uint8_t>(CrossPointSettings::COVER),
      static_cast<uint8_t>(CrossPointSettings::BLANK),
      static_cast<uint8_t>(CrossPointSettings::COVER_CUSTOM),
      static_cast<uint8_t>(CrossPointSettings::OVERLAY),
      static_cast<uint8_t>(CrossPointSettings::MINIMAL_SLEEP),
      static_cast<uint8_t>(CrossPointSettings::QUICK_RESUME),
      static_cast<uint8_t>(CrossPointSettings::MINIMAL_STATS_SLEEP),
  });
  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. When an
// SdCardFontRegistry is supplied AND has SD card fonts installed, the
// font-family entry is replaced in a per-call copy with a registry-aware
// version. Callers without SD fonts pay only a vector copy.
// v4.7.3: the immutable base list, by reference -- no copy, no allocation after
// first use. Callers that only read key / valuePtr / stringOffset (persistence)
// must use this: getSettingsList() deep-copies ~64 entries, each with its own
// enum-string vectors, and on ESP32 a failed allocation in that copy aborts the
// process rather than failing. That copy ran on every settings write.
inline const std::vector<SettingInfo>& getSettingsListBase() {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v;
    // v4.7.5: MUST stay >= the number of add() calls below (69 at time of
    // writing). SettingInfo is a ~200-byte struct (three enum vectors, four
    // std::function slots, a children vector), and this list is a function-
    // local static that lives for the whole process -- so an under-reserve is
    // not a transient cost. At reserve(64) the 69 add()s pushed capacity to
    // 128, leaving ~59 unused slots (~13 KB of DRAM) resident forever on a
    // device with ~380 KB total. Overshooting wastes the same way, so keep the
    // headroom small and bump it deliberately; the check after the loop shouts
    // if a new setting outgrows it.
    constexpr size_t kBaseSettingsReserve = 72;
    v.reserve(kBaseSettingsReserve);
    auto add = [&v](SettingInfo setting) { v.push_back(std::move(setting)); };

    // --- Display ---
    add(buildSleepScreenSetting());
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_CYCLE_SCREENSAVER_ON_TAP, &CrossPointSettings::cycleScreensaverOnTap,
                            "cycleScreensaverOnTap", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SLEEP_CYCLE_SKIP_GRAYSCALE,
                            &CrossPointSettings::sleepCycleSkipGrayscale,
                            "sleepCycleSkipGrayscale", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SLEEP_CYCLE_DOUBLE_TAP_BACK,
                            &CrossPointSettings::sleepCycleDoubleTapBack,
                            "sleepCycleDoubleTapBack", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SLEEP_CYCLE_DAILY_MODE,
                            &CrossPointSettings::sleepCycleDailyMode,
                            "sleepCycleDailyMode", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_SLEEP_SCREEN_ORDER, &CrossPointSettings::sleepScreenOrder,
                          {StrId::STR_SLEEP_ORDER_RANDOM, StrId::STR_SLEEP_ORDER_ALPHABETICAL}, "sleepScreenOrder",
                          StrId::STR_CAT_DISPLAY));
    // CrumBLE: was Off / On (mapped Off=NEVER, On=AFTER_TIMEOUT). Bumped
    // to a three-option enum so the user can pick "Always" for fast wake
    // regardless of how they put the device to sleep -- pairs naturally
    // with a custom sleep image (sleepScreen = CUSTOM / COVER / etc.).
    // Indices map 1:1 to the QUICK_RESUME_SLEEP_SCREEN enum values.
    add(SettingInfo::Enum(StrId::STR_QUICK_RESUME, &CrossPointSettings::quickResumeSleepScreen,
                          {StrId::STR_STATE_OFF, StrId::STR_QUICK_RESUME_AFTER_TIMEOUT,
                           StrId::STR_QUICK_RESUME_ALWAYS_LABEL},
                          "quickResumeSleepScreen", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                          StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                          {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                           StrId::STR_THEME_FLOW, StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_MINIMAL,
                           StrId::STR_THEME_DASHBOARD
#if defined(CROSSINK_ENABLE_LYRA_CAROUSEL) && CROSSINK_ENABLE_LYRA_CAROUSEL
                           ,
                           StrId::STR_THEME_LYRA_CAROUSEL
#endif
                          },
                          "uiTheme", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_THEME::CLASSIC, CrossPointSettings::UI_THEME::LYRA,
                                CrossPointSettings::UI_THEME::LYRA_3_COVERS, CrossPointSettings::UI_THEME::LYRA_FLOW,
                                CrossPointSettings::UI_THEME::ROUNDEDRAFF, CrossPointSettings::UI_THEME::MINIMAL,
                                CrossPointSettings::UI_THEME::DASHBOARD
#if defined(CROSSINK_ENABLE_LYRA_CAROUSEL) && CROSSINK_ENABLE_LYRA_CAROUSEL
                                ,
                                CrossPointSettings::UI_THEME::LYRA_CAROUSEL
#endif
            }));
    add(SettingInfo::Enum(StrId::STR_RECENT_BOOKS_VIEW, &CrossPointSettings::recentBooksView,
                          {StrId::STR_LIST_VIEW, StrId::STR_GRID_VIEW}, "recentBooksView", StrId::STR_CAT_DISPLAY));
    // CrumBLE #133: persistence-only registration. Category left as
    // STR_NONE_OPT so SettingsActivity skips it (line ~140 filters those
    // out); the user-facing toggle lives in BookshelfPickerActivity's
    // "Layout" row instead. JsonSettingsIO still picks it up because it
    // iterates the registered list regardless of category.
    add(SettingInfo::Enum(StrId::STR_BOOKSHELF_LAYOUT, &CrossPointSettings::bookshelfLayout,
                          {StrId::STR_LAYOUT_3X3, StrId::STR_LAYOUT_4X4, StrId::STR_LAYOUT_2X2}, "bookshelfLayout"));
    // CrumBLE #133 follow-up: also persistence-only. Toggle lives in
    // the BookshelfPicker's "Title Placement" row, not in Settings UI.
    add(SettingInfo::Enum(StrId::STR_BOOKSHELF_TITLE_PLACEMENT, &CrossPointSettings::bookshelfTitlePlacement,
                          {StrId::STR_PLACEMENT_BOTTOM, StrId::STR_PLACEMENT_TOP}, "bookshelfTitlePlacement"));
    // CrumBLE 4.6: Cover Tone setting disabled before 4.5.0 ship -- range
    // compression LUTs (Mild=30..225, Strong=60..200) produced no perceptible
    // difference on 1-bit Atkinson dither at thumbnail sizes during testing.
    // Setting field + ToneCurve LUT module + converter coverTone params stay
    // (dead code) so re-enabling is just uncommenting this Enum registration
    // and the per-tick poll in main.cpp.
    add(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY));
    // v18.9.9.178: UI Font Fallback picker removed. Feature was armed by
    // the setting value and cost ~8-9 KB per session on first glyph miss.
    // Retired for heap headroom; box glyphs render as tofu; long-term fix
    // is expanding Bitter's Unicode coverage (see memory notes).

    // --- Reader ---
    // Built-in font-family entry. Replaced per-call with a registry-aware
    // version when SD fonts are installed.
    add(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                          {
#ifndef OMIT_LEXENDDECA_FONT
                              StrId::STR_LEXEND_DECA,
#endif
#ifndef OMIT_BITTER_FONT
                              StrId::STR_BITTER,
#endif
#ifndef OMIT_CHAREINK_FONT
                              StrId::STR_CHAREINK,
#endif
                          },
                          "fontFamily",
                          StrId::STR_CAT_READER)
            .withEnumRawValues({
#ifndef OMIT_LEXENDDECA_FONT
                static_cast<uint8_t>(CrossPointSettings::LEXENDDECA),
#endif
#ifndef OMIT_BITTER_FONT
                static_cast<uint8_t>(CrossPointSettings::BITTER),
#endif
#ifndef OMIT_CHAREINK_FONT
                static_cast<uint8_t>(CrossPointSettings::CHAREINK),
#endif
            }));
    add(buildBuiltinFontSizeSetting());
    // CrumBLE: "Download Font Size Range" (sdFontSizeRange) is omitted from the
    // settings UI. It existed to pick SD-font point sizes per hardware variant,
    // but this single build ships a fixed set of reader font sizes, and the
    // compile-time default already matches them. The field + default are kept so
    // SD-font downloads still resolve point sizes; only the user-facing picker is
    // removed (it was redundant and confusing).
    add(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing",
                          StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                          "orientation", StrId::STR_CAT_READER));
    add(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                           StrId::STR_CAT_READER));
    add(SettingInfo::Enum(
        StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
        "paragraphAlignment", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER));
    // v18.9.9.405: opt-in single-pass page turn. See CrossPointSettings.h
    // for the rationale.
    add(SettingInfo::Toggle(StrId::STR_SINGLE_PASS_PAGE_TURN, &CrossPointSettings::singlePassPageTurn,
                            "singlePassPageTurn", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::readerDarkMode, "readerDarkMode",
                            StrId::STR_CAT_READER));
    // CrumBLE 4.4 (ported from CPR-vCodex): Text Darkness, 4-way enum.
    // Affects only the 2-bit grayscale glyph blit (visible when Text AA is on).
    add(SettingInfo::Enum(StrId::STR_TEXT_DARKNESS, &CrossPointSettings::textDarkness,
                          {StrId::STR_TEXT_DARKNESS_NORMAL, StrId::STR_TEXT_DARKNESS_LEGACY_BW,
                           StrId::STR_TEXT_DARKNESS_DARK, StrId::STR_TEXT_DARKNESS_EXTRA_DARK},
                          "textDarkness", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER));
    // v18.9.9.24: user-facing tables toggle. PARAGRAPHS collapses cells to
    // paragraph runs at parse time (same guard Compat mode uses). Locked to
    // PARAGRAPHS while APP_STATE.readerCompatModeActive is true -- see
    // isCompatLockedSetting in SettingsActivity.cpp.
    add(SettingInfo::Enum(StrId::STR_TABLES, &CrossPointSettings::tableRendering,
                          {StrId::STR_TABLES_DISPLAY, StrId::STR_TABLES_PARAGRAPHS},
                          "tableRendering", StrId::STR_CAT_READER));
    // CrumBLE 4.4: promoted from a two-state toggle ("Extra Spacing") to a
    // three-way enum ("Paragraph Spacing"). 0/TIGHT = classic text-indent
    // paragraphs with no vertical gap; 1/NORMAL = block-style paragraphs with
    // a lineHeight/2 gap (the prior toggle-on default); 2/WIDE = block-style
    // with a full lineHeight gap. The on-disk byte format is unchanged so
    // existing configs and section caches with 0/1 round-trip identically.
    add(SettingInfo::Enum(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE},
                          "extraParagraphSpacing", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &CrossPointSettings::forceParagraphIndents,
                            "forceParagraphIndents", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReadingEnabled,
                            "bionicReadingEnabled", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_GUIDE_READING, &CrossPointSettings::guideReadingEnabled, "guideReadingEnabled",
                            StrId::STR_CAT_READER));
    // v18.9.9.78: Stable Page Numbers toggle. When on, status bar shows book-wide
    // "Stable page X of Y" derived from byte position. Fixed 1500-char divisor
    // matches CrossInk / KOReader default. Persistence-only for stablePageChars
    // (advanced setting via /api/save-reader-settings; not in on-device UI).
    add(SettingInfo::Toggle(StrId::STR_STABLE_PAGE_NUMBERS, &CrossPointSettings::showStablePageNumbers,
                            "showStablePageNumbers", StrId::STR_CAT_READER));
    // CrumBLE 4.4: persistence-only registration (no on-device Settings UI).
    // Diagnostic toggle to A/B test whether the v40 glyph atlas install path
    // is the source of the FT upload heap regression. Default ON keeps the
    // optimized render path; flip OFF via /api/save-reader-settings to fall
    // back to the v39 embedded subset (or full SD-font fetch) path.
    add(SettingInfo::Toggle(StrId::STR_NONE_OPT, &CrossPointSettings::glyphAtlasEnabled,
                            "glyphAtlasEnabled"));

    // --- Controls ---
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::sideButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_YES}, "sideButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LONG_PRESS, &CrossPointSettings::sideButtonLongPress,
                          {StrId::STR_CHAPTER_SKIP_OPT, StrId::STR_CHANGE_FONT_SIZE, StrId::STR_IGNORE,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "sideButtonLongPress", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::frontButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_NAV_BUTTONS, StrId::STR_ALL_BUTTONS},
                          "frontButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(
        StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT,
         StrId::STR_TOGGLE_GUIDE_DOTS, StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_TOGGLE_BOOKMARK,
         StrId::STR_SYNC_PROGRESS, StrId::STR_MARK_FINISHED, StrId::STR_READING_STATS, StrId::STR_SCREENSHOT_BUTTON,
         StrId::STR_CYCLE_PAGE_TURN, StrId::STR_FILE_TRANSFER},
        "shortPwrBtn", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(
        StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::longPwrBtn,
        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT,
         StrId::STR_TOGGLE_GUIDE_DOTS, StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_TOGGLE_BOOKMARK,
         StrId::STR_SYNC_PROGRESS, StrId::STR_MARK_FINISHED, StrId::STR_READING_STATS, StrId::STR_SCREENSHOT_BUTTON,
         StrId::STR_CYCLE_PAGE_TURN, StrId::STR_FILE_TRANSFER},
        "longPwrBtn", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_MENU_ACTION, &CrossPointSettings::longPressMenuAction,
                          {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_CHANGE_FONT, StrId::STR_TOGGLE_GUIDE_DOTS,
                           StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_TOGGLE_BOOKMARK, StrId::STR_FORCE_REFRESH,
                           StrId::STR_SYNC_PROGRESS, StrId::STR_MARK_FINISHED, StrId::STR_READING_STATS,
                           StrId::STR_SCREENSHOT_BUTTON, StrId::STR_CYCLE_PAGE_TURN, StrId::STR_FILE_TRANSFER,
                           StrId::STR_BOOK_SETTINGS},
                          "longPressMenuAction", StrId::STR_CAT_CONTROLS));

    // --- System ---
    add(SettingInfo::Value(
        StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
        {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
    // v18.9.4/18.9.5.1: BT auto-disconnect. Slider UI matches Time to Sleep
    // exactly (min 1, max 30, step 1) so the two related settings render
    // identically. Runtime check lives in main.cpp loop() and disables BT
    // when idle exceeds this window.
    add(SettingInfo::Value(
        StrId::STR_BT_AUTO_DISCONNECT, &CrossPointSettings::btAutoDisconnectMinutes,
        {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "btAutoDisconnectMinutes", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                            "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));
    // CrumBLE: opt-in series detection. Off by default to skip the
    // first-time OPF scan on libraries where most books don't have
    // Calibre / EPUB-3 series metadata anyway.
    add(SettingInfo::Toggle(StrId::STR_SERIES_DETECTION, &CrossPointSettings::seriesDetectionEnabled,
                            "seriesDetectionEnabled", StrId::STR_CAT_SYSTEM));
    // CrumBLE prebake: master switch for the off-device chapter-index
    // optimizer. Off by default so the device behaves exactly like stock
    // 3.7.3 until the user opts in. When on:
    //   - Section.cpp tries sections-prebake/<n>.bin as a read-only fallback
    //     (the prebake CLI / web optimizer puts its output there)
    //   - EpubReaderActivity loads prebake-manifest.json on book open and
    //     fires the "Use prepared layout?" prompt when current SETTINGS
    //     don't match the cache's recorded fingerprint
    //   - The reader's lazy background extractor processes any pending
    //     <book-hash>.zip dropped via the file manager into sections-prebake/
    add(SettingInfo::Toggle(StrId::STR_OPTIMIZE_CHAPTER_INDEXING, &CrossPointSettings::optimizeChapterIndexing,
                            "optimizeChapterIndexing", StrId::STR_CAT_SYSTEM));

    // v18.9.9.172: toggle for the C2 "Indexing page X of Y" popup text.
    add(SettingInfo::Toggle(StrId::STR_INDEXING_SHOW_PAGE_COUNT, &CrossPointSettings::showIndexingPageCount,
                            "showIndexingPageCount", StrId::STR_CAT_SYSTEM));

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
        [](const std::string& v) {
          KOREADER_STORE.setServerUrl(v);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        [](uint8_t v) {
          KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
          KOREADER_STORE.saveToFile();
        },
        "koMatchMethod", StrId::STR_KOREADER_SYNC));

    // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
    add(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                            "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
    // v18.9.9.463 (CrossInk parity): estimated time-left in status bar.
    // Needs per-book pace data (stats.avgSecondsPerForwardPage) to be
    // meaningful; hidden by default until the book has enough page turns
    // for a stable average.
    add(SettingInfo::Toggle(StrId::STR_TIME_LEFT, &CrossPointSettings::statusBarTimeLeft, "statusBarTimeLeft",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
    // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
    add(SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    // v18.9.9.343: Home header clock lives under Display>Theme&Layout,
    // separate from the in-book status-bar clock above so users can enable
    // one without the other (esp. on X4 where enabling either triggers
    // an NTP-sync silent-restart at boot).
    add(SettingInfo::Toggle(StrId::STR_HOME_CLOCK, &CrossPointSettings::homeClockShow, "homeClockShow",
                            StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
    // on next WiFi connect, which is useful when crossing time zones.
    add(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3).
    if (halTiltSensor.isAvailable()) {
      for (auto& setting : v) {
        if (setting.nameId == StrId::STR_SHORT_PWR_BTN || setting.nameId == StrId::STR_LONG_PRESS_ACTION ||
            setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION) {
          setting.enumValues.push_back(StrId::STR_TILT_PAGE_TURN);
        }
      }
      const auto shortPowerIt = std::find_if(
          v.begin(), v.end(), [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SHORT_PWR_BTN; });
      if (shortPowerIt != v.end()) {
        v.insert(shortPowerIt + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                                     {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                                     "tiltPageTurn", StrId::STR_CAT_CONTROLS));
      }
    }
    // v4.7.5: drift guard for kBaseSettingsReserve. Growing past the reserve
    // doubles capacity and strands the difference for the life of the process,
    // which is silent and invisible in testing -- this is how the list came to
    // sit at capacity 128 for 69 entries. Deliberately not a log call: this
    // runs during static init, potentially before logging is up. Shrinking to
    // fit costs one transient reallocation on the rare boot where the reserve
    // was outgrown, and keeps the resident cost honest either way.
    if (v.size() > kBaseSettingsReserve) v.shrink_to_fit();
    return v;
  }();
  return baseList;
}

inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
  // v4.7.5: reserve before copying. The two font-fallback rows below are NOT
  // in the base list (see the v18.9.9.318 note), so they are always appended
  // -- and a plain copy-construct allocates capacity exactly equal to the
  // source size. The first append therefore reallocated the whole vector,
  // briefly holding the old ~69-element buffer and a new ~138-element one at
  // the same time. At ~200 bytes per SettingInfo that is a ~30 KB transient
  // spike, on the exact path whose heap peak makes SettingsActivity::onEnter
  // decide to silent-restart. Reserving up front makes this one allocation.
  const std::vector<SettingInfo>& base = getSettingsListBase();
  std::vector<SettingInfo> v;
  v.reserve(base.size() + 2);
  v.assign(base.begin(), base.end());
  // v18.9.9.318: baseList never added placeholders for STR_UI_FONT_FALLBACK
  // or STR_UI_FONT_FALLBACK_SIZE, so the previous "replace via find_if"
  // pattern silently no-op'd -- the settings never made it into
  // allSettings, pushByName failed with "missing setting nameId=290/291",
  // and the Reader > Font submenu rendered short (visible symptom: empty
  // "Font" page after picking a new SD family). Now: replace if found,
  // else push. Runs BEFORE the FONT_FAMILY block so both substitutions
  // are in place even when registry is null.
  {
    auto it =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_UI_FONT_FALLBACK; });
    if (it != v.end()) {
      *it = buildUiFontFallbackSetting(registry);
    } else {
      v.push_back(buildUiFontFallbackSetting(registry));
    }
  }
  {
    auto it = std::find_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_UI_FONT_FALLBACK_SIZE; });
    if (it != v.end()) {
      *it = buildUiFontFallbackSizeSetting(registry);
    } else {
      v.push_back(buildUiFontFallbackSizeSetting(registry));
    }
  }
  if (registry && registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
    auto fontSizeIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (fontSizeIt != v.end()) {
      *fontSizeIt = buildFontSizeSetting(registry);
    }
  }
  return v;
}
