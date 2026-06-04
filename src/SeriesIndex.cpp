#include "SeriesIndex.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
constexpr char SERIES_INDEX_FILE[] = "/.crosspoint/series_index.json";
constexpr uint8_t SERIES_INDEX_VERSION = 1;
}  // namespace

SeriesIndex SeriesIndex::instance;

uint32_t SeriesIndex::appendString(std::string_view s) {
  const uint32_t offset = static_cast<uint32_t>(stringPool.size());
  stringPool.insert(stringPool.end(), s.begin(), s.end());
  stringPool.push_back('\0');
  return offset;
}

int SeriesIndex::indexOfPath(std::string_view path) const {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (path == pathOf(entries[i])) return static_cast<int>(i);
  }
  return -1;
}

void SeriesIndex::rebuildFrom(std::vector<ScratchEntry>& scratch) {
  // Compute the rough pool size upfront so the new pool lands in a
  // single contiguous allocation -- the whole point of this refactor is
  // to give the heap allocator big atomic blocks to work with instead
  // of N small ones.
  size_t poolBytes = 0;
  for (const auto& s : scratch) poolBytes += s.path.size() + s.name.size() + s.index.size() + 3;

  std::vector<SeriesEntry> newEntries;
  std::vector<char> newPool;
  newEntries.reserve(scratch.size());
  newPool.reserve(poolBytes);

  for (auto& s : scratch) {
    SeriesEntry e{};
    e.pathOffset = static_cast<uint32_t>(newPool.size());
    newPool.insert(newPool.end(), s.path.begin(), s.path.end());
    newPool.push_back('\0');
    e.nameOffset = static_cast<uint32_t>(newPool.size());
    newPool.insert(newPool.end(), s.name.begin(), s.name.end());
    newPool.push_back('\0');
    e.indexOffset = static_cast<uint32_t>(newPool.size());
    newPool.insert(newPool.end(), s.index.begin(), s.index.end());
    newPool.push_back('\0');
    newEntries.push_back(e);
  }

  entries.swap(newEntries);
  stringPool.swap(newPool);
}

void SeriesIndex::begin() {
  if (jsonLoaded) return;
  loadFromFile();
  jsonLoaded = true;
}

void SeriesIndex::record(const std::string& path, const std::string& name, const std::string& index) {
  if (path.empty()) return;
  const int existing = indexOfPath(path);
  if (existing >= 0) {
    const SeriesEntry& e = entries[existing];
    if (name == nameOf(e) && index == indexOf(e)) {
      // no-op: already recorded with same values, skip disk write.
      return;
    }
  }

  // Build a scratch list off the current pool, mutate the target entry,
  // then rebuild atomically. The rebuild guarantees the new pool is one
  // fresh contiguous block (matches LibraryIndex's pattern + reasoning).
  std::vector<ScratchEntry> scratch;
  scratch.reserve(entries.size() + 1);
  for (const auto& e : entries) {
    scratch.push_back({pathOf(e), nameOf(e), indexOf(e)});
  }
  if (existing >= 0) {
    scratch[existing].name = name;
    scratch[existing].index = index;
  } else {
    scratch.push_back({path, name, index});
  }
  rebuildFrom(scratch);
  saveToFile();
}

bool SeriesIndex::hasBeenChecked(const std::string& path) const { return indexOfPath(path) >= 0; }

const SeriesEntry* SeriesIndex::find(const std::string& path) const {
  const int i = indexOfPath(path);
  if (i < 0) return nullptr;
  return &entries[i];
}

void SeriesIndex::releaseMemory() {
  // Drop entries + stringPool capacity. shrink_to_fit on a cleared
  // vector returns capacity to zero on libstdc++. No on-disk change;
  // begin() repopulates from JSON whenever the device restarts.
  entries.clear();
  entries.shrink_to_fit();
  stringPool.clear();
  stringPool.shrink_to_fit();
}

void SeriesIndex::forgetPath(const std::string& path) {
  const int existing = indexOfPath(path);
  if (existing < 0) return;
  std::vector<ScratchEntry> scratch;
  scratch.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (static_cast<int>(i) == existing) continue;
    scratch.push_back({pathOf(entries[i]), nameOf(entries[i]), indexOf(entries[i])});
  }
  rebuildFrom(scratch);
  saveToFile();
}

std::string SeriesIndex::seriesKey(const std::string& rawName) {
  // lowercase + collapse internal whitespace + trim. Matches aalu's
  // normalisation so a series called "The Foundation" survives
  // capitalization or stray-double-space drift across books.
  std::string out;
  out.reserve(rawName.size());
  bool prevSpace = true;  // start-of-string trims leading space
  for (char raw : rawName) {
    const auto ch = static_cast<unsigned char>(raw);
    if (std::isspace(ch)) {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(ch)));
      prevSpace = false;
    }
  }
  // Trim a single trailing space if present (the loop adds one when a
  // run of whitespace is followed by end-of-string).
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool SeriesIndex::indexLess(const std::string& a, const std::string& b) {
  // Parse leading numeric prefix. Allows "1", "1.5", "10", and falls
  // back to lexicographic for unparseable strings like "VII".
  auto parsePrefix = [](const std::string& s, double& outVal) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;  // didn't consume any chars
    outVal = v;
    return true;
  };
  double va = 0.0;
  double vb = 0.0;
  const bool pa = parsePrefix(a, va);
  const bool pb = parsePrefix(b, vb);
  if (pa && pb) {
    if (va != vb) return va < vb;
    return a < b;  // tie-break by raw string (handles "1" vs "1.0")
  }
  if (pa != pb) return pa;  // numeric-prefix entries sort before unparseable
  return a < b;
}

bool SeriesIndex::loadFromFile() {
  if (!Storage.exists(SERIES_INDEX_FILE)) return false;
  String json = Storage.readFile(SERIES_INDEX_FILE);
  if (json.isEmpty()) return false;

  JsonDocument doc;
  auto err = deserializeJson(doc, json.c_str());
  if (err) {
    LOG_ERR("SER", "series_index.json parse error: %s", err.c_str());
    return false;
  }

  // Stage entries into a scratch list, then rebuild atomically so the
  // final entries + stringPool land in single contiguous allocations
  // (the ArduinoJson DOM already pinned ~the JSON bytes; once we're done
  // staging the scratch list goes out of scope and frees those, leaving
  // only our compact pool behind).
  std::vector<ScratchEntry> scratch;
  JsonArrayConst arr = doc["books"];
  if (!arr.isNull()) {
    scratch.reserve(arr.size());
    for (JsonObjectConst entry : arr) {
      ScratchEntry s;
      s.path = entry["path"] | std::string("");
      if (s.path.empty()) continue;
      s.name = entry["name"] | std::string("");
      s.index = entry["index"] | std::string("");
      scratch.push_back(std::move(s));
    }
  }
  rebuildFrom(scratch);
  LOG_DBG("SER", "Loaded series index with %zu entries", entries.size());
  return true;
}

bool SeriesIndex::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["version"] = SERIES_INDEX_VERSION;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& e : entries) {
    JsonObject entry = arr.add<JsonObject>();
    entry["path"] = pathOf(e);
    entry["name"] = nameOf(e);
    entry["index"] = indexOf(e);
  }
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(SERIES_INDEX_FILE, json);
}
