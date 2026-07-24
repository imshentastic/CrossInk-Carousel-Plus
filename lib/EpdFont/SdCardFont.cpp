#include "SdCardFont.h"

#include <Arduino.h>  // CrumBLE 4.2: millis() lives here; transitively pulled
                      // in on-device through HalStorage but the host_shim
                      // version doesn't include it, so the WASM build needs
                      // it explicit.
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// Walks a null-terminated UTF-8 string and appends each unique codepoint to
// codepoints[0..cpCount-1] via O(n²) dedup.  Returns true if the buffer
// reached maxCount (cap hit), false if all codepoints fit.
bool collectUniqueCodepoints(const char* text, uint32_t* codepoints, uint32_t& cpCount, uint32_t maxCount) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (cpCount >= maxCount) return true;
      codepoints[cpCount++] = cp;
    }
  }
  return false;
}

const char* asCStr(const std::string& s) { return s.c_str(); }
const char* asCStr(const char* s) { return s; }

}  // namespace

SdCardFont::~SdCardFont() { freeAll(); }

void SdCardFont::clearMiniDataForStyle(uint8_t styleIdx) {
  if (styleIdx >= MAX_STYLES) return;
  freeStyleMiniData(styles_[styleIdx]);
}

// --- Per-style free/cleanup ---

// v18.9.9.135: shared failsafe buffer for the miniBitmap allocation. Under
// BT-connected reads maxAlloc drops to ~1 KB and `new[]` for a ~3 KB
// miniBitmap fails, forcing per-glyph SD reads that make page turns feel
// sticky. When prewarmStyle's heap alloc fails, we fall back to this buffer
// if the total size fits.
// v18.9.9.139: LAZY-allocated instead of static BSS. The static form cost
// 4 KB permanent boot heap and NimBLE's controller_init malloc started
// failing silently right at the ~60 KB budget edge -- BT stopped
// connecting. Lazy: we alloc on the FIRST failed prewarm (heap at that
// point is what it is, but this only fires post-BT-connect when heap is
// already tight; before then the normal `new[]` path succeeds). Once
// allocated the buffer stays around for the session (releasing/re-acquiring
// on every prewarm would defeat the purpose). Only ONE style at a time.
static constexpr size_t kSdcfFailsafeBufferSize = 4096;
static uint8_t* g_sdcfFailsafeBuffer = nullptr;
static bool g_sdcfFailsafeBufferInUse = false;

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  if (s.miniBitmap == g_sdcfFailsafeBuffer) {
    g_sdcfFailsafeBufferInUse = false;
  } else {
    delete[] s.miniBitmap;
  }
  s.miniBitmap = nullptr;
  // v18.9.9.307: chunked bitmap storage. unique_ptr[] frees each chunk on
  // vector clear; the position side-table is trivially released too.
  s.miniBitmapChunks.clear();
  s.miniBitmapChunks.shrink_to_fit();
  s.miniBitmapChunkPos.clear();
  s.miniBitmapChunkPos.shrink_to_fit();
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;

  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.kernLeftClasses = nullptr;
  s.miniData.kernRightClasses = nullptr;
  s.miniData.kernMatrix = nullptr;
  s.miniData.kernLeftEntryCount = 0;
  s.miniData.kernRightEntryCount = 0;
  s.miniData.kernLeftClassCount = 0;
  s.miniData.kernRightClassCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  // v18.9.7: single owner. The three individual pointers are aliases into
  // miniKernBlock -- do NOT delete[] them individually.
  delete[] s.miniKernBlock;
  s.miniKernBlock = nullptr;
  s.miniKernLeftClasses = nullptr;
  s.miniKernRightClasses = nullptr;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
  // CrumBLE 4.4 task #51: release the persistent glyph file handle on
  // unload. Re-opened on the next load().
  if (persistentGlyphFile_.isOpen()) {
    persistentGlyphFile_.close();
  }
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  data.kernMatrix = s.miniKernMatrix;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s) {
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    s.kernLeftClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    s.kernRightClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];

    if (!s.kernLeftClasses || !s.kernRightClasses) {
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      freeStyleKernLigatureData(s);
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz)) {
      LOG_ERR("SDCF", "Failed to read kern classes");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  s.kernLigLoaded = true;

  // Make ligatures visible to the stub (used when no mini data built yet).
  // Kern stays nullptr on the stub — it is only wired in miniData via
  // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Kern classes + lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Lazy per-style interval loader (CrumBLE, port from rhythmerc bb303d7) ---

// fullIntervals are allocated and read on first access. Validation runs once
// here; subsequent calls are a cheap null check. Failures clear the partial
// allocation so a retry path stays consistent.
bool SdCardFont::ensureStyleIntervalsLoaded(uint8_t styleIdx) {
  if (styleIdx >= MAX_STYLES) return false;
  auto& s = styles_[styleIdx];
  if (!s.present) return false;
  if (s.fullIntervals) return true;

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for intervals: %s", filePath_);
    return false;
  }

  s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
  if (!s.fullIntervals) {
    LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, styleIdx);
    return false;
  }

  if (!file.seekSet(s.intervalsFileOffset)) {
    LOG_ERR("SDCF", "Failed to seek to intervals for style %u", styleIdx);
    delete[] s.fullIntervals;
    s.fullIntervals = nullptr;
    return false;
  }
  const size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
  if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
    LOG_ERR("SDCF", "Failed to read intervals for style %u", styleIdx);
    delete[] s.fullIntervals;
    s.fullIntervals = nullptr;
    return false;
  }

  // Validate interval contents before any later code (findGlobalGlyphIndex,
  // glyph reads) trusts them. A malformed file could otherwise drive
  // out-of-range glyph indices into bogus on-disk reads.
  uint32_t expectedOffset = 0;
  uint32_t prevLast = 0;
  for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
    const auto& iv = s.fullIntervals[j];
    if (iv.first > iv.last) {
      LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", styleIdx, j,
              static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
      delete[] s.fullIntervals;
      s.fullIntervals = nullptr;
      return false;
    }
    const uint32_t span = iv.last - iv.first + 1;
    const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
    const bool spanTooBig = (span > s.header.glyphCount);
    const bool offsetMismatch = (iv.offset != expectedOffset);
    const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
    if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
      LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", styleIdx, j,
              overlapsPrev, span, offsetMismatch, offsetOverruns);
      delete[] s.fullIntervals;
      s.fullIntervals = nullptr;
      return false;
    }
    expectedOffset += span;
    prevLast = iv.last;
  }

  return true;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  freeStyleMiniKern(s);
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    return true;  // font has no kern classes — nothing to build
  }

  // Step 1: mark used left/right classes via a 256-wide bitmap (class IDs are uint8_t).
  bool usedLeft[256] = {};
  bool usedRight[256] = {};
  for (uint32_t i = 0; i < cpCount; i++) {
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) usedLeft[lc] = true;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) usedRight[rc] = true;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t leftRenumber[256] = {};
  uint8_t rightRenumber[256] = {};
  uint8_t newToOldLeft[256] = {};
  uint8_t newToOldRight[256] = {};
  uint8_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // v18.9.7: one contiguous block sliced into three ranges. Fragmentation-
  // resilient vs the previous three sequential news, which routinely failed
  // under BT+reader (~16 KB maxAlloc, ~1.5 KB total need, but three separate
  // holes couldn't be found reliably). One malloc for the whole
  // (left + right + matrix) footprint only needs ONE hole of the total size.
  // EpdKernClassEntry is packed to 3 bytes so no alignment padding is needed
  // between the three sections; matrix is int8_t so it inherits alignment 1.
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  const uint32_t leftBytes = static_cast<uint32_t>(miniLeftCount) * sizeof(EpdKernClassEntry);
  const uint32_t rightBytes = static_cast<uint32_t>(miniRightCount) * sizeof(EpdKernClassEntry);
  const uint32_t totalBytes = leftBytes + rightBytes + matrixBytes;
  s.miniKernBlock = new (std::nothrow) uint8_t[totalBytes];
  if (!s.miniKernBlock) {
    LOG_ERR("SDCF", "Failed to allocate mini kern block (%u bytes total: %u+%u+%u)", totalBytes, leftBytes, rightBytes,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }
  s.miniKernLeftClasses = reinterpret_cast<EpdKernClassEntry*>(s.miniKernBlock);
  s.miniKernRightClasses = reinterpret_cast<EpdKernClassEntry*>(s.miniKernBlock + leftBytes);
  s.miniKernMatrix = reinterpret_cast<int8_t*>(s.miniKernBlock + leftBytes + rightBytes);

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: read the full matrix's rows for each used left class, keep only
  // columns for used right classes. One SD seek + one read per used left class;
  // a row is kernRightClassCount bytes (~200 for Literata).
  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }

  std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[s.header.kernRightClassCount]);
  if (!rowBuf) {
    LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", s.header.kernRightClassCount);
    freeStyleMiniKern(s);
    return false;
  }

  for (uint8_t newL = 1; newL <= numLeft; newL++) {
    const uint8_t oldL = newToOldLeft[newL];
    const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * s.header.kernRightClassCount;
    if (!file.seekSet(rowFileOff)) {
      LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), s.header.kernRightClassCount) !=
        static_cast<int>(s.header.kernRightClassCount)) {
      LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
    for (uint8_t newR = 1; newR <= numRight; newR++) {
      miniRow[newR - 1] = rowBuf[newToOldRight[newR] - 1u];
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = numLeft;
  s.miniKernRightClassCount = numRight;

  LOG_DBG("SDCF", "Built mini kern: %u×%u matrix (%u bytes, full was %u×%u = %u bytes)", numLeft, numRight, matrixBytes,
          s.header.kernLeftClassCount, s.header.kernRightClassCount,
          static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  FsFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Initialize per-style stub data and glyph-miss callback. fullIntervals are
  // NOT allocated here -- they're lazy per style (see ensureStyleIntervalsLoaded)
  // so a multi-style .cpfont only pays for the styles actually rendered. On a
  // theme like RoundedRaff (4 styles x 4 deduped fonts x ~150 intervals x 12
  // bytes = ~28KB), an activity that only uses Regular saves ~21KB at boot.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  // CrumBLE 4.4 task #51: open the persistent glyph-miss file handle
  // NOW while the heap is still healthy. onGlyphMiss() reuses this
  // handle (seek + read, no allocation) instead of opening the file
  // fresh on every miss. Pre-NimBLE heap is comfortable, so the open
  // will succeed; under post-NimBLE pressure where the per-call open
  // would fail, this handle is already open and the seek/read
  // succeeds. If this open fails for some reason, persistentGlyphFile_
  // stays closed and onGlyphMiss falls back to its original per-call
  // open path -- graceful degradation, no behavior change vs old.
  if (!Storage.openFileForRead("SDCF", filePath_, persistentGlyphFile_)) {
    LOG_ERR("SDCF", "Failed to pre-open persistent glyph file; "
                    "onGlyphMiss will fall back to per-call open path");
  }

  // CrumBLE 4.4 task #51: also eagerly load each style's full intervals
  // table NOW so onGlyphMiss never triggers the lazy
  // ensureStyleIntervalsLoaded path -- that path opens the file AND
  // allocates `new EpdUnicodeInterval[intervalCount]`, both of which
  // fail under post-NimBLE heap pressure. Doing it here costs maybe a
  // few KB of resident memory per book but happens against a healthy
  // boot heap. The lazy entry point still exists for other callers
  // (e.g. prebake tools) and as a fallback if this eager load fails.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    if (!ensureStyleIntervalsLoaded(i)) {
      LOG_ERR("SDCF", "Failed to pre-load intervals for style %u; "
                      "onGlyphMiss will retry the lazy load (may fail on tight heap)", i);
    }
  }

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const auto& interval = s.fullIntervals[mid];
    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval.offset + (codepoint - interval.first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  //
  // CrumBLE 4.3 task #2 follow-up: this buffer is now STATIC (one-time
  // 2 KB BSS allocation at program start, reused every render). The
  // previous per-render heap allocation was the operator new[] that
  // throws bad_alloc post-NimBLE, which terminates the render task on
  // SD-font + BT. Moving the buffer to BSS removes the per-render heap
  // pressure entirely. Safety: prewarm() is only called from the render
  // task (FontCacheManager::PrewarmScope::endScanAndPrewarm); the task
  // is single-threaded so there's no reentrancy concern. Two SdCardFont
  // instances cannot prewarm concurrently because the render task is
  // the sole caller.
  static uint32_t codepoints[MAX_PAGE_GLYPHS];
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // kern/lig data for all styles upfront (~22KB per style). Kern/lig is
  // loaded per-style in prewarmStyle() during the full render prewarm instead.
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints, codepoints + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    totalMissed += prewarmStyle(si, codepoints, cpCount, metadataOnly);
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly) {
  if (!ensureStyleIntervalsLoaded(styleIdx)) {
    return static_cast<int>(cpCount);
  }
  auto& s = styles_[styleIdx];

  // v18.9.9.152: heap pre-flight. The prewarm pipeline attempts an EpdGlyph
  // array sized cpCount*16 B (~10 KB for text-heavy pages). Under post-BT
  // tight heap (maxAlloc~1 KB), the alloc fails but leaves ~800 B of
  // fragmentation from the mappings/intervals sub-allocs that already
  // succeeded -- and that fragmentation starves downstream PageLine allocs
  // (which need maxAlloc>=256 to pass the shared_ptr ctrl-block safety
  // check at Page.cpp:80). Skip cleanly here so heap stays untouched;
  // rendering falls back to per-glyph reads, slower but functional.
  const size_t worstCaseMiniGlyphs = static_cast<size_t>(cpCount) * sizeof(EpdGlyph);
  const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
  if (maxAllocNow < worstCaseMiniGlyphs + 512) {
    LOG_INF("SDCF",
            "prewarmStyle %u pre-flight skip: maxAlloc=%u < needed=%u (cpCount=%u); "
            "avoids fragmenting heap for downstream PageLine allocs",
            styleIdx, maxAllocNow, static_cast<unsigned>(worstCaseMiniGlyphs + 512), cpCount);
    return static_cast<int>(cpCount);
  }

  // CrumBLE 4.5.4 task #5 part B diag: one-shot dump of the first few
  // intervals so we can verify WASM is parsing the .cpfont intervals
  // correctly. Field report: section 1 of a CJK book finds 4/81
  // codepoints in the same LXGW that the device finds 90% in. Strong
  // hypothesis: the intervals in WASM's s.fullIntervals aren't the
  // expected codepoint ranges -- either byte-swapped, off-by-N from
  // the wrong file offset, or truncated. This log lets us see.
  static bool dumpedStyle[MAX_STYLES] = {false, false, false, false};
  if (styleIdx < MAX_STYLES && !dumpedStyle[styleIdx]) {
    dumpedStyle[styleIdx] = true;
    LOG_INF("SDCF-IV", "style=%u intervalCount=%u glyphCount=%u (first %u of %u intervals follow)",
            styleIdx, s.header.intervalCount, s.header.glyphCount,
            std::min<uint32_t>(s.header.intervalCount, 5u), s.header.intervalCount);
    const uint32_t dumpN = std::min<uint32_t>(s.header.intervalCount, 5u);
    for (uint32_t j = 0; j < dumpN; ++j) {
      const auto& iv = s.fullIntervals[j];
      LOG_INF("SDCF-IV", "  iv[%u]: first=0x%04X last=0x%04X offset=%u (span=%u)",
              j, static_cast<unsigned>(iv.first), static_cast<unsigned>(iv.last),
              iv.offset, iv.last - iv.first + 1);
    }
    // Also probe specific codepoints that the field report says fail:
    // U+0041 'A', U+4E00 '一' (most common Han char), U+5929 '天' (sky/heaven,
    // very common). Reports findGlobalGlyphIndex result for each.
    for (uint32_t probe : {static_cast<uint32_t>(0x0041), static_cast<uint32_t>(0x4E00),
                            static_cast<uint32_t>(0x5929), static_cast<uint32_t>(0x6708)}) {
      const int32_t idx = findGlobalGlyphIndex(s, probe);
      LOG_INF("SDCF-IV", "  probe U+%04X -> %s%d", static_cast<unsigned>(probe),
              idx >= 0 ? "FOUND idx=" : "MISS (",
              idx >= 0 ? idx : -1);
    }
  }

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[cpCount];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  // CrumBLE 4.5.4 diag: collect missed codepoints so we can log them when
  // a high miss rate is suspicious. The prebake CJK report showed 77/81
  // misses for chapter text on LXGW -- with this we can see whether the
  // missed codepoints are actual CJK chars (would mean font coverage gap)
  // or weird values like raw UTF-8 bytes (would mean a parser bug
  // upstream of prewarm).
  static uint32_t missedCps[64];
  uint32_t missedCpsCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    } else if (missedCpsCount < 64) {
      missedCps[missedCpsCount++] = codepoints[i];
    }
  }
  int missed = static_cast<int>(cpCount - validCount);
  if (missed > 4 && missed >= static_cast<int>(cpCount) * 3 / 4) {
    // Log up to 32 missed codepoints in hex. If most or all are >= 0x80,
    // they're real (or near-real) Unicode and the font genuinely lacks
    // coverage. If a chunk are 0x80-0xFF (UTF-8 trail bytes), the caller
    // gave us bytes instead of codepoints.
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "SDCF MISS DUMP style=%u missed=%u/%u: ", styleIdx, missedCpsCount, cpCount);
    for (uint32_t i = 0; i < missedCpsCount && i < 32 && n < static_cast<int>(sizeof(buf)) - 10; i++) {
      n += snprintf(buf + n, sizeof(buf) - n, "U+%04lX ", static_cast<unsigned long>(missedCps[i]));
    }
    LOG_INF("SDCF", "%s", buf);
  }

  if (validCount == 0) {
    freeStyleMiniData(s);
    delete[] mappings;
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints
  freeStyleMiniData(s);

  uint32_t intervalCapacity = validCount;
  s.miniIntervals = new (std::nothrow) EpdUnicodeInterval[intervalCapacity];
  if (!s.miniIntervals) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    delete[] mappings;
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Allocate mini glyph array
  s.miniGlyphCount = validCount;
  s.miniGlyphs = new (std::nothrow) EpdGlyph[s.miniGlyphCount];
  if (!s.miniGlyphs) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  // Build sorted read order for sequential I/O
  uint32_t* readOrder = new (std::nothrow) uint32_t[validCount];
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
    delete[] readOrder;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
  // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
  // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
  // be false and we'd read from the file's current position — the header — which
  // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
  // containing that codepoint beyond page width).
  int32_t lastReadIndex = INT32_MIN;
  for (uint32_t i = 0; i < validCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (gIdx != lastReadIndex + 1) {
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "Prewarm: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
        file.close();
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      seekCount++;
    }
    if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      delete[] readOrder;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    lastReadIndex = gIdx;
  }

  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    s.miniBitmap = new (std::nothrow) uint8_t[totalBitmapSize > 0 ? totalBitmapSize : 1];
    if (!s.miniBitmap) {
      // v18.9.9.135/139: fallback to a lazy-allocated failsafe buffer.
      // Lazy-alloc on first failure so the buffer doesn't consume boot heap
      // when nothing has needed it yet (BT never enabled). Once allocated,
      // it stays around for the session.
      if (!g_sdcfFailsafeBuffer) {
        g_sdcfFailsafeBuffer = new (std::nothrow) uint8_t[kSdcfFailsafeBufferSize];
        if (g_sdcfFailsafeBuffer) {
          LOG_INF("SDCF",
                  "Allocated failsafe buffer (%u B) for tight-heap prewarm fallback",
                  (unsigned)kSdcfFailsafeBufferSize);
        }
      }
      if (g_sdcfFailsafeBuffer && totalBitmapSize <= kSdcfFailsafeBufferSize &&
          !g_sdcfFailsafeBufferInUse) {
        LOG_INF("SDCF",
                "mini bitmap alloc (%u B) failed for style %u -- using failsafe buffer",
                totalBitmapSize, styleIdx);
        s.miniBitmap = g_sdcfFailsafeBuffer;
        g_sdcfFailsafeBufferInUse = true;
      }
      // v18.9.9.307: chunked fallback. When totalBitmapSize is bigger than
      // the 4 KB failsafe (typical for CJK pages, which need 15-25 KB), split
      // the allocation into ~6 KB chunks and set the glyphBitmapFetch
      // callback so getGlyphBitmap resolves per-glyph pointers into chunks.
      // Each 6 KB chunk fits under the tight ~15 KB maxAlloc that BT
      // leaves after NimBLE's ~90 KB tax. Skips if any single chunk alloc
      // fails; caller falls through to freeStyleMiniData + bail below.
      constexpr uint32_t kChunkSize = 6u * 1024u;
      if (!s.miniBitmap) {
        LOG_INF("SDCF",
                "mini bitmap alloc (%u B) failed for style %u -- switching to chunked mode (%u B per chunk)",
                totalBitmapSize, styleIdx, (unsigned)kChunkSize);
        s.miniBitmapChunks.clear();
        s.miniBitmapChunkPos.assign(validCount, UINT32_MAX);
      }
      if (!s.miniBitmap && !s.miniBitmapChunkPos.empty()) {
        // Chunked mode: read glyphs sorted by file offset for sequential I/O,
        // packing into 6 KB chunks. A glyph that would cross a chunk boundary
        // starts a new chunk (glyphs always fit within one chunk since even
        // the largest CJK glyph at 14pt is ~200 B, far below the 6 KB budget).
        std::sort(readOrder, readOrder + validCount,
                  [&](uint32_t a, uint32_t b) {
                    return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset;
                  });

        uint32_t curChunkIdx = 0;
        uint32_t curChunkFill = 0;
        std::unique_ptr<uint8_t[]> curChunk(new (std::nothrow) uint8_t[kChunkSize]);
        if (!curChunk) {
          LOG_ERR("SDCF", "Chunked prewarm: failed to allocate first %u B chunk for style %u",
                  (unsigned)kChunkSize, styleIdx);
          delete[] readOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          file.close();
          return static_cast<int>(cpCount);
        }
        s.miniBitmapChunks.emplace_back(std::move(curChunk));

        uint32_t lastBitmapEnd = UINT32_MAX;
        for (uint32_t i = 0; i < validCount; i++) {
          const uint32_t mapIdx = readOrder[i];
          EpdGlyph& glyph = s.miniGlyphs[mapIdx];

          if (glyph.dataLength == 0) {
            // Zero-length glyph (e.g. space): position-table sentinel.
            s.miniBitmapChunkPos[mapIdx] = UINT32_MAX;
            continue;
          }

          if (glyph.dataLength > kChunkSize) {
            // Pathological: a single glyph bigger than our chunk. Would
            // require a special-case oversized chunk; not worth the code
            // for a case that shouldn't happen at realistic point sizes.
            LOG_ERR("SDCF",
                    "Chunked prewarm: glyph %u dataLength %u exceeds chunk size %u -- bailing",
                    mapIdx, glyph.dataLength, (unsigned)kChunkSize);
            delete[] readOrder;
            delete[] mappings;
            freeStyleMiniData(s);
            file.close();
            return static_cast<int>(cpCount);
          }

          // Start a new chunk if this glyph won't fit in the current one.
          if (curChunkFill + glyph.dataLength > kChunkSize) {
            std::unique_ptr<uint8_t[]> nextChunk(new (std::nothrow) uint8_t[kChunkSize]);
            if (!nextChunk) {
              LOG_ERR("SDCF",
                      "Chunked prewarm: failed to allocate chunk %u for style %u (had placed %u/%u glyphs)",
                      (unsigned)(s.miniBitmapChunks.size() + 1), styleIdx, i, validCount);
              delete[] readOrder;
              delete[] mappings;
              freeStyleMiniData(s);
              file.close();
              return static_cast<int>(cpCount);
            }
            s.miniBitmapChunks.emplace_back(std::move(nextChunk));
            curChunkIdx = static_cast<uint32_t>(s.miniBitmapChunks.size() - 1);
            curChunkFill = 0;
          }

          const uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
          if (fileOff != lastBitmapEnd) {
            if (!file.seekSet(fileOff)) {
              LOG_ERR("SDCF", "Chunked prewarm: failed to seek to bitmap (style %u)", styleIdx);
              delete[] readOrder;
              delete[] mappings;
              freeStyleMiniData(s);
              file.close();
              return static_cast<int>(cpCount);
            }
            seekCount++;
          }
          uint8_t* dst = s.miniBitmapChunks[curChunkIdx].get() + curChunkFill;
          if (file.read(dst, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
            LOG_ERR("SDCF", "Chunked prewarm: short bitmap read (style %u)", styleIdx);
            delete[] readOrder;
            delete[] mappings;
            freeStyleMiniData(s);
            file.close();
            return static_cast<int>(cpCount);
          }
          lastBitmapEnd = fileOff + glyph.dataLength;

          // Pack (chunkIdx << 20) | localOffset. dataOffset in glyph is
          // meaningless in chunked mode (callback uses chunkPos), but we
          // clear it to 0 to avoid confusing debugging.
          s.miniBitmapChunkPos[mapIdx] = (curChunkIdx << 20) | curChunkFill;
          glyph.dataOffset = 0;
          curChunkFill += glyph.dataLength;
        }

        LOG_INF("SDCF",
                "Chunked prewarm complete: style %u used %u chunks of %u B (%u B total)",
                styleIdx, (unsigned)s.miniBitmapChunks.size(),
                (unsigned)kChunkSize, totalBitmapSize);
        // Jump past the contiguous-mode fill loop below.
        file.close();
        goto chunked_fill_done;
      }
      if (!s.miniBitmap) {
        LOG_ERR("SDCF",
                "Failed to allocate mini bitmap (%u bytes) for style %u",
                totalBitmapSize, styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
    }

    // Read bitmap data sorted by file offset
    std::sort(readOrder, readOrder + validCount,
              [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = s.miniGlyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "Prewarm: failed to seek to bitmap (style %u)", styleIdx);
          file.close();
          delete[] readOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        seekCount++;
      }
      if (file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastBitmapEnd = fileOff + glyph.dataLength;

      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }
  // v18.9.9.307: chunked-mode path jumps here after filling chunks and
  // closing the file. readOrder/mappings still need cleanup and stats
  // still need to accumulate.
chunked_fill_done:

  uint32_t sdTime = millis() - sdStart;
  delete[] readOrder;
  delete[] mappings;

  // Full render prewarm: load the persistent kern classes + ligatures (one-time
  // per style, small — the big matrix is NOT loaded here) and then build the
  // per-page mini kern matrix restricted to class pairs reachable from this
  // page's codepoints. Skip during metadata-only prewarm — layout only needs
  // advanceX and the mini kern would be thrown away before rendering.
  bool kernLigOk = false;
  if (!metadataOnly) {
    if (loadStyleKernLigatureData(s)) {
      kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount);
    }
  }

  // Populate miniData and swap
  memset(&s.miniData, 0, sizeof(s.miniData));
  // v18.9.9.307: in chunked mode miniBitmap is nullptr and getGlyphBitmap
  // must route through the chunkedBitmapFetch callback to resolve
  // per-glyph pointers. In contiguous mode the callback is left null so
  // getGlyphBitmap uses the fast `&bitmap[dataOffset]` path unchanged.
  s.miniData.bitmap = s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  if (!s.miniBitmapChunks.empty()) {
    s.miniData.glyphBitmapFetch = &SdCardFont::chunkedBitmapFetch;
    s.miniData.glyphBitmapCtx = &overflowCtx_[styleIdx];
  }

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;

  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads. Use
  // clearPersistentCache() to wipe it.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

void SdCardFont::releaseForLowMemory() {
  clearOverflow();
  clearPersistentCache();

  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    freeStyleKernLigatureData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

// --- Advance table ---

void SdCardFont::clearPersistentCache() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
  }
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const {
  const AdvanceEntry* table = advanceTable_[styleIdx];
  const uint32_t size = advanceTableSize_[styleIdx];
  if (!table || size == 0) return false;
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    if (outAdvance) *outAdvance = table[lo].advanceX;
    return true;
  }
  return false;
}

void SdCardFont::mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount) {
  if (newCount == 0) return;
  const uint32_t oldSize = advanceTableSize_[styleIdx];
  if (oldSize >= ADVANCE_CACHE_LIMIT) return;  // already full

  // Cap the merged size at ADVANCE_CACHE_LIMIT. Anything past the cap is
  // dropped from the tail of the sorted merge — a deterministic, bounded loss
  // that doesn't bias which codepoints get cached on subsequent passes.
  uint32_t mergedCap = oldSize + newCount;
  if (mergedCap > ADVANCE_CACHE_LIMIT) mergedCap = ADVANCE_CACHE_LIMIT;

  AdvanceEntry* merged = new (std::nothrow) AdvanceEntry[mergedCap];
  if (!merged) {
    LOG_ERR("SDCF", "mergeIntoAdvanceTable: alloc failed (%u entries) style %u", mergedCap, styleIdx);
    return;
  }

  const AdvanceEntry* a = advanceTable_[styleIdx];
  const AdvanceEntry* b = sortedNew;
  uint32_t i = 0, j = 0, k = 0;
  while (k < mergedCap && (i < oldSize || j < newCount)) {
    if (i < oldSize && (j >= newCount || a[i].codepoint <= b[j].codepoint)) {
      merged[k++] = a[i++];
    } else {
      merged[k++] = b[j++];
    }
  }

  delete[] advanceTable_[styleIdx];
  advanceTable_[styleIdx] = merged;
  advanceTableSize_[styleIdx] = k;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTable_[i]) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  if (!advanceTable_[style]) return 0;
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  return 0;
}

// Given a sorted array of unique codepoints, resolve glyph indices per style,
// batch-read advanceX from SD, and merge into the persistent advance table.
// Caller owns the codepoints buffer.
int SdCardFont::fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask) {
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    if (!ensureStyleIntervalsLoaded(si)) {
      totalMissed += static_cast<int>(cpCount);
      continue;
    }
    const auto& s = styles_[si];

    // Stop fetching once the cache is full — further inserts would be dropped
    // by the merge anyway. The renderer fast path tolerates missing entries
    // (returns 0); the slow path is still correct for those codepoints.
    if (advanceTableSize_[si] >= ADVANCE_CACHE_LIMIT) continue;

    // For each codepoint in `codepoints`, skip those already cached, then
    // resolve to a glyph index. Build a parallel array sorted by glyph index
    // for sequential SD reads.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    std::unique_ptr<CpIdx[]> mappings(new (std::nothrow) CpIdx[cpCount]);
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t needCount = 0;
    uint32_t missedThisStyle = 0;
    const int32_t replacementIdx = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      if (advanceTableLookup(si, cp, nullptr)) continue;  // already cached
      int32_t idx = findGlobalGlyphIndex(s, cp);
      if (idx < 0) {
        if (replacementIdx < 0) {
          missedThisStyle++;
          continue;
        }
        idx = replacementIdx;
      }
      mappings[needCount].codepoint = cp;
      mappings[needCount].glyphIndex = idx;
      needCount++;
    }
    totalMissed += static_cast<int>(missedThisStyle);

    if (needCount == 0) continue;

    // Sort by glyph index so SD reads are mostly sequential.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // Open file once and read advanceX for each needed glyph.
    FsFile file;
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      continue;
    }

    std::unique_ptr<AdvanceEntry[]> staged(new (std::nothrow) AdvanceEntry[needCount]);
    if (!staged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate staging for style %u", si);
      file.close();
      continue;
    }

    uint32_t fetched = 0;
    EpdGlyph tempGlyph;
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < needCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, si);
          break;
        }
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        break;
      }
      lastReadIndex = gIdx;
      staged[fetched].codepoint = mappings[i].codepoint;
      staged[fetched].advanceX = tempGlyph.advanceX;
      fetched++;
    }
    file.close();

    if (fetched > 0) {
      // Sort staged by codepoint, then merge into the persistent table.
      std::sort(staged.get(), staged.get() + fetched,
                [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
      mergeIntoAdvanceTable(si, staged.get(), fetched);
    }

    LOG_DBG("SDCF", "Advance table style %u: +%u from SD, total=%u/%u", si, fetched, advanceTableSize_[si],
            ADVANCE_CACHE_LIMIT);
  }

  return totalMissed;
}

template <typename Iter>
int SdCardFont::buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // +2 reserved slots for space and hyphen injected after the main scan.
  static constexpr uint32_t MAX_UNIQUE_CODEPOINTS = 4096;
  uint32_t* codepoints = new (std::nothrow) uint32_t[MAX_UNIQUE_CODEPOINTS + 2];
  if (!codepoints) {
    LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes)", MAX_UNIQUE_CODEPOINTS * 4);
    return -1;
  }
  uint32_t cpCount = 0;
  bool hitCap = false;

  for (auto it = begin; it != end && !hitCap; ++it) {
    hitCap = collectUniqueCodepoints(asCStr(*it), codepoints, cpCount, MAX_UNIQUE_CODEPOINTS);
  }

  if (includeSpace && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == ' '; }))
    codepoints[cpCount++] = ' ';
  if (includeHyphen && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == '-'; }))
    codepoints[cpCount++] = '-';

  if (hitCap) {
    LOG_ERR("SDCF", "buildAdvanceTable: unique codepoint cap (%u) hit, layout may be approximate",
            MAX_UNIQUE_CODEPOINTS);
  }
  std::sort(codepoints, codepoints + cpCount);
  int totalMissed = fetchAdvancesForCodepoints(codepoints, cpCount, styleMask);
  delete[] codepoints;
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask) {
  return buildAdvanceTableRange(&utf8Text, &utf8Text + 1, false, false, styleMask);
}

int SdCardFont::buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask) {
  return buildAdvanceTableRange(words.begin(), words.end(), words.size() > 1, includeHyphen, styleMask);
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  if (!self->ensureStyleIntervalsLoaded(styleIdx)) return nullptr;
  const auto& s = self->styles_[styleIdx];

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // CrumBLE 4.4 task #51: use the persistent file handle when
  // available. The per-call Storage.openFileForRead path allocates
  // ~12 bytes for HalFile::Impl and crashes under post-NimBLE heap
  // pressure. The persistent handle was opened during load() while
  // the heap was healthy; here we just seek and read on it. Fallback
  // to the per-call open path if the persistent handle isn't open
  // (load-time pre-open failed -- graceful degradation).
  FsFile fallbackFile;
  HalFile* fileRef = nullptr;
  if (self->persistentGlyphFile_.isOpen()) {
    fileRef = &self->persistentGlyphFile_;
  } else {
    if (!Storage.openFileForRead("SDCF", self->filePath_, fallbackFile)) {
      LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
      return nullptr;
    }
    fileRef = &fallbackFile;
  }
  HalFile& file = *fileRef;

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    // Do NOT close on seek failure -- file is still valid, only the
    // seek failed. Closing would force every future miss back to the
    // per-call open path, defeating the fix. fallbackFile (if used)
    // closes itself on scope exit.
    return nullptr;
  }
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      // CrumBLE 4.4 task #51: do NOT close the persistent handle on a
      // bad seek -- the file is still valid. Closing would force every
      // subsequent miss back to the per-call open path.
      return nullptr;
    }
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  if (wasAtCapacity) {
    delete[] self->overflow_[slot].bitmap;
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }

// v18.9.9.311: cheap contentHash peek for the prebake-fontId-rescue path.
// Mirrors the header + TOC hashing done in load() (see hash accumulation
// around line 586 / 606) but stops before touching intervals / glyphs /
// bitmap. Total read: 32 B header + (styleCount * 32 B) TOC entries. At
// most 4 styles = 160 bytes. Uses HalStorage's tag-based path lookup
// same as load().
bool SdCardFont::peekContentHash(const char* path, uint32_t& outHash) {
  if (!path || !*path) return false;
  FsFile f;
  if (!Storage.openFileForRead("SDCF", path, f)) return false;

  uint8_t headerBuf[HEADER_SIZE];
  if (f.read(headerBuf, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) {
    f.close();
    return false;
  }
  // Validate magic + version so a corrupt / wrong-format file produces false
  // instead of a nonsense hash that could still spuriously match some other
  // fontId in the search.
  if (std::memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    f.close();
    return false;
  }
  const uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    f.close();
    return false;
  }
  const uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    f.close();
    return false;
  }

  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (f.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != static_cast<int>(STYLE_TOC_ENTRY_SIZE)) {
      f.close();
      return false;
    }
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);
  }
  f.close();
  outHash = hash;
  return true;
}

// v18.9.9.307: per-glyph bitmap resolver for the chunked prewarm fallback.
// ctx is an OverflowContext (same one used by onGlyphMiss); we recover the
// SdCardFont and styleIdx, compute the glyph index (glyph ptr offset from
// miniGlyphs base), and look up the packed (chunkIdx << 20) | localOffset
// entry in miniBitmapChunkPos. Returns nullptr for zero-length glyphs
// (dataLength=0 sentinel), out-of-range indices, or missing chunks --
// matches the contract getGlyphBitmap already handles for other paths.
const uint8_t* SdCardFont::chunkedBitmapFetch(void* ctx, const EpdGlyph* glyph) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  if (!oc || !oc->self) return nullptr;
  auto& s = oc->self->styles_[oc->styleIdx];
  if (glyph < s.miniGlyphs) return nullptr;
  const uint32_t glyphIndex = static_cast<uint32_t>(glyph - s.miniGlyphs);
  if (glyphIndex >= s.miniBitmapChunkPos.size()) return nullptr;
  const uint32_t packed = s.miniBitmapChunkPos[glyphIndex];
  if (packed == UINT32_MAX) return nullptr;  // zero-length glyph sentinel
  const uint32_t chunkIdx = packed >> 20;
  const uint32_t localOffset = packed & 0xFFFFFu;
  if (chunkIdx >= s.miniBitmapChunks.size()) return nullptr;
  return s.miniBitmapChunks[chunkIdx].get() + localOffset;
}

// CrumBLE 4.3: prebake-CLI-facing read-only views into the prewarmed
// per-style mini-data. Implementations are trivial -- they just return
// stored fields -- but live in the .cpp so the public header doesn't need
// to expose PerStyle. All accessors return safe defaults (nullptr / 0)
// for an out-of-range styleIdx so callers can iterate 0..MAX_STYLES
// blindly.
namespace {
constexpr uint8_t kMaxStylesForAccessor = 4;  // matches MAX_STYLES in the class
}

uint32_t SdCardFont::miniIntervalCount(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniIntervalCount;
}
uint32_t SdCardFont::miniGlyphCount(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniGlyphCount;
}
uint32_t SdCardFont::miniBitmapSize(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  // Sum the dataLength of every prewarmed glyph; bitmap layout is
  // contiguous so sum == total bytes used in miniBitmap.
  const PerStyle& s = styles_[styleIdx];
  uint32_t total = 0;
  for (uint32_t i = 0; i < s.miniGlyphCount; ++i) {
    total += s.miniGlyphs[i].dataLength;
  }
  return total;
}
const EpdUnicodeInterval* SdCardFont::miniIntervalsPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].miniIntervals;
}
const EpdGlyph* SdCardFont::miniGlyphsPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].miniGlyphs;
}
const uint8_t* SdCardFont::miniBitmapPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].miniBitmap;
}
uint8_t SdCardFont::miniAdvanceY(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniData.advanceY;
}
int16_t SdCardFont::miniAscender(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return static_cast<int16_t>(styles_[styleIdx].miniData.ascender);
}
int16_t SdCardFont::miniDescender(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return static_cast<int16_t>(styles_[styleIdx].miniData.descender);
}
bool SdCardFont::miniIs2Bit(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return false;
  return styles_[styleIdx].miniData.is2Bit;
}
uint16_t SdCardFont::miniKernLeftEntryCount(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniKernLeftEntryCount;
}
uint16_t SdCardFont::miniKernRightEntryCount(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniKernRightEntryCount;
}
uint8_t SdCardFont::miniKernLeftClassCount(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniKernLeftClassCount;
}
uint8_t SdCardFont::miniKernRightClassCount(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].miniKernRightClassCount;
}
uint16_t SdCardFont::miniLigaturePairCount(uint8_t styleIdx) const {
  // CrumBLE 4.3 v2: ligature pairs are FONT-WIDE (header), not per-style.
  // Returned once for any installed style so the emit code can write the
  // same ligature table per style slot.
  if (styleIdx >= kMaxStylesForAccessor) return 0;
  return styles_[styleIdx].header.ligaturePairCount;
}
const EpdKernClassEntry* SdCardFont::miniKernLeftClassesPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].miniKernLeftClasses;
}
const EpdKernClassEntry* SdCardFont::miniKernRightClassesPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].miniKernRightClasses;
}
const int8_t* SdCardFont::miniKernMatrixPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].miniKernMatrix;
}
const EpdLigaturePair* SdCardFont::miniLigaturePairsPtr(uint8_t styleIdx) const {
  if (styleIdx >= kMaxStylesForAccessor) return nullptr;
  return styles_[styleIdx].ligaturePairs;
}
