#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Dictionary {
 public:
  // v18.9.9.259: multi-dictionary support. discoverAll() walks the same
  // fixed candidate folders (SD root, /dict/, /dictionary/, /dictionaries/)
  // and returns
  // EVERY *.dict + *.idx pair rather than just the first match. Callers
  // (Lookup entry) pick one via DictionaryPickerActivity or fall through
  // to the sole match if only one is found. Discovery is cheap (opendir
  // + readdir over three small folders) and NOT cached -- reflects any
  // dicts the user added via FT without needing a reboot. The v202
  // heap-cost concern (settings-view-cache growth from a SETTINGS
  // field) is avoided by persisting the active-dict pointer to a tiny
  // sidecar file instead of SETTINGS.
  struct DictInfo {
    std::string dictPath;     // e.g. "/dict/wiktionary.dict"
    std::string idxPath;      // e.g. "/dict/wiktionary.idx"
    std::string displayName;  // e.g. "wiktionary" (filename base, no ext)
  };
  static std::vector<DictInfo> discoverAll();

  // Set the active dict for subsequent lookup / loadIndex calls in this
  // session. Persists dictPath to a tiny sidecar file so future sessions
  // remember the pick. Also switches the .qidx cache to a per-dict path
  // derived from the dict path hash -- separate dicts don't collide on
  // one shared cache file. Pass an empty DictInfo to clear.
  static void setActive(const DictInfo& dict);

  // Read the persisted active-dict pointer. Returns an empty DictInfo if
  // no sidecar exists OR the previously-active dict is no longer on SD.
  // Caller falls back to discoverAll() + picker in either case.
  static DictInfo getActive();

  // Checks if the required StarDict files exist on the SD card
  static bool exists();

  // Looks up a word and returns its definition. Supports progress callbacks.
  // v18.9.9.198: returns the FIRST matching entry only; kept for callers that
  // only need a single definition. For multi-entry navigation use lookupAll.
  static std::string lookup(const std::string& word, const std::function<void(int percent)>& onProgress = nullptr,
                            const std::function<bool()>& shouldCancel = nullptr);

  // v18.9.9.198: returns every entry the .idx chunk contains for the target
  // key (up to 6). StarDict allows multiple records that normalize to the
  // same key ("Quiver" proper noun + "quiver" common noun). Caller can
  // cycle through them; per-entry wrap+render keeps peak heap small vs
  // wrapping a concatenated blob. Empty vector => not found.
  static std::vector<std::string> lookupAll(const std::string& word,
                                            const std::function<void(int percent)>& onProgress = nullptr,
                                            const std::function<bool()>& shouldCancel = nullptr);

  // v18.9.9.247: chunked reader interface. lookupAllHandles returns just
  // {offset,size} refs to matching .dict entries -- no data loaded. Caller
  // then calls readDefinitionRange to load a byte range on demand. Enables
  // paginated display of large entries (Wiktionary "light" at 10+ KB) that
  // don't fit whole on tight X3+BLE heap: peak per lookup = chunk_size +
  // wrap-of-chunk (~5-6 KB) independent of total entry size.
  struct EntryHandle {
    uint32_t offset;
    uint32_t size;
  };
  static std::vector<EntryHandle> lookupAllHandles(const std::string& word,
                                                    const std::function<bool()>& shouldCancel = nullptr);

  // Read a byte range from a single entry and apply HTML strip (in-place)
  // to the returned string. `entryOffset` and `entryTotalSize` come from
  // an EntryHandle. `rangeStart` is byte offset within the entry. Range
  // is clamped to entryTotalSize. Applies pre-flight heap guard on
  // rangeSize + 4 KB margin. Returns empty on refusal or IO error.
  static std::string readDefinitionRange(uint32_t entryOffset, uint32_t entryTotalSize,
                                          uint32_t rangeStart, uint32_t rangeSize);

  // Removes punctuation and converts to lowercase
  static std::string cleanWord(const std::string& word);

  // Generates basic English stem variants (e.g., "running" -> "run")
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Detects inflection-stub definitions ("plural of missile") and extracts
  // the base headword so callers can redirect to the real definition.
  // def must already be HTML-stripped. Returns false for normal definitions.
  static bool extractFormOfBase(const std::string& def, std::string& outBase);

  // Finds similar words using Levenshtein distance (Did you mean?)
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults);

  static void freeMemory();

  // CrumBLE: gating helpers for the explicit-consent prompt before the
  // one-time ~10s index scan. isIndexReady() returns true if the sparse
  // offset table is already in RAM (no work needed). hasCachedIndex()
  // returns true if /.crosspoint/dict_idx.cache exists on disk (cheap
  // existence check -- callers should still call loadIndex to actually
  // populate the in-RAM table, which is fast from cache). loadIndex
  // and loadCachedIndex are promoted to public so the LOOKUP entry
  // point can drive the load explicitly instead of letting it happen
  // inside the first lookup() call.
  static bool isIndexReady();
  static bool hasCachedIndex();

  // v18.9.9.237: memory-refusal signal so callers can distinguish "the
  // word isn't in the dictionary" from "we couldn't load the payload
  // because the heap was too tight." Reset to false at the start of
  // every lookup / lookupAll call; set true if readDefinition's
  // pre-flight guard refuses. Callers should query IMMEDIATELY after
  // lookup returns empty -- another lookup will reset it.
  static bool wasLastRefusedDueToMemory();
  static void clearLastRefusedDueToMemory();

  static bool loadIndex(const std::function<void(int percent)>& onProgress = nullptr,
                        const std::function<bool()>& shouldCancel = nullptr);
  static bool loadCachedIndex();

 private:
  // v18.9.9.224: interval 512 -> 256 (matches CrossPoint feat/slim-
  // dictionary). Halves the worst-case linear-scan chunk during lookup.
  static constexpr int SPARSE_INTERVAL = 256;

  // v18.9.9.224: sample-offset sidecar (.qidx) replaces the in-RAM
  // sparse offset table. Binary search happens against the FILE
  // (log2(N) small seek+read pairs, ~8ms for 100k-word dict on a
  // typical SD) so the offset array never occupies heap at rest.
  //
  // Sidecar format (24-byte header + N uint32_t offsets, all LE):
  //   [0..3]   magic "QIDX"
  //   [4]      version 0x01
  //   [5..7]   padding
  //   [8..11]  interval (== SPARSE_INTERVAL at build time)
  //   [12..15] totalWords
  //   [16..19] indexedIdxSize -- .idx file size at build time
  //                              (staleness marker: rebuild if source .idx
  //                               grew/shrank vs this)
  //   [20..23] reserved
  //   [24..]   uint32_t offsets[qidxCount()] pointing into .idx
  //
  // Only totalWords_ + indexedIdxSize_ are kept in RAM after load; the
  // offset array stays on disk. `qidxCount()` = ceil(totalWords_/interval).
  static uint32_t totalWords_;
  static uint32_t indexedIdxSize_;
  static bool indexLoaded_;

  // Read a single sparse offset from the sidecar. Returns false + zeros dst on
  // any IO / bounds error. Callers should have already validated header via
  // loadCachedIndex or built the sidecar via loadIndex.
  static bool readQidxOffset(int sparseIndex, uint32_t* outOffset);
  // Count of offset entries in the sidecar.
  static int qidxCount();

  static std::string readWord(FsFile& file);
  static std::string readDefinition(uint32_t offset, uint32_t size);
  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};