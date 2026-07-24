#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// CrumBLE — persistent index of every book file the device has seen on
// SD, with a first-seen timestamp per entry. Backs the auto-managed
// virtual collections "All Books" and "Recently Added" (sorted by
// firstSeen DESC). Lives at /.crosspoint/library_index.json so a full SD
// walk only needs to happen once (initial discovery) plus on explicit
// rescan; subsequent boots load the cached index in milliseconds.
//
// Memory shape: paths are pooled into a single contiguous std::vector<char>
// (null-terminated, back-to-back) and each LibraryEntry holds an offset
// into it. Two reasons over the original "std::string per entry":
//   1. N separate small heap allocs scatter the heap; the freed slots
//      don't always coalesce into a contiguous block. File-transfer's
//      WiFi stack needs 8-16 KB contiguous chunks and stalls when the
//      heap is fragmented even though total free is healthy.
//   2. releaseMemory() on a pool returns ONE large hole, not N scattered
//      holes. Reclaim is clean.
// 500-book library at ~120 chars/path ≈ 60 KB pool + 16 KB entry vector,
// still within budget on the ESP32-C3.

struct LibraryEntry {
  // Offset into LibraryIndex::pathPool of this entry's null-terminated path.
  // The pool's contents are stable for the lifetime of one rescan/load;
  // rescan rebuilds it atomically. Use LibraryIndex::pathOf() to
  // dereference -- never index the pool directly from outside the class.
  uint32_t pathOffset = 0;
  // Sort key for "Recently Added" / "Date Added" (DESC = newest first). Despite
  // the legacy name, this is a persistent monotonic "first seen by the device"
  // counter, NOT a wall-clock time: millis() resets each boot, and file
  // timestamps are unreliable on this device (WiFi transfers stamp a default
  // date when the clock is unset). New books -- and same-name files whose size
  // changed (a replaced copy) -- get the next counter value so they sort newest.
  uint64_t firstSeenMillis = 0;
  // File size in bytes at last index (capped to uint32). Lets a rescan notice a
  // same-path file was replaced and re-date it. 0 = unknown (legacy entry).
  uint32_t fileSize = 0;
  // CrumBLE 4.2.1: persisted lower-cased "last name" key for AuthorAlpha sort.
  // Offset into LibraryIndex::authorKeyPool (null-terminated).
  //   0          = "not yet attempted" -- populate will try.
  //   1          = "attempted but no author found" sentinel (CrumBLE 4.5.4) --
  //                pool byte at offset 1 is '\0' so the sort reads back as "".
  //                populate skips entries with this set so we don't re-peek
  //                corrupt OPFs on every walk. Reset to 0 by the walker when
  //                the underlying file changes (size/mtime mismatch), giving
  //                replaced books a fresh attempt without manual intervention.
  //   >= 2       = real key bytes in authorKeyPool.
  // Populated by populateAuthorKeysIfNeeded() (called from ensureWalked) and
  // by every successful EpubReaderActivity::onEnter (cheap once the book is
  // already loaded), so the cache fills both proactively and lazily.
  uint32_t authorKeyOffset = 0;
};

class LibraryIndex {
  static LibraryIndex instance;
  std::vector<LibraryEntry> entries;
  // Single contiguous backing buffer for every entry's path. Paths are
  // appended back-to-back, each null-terminated. Indexed via
  // entries[i].pathOffset. Stable for the lifetime of one load/rescan;
  // mutating operations rebuild the pool wholesale.
  std::vector<char> pathPool;
  bool jsonLoaded = false;  // /.crosspoint/library_index.json has been read
  bool walkPerformed = false;  // an SD walk has run this session (refresh or initial)
  bool freshFirstBoot = false;  // begin() ran with no index file on SD — used to gate welcome UI

  LibraryIndex() = default;

 public:
  static LibraryIndex& getInstance() { return instance; }

  // Loads /.crosspoint/library_index.json if it exists. Idempotent.
  // Does NOT trigger an SD walk — that's deferred to ensureWalked() so
  // boot stays fast.
  void begin();

  // True when begin() ran and found no existing index file. main.cpp
  // checks this to drive the first-boot welcome screen — the SD walk
  // gets framed as "indexing your library" rather than an unexplained
  // pause on a later L/R cycle.
  bool wasFreshFirstBoot() const { return freshFirstBoot; }

  // If we haven't done an SD walk this session, do one now. Adds new
  // book files to the index with current millis() as firstSeen and
  // removes entries whose underlying file disappeared. Progress callback
  // is invoked periodically with 0..100 — wire to GUI.fillPopupProgress
  // for a visible loading bar during the first walk.
  void ensureWalked(const std::function<void(int /*progress0to100*/)>& progress = nullptr);

  // True iff the SD walk has run this session. Lets callers skip the
  // Loading-popup setup when ensureWalked() would be a no-op (the popup
  // flash on every virtual-collection cycle was the symptom this fixes).
  bool hasWalked() const { return walkPerformed; }

  // Force a rescan even if walkPerformed is already true (e.g. user
  // triggered "Rescan library" manually).
  void rescan(const std::function<void(int)>& progress = nullptr);

  // Marks the index as needing a fresh walk on next access. Used by
  // file-server / hotspot exit paths where books may have just been
  // uploaded over WiFi — the walk itself is still lazy (deferred to
  // next virtual-collection access) so onExit stays snappy.
  void markStale() { walkPerformed = false; }

  // CrumBLE 4.5.4: read-only accessor for HomeActivity's background
  // populate pump -- only ticks when an SD walk has populated entries.
  bool isWalkPerformed() const { return walkPerformed; }

  // CrumBLE: free both the entry vector AND the path pool to reclaim heap.
  // The path pool is the persistent state; releasing it returns one large
  // contiguous hole (instead of N scattered std::string holes the original
  // implementation produced). The pool-shape change is what made
  // file-transfer + WiFi reliably get its 8-16 KB contiguous allocs.
  // Next ensureWalked()/begin() repopulates from the on-disk JSON plus an
  // SD walk; web-server exit paths already markStale() so the rebuild is
  // automatic. Only call when nothing is actively reading the index.
  void releaseMemory() {
    std::vector<LibraryEntry>().swap(entries);
    std::vector<char>().swap(pathPool);
    std::vector<char>().swap(authorKeyPool);
    jsonLoaded = false;
    walkPerformed = false;
  }

  // Read accessors. Both produce a fresh vector copy — the underlying
  // index is small enough that this is cheap.
  std::vector<std::string> getAllBookPaths() const;        // sorted by path (case-insensitive)
  std::vector<std::string> getRecentlyAddedPaths(int maxCount) const;  // newest first
  // Returns the firstSeenMillis recorded for `path`. Used by
  // CollectionsStore::resolveBookPaths to sort user collections by
  // Date Added without duplicating the timestamp into the
  // collection's stored payload. Returns 0 if the path isn't in the
  // index (e.g. user added a book to Favorites that hasn't been
  // walked yet — the unknown books sort to the end / start depending
  // on Asc/Desc).
  uint64_t getFirstSeen(const std::string& path) const;

  // CrumBLE 4.2.1: lower-cased "last name" author key for AuthorAlpha sort.
  // Returns an empty view when the path isn't indexed or the key hasn't been
  // populated yet. Callers should fall back to a basename-derived key for
  // unknown paths so the sort still produces a stable order on uncached books.
  std::string_view getAuthorKey(const std::string& path) const;
  // Sets the author key for `path`. Lower-cases / extracts the last name
  // internally so callers can pass the raw `epub.getAuthor()` string.
  // Triggers a saveToFile() so the cache survives a reboot. No-op if the
  // path isn't currently indexed (caller should trigger a rescan first).
  void setAuthorFromRaw(const std::string& path, const std::string& rawAuthor);
  // Walk the index and lazily populate any entries whose authorKey is still
  // empty by load()-ing the EPUB's metadata cache (no full re-index). Heap-
  // and watchdog-safe: yields every N books, breaks early if maxAllocHeap
  // v18.9.9.223: populateAuthorKeysIfNeeded removed. Superseded by
  // populateAuthorKeysWithProgress (below), the progress-callback
  // variant. v219 removed the only two live callers (ensureWalked
  // inline populate + HomeActivity background tick).

  // Bookkeeping after a destructive action elsewhere (e.g. file delete).
  // The next ensureWalked() would pick this up, but for instant UI
  // refresh callers can drop the entry directly.
  void forgetPath(const std::string& path);

  // Static helper: collect every book-file path under `dirPath` using the
  // same recursive walker and extension filter as the library scan
  // (.epub / .xtc / .txt / .md / .markdown, max 8 levels deep, skips
  // dot-prefixed entries and the XTcache folder). Does NOT modify the
  // index — pure read. Used by the file browser's "Make collection from
  // folder" action so callers can act on a subtree without rescanning
  // the whole SD.
  static std::vector<std::string> collectBookPaths(const std::string& dirPath);

 private:
  bool loadFromFile();
  bool saveToFile() const;
  // Recursive descent. depth caps at 8 to avoid runaway nesting. Skips
  // dot-prefixed directories (".crosspoint", ".Trashes", etc.). For each book
  // file found, appends its path to outPaths and its size (bytes) to outSizes at
  // the same index.
  void walkRecursive(const std::string& dirPath, int depth, std::vector<std::string>& outPaths,
                     std::vector<uint32_t>& outSizes) const;
  // True if the file extension marks it as one of our supported book
  // formats (.epub / .xtc / .txt / .md / .markdown).
  static bool isBookPath(const std::string& path);

  // Internal helpers for working with the pooled paths.
  const char* pathOf(const LibraryEntry& e) const { return pathPool.data() + e.pathOffset; }
  std::string_view pathViewOf(const LibraryEntry& e) const { return std::string_view{pathOf(e)}; }
  // Append `path` (no null terminator required) into pathPool with a
  // trailing '\0'. Returns the offset where the path's first byte landed.
  // The pool may reallocate; callers must not hold raw c_str() / pathOf()
  // pointers across calls to appendPath().
  uint32_t appendPath(std::string_view path);

  // CrumBLE 4.2.1: parallel pool + helpers for the cached author key. Same
  // shape as the path pool (single contiguous std::vector<char> of NUL-
  // terminated strings) so it survives releaseMemory() / rebuilds with the
  // same lifetime rules. Offset 0 is reserved as the "empty string"
  // sentinel — newly-constructed LibraryEntry::authorKeyOffset defaults
  // to 0 and the pool is seeded with a single '\0' byte so authorKeyOf()
  // on an uncached entry returns "" cheaply.
  // CrumBLE 4.5.4: pool now seeded with TWO '\0' bytes so offset 1 also
  // reads back as "". Offset 1 is the "tried but no author found" sentinel
  // that populateAuthorKeysIfNeeded sets on failure so we don't re-peek
  // corrupt OPFs every walk.
  static constexpr uint32_t kAuthorKeyOffsetTriedEmpty = 1u;
  std::vector<char> authorKeyPool;
  const char* authorKeyOf(const LibraryEntry& e) const { return authorKeyPool.data() + e.authorKeyOffset; }
  // Append `key` to authorKeyPool with a trailing '\0' and return the offset.
  // 0-offset entry = "not yet attempted"; 1-offset = "attempted, no author".
  uint32_t appendAuthorKey(std::string_view key);

 public:
  // CrumBLE 4.5.4: deferred / background populate.
  //
  // # of entries that still need an author-key population pass (offset 0).
  // Cheap O(N) scan; intended for the UI to know whether deferred work is
  // pending so it can show a 'still indexing N books' badge.
  size_t pendingAuthorKeyCount() const;
  // Process up to `maxBooks` entries from the populate queue. Returns true
  // if more entries still need work after this batch (caller can pump
  // again on the next idle tick). Yields the FreeRTOS scheduler every
  // few books so a 50-book batch costs ~3-5 s wall but doesn't starve
  // the UI task. Heap-aware: bails early if MaxAlloc drops below floor.
  // Safe to call from HomeActivity's loop tick.
  bool tickPopulateAuthorKeys(size_t maxBooks);
  // v18.9.9.218: caller-driven variant of populateAuthorKeysIfNeeded() with
  // a progress callback (0-100). Used by the Sort-by-Author trigger to
  // fill missing keys just-in-time with a progress popup, so unopened
  // books sort correctly on first-ever author-sort rather than clustering
  // at the end. Optional callback fires ~once per book. Behavior otherwise
  // identical to populateAuthorKeysIfNeeded(): same 15 KB heap floor,
  // same yield cadence, same OPF-peek fallback.
  void populateAuthorKeysWithProgress(std::function<void(int)> progress);
  // v18.9.9.222: reset all cached author keys to 0 (never attempted) so a
  // subsequent Sort by Author re-populates via OPF peek with any updated
  // key-derivation logic. Used by Settings > "Rebuild author keys" to
  // pick up v220's trailing-punctuation / ';' -split fixes on entries
  // whose keys were populated before v220.
  void resetAuthorKeys();
};
