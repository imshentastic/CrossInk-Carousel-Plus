#include "LibraryIndex.h"

#include <Arduino.h>  // millis()
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include "CollectionsStore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>  // strcasecmp
#include <unordered_map>

namespace {
constexpr char LIBRARY_INDEX_FILE[] = "/.crosspoint/library_index.json";
// On-disk format marker (first line). The file is a streamed, line-based
// format — NOT JSON, despite the historical .json filename — written and
// read one entry at a time so we never hold a second full in-RAM copy of
// every path (a JsonDocument + serialized String). That doubled peak memory
// and could OOM mid-save on large libraries, which left the index file
// unwritten and boot-looped the "indexing your library" screen. Each entry
// line is "<firstSeenMillis>\t<path>"; tab/newline can't appear in FAT/exFAT
// filenames so no escaping is needed. An older single-blob JSON index starts
// with '{' (≠ the marker's 'C'), so loadFromFile rejects it and a rescan
// rewrites it in this format.
constexpr char LIBRARY_INDEX_HEADER[] = "CRUMBLE-LIBIDX v1";
constexpr int MAX_WALK_DEPTH = 8;

// Buffered line reader over a HalFile/FsFile. Reads in chunks (one HAL call
// per ~256 bytes instead of per byte) and splits on '\n'. Over-long lines
// (e.g. an old single-line JSON index) are truncated at kMaxLine so a corrupt
// or legacy file can't balloon a single std::string — we only need enough to
// reject it via the header check.
class LineReader {
  FsFile& file;
  char buf[256] = {};
  int len = 0;
  int pos = 0;
  static constexpr size_t kMaxLine = 1024;

 public:
  explicit LineReader(FsFile& f) : file(f) {}
  // Reads the next line into `out` (without the trailing newline). Returns
  // false only at end-of-file with nothing read.
  bool next(std::string& out) {
    out.clear();
    bool any = false;
    for (;;) {
      if (pos >= len) {
        len = file.read(buf, sizeof(buf));
        pos = 0;
        if (len <= 0) break;  // EOF or error
      }
      const char c = buf[pos++];
      any = true;
      if (c == '\n') return true;
      if (c == '\r') continue;  // tolerate CRLF
      if (out.size() < kMaxLine) out.push_back(c);
    }
    return any;
  }
};

bool iLess(const std::string& a, const std::string& b) {
  // Case-insensitive less-than for the default "All Books" sort order.
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}
}  // namespace

LibraryIndex LibraryIndex::instance;

uint32_t LibraryIndex::appendPath(std::string_view path) {
  const uint32_t offset = static_cast<uint32_t>(pathPool.size());
  pathPool.insert(pathPool.end(), path.begin(), path.end());
  pathPool.push_back('\0');
  return offset;
}

// CrumBLE v3.7.2: subtree-scoped book walk for the "make collection from
// folder" file-browser action. walkRecursive is const + uses no mutable
// instance state, so it's safe to invoke on the singleton even when the
// library isn't loaded.
std::vector<std::string> LibraryIndex::collectBookPaths(const std::string& dirPath) {
  std::vector<std::string> paths;
  std::vector<uint32_t> sizes;  // discarded; walkRecursive populates both in lockstep
  paths.reserve(16);
  sizes.reserve(16);
  getInstance().walkRecursive(dirPath, 0, paths, sizes);
  return paths;
}

bool LibraryIndex::isBookPath(const std::string& path) {
  if (!(FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) ||
        FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path))) {
    return false;
  }
  // Blacklist: system files that happen to share a book extension. Match
  // by basename (case-insensitive) so a user file with a similar name in
  // a subdirectory still gets indexed.
  const size_t lastSlash = path.find_last_of('/');
  const std::string basename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
  std::string lower = basename;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // Firmware diagnostics — crash dump written by the panic handler.
  if (lower == "crash_report.txt") return false;
  // Reserved for future system files. Extend rather than scatter checks
  // throughout the walker.
  return true;
}

void LibraryIndex::begin() {
  if (jsonLoaded) return;
  // freshFirstBoot drives the welcome "indexing your library" screen + the
  // initial SD walk in main.cpp. We fire it whenever there's no *usable*
  // index: a genuine first run, a wiped cache, OR an upgrade from the old
  // single-blob JSON format (loadFromFile rejects that and returns false).
  // Keying off load success rather than mere file existence means the walk
  // self-heals a corrupt/legacy index instead of leaving collections empty.
  const bool loaded = loadFromFile();
  freshFirstBoot = !loaded;
  jsonLoaded = true;
}

void LibraryIndex::ensureWalked(const std::function<void(int)>& progress) {
  if (walkPerformed) return;
  // Restore the persisted index (with each book's firstSeen) before walking.
  // Normally a no-op (begin() ran at boot), but after releaseMemory() freed the
  // in-RAM vector this reloads it from JSON so the rescan below sees a populated
  // index and does an *incremental* diff -- otherwise an empty entries list
  // would make rescan() treat every book as newly-discovered and reset all the
  // "Recently Added" timestamps.
  begin();
  rescan(progress);
}

void LibraryIndex::rescan(const std::function<void(int)>& progress) {
  // Walk the whole SD, collecting every book file path and its size.
  std::vector<std::string> discovered;
  std::vector<uint32_t> discoveredSizes;
  if (progress) progress(5);
  walkRecursive("/", 0, discovered, discoveredSizes);
  const size_t walkedCount = discovered.size();
  if (progress) progress(60);

  // Order by a persistent monotonic "first seen by the device" counter. File
  // timestamps are unusable here (WiFi transfers stamp a default date) and
  // millis() resets each boot, so we instead hand each newly-seen book the next
  // value above everything already indexed -- guaranteeing it tops Recently
  // Added regardless of reboots.
  uint64_t nextSeq = 1;
  for (const auto& e : entries) nextSeq = std::max<uint64_t>(nextSeq, e.firstSeenMillis + 1);

  // Build a lookup of EXISTING paths (string_view over the current pool) so the
  // diff doesn't allocate N transient std::string keys -- that was a hidden
  // fragmentation contributor in the pre-pool implementation.
  std::unordered_map<std::string_view, size_t> oldIdxByPath;
  oldIdxByPath.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) oldIdxByPath[pathViewOf(entries[i])] = i;

  // We rebuild entries + pathPool atomically rather than mutating in place.
  // In-place mutation would invalidate string_view keys the moment we appended
  // a new path into the pool (vector<char> reallocation); the rebuild is also
  // simpler and gives the allocator a single fresh contiguous block to land in
  // instead of a stream of incremental grows.
  std::vector<LibraryEntry> newEntries;
  std::vector<char> newPool;
  newEntries.reserve(discovered.size());
  // Reserve a rough first-cut for the pool so it doesn't realloc on every
  // append. Average path ~80 chars; round up generously.
  newPool.reserve(discovered.size() * 96);

  auto appendIntoNewPool = [&newPool](std::string_view path) -> uint32_t {
    const uint32_t offset = static_cast<uint32_t>(newPool.size());
    newPool.insert(newPool.end(), path.begin(), path.end());
    newPool.push_back('\0');
    return offset;
  };

  for (size_t i = 0; i < discovered.size(); ++i) {
    const std::string& p = discovered[i];
    auto it = oldIdxByPath.find(std::string_view{p});
    if (it != oldIdxByPath.end()) {
      // Carry forward the existing metadata, but re-allocate the path into
      // the new pool. firstSeenMillis is preserved (the user's "when did this
      // book first appear" ordering survives the rebuild).
      const LibraryEntry& old = entries[it->second];
      LibraryEntry e{};
      e.pathOffset = appendIntoNewPool(p);
      e.firstSeenMillis = old.firstSeenMillis;
      e.fileSize = discoveredSizes[i];
      // A known path whose size changed = the file was replaced -> re-date it.
      // Skip when the stored size is unknown (0, legacy entry) so the first
      // scan after upgrade doesn't reshuffle the whole library.
      if (old.fileSize != 0 && old.fileSize != discoveredSizes[i]) {
        e.firstSeenMillis = nextSeq++;
      }
      newEntries.push_back(e);
    } else {
      // Brand-new book -> newest.
      LibraryEntry e{};
      e.pathOffset = appendIntoNewPool(p);
      e.firstSeenMillis = nextSeq++;
      e.fileSize = discoveredSizes[i];
      newEntries.push_back(e);
    }
  }
  if (progress) progress(85);

  // Free the walk's scratch vectors AND the old pool/entries before saving so
  // the (streaming) writer runs with maximum heap headroom. The old idx-by-path
  // map also goes here.
  std::unordered_map<std::string_view, size_t>().swap(oldIdxByPath);
  discovered.clear();
  discovered.shrink_to_fit();
  discoveredSizes.clear();
  discoveredSizes.shrink_to_fit();
  // Atomic swap into the new in-place. The old entries/pool's memory drops
  // back to the allocator as a single contiguous chunk (modulo whatever else
  // the allocator stitched into it).
  entries.swap(newEntries);
  pathPool.swap(newPool);
  newEntries.clear();
  newEntries.shrink_to_fit();
  newPool.clear();
  newPool.shrink_to_fit();

  walkPerformed = true;
  saveToFile();
  LOG_INF("LIB", "Library index now has %zu entries (walked %zu files)", entries.size(), walkedCount);
  // CrumBLE: a re-walk can add/remove books, so the BookReadingStats-derived
  // virtuals (Finished, New) might be stale relative to the new path set.
  // Invalidate so the next access rescans.
  CollectionsStore::getInstance().invalidateScannedVirtuals();
  if (progress) progress(100);
}

std::vector<std::string> LibraryIndex::getAllBookPaths() const {
  std::vector<std::string> out;
  out.reserve(entries.size());
  for (const auto& e : entries) out.emplace_back(pathOf(e));
  std::sort(out.begin(), out.end(), iLess);
  return out;
}

std::vector<std::string> LibraryIndex::getRecentlyAddedPaths(int maxCount) const {
  // Sort indices (cheap: 4 bytes each) by the underlying firstSeenMillis,
  // then materialize the top-N paths out of the pool. Avoids copying the
  // whole entry vector just to sort it.
  std::vector<size_t> idx(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [this](size_t a, size_t b) {
    return entries[a].firstSeenMillis > entries[b].firstSeenMillis;
  });
  std::vector<std::string> out;
  const int n = std::min(maxCount, static_cast<int>(idx.size()));
  out.reserve(n);
  for (int i = 0; i < n; ++i) out.emplace_back(pathOf(entries[idx[i]]));
  return out;
}

uint64_t LibraryIndex::getFirstSeen(const std::string& path) const {
  for (const auto& e : entries) {
    if (path == pathOf(e)) return e.firstSeenMillis;
  }
  return 0;
}

void LibraryIndex::forgetPath(const std::string& path) {
  auto it = std::find_if(entries.begin(), entries.end(),
                         [&](const LibraryEntry& e) { return path == pathOf(e); });
  if (it == entries.end()) return;
  // Just drop the entry from the vector. The pool keeps the dead path
  // bytes around until the next rescan() rebuilds the pool from scratch
  // -- the slop is negligible vs. the cost of compacting the pool on
  // every delete (would require re-targeting every other entry's
  // pathOffset). Rescan happens on every web-server-exit / explicit
  // refresh, so the slop never accumulates.
  entries.erase(it);
  saveToFile();
}

void LibraryIndex::walkRecursive(const std::string& dirPath, int depth, std::vector<std::string>& outPaths,
                                 std::vector<uint32_t>& outSizes) const {
  if (depth > MAX_WALK_DEPTH) {
    LOG_DBG("LIB", "walk depth cap reached at %s", dirPath.c_str());
    return;
  }
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // 256 bytes: long-form filenames (e.g. Anna's-Archive "Title -- Author -- Year
  // -- Publisher -- <hash> -- Anna's Archive.epub") overflowed the old 128-byte
  // buffer, truncating the ".epub" so isBookPath() dropped the file entirely.
  char nameBuf[256];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(nameBuf, sizeof(nameBuf));
    const std::string name(nameBuf);
    if (name.empty() || name[0] == '.') {
      // Skip dot-prefixed names — hidden / system directories like
      // ".crosspoint", ".Trashes", ".Spotlight-V100". The user's library
      // shouldn't recurse into our own cache.
      entry.close();
      continue;
    }
    std::string childPath = dirPath;
    if (!childPath.empty() && childPath.back() != '/') childPath += '/';
    childPath += name;

    if (entry.isDirectory()) {
      // CrumBLE: also skip well-known non-library folders by name
      // (case-insensitive). These hold derived/cache data that the user
      // sometimes parks at the SD root next to actual books -- we don't
      // want them surfaced in Recently Added / All Books / Unopened.
      //
      //   XTcache  -- companion XT reader's cache directory
      //
      // Add new entries here when a new "looks-like-a-book-but-isn't"
      // folder shows up in user libraries.
      static const char* const kBlacklistedDirs[] = {"XTcache"};
      bool skip = false;
      for (const char* bad : kBlacklistedDirs) {
        if (strcasecmp(name.c_str(), bad) == 0) {
          skip = true;
          break;
        }
      }
      if (skip) {
        LOG_DBG("LIB", "Skipping blacklisted directory: %s", childPath.c_str());
        entry.close();
        continue;
      }
      entry.close();  // close before recursing so we don't pile open file handles.
      walkRecursive(childPath, depth + 1, outPaths, outSizes);
    } else {
      if (isBookPath(childPath)) {
        // Capture the file size while the entry is open (a later stat would
        // re-open it). Used to spot a replaced same-name file on the next walk.
        outPaths.push_back(childPath);
        outSizes.push_back(static_cast<uint32_t>(entry.fileSize()));
      }
      entry.close();
    }
  }
  dir.close();
}

bool LibraryIndex::loadFromFile() {
  if (!Storage.exists(LIBRARY_INDEX_FILE)) return false;

  FsFile file;
  if (!Storage.openFileForRead("LIB", LIBRARY_INDEX_FILE, file)) return false;

  // Stream the file line by line so we never hold the whole thing (plus a
  // JsonDocument) in RAM. The first line must be our format marker; an older
  // single-blob JSON index won't match (it starts with '{'), so we reject it
  // and let a rescan rewrite it — without ever slurping the huge old file.
  entries.clear();
  pathPool.clear();
  LineReader reader(file);
  std::string line;
  bool headerOk = false;
  size_t lineNo = 0;
  while (reader.next(line)) {
    if (lineNo++ == 0) {
      headerOk = (line == LIBRARY_INDEX_HEADER);
      if (!headerOk) break;
      continue;
    }
    if (line.empty()) continue;
    const size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos) continue;
    // strtoull stops at the tab, so parsing the whole c_str is safe.
    const uint64_t firstSeen = strtoull(line.c_str(), nullptr, 10);
    const size_t tab2 = line.find('\t', tab1 + 1);
    uint32_t fileSize = 0;
    std::string_view path;
    if (tab2 == std::string::npos) {
      // Legacy 2-field line: "<firstSeen>\t<path>" (pre-size index).
      path = std::string_view{line}.substr(tab1 + 1);
    } else {
      fileSize = static_cast<uint32_t>(strtoul(line.c_str() + tab1 + 1, nullptr, 10));
      path = std::string_view{line}.substr(tab2 + 1);
    }
    if (path.empty()) continue;
    LibraryEntry e{};
    e.pathOffset = appendPath(path);
    e.firstSeenMillis = firstSeen;
    e.fileSize = fileSize;
    entries.push_back(e);
  }
  file.close();

  if (!headerOk) {
    // Legacy/corrupt file — treat as no index, force a rescan.
    entries.clear();
    pathPool.clear();
    return false;
  }
  LOG_DBG("LIB", "Loaded library index with %zu entries", entries.size());
  return true;
}

bool LibraryIndex::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite("LIB", LIBRARY_INDEX_FILE, file)) {
    LOG_ERR("LIB", "Could not open library index for writing");
    return false;
  }
  // Stream entries one line at a time ("<firstSeen>\t<size>\t<path>") so we never
  // build a JsonDocument + serialized String copy of the whole index in RAM
  // (the OOM-on-save that boot-looped large libraries). Legacy 2-field lines
  // (no size) still load fine.
  file.print(LIBRARY_INDEX_HEADER);
  file.print("\n");
  char numbuf[48];
  for (const auto& e : entries) {
    snprintf(numbuf, sizeof(numbuf), "%llu\t%lu", static_cast<unsigned long long>(e.firstSeenMillis),
             static_cast<unsigned long>(e.fileSize));
    file.print(numbuf);
    file.print("\t");
    file.print(pathOf(e));
    file.print("\n");
  }
  file.close();
  return true;
}
