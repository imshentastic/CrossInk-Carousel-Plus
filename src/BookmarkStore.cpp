#include "BookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <cstring>  // memset / strncpy for v4 -> v5 preview migration
#include <limits>

namespace {
constexpr uint8_t LEGACY_VERSION = 2;
// CrumBLE 4.2: V4 read-compat. v4 files have 160-byte preview fields; we
// still read them, then upgrade to the v5 layout on the next saveToFile().
constexpr uint8_t V4_VERSION = 4;
// V3 = point bookmarks (pre-highlight). V4 = ranged highlights w/ 160-byte
// preview text. V5 = ranged highlights w/ 1024-byte preview text (CrumBLE
// 4.2, to surface ~100+ word quotes in QuoteViewerActivity). Loading any
// of these is supported; writes always emit the current VERSION.
constexpr uint8_t POINT_VERSION = 3;
constexpr uint8_t VERSION = 5;
// Stored count is uint16_t in v3+, but we keep an in-memory safety cap for ESP32-C3 RAM.
constexpr uint16_t MAX_BOOKMARKS = 1024;
constexpr size_t INITIAL_BOOKMARK_RESERVE = 8;
constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";

bool readBookmarkCount(FsFile& file, const uint8_t version, uint16_t& count) {
  if (version == LEGACY_VERSION) {
    uint8_t legacyCount = 0;
    serialization::readPod(file, legacyCount);
    count = legacyCount;
    return true;
  }

  if (version == POINT_VERSION || version == V4_VERSION || version == VERSION) {
    serialization::readPod(file, count);
    return true;
  }

  return false;
}
}  // namespace

BookmarkStore BookmarkStore::instance;

bool BookmarkStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub" && bookType != "xtc" && bookType != "txt") {
    LOG_ERR("BKS", "Unknown book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  bookmarks.clear();
  if (bookmarks.capacity() < INITIAL_BOOKMARK_RESERVE) {
    bookmarks.reserve(INITIAL_BOOKMARK_RESERVE);
  }

  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  storeFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";

  if (!Storage.exists(storeFilePath.c_str())) {
    LOG_DBG("BKS", "No bookmark file for this book");
    return true;
  }

  return readFromFile();
}

void BookmarkStore::unload() {
  if (dirty) saveToFile();
  bookmarks.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

BookmarkStore::AddResult BookmarkStore::addBookmark(uint16_t spineIndex, float progress, int pageCount,
                                                    const char* chapterTitle) {
  if (pageCount > 0) {
    const float pageSlice = 1.0f / static_cast<float>(pageCount);
    const float pageStart = progress;
    const float pageEnd = progress + pageSlice;
    std::erase_if(bookmarks, [&](const Bookmark& b) {
      return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
    });
  }

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%u) reached", MAX_BOOKMARKS);
    return AddResult::LimitReached;
  }

  Bookmark bm{};
  bm.spineIndex = spineIndex;
  bm.progress = progress;
  bm.timestamp = 0;  // ESP32-C3 has no battery-backed RTC; reserved for future use
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  // Point bookmark: zero-length range with empty preview. The new
  // addHighlight() path populates these for ranged highlights.
  bm.endSpineIndex = spineIndex;
  bm.endProgress = progress;
  bm.startWord = 0;
  bm.endWord = 0;
  bm.preview.clear();

  bookmarks.push_back(bm);
  dirty = true;
  saveToFile();
  return AddResult::Added;
}

BookmarkStore::AddResult BookmarkStore::addHighlight(uint16_t startSpine, float startProgress, uint16_t startWord,
                                                     uint16_t endSpine, float endProgress, uint16_t endWord,
                                                     int startPageCount, const char* chapterTitle,
                                                     const char* preview) {
  // De-dup the same-page-and-spine collision as addBookmark, but only when the
  // range starts on the same physical page as an existing record. This lets
  // users keep multiple highlights per page (different ranges) but still
  // overwrites the legacy single-point bookmark when they upgrade it to a
  // ranged highlight on that page.
  if (startPageCount > 0) {
    const float pageSlice = 1.0f / static_cast<float>(startPageCount);
    const float pageStart = startProgress;
    const float pageEnd = startProgress + pageSlice;
    std::erase_if(bookmarks, [&](const Bookmark& b) {
      return b.spineIndex == startSpine && b.progress >= pageStart && b.progress < pageEnd &&
             b.startWord == 0 && b.endWord == 0 && b.endSpineIndex == b.spineIndex && b.preview.empty();
    });
  }

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%u) reached", MAX_BOOKMARKS);
    return AddResult::LimitReached;
  }

  Bookmark bm{};
  bm.spineIndex = startSpine;
  bm.progress = startProgress;
  bm.timestamp = 0;
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  bm.endSpineIndex = endSpine;
  bm.endProgress = endProgress;
  bm.startWord = startWord;
  bm.endWord = endWord;
  // Cap the in-memory preview at BOOKMARK_PREVIEW_MAX-1 chars; the on-disk
  // write path will zero-pad to the full slot.
  if (preview) {
    bm.preview = preview;
    if (bm.preview.size() > BOOKMARK_PREVIEW_MAX - 1) {
      bm.preview.resize(BOOKMARK_PREVIEW_MAX - 1);
    }
  } else {
    bm.preview.clear();
  }

  bookmarks.push_back(bm);
  dirty = true;
  saveToFile();
  return AddResult::Added;
}

void BookmarkStore::removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
  if (it == bookmarks.end()) return;

  bookmarks.erase(it);
  dirty = true;
  saveToFile();
}

bool BookmarkStore::removeBookmarkAt(size_t index) {
  if (index >= bookmarks.size()) return false;

  bookmarks.erase(bookmarks.begin() + index);
  dirty = true;
  saveToFile();
  return true;
}

bool BookmarkStore::hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
}

void BookmarkStore::saveToFile() {
  if (!dirty || storeFilePath.empty()) return;
  if (bookmarks.empty()) {
    if (Storage.exists(storeFilePath.c_str())) Storage.remove(storeFilePath.c_str());
    dirty = false;
    return;
  }
  if (writeToFile()) dirty = false;
}

void BookmarkStore::clearAll() {
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    if (!Storage.remove(storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to delete bookmark file");
      return;
    }
    LOG_DBG("BKS", "Bookmark file deleted");
  }
  bookmarks.clear();
  dirty = false;
}

bool BookmarkStore::readFromFile() {
  FsFile f;
  if (!Storage.openFileForRead("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for read");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != LEGACY_VERSION && version != POINT_VERSION && version != V4_VERSION && version != VERSION) {
    LOG_ERR("BKS", "Unknown bookmark file version: %u", version);
    f.close();
    return false;
  }

  uint16_t count = 0;
  if (!readBookmarkCount(f, version, count)) {
    LOG_ERR("BKS", "Failed to read bookmark count for version %u", version);
    f.close();
    return false;
  }
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark count %u exceeds max, file may be corrupt", count);
    f.close();
    return false;
  }

  std::string tmp;
  serialization::readString(f, tmp);  // title — not validated
  serialization::readString(f, tmp);  // author — not validated
  std::string storedPath;
  serialization::readString(f, storedPath);
  if (storedPath != bookFilePath) {
    LOG_ERR("BKS", "Bookmark file path mismatch, file may belong to a different book");
    f.close();
    return false;
  }

  bookmarks.clear();
  bookmarks.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    Bookmark bm{};
    if (f.available() < static_cast<int>(sizeof(bm.spineIndex))) {
      LOG_ERR("BKS", "Bookmark file truncated at spineIndex, record %u", i);
      f.close();
      return false;
    }
    serialization::readPod(f, bm.spineIndex);
    if (f.available() < static_cast<int>(sizeof(bm.progress))) {
      LOG_ERR("BKS", "Bookmark file truncated at progress, record %u", i);
      f.close();
      return false;
    }
    serialization::readPod(f, bm.progress);
    if (f.available() < static_cast<int>(sizeof(bm.timestamp))) {
      LOG_ERR("BKS", "Bookmark file truncated at timestamp, record %u", i);
      f.close();
      return false;
    }
    serialization::readPod(f, bm.timestamp);
    const int chRead = f.read(reinterpret_cast<uint8_t*>(bm.chapterTitle), sizeof(bm.chapterTitle));
    bm.chapterTitle[sizeof(bm.chapterTitle) - 1] = '\0';
    if (chRead != static_cast<int>(sizeof(bm.chapterTitle))) {
      LOG_ERR("BKS", "Bookmark file truncated at chapterTitle, record %u", i);
      f.close();
      return false;
    }

    // V4/V5 additions: end anchor + word indices + preview. Older formats
    // (v2 legacy / v3 point) mirror start->end and leave preview empty;
    // the next saveToFile() will rewrite as v5.
    // V4 vs V5 differ only in the preview field's on-disk size
    // (V4=160 bytes vs V5=1024 bytes); both carry the same v4-shaped
    // end anchor + word indices block first.
    if (version == V4_VERSION || version == VERSION) {
      if (f.available() < static_cast<int>(sizeof(bm.endSpineIndex) + sizeof(bm.endProgress) +
                                            sizeof(bm.startWord) + sizeof(bm.endWord))) {
        LOG_ERR("BKS", "Bookmark file truncated at v4/v5 range fields, record %u", i);
        f.close();
        return false;
      }
      serialization::readPod(f, bm.endSpineIndex);
      serialization::readPod(f, bm.endProgress);
      serialization::readPod(f, bm.startWord);
      serialization::readPod(f, bm.endWord);

      // CrumBLE 4.2.1: read the on-disk preview slot into a stack scratch
      // buffer, then assign into bm.preview as a std::string sized to the
      // actual NUL-terminated content. This avoids holding 1024 zero-padded
      // bytes per Bookmark in RAM when the typical highlight is ~100-400
      // chars; the bookmark-list activity used to OOM at the by-value
      // copy of the bookmarks vector under tight heap.
      const size_t onDiskPreviewSize =
          (version == V4_VERSION) ? BOOKMARK_PREVIEW_MAX_V4 : BOOKMARK_PREVIEW_MAX;
      char scratch[BOOKMARK_PREVIEW_MAX];
      const int prevRead = f.read(reinterpret_cast<uint8_t*>(scratch), onDiskPreviewSize);
      if (prevRead != static_cast<int>(onDiskPreviewSize)) {
        LOG_ERR("BKS", "Bookmark file truncated at v%u preview, record %u", version, i);
        f.close();
        return false;
      }
      // Force NUL at the disk-slot boundary so a corrupt/unterminated slot
      // doesn't run past it. Then assign — std::string finds the NUL itself,
      // so the resulting in-memory size matches the actual content length.
      scratch[onDiskPreviewSize - 1] = '\0';
      bm.preview.assign(scratch);
    } else {
      // v2/v3 migration: degenerate (zero-length) highlight at the point.
      bm.endSpineIndex = bm.spineIndex;
      bm.endProgress = bm.progress;
      bm.startWord = 0;
      bm.endWord = 0;
      bm.preview.clear();
    }

    bookmarks.push_back(bm);
  }

  f.close();
  if (version != VERSION) {
    dirty = true;
    saveToFile();
    LOG_DBG("BKS", "Migrated bookmark file from v%u -> v%u", version, VERSION);
  }
  LOG_DBG("BKS", "Loaded %u bookmark(s)", count);
  return true;
}

bool BookmarkStore::writeToFile() const {
  Storage.mkdir(BOOKMARKS_DIR);

  FsFile f;
  if (!Storage.openFileForWrite("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for write");
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(bookmarks.size());
  serialization::writePod(f, VERSION);
  serialization::writePod(f, count);
  serialization::writeString(f, bookTitle);
  serialization::writeString(f, bookAuthor);
  serialization::writeString(f, bookFilePath);

  for (const auto& bm : bookmarks) {
    serialization::writePod(f, bm.spineIndex);
    serialization::writePod(f, bm.progress);
    serialization::writePod(f, bm.timestamp);
    f.write(reinterpret_cast<const uint8_t*>(bm.chapterTitle), sizeof(bm.chapterTitle));
    // v4: range + word indices + preview
    serialization::writePod(f, bm.endSpineIndex);
    serialization::writePod(f, bm.endProgress);
    serialization::writePod(f, bm.startWord);
    serialization::writePod(f, bm.endWord);
    // CrumBLE 4.2.1: write the preview into the fixed-size on-disk slot
    // (BOOKMARK_PREVIEW_MAX bytes), zero-padded after the actual content.
    // Truncates at MAX-1 chars to leave room for the trailing NUL the
    // reader's std::string::assign(const char*) relies on. Using a stack
    // buffer keeps the heap state predictable across save() calls.
    char slot[BOOKMARK_PREVIEW_MAX] = {0};
    const size_t copyLen = std::min(bm.preview.size(), static_cast<size_t>(BOOKMARK_PREVIEW_MAX - 1));
    if (copyLen > 0) std::memcpy(slot, bm.preview.data(), copyLen);
    f.write(reinterpret_cast<const uint8_t*>(slot), sizeof(slot));
  }

  f.close();
  LOG_DBG("BKS", "Saved %u bookmark(s) (v%u)", count, VERSION);
  return true;
}

void BookmarkStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  const std::string path = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
  if (!Storage.exists(path.c_str())) return;
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("BKS", "Failed to delete bookmark file: %s", path.c_str());
  } else {
    LOG_DBG("BKS", "Deleted bookmark file for: %s", filePath.c_str());
  }
}

bool BookmarkStore::hasAnyBookmarks() {
  if (!Storage.exists(BOOKMARKS_DIR)) return false;
  return !Storage.listFiles(BOOKMARKS_DIR).empty();
}

bool BookmarkStore::getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out) {
  if (!Storage.exists(BOOKMARKS_DIR)) return true;

  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  for (const auto& name : files) {
    const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();

    FsFile f;
    if (!Storage.openFileForRead("BKS", fullPath, f)) continue;

    if (f.available() < static_cast<int>(sizeof(uint8_t))) {
      f.close();
      continue;
    }
    uint8_t version;
    serialization::readPod(f, version);
    if (version != LEGACY_VERSION && version != POINT_VERSION && version != V4_VERSION && version != VERSION) {
      LOG_DBG("BKS", "Skipping bookmark file with unknown version: %s", name.c_str());
      f.close();
      continue;
    }

    if (f.available() < static_cast<int>(version == LEGACY_VERSION ? sizeof(uint8_t) : sizeof(uint16_t))) {
      f.close();
      continue;
    }
    uint16_t count = 0;
    if (!readBookmarkCount(f, version, count)) {
      f.close();
      continue;
    }

    // Reads a length-prefixed string, returning false if the file is truncated.
    auto readCheckedString = [&f](std::string& s) -> bool {
      uint32_t len;
      if (f.available() < static_cast<int>(sizeof(len))) return false;
      serialization::readPod(f, len);
      if (f.available() < static_cast<int>(len)) return false;
      s.resize(len);
      f.read(reinterpret_cast<uint8_t*>(&s[0]), len);
      return true;
    };

    std::string title, author, path;
    if (!readCheckedString(title) || !readCheckedString(author) || !readCheckedString(path)) {
      f.close();
      continue;
    }
    f.close();

    if (path.empty() || count == 0) continue;
    if (!Storage.exists(path.c_str())) continue;

    std::string bookType = "epub";
    const std::string nameStr = name.c_str();
    size_t underscorePos = nameStr.find('_');
    if (underscorePos != std::string::npos) {
      bookType = nameStr.substr(0, underscorePos);
    }

    out.push_back({std::move(title), std::move(author), std::move(path), std::move(bookType), count});
  }

  return true;
}
