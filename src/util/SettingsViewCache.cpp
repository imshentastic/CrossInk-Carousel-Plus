#include "SettingsViewCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"

namespace {
constexpr uint32_t kCacheMagic = 0x53564356;  // 'SVCV'
constexpr uint8_t kCacheVersion = 1;
constexpr const char* kCachePath = "/.crosspoint/crumble-settings-view.bin";

bool writeU8(HalFile& f, uint8_t v) { return f.write(&v, 1) == 1; }
bool writeU16(HalFile& f, uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
  return f.write(b, 2) == 2;
}
bool writeU32(HalFile& f, uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                  static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
  return f.write(b, 4) == 4;
}
bool writeString(HalFile& f, const std::string& s) {
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFF));
  if (!writeU16(f, len)) return false;
  if (len == 0) return true;
  return f.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len;
}

bool readU8(HalFile& f, uint8_t& v) { return f.read(&v, 1) == 1; }
bool readU16(HalFile& f, uint16_t& v) {
  uint8_t b[2];
  if (f.read(b, 2) != 2) return false;
  v = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
  return true;
}
bool readU32(HalFile& f, uint32_t& v) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return false;
  v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
      (static_cast<uint32_t>(b[3]) << 24);
  return true;
}
bool readString(HalFile& f, std::string& s) {
  uint16_t len = 0;
  if (!readU16(f, len)) return false;
  s.clear();
  if (len == 0) return true;
  s.resize(len);
  return f.read(reinterpret_cast<uint8_t*>(s.data()), len) == len;
}

// Evaluate a row's current uint8 value (TOGGLE/ENUM/VALUE). Returns 0xFF
// if not applicable.
uint8_t currentValueFor(const SettingInfo& s) {
  if (s.valuePtr != nullptr) return SETTINGS.*(s.valuePtr);
  if (s.valueGetter) return s.valueGetter();
  return 0xFF;
}

std::string currentStringFor(const SettingInfo& s) {
  if (s.stringOffset > 0 && s.stringMaxLen > 0) {
    const char* base = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
    return std::string(base);
  }
  if (s.stringGetter) return s.stringGetter();
  return "";
}

bool writeRow(HalFile& f, const SettingInfo& s) {
  if (!writeU8(f, static_cast<uint8_t>(s.type))) return false;
  if (!writeU16(f, static_cast<uint16_t>(s.nameId))) return false;
  if (!writeU16(f, static_cast<uint16_t>(s.category))) return false;
  // Current value snapshot.
  uint8_t current = 0xFF;
  if (s.type == SettingType::TOGGLE || s.type == SettingType::ENUM || s.type == SettingType::VALUE) {
    current = currentValueFor(s);
  }
  if (!writeU8(f, current)) return false;
  // Enum labels: enumValues (StrIds) parallel with enumStringValues
  // (runtime strings). Both may be empty. If enumStringValues is
  // non-empty and enumValues is empty, we still need to write the
  // string count; use STR_NONE_OPT sentinel in the StrId slot.
  const size_t enumCount = std::max(s.enumValues.size(), s.enumStringValues.size());
  const uint8_t safeCount = static_cast<uint8_t>(std::min<size_t>(enumCount, 0xFF));
  if (!writeU8(f, safeCount)) return false;
  for (size_t i = 0; i < safeCount; ++i) {
    const uint8_t raw = i < s.enumRawValues.size() ? s.enumRawValues[i] : static_cast<uint8_t>(i);
    const StrId sid = i < s.enumValues.size() ? s.enumValues[i] : StrId::STR_NONE_OPT;
    const std::string sval = i < s.enumStringValues.size() ? s.enumStringValues[i] : std::string();
    if (!writeU8(f, raw)) return false;
    if (!writeU16(f, static_cast<uint16_t>(sid))) return false;
    if (!writeString(f, sval)) return false;
  }
  // String snapshot for STRING type. Others write empty.
  const std::string strValue = (s.type == SettingType::STRING) ? currentStringFor(s) : std::string();
  if (!writeString(f, strValue)) return false;
  return true;
}

bool readRow(HalFile& f, SettingsViewRow& out) {
  uint8_t t = 0;
  if (!readU8(f, t)) return false;
  out.type = static_cast<SettingType>(t);
  uint16_t nameU = 0, catU = 0;
  if (!readU16(f, nameU)) return false;
  if (!readU16(f, catU)) return false;
  out.nameId = static_cast<StrId>(nameU);
  out.categoryId = static_cast<StrId>(catU);
  if (!readU8(f, out.currentValue)) return false;
  uint8_t enumCount = 0;
  if (!readU8(f, enumCount)) return false;
  out.enumRawValues.reserve(enumCount);
  out.enumStrIds.reserve(enumCount);
  out.enumStringLabels.reserve(enumCount);
  for (uint8_t i = 0; i < enumCount; ++i) {
    uint8_t raw = 0;
    uint16_t sid = 0;
    std::string sval;
    if (!readU8(f, raw)) return false;
    if (!readU16(f, sid)) return false;
    if (!readString(f, sval)) return false;
    out.enumRawValues.push_back(raw);
    out.enumStrIds.push_back(static_cast<StrId>(sid));
    out.enumStringLabels.push_back(std::move(sval));
  }
  return readString(f, out.stringValue);
}

// Recursively count top-level + SUBMENU children so we know the total
// row count for the header. Flatten by walking depth-first; each
// SUBMENU emits its parent row and its children back-to-back.
size_t countFlat(const std::vector<SettingInfo>& list) {
  size_t n = 0;
  for (const auto& s : list) {
    ++n;
    if (s.type == SettingType::SUBMENU) n += countFlat(s.children);
  }
  return n;
}

bool writeFlat(HalFile& f, const std::vector<SettingInfo>& list) {
  for (const auto& s : list) {
    if (!writeRow(f, s)) return false;
    if (s.type == SettingType::SUBMENU) {
      if (!writeFlat(f, s.children)) return false;
    }
  }
  return true;
}
}  // namespace

bool saveSettingsViewCache(const std::vector<SettingInfo>& list) {
  Storage.mkdir("/.crosspoint");
  const std::string tmpPath = std::string(kCachePath) + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("SVC", tmpPath.c_str(), f)) {
      LOG_ERR("SVC", "openFileForWrite failed: %s", tmpPath.c_str());
      return false;
    }
    if (!writeU32(f, kCacheMagic) || !writeU8(f, kCacheVersion)) {
      f.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    const size_t total = countFlat(list);
    const uint16_t safeTotal = static_cast<uint16_t>(std::min<size_t>(total, 0xFFFF));
    if (!writeU16(f, safeTotal)) {
      f.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (!writeFlat(f, list)) {
      f.close();
      Storage.remove(tmpPath.c_str());
      LOG_ERR("SVC", "writeFlat failed");
      return false;
    }
    f.close();
  }
  Storage.remove(kCachePath);
  if (!Storage.rename(tmpPath.c_str(), kCachePath)) {
    LOG_ERR("SVC", "rename %s -> %s failed", tmpPath.c_str(), kCachePath);
    Storage.remove(tmpPath.c_str());
    return false;
  }
  LOG_INF("SVC", "Wrote settings-view cache: %s", kCachePath);
  return true;
}

bool loadSettingsViewCache(std::vector<SettingsViewRow>& out) {
  out.clear();
  if (!Storage.exists(kCachePath)) return false;
  HalFile f;
  if (!Storage.openFileForRead("SVC", kCachePath, f)) return false;
  uint32_t magic = 0;
  uint8_t version = 0;
  uint16_t rowCount = 0;
  if (!readU32(f, magic) || magic != kCacheMagic) {
    f.close();
    LOG_INF("SVC", "cache magic mismatch; ignoring");
    return false;
  }
  if (!readU8(f, version) || version != kCacheVersion) {
    f.close();
    LOG_INF("SVC", "cache version=%u expected=%u; ignoring", version, kCacheVersion);
    return false;
  }
  if (!readU16(f, rowCount)) {
    f.close();
    return false;
  }
  out.reserve(rowCount);
  for (uint16_t i = 0; i < rowCount; ++i) {
    SettingsViewRow row;
    if (!readRow(f, row)) {
      f.close();
      LOG_ERR("SVC", "cache row %u parse failed; discarding", i);
      out.clear();
      return false;
    }
    out.push_back(std::move(row));
  }
  f.close();
  LOG_INF("SVC", "Loaded settings-view cache: %u rows", static_cast<unsigned>(out.size()));
  return true;
}

bool settingsViewCacheExists() { return Storage.exists(kCachePath); }
