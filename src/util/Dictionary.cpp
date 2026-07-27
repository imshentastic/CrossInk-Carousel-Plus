#include "Dictionary.h"

#include <Arduino.h>  // millis()
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstring>

// v18.9.9.44 (task #29): StarDict files can live at any of these
// locations. Search order tries SD root first (legacy default), then
// /dict/, then /dictionary/. First location where BOTH .dict and .idx
// exist wins. Resolution is cached in resolvedDictBase_ so repeated
// exists() / open() calls don't re-scan.
// v18.9.9.202: reverted v199's enumeration + SETTINGS.dictionaryPath picker
// to restore the ~150 bytes of static heap it cost. On X3 the picker's
// settings-view-cache growth + std::string holder pushed contiguous-heap
// past the BT-enable floor. Multi-dict support returns to "rename your
// preferred dict to dictionary.dict; put it in /, /dict/, or /dictionary/".
//
// v18.9.9.224: sidecar format bump. The old dict_idx.cache stored
// [totalWords][offsets...] with no header or version. New .qidx has a
// 24-byte header (magic QIDX, version, interval, totalWords, indexed
// idxSize for staleness detection). Existing .cache files are left
// orphaned on SD and a fresh .qidx is built next time. Path CHANGED
// from dict_idx.cache to dict_idx.qidx so a downgrade to v223 doesn't
// try to parse the new format as old.
//
// v18.9.9.259: per-dict cache. The default path below is the legacy
// single-dict cache; when the user picks a specific dict via the
// picker (setActive), the cache path becomes
// /.crosspoint/dict_idx_<hash>.qidx where <hash> is a 32-bit fnv hash
// of the dict file path -- so switching dicts doesn't invalidate
// caches for previously-picked ones. Access via activeCachePath()
// which uses s_activeDict.dictPath when set.
static const char* CACHE_FILE_DEFAULT = "/.crosspoint/dict_idx.qidx";
static const char* ACTIVE_DICT_SIDECAR = "/.crosspoint/dict_active.txt";
static constexpr uint32_t QIDX_MAGIC = 0x58444951u;  // "QIDX" LE
static constexpr uint8_t QIDX_VERSION = 1;
static constexpr size_t QIDX_HEADER_SIZE = 24;

namespace {
struct DictBase {
  const char* dictPath;
  const char* idxPath;
};
constexpr DictBase kDictCandidates[] = {
    {"/dictionary.dict", "/dictionary.idx"},
    {"/dict/dictionary.dict", "/dict/dictionary.idx"},
    {"/dictionary/dictionary.dict", "/dictionary/dictionary.idx"},
    {"/dictionaries/dictionary.dict", "/dictionaries/dictionary.idx"},
};

const DictBase* resolvedDictBase_ = nullptr;

// v18.9.9.259: runtime-settable active dict (populated by
// Dictionary::setActive from the picker). Storage-wise: two std::strings
// on the heap. The DictBase wrapper points into their c_str() and stays
// valid as long as no one calls setActive again mid-lookup (single
// call-site: DictionaryPickerActivity result handler, invoked from
// EpubReaderActivity's Lookup entry -- no concurrent lookup possible).
struct ActiveDictStorage {
  std::string dictPath;
  std::string idxPath;
  DictBase base = {nullptr, nullptr};
  bool set = false;
};
ActiveDictStorage s_activeDict;

// fnv1a-32 hash for building per-dict cache filenames. Same algorithm
// used in Epub::cachePathForFilePath; small + deterministic.
uint32_t fnv1a32(const char* s) {
  uint32_t h = 0x811C9DC5u;
  while (*s) {
    h ^= static_cast<uint32_t>(*s++);
    h *= 0x01000193u;
  }
  return h;
}

// Returns the cache path for the currently-active dict. Legacy path when
// no dict has been explicitly set (matches pre-v259 behaviour). Per-dict
// path once setActive has been called or the sidecar restored a previous
// pick. Small heap alloc per call (a std::string) -- callers convert to
// c_str() at the storage-API boundary.
std::string activeCachePath() {
  if (!s_activeDict.set || s_activeDict.dictPath.empty()) return std::string(CACHE_FILE_DEFAULT);
  const uint32_t h = fnv1a32(s_activeDict.dictPath.c_str());
  char buf[64];
  snprintf(buf, sizeof(buf), "/.crosspoint/dict_idx_%08x.qidx", static_cast<unsigned>(h));
  return std::string(buf);
}

// v18.9.9.247: file-scope HTML strip. Extracted from the lambda in
// lookupAll so readDefinitionRange can reuse it without duplicating
// the whole state machine. Modifies s in place, deletes-only (write <=
// read always), shrinks with s.resize.
void stripHtmlInPlace(std::string& s) {
  if (s.empty()) return;
  const size_t n = s.size();
  size_t write = 0;
  bool inTag = false;
  bool lastWasSpace = false;
  size_t read = 0;
  while (read < n) {
    const char c = s[read];
    if (inTag) {
      if (c == '>') inTag = false;
      ++read;
      continue;
    }
    if (c == '<') { inTag = true; ++read; continue; }
    if (c == '&') {
      const size_t semi = s.find(';', read + 1);
      if (semi != std::string::npos && semi - read <= 8) {
        const size_t entLen = semi - read - 1;
        char decoded = 0;
        auto entMatches = [&](const char* name, size_t len) {
          if (entLen != len) return false;
          for (size_t k = 0; k < len; ++k)
            if (s[read + 1 + k] != name[k]) return false;
          return true;
        };
        if (entMatches("amp", 3)) decoded = '&';
        else if (entMatches("lt", 2)) decoded = '<';
        else if (entMatches("gt", 2)) decoded = '>';
        else if (entMatches("quot", 4)) decoded = '"';
        else if (entMatches("apos", 4)) decoded = '\'';
        else if (entMatches("nbsp", 4)) decoded = ' ';
        else if (entLen > 0 && s[read + 1] == '#') {
          int v = 0;
          bool ok = entLen > 1;
          for (size_t k = read + 2; k < semi && ok; ++k) {
            const char d = s[k];
            if (d < '0' || d > '9') { ok = false; break; }
            v = v * 10 + (d - '0');
          }
          if (ok && v > 0 && v < 128) decoded = static_cast<char>(v);
        }
        if (decoded != 0) {
          if (decoded == ' ') {
            if (!lastWasSpace) { s[write++] = ' '; lastWasSpace = true; }
          } else {
            s[write++] = decoded;
            lastWasSpace = false;
          }
          read = semi + 1;
          continue;
        }
      }
      ++read;
      continue;
    }
    if (c == '\r') { ++read; continue; }
    if (c == '\n' || c == '\t' || c == ' ') {
      if (!lastWasSpace) {
        s[write++] = (c == '\n' ? '\n' : ' ');
        lastWasSpace = (c != '\n');
      }
      ++read;
      continue;
    }
    {
      const unsigned char ub = static_cast<unsigned char>(c);
      if (ub == 0xCC && read + 1 < n) {
        const unsigned char nb = static_cast<unsigned char>(s[read + 1]);
        if (nb >= 0x80 && nb <= 0xBF) { read += 2; continue; }
      } else if (ub == 0xCD && read + 1 < n) {
        const unsigned char nb = static_cast<unsigned char>(s[read + 1]);
        if (nb >= 0x80 && nb <= 0xAF) { read += 2; continue; }
      }
    }
    s[write++] = c;
    lastWasSpace = false;
    ++read;
  }
  s.resize(write);
}


const DictBase* resolveDictBase() {
  // v18.9.9.259: runtime override wins. Set by Dictionary::setActive
  // (called from DictionaryPickerActivity or the sidecar-restore path
  // on first lookup after boot).
  if (s_activeDict.set) return &s_activeDict.base;
  if (resolvedDictBase_) return resolvedDictBase_;
  for (const auto& cand : kDictCandidates) {
    if (Storage.exists(cand.dictPath) && Storage.exists(cand.idxPath)) {
      resolvedDictBase_ = &cand;
      LOG_INF("DICT", "Resolved StarDict location: %s + %s", cand.dictPath, cand.idxPath);
      return resolvedDictBase_;
    }
  }
  return nullptr;
}
}  // namespace


// Static member initialization
uint32_t Dictionary::totalWords_ = 0;
uint32_t Dictionary::indexedIdxSize_ = 0;
bool Dictionary::indexLoaded_ = false;

// v18.9.9.237: transient flag set by readDefinition's pre-flight guard so
// callers can distinguish "not in dict" from "refused for memory."
static bool s_lastRefusedDueToMemory = false;
bool Dictionary::wasLastRefusedDueToMemory() { return s_lastRefusedDueToMemory; }
void Dictionary::clearLastRefusedDueToMemory() { s_lastRefusedDueToMemory = false; }

// Helper to convert Big-Endian uint32 to ESP32 Little-Endian
static uint32_t swap32(uint32_t val) {
  return ((val << 24) & 0xFF000000) | ((val << 8) & 0x00FF0000) | ((val >> 8) & 0x0000FF00) |
         ((val >> 24) & 0x000000FF);
}

bool Dictionary::exists() {
  // v18.9.9.264: fall back to discoverAll() when the legacy hardcoded
  // name lookup misses. Pre-v259 the only valid names were
  // {/,/dict/,/dictionary/}/dictionary.{dict,idx}; multi-dict support
  // (v259) allows arbitrary basenames under /dict/ and /dictionary/,
  // but this exists() gate was still checking only the hardcoded list,
  // so a user with e.g. /dict/wiktionary.dict + wiktionary.idx would
  // hit "Dictionary not installed" in the Lookup pre-check before
  // discoverAll ran. discoverAll is cheap (walks 2 small folders +
  // a couple stat probes at root) so calling it here is fine.
  if (resolveDictBase() != nullptr) return true;
  return !discoverAll().empty();
}

// v18.9.9.259: multi-dict discovery + active-pointer persistence.

namespace {
// Try to derive a paired .idx path from a .dict path: replace ".dict"
// with ".idx". Returns empty string if the input doesn't end in .dict.
std::string idxPathFromDict(const std::string& dictPath) {
  constexpr const char* kSuffix = ".dict";
  constexpr size_t kSuffixLen = 5;
  if (dictPath.size() <= kSuffixLen) return {};
  const size_t off = dictPath.size() - kSuffixLen;
  for (size_t i = 0; i < kSuffixLen; ++i) {
    char a = dictPath[off + i];
    char b = kSuffix[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (a != b) return {};
  }
  std::string idx = dictPath.substr(0, off);
  idx += ".idx";
  return idx;
}

// Extract "wiktionary" from "/dict/wiktionary.dict" for a display label.
std::string basenameNoExt(const std::string& path) {
  size_t slash = path.find_last_of('/');
  size_t start = (slash == std::string::npos) ? 0 : slash + 1;
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot < start) return path.substr(start);
  return path.substr(start, dot - start);
}

// Walk the fixed candidate folders (root, /dict/, /dictionary/) and
// accumulate every *.dict path with a paired *.idx into `out`. Skips
// dotfiles. Bounded (three small folders) -- no risk of deep recursion.
void enumerateDictFolder(const char* folder, std::vector<Dictionary::DictInfo>& out) {
  FsFile dir = Storage.open(folder);
  bool dirOk = (bool)dir && dir.isDirectory();
  if (!dirOk) {
    // v18.9.9.263: fallback with a trailing slash. Some SDFat/HalStorage
    // combos treat a folder path without a trailing slash as a phantom
    // file entry (opens successfully but isDirectory() returns false).
    // Field report: /dict returned false while /dict/ worked. Both
    // strings are tried before giving up.
    if (dir) dir.close();
    std::string withSlash(folder);
    if (withSlash.empty() || withSlash.back() != '/') withSlash += '/';
    dir = Storage.open(withSlash.c_str());
    dirOk = (bool)dir && dir.isDirectory();
    if (!dirOk) {
      LOG_INF("DICT", "discoverAll: %s not a directory (also tried %s)",
              folder, withSlash.c_str());
      if (dir) dir.close();
      return;
    }
    LOG_INF("DICT", "discoverAll: %s opened with trailing slash", withSlash.c_str());
  }
  // v18.9.9.261: dot-index the dir iteration up front so nested
  // Storage.exists() calls (for the .idx probe) don't interfere with
  // the dir handle. Observed: /dict enumerator was returning zero
  // matches for the user's real files even though the .dict/.idx pairs
  // were present -- the SDFat dir iterator gets confused when a fresh
  // Storage.exists() opens a file mid-iteration on the same volume.
  // Two-pass: pass 1 grabs every filename, pass 2 filters + resolves.
  std::vector<std::string> filenames;
  filenames.reserve(8);
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDir = file.isDirectory();
    file.getName(name, sizeof(name));
    file.close();
    if (isDir) continue;
    filenames.emplace_back(name);
  }
  dir.close();
  LOG_INF("DICT", "discoverAll: %s has %u entries", folder, (unsigned)filenames.size());
  for (const auto& filename : filenames) {
    LOG_INF("DICT", "discoverAll: entry: '%s' (%u chars)", filename.c_str(), (unsigned)filename.size());
    if (filename.empty() || filename[0] == '.') continue;
    const std::string dictPath = std::string(folder) + (folder[strlen(folder) - 1] == '/' ? "" : "/") + filename;
    const std::string idxPath = idxPathFromDict(dictPath);
    if (idxPath.empty()) {
      LOG_DBG("DICT", "discoverAll: skip %s (not .dict)", dictPath.c_str());
      continue;
    }
    if (!Storage.exists(idxPath.c_str())) {
      LOG_INF("DICT", "discoverAll: skip %s (missing pair %s)", dictPath.c_str(), idxPath.c_str());
      continue;
    }
    Dictionary::DictInfo info;
    info.dictPath = dictPath;
    info.idxPath = idxPath;
    info.displayName = basenameNoExt(dictPath);
    LOG_INF("DICT", "discoverAll: found %s -> %s (name=%s)", info.dictPath.c_str(),
            info.idxPath.c_str(), info.displayName.c_str());
    out.push_back(std::move(info));
  }
}
}  // namespace

std::vector<Dictionary::DictInfo> Dictionary::discoverAll() {
  std::vector<DictInfo> out;
  out.reserve(4);
  // Root: only the legacy dictionary.dict/idx pair (not arbitrary root
  // .dict files -- would false-positive on backups). Then the dedicated
  // folders for user-added dicts.
  if (Storage.exists("/dictionary.dict") && Storage.exists("/dictionary.idx")) {
    DictInfo root;
    root.dictPath = "/dictionary.dict";
    root.idxPath = "/dictionary.idx";
    root.displayName = "dictionary";
    out.push_back(std::move(root));
  }
  enumerateDictFolder("/dict", out);
  enumerateDictFolder("/dictionary", out);
  enumerateDictFolder("/dictionaries", out);  // casper-ecosystem convention
  return out;
}

void Dictionary::setActive(const DictInfo& dict) {
  if (dict.dictPath.empty() || dict.idxPath.empty()) {
    // Clear override.
    s_activeDict.set = false;
    s_activeDict.dictPath.clear();
    s_activeDict.idxPath.clear();
    Storage.remove(ACTIVE_DICT_SIDECAR);
    LOG_INF("DICT", "setActive: cleared active dict");
    return;
  }
  s_activeDict.dictPath = dict.dictPath;
  s_activeDict.idxPath = dict.idxPath;
  s_activeDict.base.dictPath = s_activeDict.dictPath.c_str();
  s_activeDict.base.idxPath = s_activeDict.idxPath.c_str();
  s_activeDict.set = true;
  // Also drop the "resolved" auto-discovery cache -- next resolveDictBase
  // returns our override, not the previous auto-picked one.
  resolvedDictBase_ = nullptr;
  // Persist path to sidecar so future sessions restore without a picker.
  // Tiny file, no heap gate needed.
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (Storage.openFileForWrite("DICT", ACTIVE_DICT_SIDECAR, f)) {
    f.write(reinterpret_cast<const uint8_t*>(dict.dictPath.c_str()), dict.dictPath.size());
    f.close();
  }
  // v18.9.9.259: also drop the cached loaded-index state so the next
  // lookup re-reads from the new dict's .qidx (not the previous dict's).
  indexLoaded_ = false;
  totalWords_ = 0;
  indexedIdxSize_ = 0;
  LOG_INF("DICT", "setActive: %s + %s", dict.dictPath.c_str(), dict.idxPath.c_str());
}

Dictionary::DictInfo Dictionary::getActive() {
  DictInfo out;
  // If already loaded this session, return in-memory copy.
  if (s_activeDict.set) {
    out.dictPath = s_activeDict.dictPath;
    out.idxPath = s_activeDict.idxPath;
    out.displayName = basenameNoExt(out.dictPath);
    return out;
  }
  // Otherwise consult the sidecar. Might be a fresh-boot recovery.
  FsFile f;
  if (!Storage.openFileForRead("DICT", ACTIVE_DICT_SIDECAR, f)) return out;
  char buf[256];
  const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  f.close();
  if (n == 0) return out;
  buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
  // Trim any trailing newline.
  std::string dictPath(buf);
  while (!dictPath.empty() && (dictPath.back() == '\n' || dictPath.back() == '\r' || dictPath.back() == ' ')) {
    dictPath.pop_back();
  }
  if (dictPath.empty()) return out;
  const std::string idxPath = idxPathFromDict(dictPath);
  if (idxPath.empty() || !Storage.exists(dictPath.c_str()) || !Storage.exists(idxPath.c_str())) {
    // Stale sidecar -- pointed dict no longer on SD. Clear it.
    Storage.remove(ACTIVE_DICT_SIDECAR);
    return out;
  }
  out.dictPath = dictPath;
  out.idxPath = idxPath;
  out.displayName = basenameNoExt(dictPath);
  return out;
}

bool Dictionary::isIndexReady() { return indexLoaded_; }

bool Dictionary::hasCachedIndex() { return Storage.exists(activeCachePath().c_str()); }

int Dictionary::qidxCount() {
  return (totalWords_ + SPARSE_INTERVAL - 1) / SPARSE_INTERVAL;
}

// v18.9.9.224: read a single offset entry from the sidecar. Each read is
// (open, seek, read 4 bytes, close). Slower than array indexing but
// eliminates the RAM cost of the offset table. Binary search calls this
// log2(N) times per lookup (~8 reads for a 100k-word dict).
bool Dictionary::readQidxOffset(int sparseIndex, uint32_t* outOffset) {
  if (outOffset) *outOffset = 0;
  if (!outOffset || sparseIndex < 0 || sparseIndex >= qidxCount()) return false;
  FsFile f;
  if (!Storage.openFileForRead("DICT", activeCachePath().c_str(), f)) return false;
  const uint32_t byteOffset = QIDX_HEADER_SIZE + static_cast<uint32_t>(sparseIndex) * sizeof(uint32_t);
  f.seek(byteOffset);
  uint32_t v = 0;
  const size_t n = f.read(reinterpret_cast<uint8_t*>(&v), sizeof(v));
  f.close();
  if (n != sizeof(v)) return false;
  *outOffset = v;
  return true;
}

// Ported from casper's lookup fallback chain (MIT). Guards: short stubs
// only (real definitions that merely CONTAIN "a type of fish" don't
// match the marker list, and long text bails early); the base is the
// text after the LAST standalone " of " so "past participle of go"
// resolves to "go".
bool Dictionary::extractFormOfBase(const std::string& def, std::string& outBase) {
  if (def.empty() || def.size() > 120) return false;
  static const char* kMarkers[] = {
      "plural of ",      "participle of ", "gerund of ",     "past of ",
      "form of ",        "tense of ",      "indicative of ", "comparative of ",
      "superlative of ", "singular of ",   "adverb of ",
  };
  bool looksFormOf = false;
  for (const char* m : kMarkers) {
    if (def.find(m) != std::string::npos) {
      looksFormOf = true;
      break;
    }
  }
  if (!looksFormOf) return false;
  size_t ofEnd = std::string::npos;
  for (size_t p = def.find(" of "); p != std::string::npos; p = def.find(" of ", p + 1)) {
    ofEnd = p + 4;
  }
  if (ofEnd == std::string::npos || ofEnd >= def.size()) return false;
  std::string base = def.substr(ofEnd);
  const size_t cut = base.find_first_of(";.,([|/\n");
  if (cut != std::string::npos) base.resize(cut);
  base = cleanWord(base);
  if (base.size() < 2 || base.size() > 40) return false;
  outBase = std::move(base);
  return true;
}

// Accent folding: Latin diacritics transliterate to their base letters
// instead of being deleted (the old isalpha() filter dropped the UTF-8
// bytes entirely, so "café" became "caf" and could never match an
// unaccented dictionary's "cafe"). Folding runs on BOTH the query and
// the .idx headwords during search, so accented book text matches
// unaccented dictionaries and vice versa; headwords differing only by
// accent normalize to one key and surface as multiple entries, which
// the definition card already cycles. Ordering caveat: the .idx is
// sorted by raw bytes while search compares folded keys -- anomalies
// are local (accented forms sit beside their base alphabetically) and
// absorbed by the SPARSE_INTERVAL linear-scan window, same as the old
// deletion behavior.
std::string Dictionary::cleanWord(const std::string& word) {
  // Base letters for U+00C0-U+00FF (index = codepoint - 0xC0). '.' = drop.
  static const char kLatin1[64 + 1] =
      "aaaaaa.ceeeeiiiidnooooo.ouuuuy.."
      "aaaaaa.ceeeeiiiidnooooo.ouuuuy.y";
  // Base letters for U+0100-U+017F (index = codepoint - 0x100).
  static const char kLatinExtA[128 + 1] =
      "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiijjjjkkkllllllllll"
      "nnnnnnnnnoooooo..rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
  std::string clean;
  clean.reserve(word.size());
  size_t i = 0;
  const size_t n = word.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(word[i]);
    if (c < 0x80) {
      if (std::isalpha(c)) clean += static_cast<char>(std::tolower(c));
      ++i;
      continue;
    }
    if (i + 1 < n && (c == 0xC3 || c == 0xC4 || c == 0xC5)) {
      const unsigned char c2 = static_cast<unsigned char>(word[i + 1]);
      if (c2 >= 0x80 && c2 <= 0xBF) {
        const uint32_t cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (c2 & 0x3F);
        if (cp == 0xC6 || cp == 0xE6) clean += "ae";        // AE ligature
        else if (cp == 0xDE || cp == 0xFE) clean += "th";   // thorn
        else if (cp == 0xDF) clean += "ss";                 // sharp s
        else if (cp == 0x152 || cp == 0x153) clean += "oe"; // OE ligature
        else if (cp == 0x132 || cp == 0x133) clean += "ij"; // IJ digraph
        else if (cp >= 0xC0 && cp <= 0xFF) {
          const char b = kLatin1[cp - 0xC0];
          if (b != '.') clean += b;
        } else if (cp >= 0x100 && cp <= 0x17F) {
          const char b = kLatinExtA[cp - 0x100];
          if (b != '.') clean += b;
        }
        i += 2;
        continue;
      }
    }
    // Other multi-byte sequences (CJK, symbols): skip the whole sequence,
    // matching the old behavior of dropping non-Latin bytes.
    if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else ++i;
  }
  return clean;
}

std::vector<std::string> Dictionary::getStemVariants(const std::string& word) {
  std::vector<std::string> variants;
  if (word.length() < 4) return variants;
  const size_t L = word.length();

  // Simple English stemming heuristics
  if (word.back() == 's') {
    variants.push_back(word.substr(0, L - 1));  // drop 's': "lies" -> "lie"

    // v18.9.9.233: strip "es" ONLY when the base actually forms an -es
    // plural (sibilant / affricate endings: -ses, -xes, -zes, -oes, -shes,
    // -ches). The old unconditional 2-char strip produced false matches
    // like "lies" -> "li" (which lands on an unrelated Chinese "li"
    // entry in many dicts) and "flies" -> "fli".
    if (L >= 4 && word.substr(L - 2) == "es") {
      const char c3 = word[L - 3];
      const bool sibilant = (c3 == 's' || c3 == 'x' || c3 == 'z' || c3 == 'o');
      const bool digraph = L >= 5 && c3 == 'h' && (word[L - 4] == 's' || word[L - 4] == 'c');
      if (sibilant || digraph) {
        variants.push_back(word.substr(0, L - 2));  // "boxes"->"box", "kisses"->"kiss"
      }
    }

    // -ies -> -y: "flies"->"fly", "tries"->"try", "cities"->"city".
    if (L >= 4 && word.substr(L - 3) == "ies") {
      variants.push_back(word.substr(0, L - 3) + "y");
    }
  }
  if (L >= 3 && word.substr(L - 2) == "ed") {
    variants.push_back(word.substr(0, L - 1));  // e.g., baked -> bake
    variants.push_back(word.substr(0, L - 2));  // e.g., started -> start
  }
  if (L >= 4 && word.substr(L - 3) == "ing") {
    variants.push_back(word.substr(0, L - 3));        // e.g., playing -> play
    variants.push_back(word.substr(0, L - 3) + "e");  // e.g., making -> make
    // Doubled final consonant: gagging -> gag, running -> run.
    if (L >= 6 && word[L - 4] == word[L - 5]) {
      variants.push_back(word.substr(0, L - 4));
    }
  }
  // Adverbs (ported from casper's fallback chain, MIT): longest first so
  // -ily/-ably aren't shadowed by the plain -ly strip.
  if (L >= 5 && word.substr(L - 3) == "ily") {
    variants.push_back(word.substr(0, L - 3) + "y");  // happily -> happy
  }
  if (L >= 6 && (word.substr(L - 4) == "ably" || word.substr(L - 4) == "ibly")) {
    variants.push_back(word.substr(0, L - 1) + "e");  // probably -> probable
  }
  if (L >= 5 && word.substr(L - 2) == "ly") {
    variants.push_back(word.substr(0, L - 2));  // obsequiously -> obsequious
  }
  return variants;
}

// v18.9.9.224: read + validate the sidecar header. Populates totalWords_ /
// indexedIdxSize_ so subsequent qidxCount() / readQidxOffset() calls work.
// Also checks staleness -- if the source .idx file's size differs from
// what the sidecar recorded, the sidecar is stale and rebuild is needed.
bool Dictionary::loadCachedIndex() {
  FsFile f;
  if (!Storage.openFileForRead("DICT", activeCachePath().c_str(), f)) return false;
  uint8_t hdr[QIDX_HEADER_SIZE];
  if (f.read(hdr, QIDX_HEADER_SIZE) != QIDX_HEADER_SIZE) { f.close(); return false; }
  f.close();
  auto rdU32 = [](const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  if (rdU32(hdr + 0) != QIDX_MAGIC) return false;
  if (hdr[4] != QIDX_VERSION) return false;
  const uint32_t interval = rdU32(hdr + 8);
  if (interval != SPARSE_INTERVAL) return false;  // format bumped or built by another interval
  totalWords_ = rdU32(hdr + 12);
  indexedIdxSize_ = rdU32(hdr + 16);
  // Staleness: compare recorded idx size against current on-disk size.
  // If the user replaced their dict, this triggers a rebuild.
  const DictBase* base = resolveDictBase();
  if (base) {
    FsFile idxFile;
    if (Storage.openFileForRead("DICT", base->idxPath, idxFile)) {
      const uint32_t currentIdxSize = idxFile.size();
      idxFile.close();
      if (currentIdxSize != indexedIdxSize_) {
        LOG_INF("DICT", "loadCachedIndex: stale sidecar (recorded idxSize=%u, current=%u); rebuild",
                indexedIdxSize_, currentIdxSize);
        return false;
      }
    }
  }
  indexLoaded_ = true;
  return true;
}

std::string Dictionary::readWord(FsFile& file) {
  std::string word;
  char c;
  while (file.read(reinterpret_cast<uint8_t*>(&c), 1) == 1 && c != '\0') {
    word += c;
  }
  return word;
}

bool Dictionary::loadIndex(const std::function<void(int percent)>& onProgress,
                           const std::function<bool()>& shouldCancel) {
  if (loadCachedIndex()) {
    LOG_INF("DICT", "loadIndex: cache hit, returning fast");
    return true;
  }

  const DictBase* base = resolveDictBase();
  if (!base) {
    LOG_ERR("DICT", "loadIndex: no StarDict files found in known locations");
    return false;
  }
  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", base->idxPath, idxFile)) {
    LOG_ERR("DICT", "loadIndex: failed to open %s", base->idxPath);
    return false;
  }

  const uint32_t fileSize = idxFile.size();
  LOG_INF("DICT", "loadIndex: scanning %s (%u bytes)", base->idxPath, fileSize);
  const uint32_t scanStartMs = millis();
  uint32_t currentOffset = 0;
  totalWords_ = 0;
  indexedIdxSize_ = fileSize;

  // v18.9.9.224: stream sample offsets STRAIGHT to the sidecar file as
  // we scan the .idx. Old code accumulated ~800 B per 100k words in a
  // std::vector<uint32_t> then wrote it out at the end -- fine at 100k
  // scale, but this scales linearly with dict size. Now we open the
  // sidecar, write a placeholder header, append each new sample offset
  // as we hit a SPARSE_INTERVAL boundary, and back-patch the real
  // header at the end.
  Storage.mkdir("/.crosspoint");
  // v18.9.9.236: explicit delete before rebuild. openFileForWrite already
  // overwrites, but if a rebuild fails mid-write, the corpse partial
  // sidecar can linger with a valid header pointing at truncated data.
  // Explicit remove signals intent and defends against filesystem
  // quirks that might leak partial state on power loss / retry.
  const std::string cachePath = activeCachePath();
  Storage.remove(cachePath.c_str());
  FsFile qidxFile;
  if (!Storage.openFileForWrite("DICT", cachePath.c_str(), qidxFile)) {
    LOG_ERR("DICT", "loadIndex: failed to open %s for write", cachePath.c_str());
    idxFile.close();
    return false;
  }
  // Placeholder header (zeros) so file bytes 0..QIDX_HEADER_SIZE-1 are
  // reserved and the first sample offset lands at byte 24.
  const uint8_t zeros[QIDX_HEADER_SIZE] = {0};
  qidxFile.write(zeros, QIDX_HEADER_SIZE);

  auto writeSampleOffset = [&qidxFile](uint32_t off) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(off & 0xFFu),
        static_cast<uint8_t>((off >> 8) & 0xFFu),
        static_cast<uint8_t>((off >> 16) & 0xFFu),
        static_cast<uint8_t>((off >> 24) & 0xFFu),
    };
    qidxFile.write(bytes, 4);
  };

  while (currentOffset < fileSize) {
    if (shouldCancel && shouldCancel()) {
      qidxFile.close();
      idxFile.close();
      Storage.remove(activeCachePath().c_str());
      return false;
    }

    if (totalWords_ % SPARSE_INTERVAL == 0) {
      writeSampleOffset(currentOffset);
      if (onProgress) onProgress((currentOffset * 100) / fileSize);
    }
    if ((totalWords_ & 0x3F) == 0) {
      vTaskDelay(1);
      esp_task_wdt_reset();
    }

    // Read word string
    char c;
    while (idxFile.read(reinterpret_cast<uint8_t*>(&c), 1) == 1 && c != '\0') {
      currentOffset++;
    }
    currentOffset++;  // For the '\0'

    // Skip offset and size (2 * 4 bytes)
    idxFile.seek(currentOffset + 8);
    currentOffset += 8;
    totalWords_++;
  }

  idxFile.close();

  // Back-patch the header now that we know totalWords_. Layout matches
  // the read code in loadCachedIndex.
  uint8_t hdr[QIDX_HEADER_SIZE] = {0};
  const uint32_t interval = SPARSE_INTERVAL;
  auto wrU32 = [](uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
  };
  wrU32(hdr + 0, QIDX_MAGIC);
  hdr[4] = QIDX_VERSION;
  wrU32(hdr + 8, interval);
  wrU32(hdr + 12, totalWords_);
  wrU32(hdr + 16, indexedIdxSize_);
  qidxFile.seek(0);
  qidxFile.write(hdr, QIDX_HEADER_SIZE);
  qidxFile.close();

  indexLoaded_ = true;
  const uint32_t scanMs = millis() - scanStartMs;
  LOG_INF("DICT", "loadIndex: scan complete -- %u words, %u ms, sidecar written", totalWords_, scanMs);

  // v18.9.9.236: canary lookup after rebuild. If the user swapped in a
  // compressed .dict.dz renamed to .dict (or any corrupt / mismatched
  // .idx/.dict pair), the scan happily walks the .idx and builds a
  // sidecar, but readDefinition returns garbage because the offsets
  // point into a compressed or invalid data file. Every subsequent
  // lookup silently returns "Word not found," and the user thinks the
  // whole dict is empty. Try a very common word ("the") -- if we can't
  // even find that OR the payload looks like binary garbage, invalidate
  // the sidecar so the user sees an honest failure on the next lookup
  // rather than misleading not-found responses.
  {
    const auto canary = lookupAll("the");
    bool sane = false;
    for (const auto& e : canary) {
      if (e.empty()) continue;
      // Sanity: at least 4 printable-or-whitespace ASCII bytes at start.
      int printable = 0;
      for (size_t k = 0; k < e.size() && k < 16; ++k) {
        const unsigned char c = static_cast<unsigned char>(e[k]);
        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F) || c >= 0x80) printable++;
      }
      if (printable >= 4) { sane = true; break; }
    }
    if (!canary.empty() && !sane) {
      LOG_ERR("DICT", "loadIndex: canary lookup returned unreadable data; invalidating sidecar. "
                     "Dict file may be compressed (.dict.dz) or corrupt.");
      Storage.remove(activeCachePath().c_str());
      indexLoaded_ = false;
      return false;
    }
    // Note: canary empty is FINE -- many dicts just don't have "the" as a
    // headword (Chinese, medical, etc.). Only garbage payloads invalidate.
  }

  if (onProgress) onProgress(100);
  return true;
}

// v18.9.9.247: chunked reader implementation. Reads a byte range from a
// single .dict entry and applies HTML strip to the returned string. Unlike
// readDefinition (which enforces size + 8 KB margin sized for full wrap),
// this uses a smaller 4 KB margin since chunk_size is much less than a
// typical entry (~3 KB vs ~10 KB). Range is clamped to entryTotalSize.
std::string Dictionary::readDefinitionRange(uint32_t entryOffset, uint32_t entryTotalSize,
                                             uint32_t rangeStart, uint32_t rangeSize) {
  if (rangeStart >= entryTotalSize) return "";
  if (rangeStart + rangeSize > entryTotalSize) {
    rangeSize = entryTotalSize - rangeStart;
  }
  if (rangeSize == 0) return "";
  const DictBase* base = resolveDictBase();
  if (!base) return "";
  constexpr uint32_t kChunkMarginBytes = 4u * 1024u;
  const uint32_t needed = rangeSize + kChunkMarginBytes;
  const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
  if (maxAllocNow < needed) {
    LOG_INF("DICT",
            "readDefinitionRange: refusing size=%u -- maxAlloc=%u < %u",
            rangeSize, maxAllocNow, needed);
    s_lastRefusedDueToMemory = true;
    return "";
  }
  FsFile dictFile;
  if (!Storage.openFileForRead("DICT", base->dictPath, dictFile)) return "";
  dictFile.seek(entryOffset + rangeStart);
  std::string content;
  content.resize(rangeSize);
  const size_t got = dictFile.read(reinterpret_cast<uint8_t*>(content.data()), rangeSize);
  dictFile.close();
  if (got != rangeSize) return "";
  // Apply HTML strip (in-place) same as full-entry path.
  stripHtmlInPlace(content);
  return content;
}

// v18.9.9.247: return just entry handles (no data). Parallels lookupAll's
// scan logic but stops before readDefinition. Caller then loads bytes on
// demand via readDefinitionRange.
std::vector<Dictionary::EntryHandle> Dictionary::lookupAllHandles(const std::string& rawWord,
                                                                    const std::function<bool()>& shouldCancel) {
  if (!exists()) return {};
  if (!indexLoaded_ && !loadIndex(nullptr, shouldCancel)) return {};
  std::string targetWord = cleanWord(rawWord);
  if (targetWord.empty()) return {};
  const DictBase* base = resolveDictBase();
  if (!base) return {};
  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", base->idxPath, idxFile)) return {};
  const int qCount = qidxCount();
  int low = 0;
  int high = qCount - 1;
  int closestSparseIdx = 0;
  while (low <= high) {
    int mid = low + (high - low) / 2;
    uint32_t midOff = 0;
    if (!readQidxOffset(mid, &midOff)) { idxFile.close(); return {}; }
    idxFile.seek(midOff);
    std::string currentWord = cleanWord(readWord(idxFile));
    if (currentWord == targetWord) { closestSparseIdx = mid; break; }
    else if (currentWord < targetWord) { closestSparseIdx = mid; low = mid + 1; }
    else { high = mid - 1; }
  }
  uint32_t startOff = 0;
  if (!readQidxOffset(closestSparseIdx, &startOff)) { idxFile.close(); return {}; }
  idxFile.seek(startOff);
  uint32_t chunkEnd = idxFile.size();
  if (closestSparseIdx + 1 < qCount) {
    uint32_t nextOff = 0;
    if (readQidxOffset(closestSparseIdx + 1, &nextOff)) chunkEnd = nextOff;
  }
  constexpr int kMaxDefinitionsPerLookup = 6;
  std::vector<EntryHandle> out;
  out.reserve(2);
  int scanned = 0;
  while (idxFile.position() < chunkEnd) {
    if (shouldCancel && shouldCancel()) break;
    std::string word = readWord(idxFile);
    std::string clean = cleanWord(word);
    uint32_t dataOffset, dataSize;
    idxFile.read(reinterpret_cast<uint8_t*>(&dataOffset), sizeof(uint32_t));
    idxFile.read(reinterpret_cast<uint8_t*>(&dataSize), sizeof(uint32_t));
    dataOffset = swap32(dataOffset);
    dataSize = swap32(dataSize);
    if (clean == targetWord) {
      out.push_back({dataOffset, dataSize});
      if (static_cast<int>(out.size()) >= kMaxDefinitionsPerLookup) break;
    } else if (!out.empty()) {
      break;
    }
    if ((++scanned & 0x3F) == 0) {
      vTaskDelay(1);
      esp_task_wdt_reset();
    }
  }
  idxFile.close();
  return out;
}

std::string Dictionary::readDefinition(uint32_t offset, uint32_t size) {
  const DictBase* base = resolveDictBase();
  if (!base) return "";
  // v18.9.9.222: pre-flight heap guard, ported from CrossPoint
  // feat/slim-dictionary. Refuse definitions we can't allocate CLEANLY
  // (size + 8 KB working margin for wrap + overlay allocations). We
  // build with -fno-exceptions, so a std::string::resize that can't
  // grow terminates the whole program -- explicit refusal here becomes
  // a graceful "no definition" instead of a crash-to-Home. Runs BEFORE
  // opening the dict file so we don't pay the SD open cost.
  constexpr uint32_t kWorkingMarginBytes = 8u * 1024u;
  const uint32_t needed = size + kWorkingMarginBytes;
  const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
  if (maxAllocNow < needed) {
    LOG_INF("DICT",
            "readDefinition: refusing size=%u -- maxAlloc=%u < %u (size + %u KB margin)",
            size, maxAllocNow, needed, static_cast<unsigned>(kWorkingMarginBytes / 1024u));
    // v18.9.9.237: signal memory-refusal so the caller can arm auto-recover
    // instead of showing a misleading "Word not found" popup.
    s_lastRefusedDueToMemory = true;
    return "";
  }
  FsFile dictFile;
  if (!Storage.openFileForRead("DICT", base->dictPath, dictFile)) return "";

  dictFile.seek(offset);
  std::string definition;
  definition.resize(size);

  if (dictFile.read(reinterpret_cast<uint8_t*>(definition.data()), size) == size) {
    dictFile.close();
    return definition;
  }

  dictFile.close();
  return "";
}

std::string Dictionary::lookup(const std::string& rawWord, const std::function<void(int percent)>& onProgress,
                               const std::function<bool()>& shouldCancel) {
  // v18.9.9.198: thin wrapper — return first entry only. lookupAll is the
  // real workhorse; existing callers that show one string keep working.
  const auto all = lookupAll(rawWord, onProgress, shouldCancel);
  return all.empty() ? std::string() : all.front();
}

std::vector<std::string> Dictionary::lookupAll(const std::string& rawWord,
                                               const std::function<void(int percent)>& onProgress,
                                               const std::function<bool()>& shouldCancel) {
  if (!exists()) return {};
  if (!indexLoaded_ && !loadIndex(onProgress, shouldCancel)) return {};

  std::string targetWord = cleanWord(rawWord);
  if (targetWord.empty()) return {};

  const DictBase* base = resolveDictBase();
  if (!base) return {};
  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", base->idxPath, idxFile)) return {};

  // v18.9.9.224: binary search now uses the sidecar (readQidxOffset) --
  // each mid-point access is a small seek+read against .qidx rather
  // than a RAM array index. Log2(N) iterations for ~N/256 sample entries.
  const int qCount = qidxCount();
  int low = 0;
  int high = qCount - 1;
  int closestSparseIdx = 0;

  while (low <= high) {
    int mid = low + (high - low) / 2;
    uint32_t midOff = 0;
    if (!readQidxOffset(mid, &midOff)) { idxFile.close(); return {}; }
    idxFile.seek(midOff);
    std::string currentWord = cleanWord(readWord(idxFile));

    if (currentWord == targetWord) {
      closestSparseIdx = mid;
      break;
    } else if (currentWord < targetWord) {
      closestSparseIdx = mid;  // Might be in this chunk
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  // Linear scan within the identified chunk
  uint32_t startOff = 0;
  if (!readQidxOffset(closestSparseIdx, &startOff)) { idxFile.close(); return {}; }
  idxFile.seek(startOff);
  uint32_t chunkEnd = idxFile.size();
  if (closestSparseIdx + 1 < qCount) {
    uint32_t nextOff = 0;
    if (readQidxOffset(closestSparseIdx + 1, &nextOff)) chunkEnd = nextOff;
  }

  // v18.9.9.197: collect ALL matching entries in the chunk, not just the
  // first. StarDict .idx files commonly hold multiple records that
  // cleanWord() normalizes to the same key (e.g. "Quiver" the proper noun
  // + "quiver" the common noun -> both target "quiver"). ColorDict shows
  // all; CrumBLE used to stop at the first. Cap at kMaxDefinitionsPerLookup
  // to bound heap under BT-tight conditions; typical bilingual dicts have
  // 2-4 entries per common word, so 6 is safe and inclusive.
  constexpr int kMaxDefinitionsPerLookup = 6;
  struct MatchRef { uint32_t offset; uint32_t size; };
  std::vector<MatchRef> matches;
  matches.reserve(2);

  int scanned = 0;
  while (idxFile.position() < chunkEnd) {
    if (shouldCancel && shouldCancel()) break;

    std::string word = readWord(idxFile);
    std::string clean = cleanWord(word);

    uint32_t dataOffset, dataSize;
    idxFile.read(reinterpret_cast<uint8_t*>(&dataOffset), sizeof(uint32_t));
    idxFile.read(reinterpret_cast<uint8_t*>(&dataSize), sizeof(uint32_t));

    // Convert from Big-Endian to Little-Endian
    dataOffset = swap32(dataOffset);
    dataSize = swap32(dataSize);

    if (clean == targetWord) {
      matches.push_back({dataOffset, dataSize});
      if (static_cast<int>(matches.size()) >= kMaxDefinitionsPerLookup) break;
    } else if (!matches.empty()) {
      // .idx is lexicographically sorted -- once we've matched and then
      // hit a non-match, no more matches lie ahead in this chunk. Stop
      // scanning to save SD reads.
      break;
    }

    // CrumBLE: chunk has up to SPARSE_INTERVAL (512) words; on a slow SD
    // that's enough byte-by-byte reads to trip the task WDT in the worst
    // case. Feed it every 64 iterations.
    if ((++scanned & 0x3F) == 0) {
      vTaskDelay(1);
      esp_task_wdt_reset();
    }
  }

  idxFile.close();

  // v18.9.9.198: read each match into its own entry. Empty payloads are
  // skipped but their slot isn't reserved -- caller just gets fewer entries.
  std::vector<std::string> out;
  out.reserve(matches.size());
  for (const auto& m : matches) {
    std::string d = readDefinition(m.offset, m.size);
    if (!d.empty()) out.push_back(std::move(d));
  }
  // v18.9.9.222: HTML strip, ported from CrossPoint. Many public StarDict
  // dumps (esp. Wiktionary derivatives) ship HTML in the definition
  // payload. Without this we render raw `<b>...</b>` and `<br/>` as
  // literal text in the popup.
  //
  // v18.9.9.236: rewritten as TRULY in-place. v222's version claimed
  // "no extra allocation" in the comment but actually allocated
  // dst.reserve(s.size()) -- a 2x-transient spike that was the leading
  // suspect for the v233/234 second-lookup terminate at maxAlloc=8180
  // on the user's 10 KB-entry dict. HTML strip only DELETES chars
  // (tags collapse, entities shrink to 1 byte), never inserts more, so
  // write-pointer <= read-pointer always holds and modifying the source
  // std::string in place is safe. Peak transient allocation now: 0.
  auto stripInPlace = [](std::string& s) {
    if (s.empty()) return;
    const size_t n = s.size();
    size_t write = 0;
    bool inTag = false;
    bool lastWasSpace = false;
    size_t read = 0;
    while (read < n) {
      const char c = s[read];
      if (inTag) {
        if (c == '>') inTag = false;
        ++read;
        continue;
      }
      if (c == '<') { inTag = true; ++read; continue; }
      if (c == '&') {
        // Look for ';' within a small window. Byte-compare entity name
        // against the source directly (no substr alloc): safe because
        // read >= write, so s[read..semi] is still original data.
        const size_t semi = s.find(';', read + 1);
        if (semi != std::string::npos && semi - read <= 8) {
          const size_t entLen = semi - read - 1;
          char decoded = 0;
          auto entMatches = [&](const char* name, size_t len) {
            if (entLen != len) return false;
            for (size_t k = 0; k < len; ++k)
              if (s[read + 1 + k] != name[k]) return false;
            return true;
          };
          if (entMatches("amp", 3)) decoded = '&';
          else if (entMatches("lt", 2)) decoded = '<';
          else if (entMatches("gt", 2)) decoded = '>';
          else if (entMatches("quot", 4)) decoded = '"';
          else if (entMatches("apos", 4)) decoded = '\'';
          else if (entMatches("nbsp", 4)) decoded = ' ';
          else if (entLen > 0 && s[read + 1] == '#') {
            int v = 0;
            bool ok = entLen > 1;
            for (size_t k = read + 2; k < semi && ok; ++k) {
              const char d = s[k];
              if (d < '0' || d > '9') { ok = false; break; }
              v = v * 10 + (d - '0');
            }
            if (ok && v > 0 && v < 128) decoded = static_cast<char>(v);
          }
          if (decoded != 0) {
            if (decoded == ' ') {
              if (!lastWasSpace) { s[write++] = ' '; lastWasSpace = true; }
            } else {
              s[write++] = decoded;
              lastWasSpace = false;
            }
            read = semi + 1;
            continue;
          }
        }
        // Unknown entity: drop the &, keep going.
        ++read;
        continue;
      }
      if (c == '\r') { ++read; continue; }
      if (c == '\n' || c == '\t' || c == ' ') {
        if (!lastWasSpace) {
          s[write++] = (c == '\n' ? '\n' : ' ');
          lastWasSpace = (c != '\n');
        }
        ++read;
        continue;
      }
      // v18.9.9.237: skip combining diacritical marks (Unicode U+0300..U+036F).
      // These are non-spacing accents/pronunciation marks (breves, cedillas,
      // tildes, etc.) that our SD font doesn't include -- rendered as '?'
      // fallbacks. Common in Wiktionary pronunciation guides ("li̯ght"
      // etc.). Not part of the definition text itself; dropping them is
      // lossless for reading. UTF-8: U+0300..U+033F encodes as 0xCC 0x80..0xBF;
      // U+0340..U+036F encodes as 0xCD 0x80..0xAF.
      {
        const unsigned char ub = static_cast<unsigned char>(c);
        if (ub == 0xCC && read + 1 < n) {
          const unsigned char nb = static_cast<unsigned char>(s[read + 1]);
          if (nb >= 0x80 && nb <= 0xBF) { read += 2; continue; }
        } else if (ub == 0xCD && read + 1 < n) {
          const unsigned char nb = static_cast<unsigned char>(s[read + 1]);
          if (nb >= 0x80 && nb <= 0xAF) { read += 2; continue; }
        }
      }
      s[write++] = c;
      lastWasSpace = false;
      ++read;
    }
    s.resize(write);  // std::string::resize down doesn't reallocate; just adjusts size
  };
  for (auto& entry : out) stripInPlace(entry);
  return out;
}

int Dictionary::editDistance(const std::string& a, const std::string& b, int maxDist) {
  if (a.empty()) return b.length();
  if (b.empty()) return a.length();

  std::vector<int> v0(b.length() + 1);
  std::vector<int> v1(b.length() + 1);

  for (size_t i = 0; i <= b.length(); i++) v0[i] = i;

  for (size_t i = 0; i < a.length(); i++) {
    v1[0] = i + 1;
    int minRowDist = v1[0];

    for (size_t j = 0; j < b.length(); j++) {
      int cost = (a[i] == b[j]) ? 0 : 1;
      v1[j + 1] = std::min({v1[j] + 1, v0[j + 1] + 1, v0[j] + cost});
      minRowDist = std::min(minRowDist, v1[j + 1]);
    }
    v0 = v1;
    if (minRowDist > maxDist) return maxDist + 1;  // Early exit
  }
  return v0[b.length()];
}

std::vector<std::string> Dictionary::findSimilar(const std::string& rawWord, int maxResults) {
  std::vector<std::string> results;
  if (!indexLoaded_ || rawWord.empty()) return results;

  std::string targetWord = cleanWord(rawWord);
  const DictBase* base = resolveDictBase();
  if (!base) return results;
  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", base->idxPath, idxFile)) return results;

  struct Suggestion {
    std::string word;
    int distance;
    bool operator<(const Suggestion& other) const { return distance < other.distance; }
  };
  std::vector<Suggestion> candidates;

  // We limit the search to a few chunks around the estimated position to avoid freezing
  // For a complete implementation, a more complex heuristic is needed.
  // For now, we scan 2000 words starting from the closest sparse index block.

  const int qCount = qidxCount();
  int low = 0;
  int high = qCount - 1;
  int closestSparseIdx = 0;

  while (low <= high) {
    int mid = low + (high - low) / 2;
    uint32_t midOff = 0;
    if (!readQidxOffset(mid, &midOff)) { idxFile.close(); return {}; }
    idxFile.seek(midOff);
    std::string currentWord = cleanWord(readWord(idxFile));

    if (currentWord == targetWord) {
      closestSparseIdx = mid;
      break;
    } else if (currentWord < targetWord) {
      closestSparseIdx = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  // Scan backwards slightly and forwards to find similar prefixes
  int startChunk = std::max(0, closestSparseIdx - 2);
  uint32_t startChunkOff = 0;
  if (!readQidxOffset(startChunk, &startChunkOff)) { idxFile.close(); return {}; }
  idxFile.seek(startChunkOff);

  int wordsScanned = 0;
  while (idxFile.available() && wordsScanned < 3000) {
    std::string word = readWord(idxFile);
    idxFile.seek(idxFile.position() + 8);  // Skip offset and size
    wordsScanned++;

    std::string clean = cleanWord(word);
    // Ignore completely unrelated words (first letter must match to save CPU)
    if (clean.empty() || clean[0] != targetWord[0]) continue;

    int dist = editDistance(targetWord, clean, 3);
    if (dist <= 3) {
      candidates.push_back({word, dist});
    }

    // CrumBLE: 3000-word linear scan on the byte-by-byte SD reader is
    // multi-second on a slow card -- feed WDT every 64 iterations.
    if ((wordsScanned & 0x3F) == 0) {
      vTaskDelay(1);
      esp_task_wdt_reset();
    }
  }
  idxFile.close();

  std::sort(candidates.begin(), candidates.end());

  for (const auto& c : candidates) {
    if (results.size() >= maxResults) break;
    if (std::find(results.begin(), results.end(), c.word) == results.end()) {
      results.push_back(c.word);
    }
  }

  return results;
}

void Dictionary::freeMemory() {
  // v18.9.9.224: nothing to free in RAM anymore -- the offset table lives
  // on disk (.qidx). Reset the loaded flag so a subsequent lookup will
  // re-read the header from SD before using the sidecar.
  indexLoaded_ = false;
  totalWords_ = 0;
  indexedIdxSize_ = 0;
}
