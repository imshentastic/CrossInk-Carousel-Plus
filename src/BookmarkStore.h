#pragma once
#include <cstdint>
#include <string>
#include <vector>

// chapterTitle / preview are always NUL-terminated within their MAX bytes.
// These sizes are part of the on-disk format — do not change without
// incrementing the file version.
inline constexpr size_t BOOKMARK_CHAPTER_TITLE_MAX = 48;
// CrumBLE 4.2: bumped from 160 -> 1024. Earlier cap clipped highlights at
// ~25-30 words which made the new QuoteViewer feel pointless for longer
// passages (the user explicitly asked for 100+ word quotes). 1024 chars
// covers ~170 words at typical word length. Existing v4 bookmark files
// (160-byte preview field) read fine via the version-gated path in
// BookmarkStore::readFromFile -- they just won't grow until the user
// re-highlights. In-RAM cost: ~1.1 KB per Bookmark; a typical 50-
// bookmark book holds ~55 KB in the in-memory vector. Bookmark cap
// (MAX_BOOKMARKS = 1024 in .cpp) bounds the worst-case at ~1.1 MB on
// disk but that's only paid by power users; the in-RAM working set
// for one open book is what matters for heap.
inline constexpr size_t BOOKMARK_PREVIEW_MAX = 1024;
// CrumBLE 4.2: legacy size for v4 bookmark files. BookmarkStore reads
// these via a fixed 160-byte scratch buffer when loading a v4 file, then
// copies into the now-larger bm.preview field (rest zero-filled). New
// highlights write at the full BOOKMARK_PREVIEW_MAX size under v5.
inline constexpr size_t BOOKMARK_PREVIEW_MAX_V4 = 160;

// CrumBLE: bookmarks are now ranges, not points. Phase-1 single-page
// highlights have startSpine==endSpine and start/end page/word fields
// describe the selection extent. Phase-2 cross-page highlights have
// the same spine but different page/word. Phase-3 cross-chapter
// highlights have endSpine > startSpine. Pre-highlight ("v3 file
// format") bookmarks are migrated as zero-length highlights: all
// end* fields mirror their start* equivalents, preview is empty.
struct Bookmark {
  // Start anchor (was the only anchor in v3 file format).
  uint16_t spineIndex;
  float progress;            // 0..1 within startSpine, kept for v3 back-compat resolve
  uint32_t timestamp;
  char chapterTitle[BOOKMARK_CHAPTER_TITLE_MAX];

  // End anchor + word indices (v4 additions). Always present in-memory;
  // v3-on-disk loads with end* mirroring start* and word indices zero.
  uint16_t endSpineIndex;    // == spineIndex for single-chapter selections
  float endProgress;         // 0..1 within endSpineIndex
  uint16_t startWord;        // word index within the start page
  uint16_t endWord;          // word index within the end page (inclusive)

  // User-facing preview of the selected text. Up to BOOKMARK_PREVIEW_MAX-1
  // chars + NUL; longer selections truncate with a trailing ellipsis.
  // Empty for migrated v3 records (user can re-select to populate).
  char preview[BOOKMARK_PREVIEW_MAX];
};

struct BookmarkedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count;
};

class BookmarkStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
  };

  static BookmarkStore& getInstance() { return instance; }

  // Load bookmarks for a book. Returns true even when no file exists yet (empty store).
  // bookType must be "epub", "xtc", or "txt" — used to form the cache filename.
  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addBookmark(uint16_t spineIndex, float progress, int pageCount, const char* chapterTitle);

  // CrumBLE: add a ranged highlight. start/end may be in different chapters
  // for cross-chapter selections. preview is the user-facing snippet of the
  // selected text (typically the first N chars; ellipsized by the caller).
  // chapterTitle should be the START chapter's title (matches how the list
  // groups highlights). Same MAX_BOOKMARKS limit applies as point bookmarks.
  AddResult addHighlight(uint16_t startSpine, float startProgress, uint16_t startWord,
                          uint16_t endSpine, float endProgress, uint16_t endWord, int startPageCount,
                          const char* chapterTitle, const char* preview);

  void removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool removeBookmarkAt(size_t index);
  bool hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  const std::vector<Bookmark>& getBookmarks() const { return bookmarks; }

  // Flush to disk if dirty. Called automatically by add/remove; also call from reader onExit().
  void saveToFile();

  // Remove all bookmarks for the current book and delete its bookmark file.
  void clearAll();

  // Returns true if any bookmark files exist on disk (directory scan, no file parsing).
  static bool hasAnyBookmarks();

  // Delete the bookmark file for a given file path and book type without loading the book.
  // bookType must be "epub", "xtc", or "txt".
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);

  // Scan /.crosspoint/bookmarks/ and populate `out` with one entry per book that has bookmarks.
  // Reads only the file header (does not load full bookmark records).
  // Caller should reserve `out` before calling.
  static bool getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out);

 private:
  static BookmarkStore instance;

  std::vector<Bookmark> bookmarks;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool readFromFile();
  bool writeToFile() const;
};

#define BOOKMARKS BookmarkStore::getInstance()
