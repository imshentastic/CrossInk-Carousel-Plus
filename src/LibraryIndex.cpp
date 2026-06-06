#include "LibraryIndex.h"

#include <Arduino.h>  // millis(), ESP.getMaxAllocHeap()
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include "CollectionsStore.h"

#include <algorithm>
#include <cctype>
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
// CrumBLE 4.2.1: bumped to v2 to add a per-entry cached "last name" author
// key for fast AuthorAlpha sort without loading each EPUB. v1 indices still
// load -- their entries come back with empty authorKey, which
// populateAuthorKeysIfNeeded fills lazily on next ensureWalked() and saves
// out as v2. Format per entry line:
//   v1:  "<firstSeen>\t<size>\t<path>"
//   v2:  "<firstSeen>\t<size>\t<authorKey>\t<path>"
// authorKey comes BEFORE path so the existing "everything after the last
// tab" path parser keeps working untouched on v1 lines while the v2 parser
// pulls authorKey from between tab2 and tab3.
constexpr char LIBRARY_INDEX_HEADER_V1[] = "CRUMBLE-LIBIDX v1";
constexpr char LIBRARY_INDEX_HEADER[] = "CRUMBLE-LIBIDX v2";
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

// CrumBLE 4.2.1: extract a lowercase "last name" sort key from a raw author
// string. Mirrors CollectionsStore's lastNameLower exactly so the keys we
// cache here are byte-identical to what the v4.2.0 sort comparator would
// have produced. Returns empty for empty input -- caller uses that as the
// "sort to end" sentinel.
std::string lastNameLowerForKey(const std::string& author) {
  size_t l = author.find_first_not_of(" \t\r\n");
  if (l == std::string::npos) return {};
  size_t r = author.find_last_not_of(" \t\r\n");
  std::string s = author.substr(l, r - l + 1);
  const size_t comma = s.find(',');
  if (comma != std::string::npos) {
    s = s.substr(0, comma);
    size_t rr = s.find_last_not_of(" \t\r\n");
    if (rr != std::string::npos) s = s.substr(0, rr + 1);
  } else {
    const size_t sp = s.find_last_of(" \t");
    if (sp != std::string::npos) s = s.substr(sp + 1);
  }
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Also strip any embedded tabs -- they'd corrupt our on-disk format. (Author
  // strings should never contain tabs in practice; this is defense-in-depth.)
  s.erase(std::remove(s.begin(), s.end(), '\t'), s.end());
  return s;
}

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

// CrumBLE 4.2.1: ensure offset 0 of authorKeyPool is a NUL byte so any
// LibraryEntry whose authorKeyOffset = 0 reads back as "" via authorKeyOf().
// Called inline by appendAuthorKey() the first time the pool is empty.
static void ensureAuthorKeyPoolSentinel(std::vector<char>& pool) {
  if (pool.empty()) pool.push_back('\0');
}

uint32_t LibraryIndex::appendAuthorKey(std::string_view key) {
  ensureAuthorKeyPoolSentinel(authorKeyPool);
  if (key.empty()) return 0;  // map back to the seeded sentinel
  const uint32_t offset = static_cast<uint32_t>(authorKeyPool.size());
  authorKeyPool.insert(authorKeyPool.end(), key.begin(), key.end());
  authorKeyPool.push_back('\0');
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
  // CrumBLE 4.2.1: authorKeyPool is rebuilt in parallel with pathPool so the
  // per-entry authorKeyOffset values stay valid after the swap. Seed the new
  // pool with a NUL byte at offset 0 so any entry that didn't carry forward
  // a key (or didn't have one in the old pool) reads back as "" via
  // authorKeyOf().
  std::vector<char> newAuthorKeyPool;
  newAuthorKeyPool.push_back('\0');
  newEntries.reserve(discovered.size());
  // Reserve a rough first-cut for the pool so it doesn't realloc on every
  // append. Average path ~80 chars; round up generously.
  newPool.reserve(discovered.size() * 96);
  // Average lowercase last name ~10 chars; round up generously.
  newAuthorKeyPool.reserve(1 + discovered.size() * 16);

  auto appendIntoNewPool = [&newPool](std::string_view path) -> uint32_t {
    const uint32_t offset = static_cast<uint32_t>(newPool.size());
    newPool.insert(newPool.end(), path.begin(), path.end());
    newPool.push_back('\0');
    return offset;
  };
  auto appendIntoNewAuthorKeyPool = [&newAuthorKeyPool](std::string_view key) -> uint32_t {
    if (key.empty()) return 0;  // sentinel
    const uint32_t offset = static_cast<uint32_t>(newAuthorKeyPool.size());
    newAuthorKeyPool.insert(newAuthorKeyPool.end(), key.begin(), key.end());
    newAuthorKeyPool.push_back('\0');
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
      // CrumBLE 4.2.1: carry forward the cached author key into the new pool
      // so a rescan triggered by a wifi-upload / hotspot exit doesn't lose
      // the user's accumulated cache. authorKeyOf() reads from the OLD pool
      // here because we haven't swapped yet; copy the bytes into the new
      // pool and record the new offset on the new entry.
      e.authorKeyOffset = appendIntoNewAuthorKeyPool(std::string_view{authorKeyOf(old)});
      // A known path whose size changed = the file was replaced -> re-date it.
      // Skip when the stored size is unknown (0, legacy entry) so the first
      // scan after upgrade doesn't reshuffle the whole library.
      if (old.fileSize != 0 && old.fileSize != discoveredSizes[i]) {
        e.firstSeenMillis = nextSeq++;
        // A replaced file may have a different author; invalidate the cache
        // entry so populateAuthorKeysIfNeeded refreshes it on next pass.
        e.authorKeyOffset = 0;
      }
      newEntries.push_back(e);
    } else {
      // Brand-new book -> newest. Author key starts empty; populated lazily.
      LibraryEntry e{};
      e.pathOffset = appendIntoNewPool(p);
      e.firstSeenMillis = nextSeq++;
      e.fileSize = discoveredSizes[i];
      e.authorKeyOffset = 0;
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
  authorKeyPool.swap(newAuthorKeyPool);
  newEntries.clear();
  newEntries.shrink_to_fit();
  newPool.clear();
  newPool.shrink_to_fit();
  newAuthorKeyPool.clear();
  newAuthorKeyPool.shrink_to_fit();

  walkPerformed = true;
  saveToFile();
  LOG_INF("LIB", "Library index now has %zu entries (walked %zu files)", entries.size(), walkedCount);
  // CrumBLE: a re-walk can add/remove books, so the BookReadingStats-derived
  // virtuals (Finished, New) might be stale relative to the new path set.
  // Invalidate so the next access rescans.
  CollectionsStore::getInstance().invalidateScannedVirtuals();
  // CrumBLE 4.2.1: lazily populate any missing author keys (typical on the
  // first walk after a v1 -> v2 upgrade, or whenever new books were added).
  // Heap-aware + watchdog-safe; gracefully stops if heap pressure rises
  // and resumes on the next walk.
  populateAuthorKeysIfNeeded();
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
  authorKeyPool.clear();
  authorKeyPool.push_back('\0');  // seed sentinel so offset 0 reads back as ""
  LineReader reader(file);
  std::string line;
  // CrumBLE 4.2.1: 0 = no header read yet, 1 = v1, 2 = v2. Header v1 and v2
  // share the same entry parser code path except for the optional author
  // field tucked between size and path.
  int formatVersion = 0;
  size_t lineNo = 0;
  while (reader.next(line)) {
    if (lineNo++ == 0) {
      if (line == LIBRARY_INDEX_HEADER) {
        formatVersion = 2;
      } else if (line == LIBRARY_INDEX_HEADER_V1) {
        formatVersion = 1;
      } else {
        break;  // unknown header -> force rescan
      }
      continue;
    }
    if (line.empty()) continue;
    const size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos) continue;
    // strtoull stops at the tab, so parsing the whole c_str is safe.
    const uint64_t firstSeen = strtoull(line.c_str(), nullptr, 10);
    const size_t tab2 = line.find('\t', tab1 + 1);
    uint32_t fileSize = 0;
    std::string_view authorKey;
    std::string_view path;
    if (tab2 == std::string::npos) {
      // Legacy 2-field line: "<firstSeen>\t<path>" (pre-size index).
      path = std::string_view{line}.substr(tab1 + 1);
    } else {
      fileSize = static_cast<uint32_t>(strtoul(line.c_str() + tab1 + 1, nullptr, 10));
      if (formatVersion >= 2) {
        // v2 has authorKey between tab2 and tab3, then path after tab3.
        const size_t tab3 = line.find('\t', tab2 + 1);
        if (tab3 == std::string::npos) {
          // No third tab on a v2 line -> author key is empty, path follows tab2.
          path = std::string_view{line}.substr(tab2 + 1);
        } else {
          authorKey = std::string_view{line}.substr(tab2 + 1, tab3 - tab2 - 1);
          path = std::string_view{line}.substr(tab3 + 1);
        }
      } else {
        // v1: path follows tab2, no author field.
        path = std::string_view{line}.substr(tab2 + 1);
      }
    }
    if (path.empty()) continue;
    LibraryEntry e{};
    e.pathOffset = appendPath(path);
    e.firstSeenMillis = firstSeen;
    e.fileSize = fileSize;
    e.authorKeyOffset = appendAuthorKey(authorKey);
    entries.push_back(e);
  }
  file.close();

  if (formatVersion == 0) {
    // Legacy/corrupt file — treat as no index, force a rescan.
    entries.clear();
    pathPool.clear();
    authorKeyPool.clear();
    return false;
  }
  if (formatVersion == 1) {
    // v1 -> v2 upgrade pass: the entries loaded with empty authorKey offsets;
    // populateAuthorKeysIfNeeded() (driven by ensureWalked) will fill them
    // lazily and a saveToFile() will rewrite the index in v2 format. No
    // immediate work needed here -- the v2 read path already handled the
    // missing field gracefully.
    LOG_INF("LIB", "Loaded v1 library index with %zu entries -- will upgrade to v2 on next walk", entries.size());
  } else {
    LOG_DBG("LIB", "Loaded v%d library index with %zu entries", formatVersion, entries.size());
  }
  return true;
}

bool LibraryIndex::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite("LIB", LIBRARY_INDEX_FILE, file)) {
    LOG_ERR("LIB", "Could not open library index for writing");
    return false;
  }
  // CrumBLE 4.2.1: v2 always. Stream entries one line at a time
  // ("<firstSeen>\t<size>\t<authorKey>\t<path>") so we never build a
  // JsonDocument + serialized String copy of the whole index in RAM
  // (the OOM-on-save that boot-looped large libraries). v1 lines without
  // the size field still load fine via the v1 read fallback.
  file.print(LIBRARY_INDEX_HEADER);
  file.print("\n");
  char numbuf[48];
  for (const auto& e : entries) {
    snprintf(numbuf, sizeof(numbuf), "%llu\t%lu", static_cast<unsigned long long>(e.firstSeenMillis),
             static_cast<unsigned long>(e.fileSize));
    file.print(numbuf);
    file.print("\t");
    file.print(authorKeyOf(e));  // empty string when no key cached yet
    file.print("\t");
    file.print(pathOf(e));
    file.print("\n");
  }
  file.close();
  return true;
}

// CrumBLE 4.2.1: cached author key accessors. See header for semantics.
std::string_view LibraryIndex::getAuthorKey(const std::string& path) const {
  for (const auto& e : entries) {
    if (path == pathOf(e)) return std::string_view{authorKeyOf(e)};
  }
  return std::string_view{};
}

void LibraryIndex::setAuthorFromRaw(const std::string& path, const std::string& rawAuthor) {
  const std::string key = lastNameLowerForKey(rawAuthor);
  bool changed = false;
  for (auto& e : entries) {
    if (path == pathOf(e)) {
      const std::string_view existing{authorKeyOf(e)};
      if (existing == key) return;  // no-op, key unchanged
      // appendAuthorKey() never reuses offsets -- the old key's bytes stay in
      // the pool as orphan data until the next rescan rebuilds. For ~30-100
      // books with ~10-char keys, this is at most ~1 KB of churn between
      // rescans (rare event), so the simpler "append-only" path stays well
      // within budget. A bookmark-style compaction can land in v4.3 if the
      // numbers grow.
      e.authorKeyOffset = appendAuthorKey(std::string_view{key});
      changed = true;
      break;
    }
  }
  if (changed) saveToFile();
}

void LibraryIndex::populateAuthorKeysIfNeeded() {
  // CrumBLE 4.2.1 hotfix: for any entry whose author key is still empty
  // (typical right after a v1 -> v2 upgrade, or after a new book is added by
  // a rescan), open the book's metadata cache and extract the author. We
  // intentionally avoid Epub::load(buildIfMissing=true) so an uncached EPUB
  // does NOT trigger a full content.opf rebuild here -- that would block
  // begin() for minutes on first boot after upgrade. Books without a cached
  // book.bin (never opened by the reader) keep an empty key and stay
  // sorted to the end of AuthorAlpha until the user opens them once.
  //
  // Heap-aware: pre-flight per book at 30 KB maxAllocHeap (matches the
  // v4.2.1 AuthorAlpha sort fix) and yield every 8 books so we don't trip
  // the IDLE WDT on large libraries. Saves the index once at the end.
  constexpr size_t kYieldEvery = 8;
  constexpr uint32_t kPopulateMinMaxAlloc = 30 * 1024;
  size_t populated = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    LibraryEntry& e = entries[i];
    // Already cached -> skip (this is what makes the method cheap on warm
    // boots: only books that were added since the last walk need work).
    if (e.authorKeyOffset != 0) continue;
    const std::string path{pathOf(e)};
    if (!FsHelpers::hasEpubExtension(path)) continue;  // only EPUBs have author metadata here

    if (ESP.getMaxAllocHeap() < kPopulateMinMaxAlloc) {
      LOG_INF("LIB",
              "populateAuthorKeysIfNeeded: stopping at %zu/%zu (maxAlloc=%u below %u) -- retry on next walk",
              i, entries.size(), ESP.getMaxAllocHeap(), static_cast<unsigned>(kPopulateMinMaxAlloc));
      break;
    }
    {
      Epub epub(path, "/.crosspoint");
      // buildIfMissing=false: skip books with no cached metadata.
      // skipLoadingCss=true: we only need the author string.
      if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
        const std::string key = lastNameLowerForKey(epub.getAuthor());
        if (!key.empty()) {
          e.authorKeyOffset = appendAuthorKey(std::string_view{key});
          populated++;
        }
      }
    }  // Force Epub dtor before the yield so freed allocations are visible
       // to the heap consolidator on the next tick.
    if ((i & (kYieldEvery - 1)) == 0) vTaskDelay(1);
  }
  if (populated > 0) {
    LOG_INF("LIB", "populateAuthorKeysIfNeeded: cached %zu new author key(s), saving index", populated);
    saveToFile();
  }
}
