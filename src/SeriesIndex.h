#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// CrumBLE — per-book series metadata cache, persistent across reboots.
// Populated lazily (when a collection with collapse-series enabled is
// rendered, we parse OPFs for any of its books we haven't seen yet) so
// the first SD walk stays fast.
//
// Ported in spirit from dawsonfi/aalu (MIT, Copyright (c) 2025 Dave
// Allie) — same idea (per-book {seriesName, seriesIndex}, JSON cache,
// case-folded "series key" for grouping). Renamed and rewritten to fit
// CrumBLE's existing CollectionsStore / LibraryIndex architecture.
//
// Storage shape:
//   /.crosspoint/series_index.json
//   { "version": 1, "books": [ {"path":"…", "name":"Foundation",
//                                "index":"2"}, … ] }
//
// `seriesIndex` is stored as a string (matches aalu) so "1", "1.5",
// "VII", or "Vol. 3" round-trip exactly for display. Sorting uses a
// parsed-numeric path with a string-fallback comparator.
//
// Memory shape: paths + names + indices are pooled into a single
// contiguous std::vector<char> (each null-terminated). Each entry holds
// 3 uint32_t offsets into the pool. This avoids the heap fragmentation
// that the original 4-std::string-per-entry layout caused -- see the
// LibraryIndex doc comment for the full diagnosis. Same fix, applied
// to the next-most-fragmenting per-book metadata store.

struct SeriesEntry {
  // Offsets into SeriesIndex::stringPool of this entry's null-terminated
  // strings. Dereference via SeriesIndex::pathOf / nameOf / indexOf;
  // the pool's contents are stable for the lifetime of one
  // load/record cycle. Records that change a string's length trigger
  // an atomic rebuild rather than mutating in place.
  uint32_t pathOffset = 0;
  uint32_t nameOffset = 0;
  uint32_t indexOffset = 0;
};

class SeriesIndex {
  static SeriesIndex instance;
  std::vector<SeriesEntry> entries;
  // Single contiguous backing buffer for every entry's strings (path,
  // name, index), null-terminated and packed back-to-back. The mutating
  // operations (record / forgetPath / loadFromFile) rebuild it atomically.
  std::vector<char> stringPool;
  bool jsonLoaded = false;

  SeriesIndex() = default;

 public:
  static SeriesIndex& getInstance() { return instance; }

  // Loads /.crosspoint/series_index.json. Idempotent. Doesn't walk SD.
  void begin();

  // Records the series info parsed from a single book's OPF. Empty
  // values are honored — recording {path, "", ""} marks the book as
  // "checked, no series" so enrichment doesn't re-parse it on every
  // visit. Persists on each call. (Cheap: small JSON file.)
  void record(const std::string& path, const std::string& name, const std::string& index);

  // True if we've already attempted enrichment for this path (whether
  // we found series info or not).
  bool hasBeenChecked(const std::string& path) const;

  // Returns the entry index for `path`, or nullopt if we haven't checked
  // it yet. Caller uses pathOf/nameOf/indexOf to read the underlying
  // strings. The returned index is invalidated by any record() /
  // forgetPath() call.
  const SeriesEntry* find(const std::string& path) const;
  const char* pathOf(const SeriesEntry& e) const { return stringPool.data() + e.pathOffset; }
  const char* nameOf(const SeriesEntry& e) const { return stringPool.data() + e.nameOffset; }
  const char* indexOf(const SeriesEntry& e) const { return stringPool.data() + e.indexOffset; }

  // Removes the entry — used when a book is deleted from disk.
  void forgetPath(const std::string& path);

  // CrumBLE: drop the in-RAM cache (entries + stringPool) without touching
  // the on-disk JSON. Called from FT entry to reclaim heap for the WiFi /
  // web-server pipeline. Safe to call repeatedly; the next begin() will
  // reload from SD. For large libraries the pool can hold tens of KB --
  // typically the biggest non-WiFi sticky consumer at FT time.
  void releaseMemory();

  // Canonicalizes a series name into a stable key for grouping:
  //   - lowercase
  //   - collapses internal whitespace runs to single space
  //   - trim
  // Tolerates capitalization drift (e.g. "Foundation" vs "FOUNDATION").
  static std::string seriesKey(const std::string& rawName);

  // Compares two series indices for sorting WITHIN a series. Parses
  // leading numeric portion of each (handles "1", "1.5", "10");
  // ties / unparseable indices fall back to lexicographic compare.
  static bool indexLess(const std::string& a, const std::string& b);

 private:
  bool loadFromFile();
  bool saveToFile() const;

  // Append a single null-terminated string into stringPool. Returns the
  // offset of its first byte. Callers must not retain raw pointers
  // across appendString() calls -- the pool may reallocate.
  uint32_t appendString(std::string_view s);
  // Linear-scan find by path. Returns -1 if not found. Cheap at the
  // library sizes we run (≤ a few hundred entries) and avoids the
  // hash-map overhead the old layout used.
  int indexOfPath(std::string_view path) const;
  // Rebuild entries + stringPool from a scratch list of {path, name,
  // index} triples. Used by mutating operations that can't update in
  // place (e.g. record() with a longer replacement name -- in-place
  // would corrupt subsequent entries' offsets).
  struct ScratchEntry {
    std::string path, name, index;
  };
  void rebuildFrom(std::vector<ScratchEntry>& scratch);
};
