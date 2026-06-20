#pragma once
#include <functional>
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

// CrumBLE 4.4: shared author-metadata normalizer. Strips leading/trailing
// whitespace + separator characters (";", ",") that EPUB OPF often leaves
// behind when an author field has no value or extra delimiters between
// multiple authors. Idempotent on clean input. Applied at the storage
// layer (RecentBooksStore + JSON load) so every display site reads clean
// data without needing to know about EPUB metadata quirks.
std::string normalizeAuthorMeta(std::string s);

class RecentBooksStore;
namespace JsonSettingsIO {
bool loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

  friend bool JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

 public:
  // Cap on stored recents. 18 = two full pages of the 3x3 RecentBooksGridActivity,
  // which is the only consumer that paginates. addBook() trims to this on insert.
  static constexpr int MAX_RECENT_BOOKS = 18;

  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Deprecated compatibility wrapper. Use addOrUpdateBook so the promote-or-update behavior is explicit.
  [[deprecated("use addOrUpdateBook")]]
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  // Add a new book to the front, or refresh an existing entry and promote it
  // to the front.
  void addOrUpdateBook(const std::string& path, const std::string& title, const std::string& author,
                       const std::string& coverBmpPath);

  // updateBook updates metadata for an existing book only and must not change
  // recent-books ordering. Use addOrUpdateBook when the touched book should
  // become most recent. Returns false if the book does not exist.
  [[nodiscard]] bool updateBook(const std::string& path, const std::string& title, const std::string& author,
                                const std::string& coverBmpPath);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed (no-op + false otherwise).
  // Persistence is best-effort: a failed save is logged, not reflected in the return.
  bool removeByPath(const std::string& path);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // Remove a book from the recents list. Returns true if a matching entry
  // existed and was removed (and the store was saved). False if no match.
  // Used by the home long-press action menu's "Remove from Recent Books"
  // item — does NOT delete the underlying file or its cache.
  bool removeBook(const std::string& path);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;

  // CrumBLE: scans LibraryIndex for books with a stats.bin sidecar that
  // aren't currently in the recents list and adds them (newest stats.bin
  // mtime first), up to MAX_RECENT_BOOKS. This is the heal path for when
  // running a foreign firmware (e.g. rhythmerc/crosspoint-reader) between
  // CrumBLE boots wiped or replaced /.crosspoint/recent.json -- per-book
  // stats.bin files survive on the SD card, so we can reconstruct an
  // approximate recents list from them.
  //
  // `onProgress` (optional): invoked 0..100 during the metadata-load pass.
  // Returns number of books added (0 means recents was already at cap, no
  // candidate books with stats exist, or no metadata loads succeeded).
  int healFromStats(const std::function<void(int)>& onProgress = nullptr);

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
