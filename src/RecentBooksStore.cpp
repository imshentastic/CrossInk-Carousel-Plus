#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>
#include <unordered_set>

#include "LibraryIndex.h"

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 18;
}  // namespace

RecentBooksStore RecentBooksStore::instance;

std::string normalizeAuthorMeta(std::string s) {
  // EPUBs commonly leave dangling ";" or ", ;" in the author field (an
  // OPF separator with no value after it). Strip both ends of any
  // whitespace + separator runs so consumers can treat a "Foo Bar;" the
  // same as "Foo Bar" and an empty-after-strip result the same as a real
  // empty author.
  while (!s.empty()) {
    const char c = s.back();
    if (c == ' ' || c == '\t' || c == ';' || c == ',' || c == '\r' || c == '\n') {
      s.pop_back();
    } else {
      break;
    }
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ';' || s[i] == '\r' || s[i] == '\n')) ++i;
  if (i > 0) s.erase(0, i);
  return s;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  addOrUpdateBook(path, title, author, coverBmpPath);
}

void RecentBooksStore::addOrUpdateBook(const std::string& path, const std::string& title, const std::string& rawAuthor,
                                       const std::string& coverBmpPath) {
  // CrumBLE 4.4: normalize the author once at the storage boundary so
  // every downstream display site (Lyra carousel, Minimal theme, file
  // browser long-press subtitle, etc.) reads a clean string without
  // each having to re-strip the EPUB-leftover ";".
  const std::string author = normalizeAuthorMeta(rawAuthor);
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    it->title = title;
    it->author = author;
    it->coverBmpPath = coverBmpPath;
    if (it != recentBooks.begin()) {
      RecentBook book = std::move(*it);
      recentBooks.erase(it);
      recentBooks.insert(recentBooks.begin(), std::move(book));
    }
  } else {
    recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});
    if (recentBooks.size() > MAX_RECENT_BOOKS) {
      recentBooks.resize(MAX_RECENT_BOOKS);
    }
  }
  saveToFile();
}

bool RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& rawAuthor,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  RecentBook& book = *it;
  book.title = title;
  book.author = normalizeAuthorMeta(rawAuthor);
  book.coverBmpPath = coverBmpPath;
  saveToFile();
  return true;
}

bool RecentBooksStore::removeBook(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  saveToFile();
  return true;
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), normalizeAuthorMeta(epub.getAuthor()), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), normalizeAuthorMeta(xtc.getAuthor()), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(RECENT_BOOKS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      saveToFile();
      Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
      LOG_DBG("RBS", "Migrated recent.bin to recent.json");
      return true;
    }
  }

  return false;
}

int RecentBooksStore::healFromStats(const std::function<void(int)>& onProgress) {
  // Existing entries — never add a duplicate path.
  std::unordered_set<std::string> existing;
  existing.reserve(recentBooks.size());
  for (const auto& b : recentBooks) existing.insert(b.path);

  const int slotsAvail = MAX_RECENT_BOOKS - static_cast<int>(recentBooks.size());
  if (slotsAvail <= 0) return 0;

  // Snapshot LibraryIndex. Caller is expected to have ensureWalked() it
  // already (on HomeActivity::onEnter, the virtual-collection access
  // path normally already triggered a walk this boot). Walking from
  // here would risk a long pause during heal.
  const auto allPaths = LibraryIndex::getInstance().getAllBookPaths();
  if (allPaths.empty()) return 0;

  // Candidates: paths with stats.bin that aren't in recents already.
  // Each gets the stats.bin's modify-time key so we can sort newest-first.
  struct Candidate {
    std::string path;
    uint32_t mtime = 0;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(16);

  for (const auto& path : allPaths) {
    if (existing.count(path)) continue;
    std::string cachePath;
    if (FsHelpers::hasEpubExtension(path)) {
      cachePath = Epub::cachePathForFilePath(path, "/.crosspoint");
    } else if (FsHelpers::hasXtcExtension(path)) {
      // Mirrors Xtc(filepath, cacheDir) constructor's path derivation.
      cachePath = std::string("/.crosspoint/xtc_") + std::to_string(std::hash<std::string>{}(path));
    } else {
      continue;  // TXT / Markdown have no stats.bin sidecar
    }
    const std::string statsPath = cachePath + "/stats.bin";
    if (!Storage.exists(statsPath.c_str())) continue;

    uint32_t mtime = 0;
#ifndef SIMULATOR
    // Device HalFile exposes FAT modify-time as a sortable key. The
    // simulator's HalFile (vendored library) doesn't, and the timestamp
    // sort is a nice-to-have, not a correctness requirement -- fall back
    // to LibraryIndex iteration order on the sim.
    FsFile f;
    if (Storage.openFileForRead("RBS", statsPath, f)) {
      mtime = f.getModifyTimeKey();
      f.close();
    }
#endif
    candidates.push_back({path, mtime});
  }

  if (candidates.empty()) return 0;

  // Newest stats.bin first. Books opened more recently were more
  // recently touched; this is the best proxy for "should be near the top
  // of the carousel" we have without a real recents log.
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.mtime > b.mtime; });

  // Cap to remaining slots so we don't load metadata for books we'll
  // never use.
  if (static_cast<int>(candidates.size()) > slotsAvail) {
    candidates.resize(slotsAvail);
  }

  const int total = static_cast<int>(candidates.size());
  int added = 0;
  for (int i = 0; i < total; ++i) {
    if (onProgress) onProgress((i * 100) / total);
    RecentBook book = getDataFromBook(candidates[i].path);
    if (book.title.empty()) continue;  // omit entries whose metadata load failed
    recentBooks.push_back(book);
    added++;
  }
  if (onProgress) onProgress(100);

  if (added > 0) {
    LOG_INF("RBS", "Heal: added %d books from stats.bin sidecars", added);
    saveToFile();
  }
  return added;
}

bool RecentBooksStore::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    // Old version, just read paths
    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
      std::string path;
      serialization::readString(inputFile, path);

      // load book to get missing data
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        // Fall back to loading what we can from the store
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        recentBooks.push_back({path, title, author, ""});
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == 3) {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);
    uint8_t omitted = 0;

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);

      // Omit books with missing title (e.g. saved before metadata was available)
      if (title.empty()) {
        omitted++;
        continue;
      }

      recentBooks.push_back({path, title, author, coverBmpPath});
    }

    if (omitted > 0) {
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
