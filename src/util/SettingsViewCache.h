#pragma once
// v18.9.9.50 (task #35): SD-cached snapshot of the reader settings list.
// Purpose: let ReaderOptionsActivity and BookSettingsDrawerActivity
// render current values under BT (where NimBLE holds ~58 KB and the
// live getSettingsList() build peaks past the maxAlloc floor) without
// having to silent-restart just to look. Editing still needs the
// silent-restart-with-OpenReaderOptions path from v18.9.9.49.
//
// The cache is a compact binary blob of every top-level row's:
//   * type
//   * name / category (StrIds)
//   * current value (evaluated at cache-write time)
//   * enum labels (StrIds AND runtime strings, so SD-font enums stay
//     legible in view mode)
//
// Rebuilt whenever CrossPointSettings::saveToFile() succeeds. If the
// file is missing (fresh device, upgrade from pre-v50 firmware), the
// consumer falls back to whatever it did before (typically the
// silent-restart path). Consumers must never treat this cache as a
// source of truth for editing -- it's a display shortcut only.

#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/settings/SettingsActivity.h"

// A row in the deserialized view. No std::function, no valuePtr -- just
// enough to render a name + a current value string.
struct SettingsViewRow {
  StrId nameId = StrId::STR_NONE_OPT;
  StrId categoryId = StrId::STR_NONE_OPT;
  SettingType type = SettingType::ACTION;
  uint8_t currentValue = 0xFF;  // 0xFF sentinel = "no value" (SECTION_HEADER, ACTION, SUBMENU marker)
  // Enum labels in row order. StrId::STR_NONE_OPT means "use string label at same index".
  std::vector<StrId> enumStrIds;
  std::vector<uint8_t> enumRawValues;
  std::vector<std::string> enumStringLabels;  // parallel to enumStrIds; only populated when the source row used enumStringValues (SD-font names).
  // Optional string value snapshot (STRING type) -- only populated when
  // available at cache-write time. Empty for other types.
  std::string stringValue;
};

// Write the settings-view cache. Iterates the provided settings list
// (typically the same one built by getSettingsList()), evaluates each
// row's current value via valuePtr / valueGetter / stringOffset /
// stringGetter, and serializes to /.crosspoint/crumble-settings-view.bin.
// SUBMENU children are recursed and flattened with a depth marker on
// each row so the reader can render nesting.
//
// Returns true iff the file was written successfully. Failures are
// non-fatal for callers (the cache is optional).
bool saveSettingsViewCache(const std::vector<SettingInfo>& list);

// Read the settings-view cache into out. Returns true iff the file
// existed, parsed, and matched the current magic + version. On any
// mismatch the caller should treat this as "no cache available" and
// fall through to whatever its normal path is.
bool loadSettingsViewCache(std::vector<SettingsViewRow>& out);

// True iff the cache file exists. Cheap Storage.exists() check, no
// parse. Consumers use this before triggering a heap-heavy live
// getSettingsList() build to decide whether to fall through to view
// mode instead.
bool settingsViewCacheExists();
