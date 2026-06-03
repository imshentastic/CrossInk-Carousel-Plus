#pragma once
#include <cstdint>
#include <string>
#include <vector>

// CrumBLE Collections — per-collection sort order. Affects what
// `resolveBookPaths()` returns. User collections default to Manual
// (insertion order), virtual collections default to whatever makes
// semantic sense (DateAddedDesc for Recently Added, TitleAlpha for All
// Books). The user can change the sort for any non-Recently-Added
// collection via the shelf-header "Sort by..." action.
enum class CollectionSort : uint8_t {
  Manual = 0,           // user collections only — preserve insertion order
  TitleAlpha = 1,       // basename A→Z (case-insensitive)
  DateAddedDesc = 2,    // newest first (by LibraryIndex firstSeenMillis)
  DateAddedAsc = 3,     // oldest first
  TitleAlphaDesc = 4,   // basename Z→A
  DateLastReadDesc = 5, // most recently opened first (by RECENT_BOOKS position; non-recents sort to end)
  // CrumBLE: sort by author last name (extracted heuristically from the
  // EPUB metadata's <dc:creator> -- handles "First Last", "Last, First",
  // and falls back to the whole author string if no split point is
  // found). Books with no cached metadata or no author sort to the end.
  AuthorAlpha = 6,      // author last name A→Z
  AuthorAlphaDesc = 7,  // author last name Z→A
};

// CrumBLE Collections — user-defined tag groups that live on top of the
// filesystem. Books can belong to zero or more collections regardless of where
// they sit on the SD card. Phase 1 ships with a single hardcoded "Favorites"
// collection; later phases add a picker, custom collections, and series
// support. See spec in PR for the long-term shape.
//
// Storage: /.crosspoint/collections.json. In-memory the entire model is a
// vector of Collection records — typically <5 KB total — so all accesses
// are O(N) over a tiny N and run during the render path without SD I/O.

struct Collection {
  std::string id;    // stable machine identifier (e.g. "favorites")
  std::string name;  // user-facing label
  std::vector<std::string> bookPaths;
  // Virtual collections aren't user-editable — their bookPaths are
  // derived from another data source (LibraryIndex) at lookup time. Not
  // persisted to collections.json. Seeded on every CollectionsStore::begin().
  bool isVirtual = false;
  // Per-collection sort. resolveBookPaths() honors this when assembling
  // the path list. User collections persist their choice in
  // collections.json; virtuals default-construct each begin() and
  // accept user overrides at runtime (also persisted).
  CollectionSort sortMode = CollectionSort::Manual;
  // CrumBLE series collapse: when true and a series has 2+ books in
  // this collection, those books render as a single shelf cell with
  // a dark spine glyph. Default ON; user can toggle per-collection
  // from the shelf header action menu. Persisted in collections.json.
  bool collapseSeries = true;
  // CrumBLE two-row shelf: when true, the Flow theme's bookshelf strip
  // renders 2 rows of 6 smaller (60x90) covers per page instead of the
  // default 1 row of 4 (100x150). Logical order is row-major: the last
  // book of row 1 is followed by the first book of row 2 in the same
  // page. Default OFF (1-row layout) per-collection; user toggles via
  // the shelf-header action menu. Persisted in collections.json.
  bool twoRowShelf = false;
};

// One slot on the bookshelf row. For single books, memberPaths.size()==1
// and seriesName is empty. For collapsed series groups, memberPaths
// holds every book in the series (sorted by series index ASC) and
// seriesName is populated. firstPath is always memberPaths[0] — used
// by the renderer as the cover thumb path.
struct ShelfEntry {
  std::string firstPath;
  std::string seriesName;
  std::vector<std::string> memberPaths;
};

class CollectionsStore {
  static CollectionsStore instance;

  std::vector<Collection> collections;
  std::string activeId;

 public:
  // Default collection id created on first boot when no JSON exists.
  static constexpr const char* FAVORITES_ID = "favorites";
  static constexpr const char* FAVORITES_NAME = "Favorites";
  // Virtual collection ids — populated from LibraryIndex on every begin().
  // Kept stable across versions so the JSON's "active" pointer can refer
  // to them and survive reboots.
  static constexpr const char* RECENTLY_ADDED_ID = "recently_added";
  static constexpr const char* RECENTLY_ADDED_NAME = "Recently Added";
  static constexpr const char* ALL_BOOKS_ID = "all_books";
  static constexpr const char* ALL_BOOKS_NAME = "All Books";
  // CrumBLE: completion-derived virtuals. Both walk LibraryIndex AND read
  // each book's BookReadingStats (1 file open per book) the first time
  // they're resolved this session; subsequent calls hit an in-memory cache
  // until invalidateScannedVirtuals() flips it.
  static constexpr const char* FINISHED_ID = "finished";
  static constexpr const char* FINISHED_NAME = "Finished";
  // ID stays "new" for any persisted reference (e.g. activeId in collections.json).
  // Display name renamed to "Unopened" -- clearer about WHY the books are there
  // (haven't been opened in the reader yet) than the ambiguous "New".
  static constexpr const char* NEW_ID = "new";
  static constexpr const char* NEW_NAME = "Unopened";

  ~CollectionsStore() = default;

  static CollectionsStore& getInstance() { return instance; }

  // Loads collections.json from SD; seeds default "Favorites" if missing.
  // Idempotent — safe to call once at boot from main.cpp.
  void begin();

  // CrumBLE: add/remove a virtual collection (Recently Added / All Books) from
  // the live list when the user toggles its visibility from the Home menu. The
  // visibility itself is persisted via CrossPointSettings; this just keeps the
  // in-memory list in sync for the current session (begin() reseeds on reboot).
  void setVirtualCollectionVisible(const char* id, const char* name, bool visible);

  // Mutators (auto-save after each).
  // Returns true if the book is now IN the collection after toggling.
  bool toggleBookInCollection(const std::string& collectionId, const std::string& bookPath);
  void setActiveId(const std::string& id);
  // Creates a brand-new collection with the given user-facing name. The
  // id is generated automatically (millis-derived; unique in practice on
  // any reasonable cadence of creates). Empty-name calls are rejected.
  // Returns the new collection's id, or empty string on failure. Persists
  // to SD on success.
  std::string createCollection(const std::string& name);
  // Renames an existing collection. Refuses virtual collections (their
  // names are seeded each begin() and would just reset). Empty-name
  // calls are rejected. Returns true on success. Persists.
  bool renameCollection(const std::string& collectionId, const std::string& newName);
  // Removes a user collection entirely. Books on disk are untouched —
  // they just lose their tag in this collection (and stay in any other
  // collections they're in). Refuses virtual collections (Recently
  // Added, All Books) and the seeded Favorites (would re-create on
  // next boot anyway). If the deleted collection was active, the
  // active id is reset to FAVORITES_ID. Persists.
  bool deleteCollection(const std::string& collectionId);
  // Sets the sort mode for the given collection. No-op (with error log)
  // for unknown ids. Persists on change. Manual sort is rejected for
  // virtual collections — their book lists aren't user-ordered.
  void setSortMode(const std::string& collectionId, CollectionSort mode);
  // Sets the collapse-series flag. Persists.
  void setCollapseSeries(const std::string& collectionId, bool on);
  // CrumBLE: sets the two-row shelf flag. Persists. No-op (with error log)
  // for unknown ids. Always honored regardless of virtual/user (the layout
  // is a visual preference independent of where the book list comes from).
  void setTwoRowShelf(const std::string& collectionId, bool on);
  // Removes the given book from every collection it appears in (used when
  // the book file is being deleted off the SD card). Auto-saves at the
  // end if any change was made. Returns the number of collections that
  // had to be modified — informational only.
  int removeBookFromAllCollections(const std::string& bookPath);

  // Bulk-add: append every path in `bookPaths` to the given user collection,
  // skipping paths already present. Saves once at the end instead of once
  // per book — keeps SD writes bounded when populating from a folder. Refuses
  // virtual collections (their book lists are derived, not stored). Returns
  // the number of books actually added (0 if all were duplicates / no
  // collection found / virtual).
  int addBooksToCollection(const std::string& collectionId, const std::vector<std::string>& bookPaths);

  // Read-only accessors.
  bool isBookInCollection(const std::string& collectionId, const std::string& bookPath) const;
  const Collection* findCollection(const std::string& collectionId) const;
  const std::vector<Collection>& getCollections() const { return collections; }
  const std::string& getActiveId() const { return activeId; }
  const Collection* getActiveCollection() const { return findCollection(activeId); }
  // Returns the effective book-path list for a collection at this exact
  // moment. For user collections this just copies stored bookPaths; for
  // virtual collections (Recently Added / All Books) it queries
  // LibraryIndex, triggering an SD walk on first access this session if
  // the index hasn't been built yet. Use this anywhere you need a live
  // view rather than the stored Collection.bookPaths (which is stale or
  // empty for virtuals).
  std::vector<std::string> resolveBookPaths(const std::string& collectionId) const;
  // Higher-level resolve that also collapses series into single
  // ShelfEntries when the collection has collapseSeries=true AND
  // SeriesIndex has 2+ members for the same series-key in this
  // collection. Books without series info, or singletons of an
  // identified series, become 1-member ShelfEntries. Used by the
  // shelf renderer; navigation indexes ShelfEntries 1:1.
  std::vector<ShelfEntry> resolveShelfEntries(const std::string& collectionId) const;
  // Convenience: returns the count for the active collection without
  // building the full vector. Still triggers a walk for virtuals on
  // first access. Use sparingly — pulls fresh data each call.
  int countBooksInCollection(const std::string& collectionId) const;

  // Returns `name` unchanged if no other collection currently uses it,
  // otherwise returns "name (N)" with the smallest unused N starting at 1.
  // Comparison is case-sensitive — "Sci-Fi" and "sci-fi" are treated as
  // distinct. The optional `ignoreId` excludes one collection from the
  // duplicate set (used by renameCollection so renaming to the same name
  // is a no-op instead of bumping to "Name (1)"). Virtual collection names
  // (Favorites / All Books / Recently Added / Finished / Unopened) are
  // included in the duplicate scan so user collections can't shadow them.
  std::string disambiguateName(const std::string& name, const std::string& ignoreId = {}) const;

  // CrumBLE: invalidate the in-memory cache of the Finished / New virtual
  // collections. Call this whenever a book's completion state or
  // sessionCount changes (BookActions::toggleEpubCompleted, the reader's
  // first 60s-session bump), or when the underlying library walk re-runs
  // (LibraryIndex::rescan). Cheap -- just flips a bool; the next resolve
  // re-scans lazily.
  void invalidateScannedVirtuals() const;

  // CrumBLE: re-sort the in-memory collections vector to match the given id
  // sequence and persist the new order. IDs present in the vector but not
  // in `orderedIds` are kept at the end in their current relative order
  // (covers the "user rearranges only some of N collections" edge case,
  // although the rearrange UI always passes the complete list). Unknown
  // IDs in `orderedIds` are ignored. Saves to collections.json on change.
  void setDisplayOrder(const std::vector<std::string>& orderedIds);

 private:
  CollectionsStore() = default;
  bool loadFromFile();
  bool saveToFile() const;
  // Ensures FAVORITES_ID exists in the collections vector. Used after load
  // when the file was missing/empty/corrupt, and on first boot.
  void seedDefaults();
  // Reorders `collections` in-place to match `displayOrderIds_`. Called at
  // the end of begin() after virtuals have been seeded, so the user's saved
  // L/R cycle order is honored across reboots (including for virtuals,
  // which aren't otherwise persisted to collections.json).
  void applyDisplayOrder();
  // The saved L/R cycle order from collections.json. May be empty (first
  // boot, pre-rearrange) -- in which case applyDisplayOrder() is a no-op and
  // the natural seeding order is used.
  std::vector<std::string> displayOrderIds_;
  // CrumBLE: lazy cache for the BookReadingStats-derived virtuals.
  // Populated on first resolveBookPaths(FINISHED_ID|NEW_ID) call this
  // session and reused until invalidateScannedVirtuals() flips the flag.
  // Mutable because the const resolveBookPaths() lazily populates them.
  mutable bool scannedVirtualsValid_ = false;
  mutable std::vector<std::string> finishedPathsCache_;
  mutable std::vector<std::string> newPathsCache_;
  void rebuildScannedVirtualsIfNeeded() const;
};
