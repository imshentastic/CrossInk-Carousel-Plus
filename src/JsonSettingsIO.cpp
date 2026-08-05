#include "JsonSettingsIO.h"

#include <Arduino.h>  // ESP.getFreeHeap/getMaxAllocHeap (v18.9.9.351)
#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "WifiCredentialStore.h"

// Convert legacy settings.
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

bool isEnumRawValueAllowed(const SettingInfo& info, uint8_t value) {
  if (info.enumRawValues.empty()) {
    return value < settingEnumOptionCount(info);
  }
  return std::find(info.enumRawValues.begin(), info.enumRawValues.end(), value) != info.enumRawValues.end();
}

uint8_t defaultEnumRawValue(const SettingInfo& info, uint8_t fieldDefault) {
  if (isEnumRawValueAllowed(info, fieldDefault)) {
    return fieldDefault;
  }
  if (!info.enumRawValues.empty()) {
    return info.enumRawValues.front();
  }
  return 0;
}

bool isSleepScreenSetting(const SettingInfo& info) { return info.key && strcmp(info.key, "sleepScreen") == 0; }

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  doc["favoriteSleepImagePath"] = s.favoriteSleepImagePath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentArr.add(s.recentSleepImages[i]);
  doc["recentSleepPos"] = s.recentSleepPos;
  doc["recentSleepFill"] = s.recentSleepFill;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["pendingBookmarkSpine"] = s.pendingBookmarkSpine;
  doc["pendingBookmarkProgress"] = s.pendingBookmarkProgress;
  doc["showBootScreen"] = s.showBootScreen;
  doc["lastCrumbleVersion"] = s.lastCrumbleVersion;

  String json;
  serializeJson(doc, json);
  // v18.9.9.311: atomic write + rolling .bak. Prior direct writeFile could
  // corrupt crumble-state.json mid-write on a cheap SD card, leaving the
  // device with no persistent state (openEpubPath / silent-restart target
  // etc). writeFileWithBackup does tmp+rename with retry and promotes the
  // prior good file to .bak; loadFromFile below auto-recovers on parse
  // failure.
  return Storage.writeFileWithBackup(path, json);
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  s.favoriteSleepImagePath = doc["favoriteSleepImagePath"] | std::string("");
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount = recentArr.isNull() ? 0
                                             : std::min(static_cast<int>(recentArr.size()),
                                                        static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) s.recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  s.recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT)
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  s.recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (s.recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) s.pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.pendingBookmarkSpine = doc["pendingBookmarkSpine"] | static_cast<uint16_t>(UINT16_MAX);
  s.pendingBookmarkProgress = doc["pendingBookmarkProgress"] | static_cast<float>(-1.0f);
  s.showBootScreen = doc["showBootScreen"] | true;
  s.lastCrumbleVersion = doc["lastCrumbleVersion"] | std::string("");
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      uint8_t value = s.*(info.valuePtr);
      if (isSleepScreenSetting(info)) {
        value = CrossPointSettings::sleepScreenModeToStorage(value);
      }
      doc[info.key] = value;
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  // Reader-specific front button remap.
  doc["readerFrontButtonsEnabled"] = s.readerFrontButtonsEnabled;
  doc["readerFrontButtonBack"] = s.readerFrontButtonBack;
  doc["readerFrontButtonConfirm"] = s.readerFrontButtonConfirm;
  doc["readerFrontButtonLeft"] = s.readerFrontButtonLeft;
  doc["readerFrontButtonRight"] = s.readerFrontButtonRight;
  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  doc["fontFamily"] = s.fontFamily;
  // SD card font family name — not in SettingsList, save manually
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }
  // CrumBLE 4.5.4: UI font fallback family. Stored as a string so the
  // saved name survives changes to the SD font set on disk (Toggle order
  // depends on registry enumeration which can shift when families are
  // added/removed). Same shape as sdFontFamilyName.
  if (s.uiFontFallbackFamily[0] != '\0') {
    doc["uiFontFallbackFamily"] = s.uiFontFallbackFamily;
  }
  if (s.uiFontFallbackPointSize > 0) {
    doc["uiFontFallbackPointSize"] = s.uiFontFallbackPointSize;
  }

  // Bluetooth bonded remote metadata is not represented in SettingsList, but it
  // must survive reboot so the firmware can reconnect to the remembered device.
  doc["bleBondedDeviceAddr"] = s.bleBondedDeviceAddr;
  doc["bleBondedDeviceName"] = s.bleBondedDeviceName;
  doc["bleBondedDeviceAddrType"] = s.bleBondedDeviceAddrType;
  // v18.9.9.256: persist bluetoothEnabled. Pre-v256 this field was set in
  // RAM by BluetoothSettingsActivity's toggle and quick-connect paths but
  // never written to JSON -- every boot loadFromFile got the struct
  // default (0). The bug went unnoticed until v245 made the field
  // load-bearing at boot (BT-off release only fires when disk says 0),
  // at which point flipping BT on in Settings + silent-restart still
  // came back to bluetoothEnabled=0 -> release ran -> post-restart
  // enable() refused (v252 guard) -> BT stuck off.
  doc["bluetoothEnabled"] = s.bluetoothEnabled;

  // CrumBLE 4.5.5: rich BLE button map. Each occupied slot serialises as a
  // 3-element array [keyKind, keyValue, button] under a fixed-size array.
  // Empty slots are skipped on write; the reader uses positional matching
  // is not required (the file is replayed back into bleKeyMap[] starting
  // at slot 0, so order survives).
  {
    JsonArray arr = doc["bleKeyMap"].to<JsonArray>();
    for (const auto& e : s.bleKeyMap) {
      if (e.button == 0xFF || e.keyKind == 0xFF) continue;
      JsonArray row = arr.add<JsonArray>();
      row.add(e.keyKind);
      row.add(e.keyValue);
      row.add(e.button);
    }
  }

  // CrumBLE: opt-in virtual-collection visibility (Recently Added / All Books /
  // Finished / New). All persist with the same JSON keys to survive reboots.
  doc["showRecentlyAdded"] = s.showRecentlyAddedCollection;
  doc["showAllBooks"] = s.showAllBooksCollection;
  doc["showFinished"] = s.showFinishedCollection;
  doc["showNew"] = s.showNewCollection;

  // Cursor advanced by SleepActivity when sleepScreenCycleMode is on; uint16_t
  // so the SettingInfo (uint8_t-only) loop does not cover it.
  doc["sleepScreenCycleIndex"] = s.sleepScreenCycleIndex;
  doc["sleepCycleDoubleTapBack"] = s.sleepCycleDoubleTapBack;
  doc["sleepCycleDailyMode"] = s.sleepCycleDailyMode;
  doc["sleepCycleLastChangeDay"] = s.sleepCycleLastChangeDay;

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

  // v18.9.9.351: measure output size before reserving to avoid bad_alloc
  // in serializeJson->String::reserve. Field crash: preflight passed
  // (free=38k/maxAlloc=18k) but serialize threw because output needed
  // more contiguous than maxAlloc.
  const size_t neededBytes = measureJson(doc);
  const uint32_t neededWithMargin = static_cast<uint32_t>(std::max<size_t>(neededBytes * 3U / 2U, 4096U));
  const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
  const uint32_t freeNow = ESP.getFreeHeap();
  if (maxAllocNow < neededWithMargin) {
    LOG_ERR("JSI",
            "Settings write skipped: measured=%u needed+margin=%u exceeds maxAlloc=%u (free=%u)",
            static_cast<unsigned>(neededBytes), neededWithMargin, maxAllocNow, freeNow);
    return false;
  }
  String json;
  json.reserve(neededBytes + 32);
  serializeJson(doc, json);
  // v18.9.9.400: skip the SD write entirely when the serialized JSON matches
  // what we wrote last time. Field data: reader prebake-fingerprint-restore
  // path (EpubReaderActivity ~L1692/2489/2571) writes 8-15 SETTINGS fields on
  // every mismatched book open even when NONE of them differ from current
  // state -- e.g. reopening a book the reader already matches. Two "Debounced
  // save committed" lines per book open observed even when the user was doing
  // no interactive settings changes. FNV-1a hash of the fresh JSON compared
  // against the last-written hash cheaply detects the no-op case. In-memory
  // only (resets on boot); cost is one guaranteed write per boot even for
  // no-op saves, which is fine.
  static uint32_t lastWrittenHash = 0;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < json.length(); ++i) {
    hash ^= static_cast<uint8_t>(json[i]);
    hash *= 16777619u;
  }
  if (lastWrittenHash != 0 && hash == lastWrittenHash) {
    LOG_INF("JSI", "Settings write skipped: content unchanged (%u bytes, hash=0x%08x)",
            static_cast<unsigned>(json.length()), hash);
    return true;
  }
  // v18.9.9.311: atomic write + rolling .bak (see saveState above). Losing
  // settings.json wipes user preferences (font family/size, sleep screen
  // mode, WiFi auto-connect flag, etc.) -- worth protecting.
  const bool ok = Storage.writeFileWithBackup(path, json);
  if (ok) lastWrittenHash = hash;
  return ok;
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings file.
  // Populate s with migrated values now so the generic loop below picks them up as defaults and clamps them.
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  }
  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      const std::string fieldDefault = strPtr;  // current buffer = struct-initializer default
      std::string val;
      if (info.obfuscated) {
        obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "", &status);
        if (status == obfuscation::DecodeStatus::LEGACY && !val.empty() && needsResave) {
          *needsResave = true;
        }
        if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY || val.empty()) {
          val = doc[info.key] | fieldDefault;
          if (val != fieldDefault && needsResave) *needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        continue;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (isSleepScreenSetting(info)) {
        const uint8_t storedDefault = CrossPointSettings::sleepScreenModeToStorage(fieldDefault);
        const uint8_t storedValue = doc[info.key] | storedDefault;
        v = CrossPointSettings::sleepScreenStorageToMode(storedValue);
        if (CrossPointSettings::sleepScreenModeToStorage(v) != storedValue && needsResave) *needsResave = true;
        s.*(info.valuePtr) = v;
        continue;
      }
      if (info.type == SettingType::ENUM) {
        const bool isSdFontSize = info.key && strcmp(info.key, "fontSize") == 0 &&
                                  doc["sdFontFamilyName"].is<const char*>() &&
                                  doc["sdFontFamilyName"].as<const char*>()[0] != '\0';
        if (isSdFontSize && v < CrossPointSettings::SD_FONT_MAX_SIZE_STEPS) {
          // Keep a saved SD-family size index even when this build's built-in
          // font list has fewer choices; the registry-aware settings list will
          // clamp it to the selected family once SD fonts are discovered.
        } else if (!isEnumRawValueAllowed(info, v)) {
          v = defaultEnumRawValue(info, fieldDefault);
          if (needsResave) *needsResave = true;
        }
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)CrossPointSettings::SLEEP_10_MIN, CrossPointSettings::SLEEP_TIMEOUT_COUNT,
              (uint8_t)CrossPointSettings::SLEEP_10_MIN);
    s.sleepTimeoutMinutes = CrossPointSettings::sleepTimeoutEnumToMinutes(legacyValue);
    if (needsResave) *needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);
  // Reader-specific front button remap.
  s.readerFrontButtonsEnabled = clamp(doc["readerFrontButtonsEnabled"] | (uint8_t)0, (uint8_t)2, (uint8_t)0);
  s.readerFrontButtonBack =
      clamp(doc["readerFrontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.readerFrontButtonConfirm = clamp(doc["readerFrontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM,
                                     S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_CONFIRM);
  s.readerFrontButtonLeft =
      clamp(doc["readerFrontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.readerFrontButtonRight = clamp(doc["readerFrontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT,
                                   S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateReaderFrontButtonMapping(s);

  // Bluetooth bonded remote metadata.
  const std::string bondedAddr = doc["bleBondedDeviceAddr"] | std::string(s.bleBondedDeviceAddr);
  strncpy(s.bleBondedDeviceAddr, bondedAddr.c_str(), sizeof(s.bleBondedDeviceAddr) - 1);
  s.bleBondedDeviceAddr[sizeof(s.bleBondedDeviceAddr) - 1] = '\0';

  const std::string bondedName = doc["bleBondedDeviceName"] | std::string(s.bleBondedDeviceName);
  strncpy(s.bleBondedDeviceName, bondedName.c_str(), sizeof(s.bleBondedDeviceName) - 1);
  s.bleBondedDeviceName[sizeof(s.bleBondedDeviceName) - 1] = '\0';

  s.bleBondedDeviceAddrType = doc["bleBondedDeviceAddrType"] | s.bleBondedDeviceAddrType;

  // v18.9.9.256: bluetoothEnabled. Tolerates absence (pre-v256 configs)
  // by falling back to the current struct value, which is the pre-v256
  // default (0) on a fresh install.
  s.bluetoothEnabled = doc["bluetoothEnabled"] | s.bluetoothEnabled;

  // CrumBLE 4.5.5: rich BLE button map deserialise. Clear-then-fill so a
  // freshly-emptied entry on disk wipes the in-memory slot too. Tolerates
  // a missing key (older configs) -- ble button map stays empty.
  for (auto& e : s.bleKeyMap) {
    e = CrossPointSettings::BleKeyMapEntry{};
  }
  if (doc["bleKeyMap"].is<JsonArray>()) {
    JsonArray arr = doc["bleKeyMap"].as<JsonArray>();
    uint8_t slot = 0;
    for (JsonVariant rowVar : arr) {
      if (slot >= CrossPointSettings::BLE_KEY_MAP_CAPACITY) break;
      if (!rowVar.is<JsonArray>()) continue;
      JsonArray row = rowVar.as<JsonArray>();
      if (row.size() < 3) continue;
      const uint8_t kind = row[0] | 0xFF;
      const uint8_t value = row[1] | 0;
      const uint8_t button = row[2] | 0xFF;
      if (kind == 0xFF || button == 0xFF) continue;
      s.bleKeyMap[slot].keyKind = kind;
      s.bleKeyMap[slot].keyValue = value;
      s.bleKeyMap[slot].button = button;
      ++slot;
    }
  }

  // CrumBLE: opt-in virtual-collection visibility. If either key is present the
  // config predates neither (already migrated) -- clear the pending flag so
  // main.cpp skips the one-time existing-user migration. Absent keys leave it
  // pending so an existing user keeps All Books / Recently Added on first update.
  if (!doc["showRecentlyAdded"].isNull() || !doc["showAllBooks"].isNull()) {
    s.virtualCollectionsDefaultPending = false;
  }
  s.showRecentlyAddedCollection = doc["showRecentlyAdded"] | s.showRecentlyAddedCollection;
  s.showAllBooksCollection = doc["showAllBooks"] | s.showAllBooksCollection;
  // Finished / New default to OFF for everyone (including upgrading users) --
  // they're new-feature opt-ins and never had a "previously seen" state to
  // migrate. Their cache also costs an extra SD pass per book, so opt-in by
  // explicit toggle is the right policy.
  s.showFinishedCollection = doc["showFinished"] | s.showFinishedCollection;
  s.showNewCollection = doc["showNew"] | s.showNewCollection;

  // Sleep screen cycle cursor (uint16_t, not in SettingInfo loop).
  s.sleepScreenCycleIndex = doc["sleepScreenCycleIndex"] | s.sleepScreenCycleIndex;
  s.sleepCycleDoubleTapBack = doc["sleepCycleDoubleTapBack"] | s.sleepCycleDoubleTapBack;
  s.sleepCycleDailyMode = doc["sleepCycleDailyMode"] | s.sleepCycleDailyMode;
  s.sleepCycleLastChangeDay = doc["sleepCycleLastChangeDay"] | s.sleepCycleLastChangeDay;

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  s.fontFamily = clamp(doc["fontFamily"] | (uint8_t)0, CrossPointSettings::BUILTIN_FONT_COUNT, 0);
  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  // CrumBLE 4.5.4: UI font fallback family. Mirrors sdFontFamilyName.
  const char* uifb = doc["uiFontFallbackFamily"] | "";
  strncpy(s.uiFontFallbackFamily, uifb, sizeof(s.uiFontFallbackFamily) - 1);
  s.uiFontFallbackFamily[sizeof(s.uiFontFallbackFamily) - 1] = '\0';
  s.uiFontFallbackPointSize = static_cast<uint8_t>(doc["uiFontFallbackPointSize"] | 0);

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    s.language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["lastConnectedSsid"] = store.getLastConnectedSsid();

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : store.getCredentials()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WCS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.lastConnectedSsid = doc["lastConnectedSsid"] | std::string("");

  store.credentials.clear();
  JsonArray arr = doc["credentials"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.credentials.size() >= store.MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | std::string("");
    if (cred.ssid.empty()) {
      LOG_ERR("WCS", "Skipping WiFi credential with empty SSID");
      continue;
    }

    obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &status);
    // v18.9.9.347: DO NOT auto-mark LEGACY reads for resave. Field
    // observation: some OTA flashes drop through XOR-with-HW-MAC into
    // garbage bytes that still look non-empty; the old auto-resave then
    // locked those garbage bytes into the CPV1-validated slot on disk,
    // permanently breaking the credential. Symptom: users had to
    // forget+rejoin WiFi after a random subset of firmware upgrades.
    // Only mark for resave when we PROVE the password works -- that
    // happens later in setLastConnectedSsid (i.e. after a successful
    // connect). Until then, keep the legacy string in memory but leave
    // the on-disk copy alone.
    if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY ||
        cred.password.empty()) {
      cred.password = obj["password"] | std::string("");
      if (!cred.password.empty() && needsResave) *needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID && cred.password.empty()) {
      LOG_ERR("WCS", "Skipping WiFi credential with unreadable password: %s", cred.ssid.c_str());
      continue;
    }
    store.credentials.push_back(cred);
  }

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", store.credentials.size());
  return true;
}

// ---- RecentBooksStore ----

bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore& store, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadRecentBooks(RecentBooksStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RBS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.recentBooks.clear();
  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.getCount() >= RecentBooksStore::MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    // CrumBLE 4.4: clean previously-stored authors at load time too.
    // Pre-4.4 saves wrote whatever Epub::getAuthor returned, often with a
    // trailing ";" from OPF separator artifacts. Normalizing on load
    // means existing recent.json content displays cleanly without
    // needing a forced rewrite.
    book.author = normalizeAuthorMeta(obj["author"] | std::string(""));
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    store.recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", store.getCount());
  return true;
}

// ---- OpdsServerStore ----
// Follows the same save/load pattern as WifiCredentialStore above.
// Passwords are XOR-obfuscated with the device MAC and base64-encoded ("password_obf" key).

bool JsonSettingsIO::saveOpds(const OpdsServerStore& store, const char* path) {
  JsonDocument doc;

  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : store.getServers()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadOpds(OpdsServerStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("OPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.servers.clear();
  JsonArray arr = doc["servers"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | std::string("");
    server.url = obj["url"] | std::string("");
    server.username = obj["username"] | std::string("");
    // Try the obfuscated key first; fall back to plaintext "password" for
    // files written before obfuscation was added (or hand-edited JSON).
    obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &status);
    if (status == obfuscation::DecodeStatus::LEGACY && !server.password.empty() && needsResave) {
      *needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY ||
        server.password.empty()) {
      server.password = obj["password"] | std::string("");
      if (!server.password.empty() && needsResave) *needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID && server.password.empty()) {
      LOG_ERR("OPS", "Ignoring unreadable password for OPDS server: %s", server.name.c_str());
    }
    store.servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", store.servers.size());
  return true;
}
