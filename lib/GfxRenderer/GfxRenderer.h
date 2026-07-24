#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class FontCacheManager;
class SdCardFont;

#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
// CrumBLE: VeryDarkGray is the inverse of LightGray's 2x2 period -- black
// at every pixel EXCEPT (x even AND y even), i.e. 3-of-4 coverage (~75%).
// Sits between DarkGray (50%) and Black (100%); used by the transition
// popups to read denser than DarkGray without becoming flat-black.
enum Color : uint8_t {
  Clear = 0x00,
  White = 0x01,
  LightGray = 0x05,
  DarkGray = 0x0A,
  VeryDarkGray = 0x0D,
  Black = 0x10,
};

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

  // CrumBLE Phase 1: path-keyed in-RAM bitmap cache for the new Library
  // shelf paint path. A `CachedBitmap` holds the BMP's full decoded
  // 2-bit-per-pixel packed pixels (matches Bitmap::readNextRow output),
  // plus an optional pre-scaled 1-bit-per-pixel buffer at the most
  // recently requested target dimensions. The 1bpp scaled buffer is what
  // drawCachedBitmap actually blits to the framebuffer -- subsequent
  // paints at the same target size hit memory only.
  //
  // Stride for `pixels`        = (width  + 3) / 4   bytes (2bpp packed)
  // Stride for `scaledPixels`  = (scaledWidth + 7) / 8 bytes (1bpp MSB-first)
  struct CachedBitmap {
    std::unique_ptr<uint8_t[]> pixels;
    size_t pixelsBytes = 0;
    int width = 0;
    int height = 0;
    bool topDown = false;
    uint32_t lastUsedTick = 0;
    // v18.9.9.192: caller-requested "please survive eviction" flag. Used by
    // shelf covers on the Home screen so they don't get flushed when NimBLE
    // starves imageCacheBudget_ to 0. Total pinned bytes are capped
    // (kPinnedBytesCap_ in the .cpp) so pinning can't blow up NimBLE headroom.
    bool pinned = false;

    std::unique_ptr<uint8_t[]> scaledPixels;
    size_t scaledPixelsBytes = 0;
    int scaledWidth = 0;
    int scaledHeight = 0;
    // CrumBLE: cropX/cropY (0.0-1.0) of the SOURCE that was trimmed
    // before scaling into scaledPixels. Stored so the cache invalidates
    // the scaled buffer when a different crop is requested for the same
    // target size (e.g. carousel center vs. side aspect-fill).
    float scaledCropX = 0.0f;
    float scaledCropY = 0.0f;
  };

 private:
  // BW backup for the grayscale anti-aliasing pass uses PackBits-style RLE
  // compression. Reader pages are >95% same-byte runs, so a 48 KB framebuffer
  // typically encodes to 2-5 KB. We allocate a single bounded buffer instead
  // of 12 × 4 KB chunks, which dramatically reduces fragmentation pressure
  // when NimBLE + EPUB allocations have split the heap.
  // Cap at 32 KB so image-heavy pages (which compress poorly because their
  // dithered patterns have few same-byte runs) still fit. Field measurements
  // showed dense pages compress to ~25-26 KB. If even 32 KB isn't enough we
  // gracefully skip grayscale for that page (same UX as the old chunked
  // alloc-failure path).
  //
  // A 16 KB cap was tried (CrumBLE) to make the per-page allocation more
  // likely to succeed when NimBLE has fragmented the heap, but it backfired:
  // pages that compress to 16-26 KB then overflow the buffer and lose AA
  // *unconditionally*, even on a clean heap with no Bluetooth — a worse
  // regression than 32 KB's "occasionally skips under BLE memory pressure".
  // So 32 KB stays. The AA-under-BLE inconsistency is accepted as graceful
  // degradation (see CHANGELOG known limitation).
  static constexpr size_t MAX_BW_COMPRESSED_SIZE = 32U * 1024U;

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  // CrumBLE 4.4 (ported from CPR-vCodex): Text Darkness. Read by the 2-bit
  // glyph blit in renderCharImpl to choose which AA buckets get inked.
  // 0=Normal, 1=Legacy BW, 2=Dark, 3=Extra Dark.
  uint8_t textDarkness = 0;
  mutable bool renderStarved = false;
  // Set when an image was decoded this render but not cached to .pxc (partial /
  // off-screen). Such a page re-decodes on every repaint, so it is not BLE-safe.
  mutable bool imageRepaintUnsafe_ = false;
  // BT No Images Quick Connect: when true, ImageBlock::render skips decoding and
  // draws a placeholder border instead, so an image-heavy book can be read over a
  // BLE remote without the decoder's large contiguous allocations starving NimBLE.
  // Session-scoped (reset on reader entry); auto-cleared when Bluetooth drops.
  bool suppressImages_ = false;
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  uint8_t* bwCompressedBackup = nullptr;
  size_t bwCompressedBackupSize = 0;
  std::map<int, EpdFontFamily> fontMap;
  // Shared bitmap row buffers. Every read/write must be inside BitmapScratchLock;
  // ensureBitmapScratchBuffers() asserts that contract before exposing them.
  mutable SemaphoreHandle_t bitmapScratchMutex_ = nullptr;
  mutable uint8_t* bitmapScratchOutputRow_ = nullptr;
  mutable size_t bitmapScratchOutputRowSize_ = 0;
  mutable uint8_t* bitmapScratchRowBytes_ = nullptr;
  mutable size_t bitmapScratchRowBytesSize_ = 0;

  class BitmapScratchLock {
    const GfxRenderer& renderer_;
    bool locked_ = false;

   public:
    explicit BitmapScratchLock(const GfxRenderer& renderer);
    BitmapScratchLock(const BitmapScratchLock&) = delete;
    BitmapScratchLock& operator=(const BitmapScratchLock&) = delete;
    ~BitmapScratchLock();

    bool isLocked() const { return locked_; }
  };

  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwCompressedBackup();
  void freeBitmapScratchBuffers();
  bool ensureBitmapScratchBuffers(size_t outputRowSize, size_t rowBytesSize) const;
  bool bitmapScratchLockHeldByCurrentTask() const;
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // CrumBLE Phase 2: byte-aligned rectangle fill (rhythmerc/crosspoint port).
  // Clips, rotates the two opposing logical corners into physical-framebuffer
  // space, then walks each physical row with head-mask + memset middle +
  // tail-mask byte writes -- no per-pixel rotate, no per-pixel RMW. For
  // dither colors (LightGray, DarkGray), the per-row 8-bit pattern is
  // precomputed from inverse-rotated logical (x, y); within a row the dither
  // has period 2 so the same byte pattern applies to every full byte. All
  // fillRect() / fillRectDither() callers dispatch here.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

  // CrumBLE Phase 1: cached-bitmap state. All members are `mutable` because
  // drawCachedBitmap() and lookupCachedBitmap() are exposed as const-on-this
  // (consistent with the rest of the renderer's "const paint methods")
  // even though they mutate the cache (insert, scale, evict, touch LRU).
  mutable std::unordered_map<std::string, CachedBitmap> imageCache_;
  mutable size_t imageCacheBytes_ = 0;
  // Dynamic budget: starts at 64 KB on the assumption BLE is off; shrinks
  // when free-heap drops (see reconcileImageCacheBudget()). Always 0 in BW
  // mode with NimBLE actively starving the heap — the budget tracker
  // evicts to 0 below the floor.
  mutable size_t imageCacheBudget_ = 64u * 1024u;
  mutable uint32_t imageCacheTick_ = 0;
  // v18.9.9.192: total bytes held by CachedBitmap entries with pinned=true.
  // Kept separate from imageCacheBytes_ so we can cap pinned growth
  // independent of the free-heap-driven budget policy.
  mutable size_t imageCachePinnedBytes_ = 0;
  // Reentrancy flag: when nonzero, lookupCachedBitmap treats the entry as
  // pinned-request-in-progress (skips budget==0 short-circuit, keeps the
  // returned entry safe from immediate eviction). Set by lookupCachedBitmapPinned.
  mutable uint32_t pinnedRequestDepth_ = 0;
  // Builds (or rebuilds) `entry->scaledPixels` at (targetW, targetH) from
  // the 2bpp source. Bytes accounted against imageCacheBytes_.
  void buildScaledBitmap(CachedBitmap* entry, int targetW, int targetH, float cropX = 0.0f,
                         float cropY = 0.0f) const;
  // Evicts LRU entries until imageCacheBytes_ <= imageCacheBudget_. No-op
  // when already within budget. Called automatically by lookupCachedBitmap
  // after each insert and by reconcileImageCacheBudget() when the budget
  // shrinks.
  void evictImageCacheToBudget() const;
  // Recomputes imageCacheBudget_ from current ESP free heap. Tiered:
  // > 120 KB: 64 KB cache, 80-120 KB: 16 KB, < 80 KB: 0 (force-flush).
  // Cheap (one esp_get_free_heap_size call); called by lookupCachedBitmap
  // before each insert.
  void reconcileImageCacheBudget() const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay),
        renderMode(BW),
        orientation(Portrait),
        fadingFix(false),
        bitmapScratchMutex_(xSemaphoreCreateMutex()) {
    assert(bitmapScratchMutex_ != nullptr && "Failed to create GfxRenderer bitmap scratch mutex");
  }
  GfxRenderer(const GfxRenderer&) = delete;
  GfxRenderer& operator=(const GfxRenderer&) = delete;
  GfxRenderer(GfxRenderer&&) = delete;
  GfxRenderer& operator=(GfxRenderer&&) = delete;
  ~GfxRenderer() {
    freeBwCompressedBackup();
    freeBitmapScratchBuffers();
  }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }

  // CrumBLE 4.3: per-section embedded glyph subset routing. Forwards to
  // EpdFontFamily::setEmbeddedGlyphData() on the matching font entry. The
  // reader calls this after a successful section.tryInstallEmbeddedGlyphSubset()
  // so the next render's glyph lookups consult the section's in-RAM block
  // before falling through to the SD-font miss handler. Always pass the
  // current section's full per-style data (nullptr for styles that weren't
  // in the embedded block); subsequent calls overwrite any prior state, so
  // a section change to one without an embedded block automatically clears
  // the previous section's pointers by passing nullptr everywhere.
  //
  // Lifetime contract: the EpdFontData pointers are owned by Section's
  // embeddedStyles_ array. They stay valid as long as the Section object
  // lives, which spans from loadSectionFile until the next section change
  // (Section is unique_ptr-owned by EpubReaderActivity). Reader is
  // responsible for clearing (passing all nullptr) before destroying the
  // owning Section.
  void setEmbeddedGlyphData(int fontId, const EpdFontData* regularData, const EpdFontData* boldData,
                              const EpdFontData* italicData, const EpdFontData* boldItalicData) {
    auto it = fontMap.find(fontId);
    if (it != fontMap.end()) {
      it->second.setEmbeddedGlyphData(regularData, boldData, italicData, boldItalicData);
    }
  }
  void clearEmbeddedGlyphData(int fontId) {
    auto it = fontMap.find(fontId);
    if (it != fontMap.end()) {
      it->second.clearEmbeddedGlyphData();
    }
  }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  void ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                             uint8_t styleMask = 0x0F) const;
  bool releaseSdCardFontForLowMemory(int fontId) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) {
    orientation = o;
#ifdef SIMULATOR
    display.setSimulatorOrientation(static_cast<int>(o));
#endif
  }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }
  void setTextDarkness(const uint8_t darkness) { textDarkness = darkness; }
  uint8_t getTextDarkness() const { return textDarkness; }

  // Render-starvation signal. Set when a glyph couldn't be decompressed for OOM
  // (getGlyphBitmap) or an image failed to decode (ImageBlock::render) — i.e.
  // the page can't be drawn because contiguous heap is too tight, typically
  // because a BLE remote (NimBLE ~58 KB) is connected. The reader reads+clears
  // it after a page render to decide whether to drop Bluetooth for the book so
  // the full heap is available. markRenderStarved is const (sets a mutable
  // flag) so the const glyph path can call it.
  void markRenderStarved() const { renderStarved = true; }
  bool takeRenderStarved() {
    const bool starved = renderStarved;
    renderStarved = false;
    return starved;
  }

  // Image-repaint-safety signal. Set when an image was DECODED this render without
  // being written to its .pxc pixel cache (a partial/off-screen image, which the
  // cache path skips). Such a page would have to decode the image again on the
  // next repaint -- so it is NOT safe to bring a BLE remote back up over it. When
  // this stays clear after a clean render, every image on the page is either
  // cached or absent, so the page repaints decoder-free (BLE-safe). The reader
  // uses this to decide whether to re-enable Bluetooth after a low-memory rebuild.
  void markImageRepaintUnsafe() const { imageRepaintUnsafe_ = true; }
  bool takeImageRepaintUnsafe() {
    const bool unsafe = imageRepaintUnsafe_;
    imageRepaintUnsafe_ = false;
    return unsafe;
  }

  // BT No Images Quick Connect image suppression. When enabled, image blocks are
  // drawn as placeholder borders instead of decoded, keeping the contiguous heap
  // free for NimBLE on image-heavy books.
  void setSuppressImages(bool suppress) { suppressImages_ = suppress; }
  bool suppressImages() const { return suppressImages_; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const;

  // 4.5.5+: passthrough for partial refresh. See HalDisplay::displayBufferRegion
  // for semantics. Caller passes the dirty bounding box (from DirtyRegion
  // typically); display layer handles bounds clamping and the 70%-of-screen
  // fallback to full refresh.
  void displayBufferRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  // CrumBLE 4.5.6 (INX highlight lattice, byte-aligned + additive):
  // paint a sparse-ink dot pattern into the given rect WITHOUT clearing
  // pixels that were already black. Every (step-th, step-th) pixel is
  // ANDed to black; other pixels are left untouched. Used by the reader's
  // highlight overlay: dot pattern reads as ~25% grey on a 1-bit panel
  // AND preserves the text glyphs underneath. Byte-aligned interior bytes
  // are &= with a precomputed lattice mask; edges use head/tail masks.
  void fillSparseInkLatticeInRect(int x, int y, int width, int height, int step = 2, bool state = true) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIconInverted(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;

  // ─── Cached-bitmap path (Library / shelf cover paints) ───────────────────
  // CrumBLE Phase 1, ported from rhythmerc/crosspoint-reader. Caches decoded
  // BMPs in RAM by path; subsequent paints at the same target size blit
  // straight from a pre-scaled 1bpp buffer (no SD I/O, no re-decode).
  //
  // First call for a given path: opens the file, parses headers, walks
  // every row through Bitmap::readNextRow into a 2bpp packed buffer,
  // inserts into imageCache_. Returns nullptr on parse / OOM failure.
  // Subsequent calls return the same handle (cache hit).
  CachedBitmap* lookupCachedBitmap(const char* path) const;
  CachedBitmap* lookupCachedBitmap(const std::string& path) const { return lookupCachedBitmap(path.c_str()); }
  // v18.9.9.192: pinned variant. Bypasses the budget==0 refuse (which fires
  // under NimBLE-tight heap and disables the whole cache), marks the entry
  // pinned so it survives evictImageCacheToBudget. Total pinned bytes are
  // capped at ~24 KB (see kImageCachePinnedCap in .cpp); over that, the
  // oldest pinned entry is unpinned and becomes an ordinary eviction
  // candidate. Used by the Home shelf so scrolling stays snappy across
  // BT-tight sessions where the general image cache is force-flushed.
  CachedBitmap* lookupCachedBitmapPinned(const char* path) const;
  CachedBitmap* lookupCachedBitmapPinned(const std::string& path) const { return lookupCachedBitmapPinned(path.c_str()); }
  // Drop pinned status on ALL cached bitmaps. Called on cover-refresh flows
  // and library invalidation so stale cover data doesn't stick.
  void unpinAllCachedBitmaps() const;
  // Reads dimensions from a cache handle without forcing a paint. Returns
  // false if handle is null or hasn't been decoded yet (which shouldn't
  // happen for handles returned by lookupCachedBitmap).
  bool getCachedBitmapDimensions(CachedBitmap* handle, int* outWidth, int* outHeight) const;
  // Blits the cached bitmap at (x, y), aspect-fit to (maxWidth x maxHeight).
  // Re-builds the 1bpp scaled buffer when the target size changes. Returns
  // false if the path can't be loaded, the bitmap is empty, or it's fully
  // clipped off-screen.
  //
  // `Opaque=false` (default) writes ONLY black-source pixels — leaves
  // whatever's already in the framebuffer where the source is white. Lets
  // a drop-shadow underneath show through. `Opaque=true` writes both
  // inks, so the caller can skip the white-substrate fillRect.
  //
  // `cornerRadius` > 0 carves rounded corners directly during the blit:
  // pixels in the corner-skip table (same `dx² + dy² > r²` test as
  // maskRoundedRectOutsideCorners) are left untouched, so any shadow
  // underneath remains visible at the corners.
  // `cropX, cropY` (0.0-1.0): fraction of source width/height to trim
  // before scaling. 0 = aspect-fit (default; matches the API's original
  // behaviour, scaled output may be smaller than max on one axis). >0 =
  // aspect-fill with crop -- the scaled output is exactly maxWidth x
  // maxHeight and (cropX/2, cropY/2) of the source is trimmed from each
  // side of the cropped axis. Computed by callers via
  // calculateCoverFillCrop and friends.
  template <bool Opaque = false>
  bool drawCachedBitmap(const char* path, int x, int y, int maxWidth, int maxHeight,
                        float cropX = 0.0f, float cropY = 0.0f, int cornerRadius = 0) const;
  template <bool Opaque = false>
  bool drawCachedBitmap(CachedBitmap* handle, int x, int y, int maxWidth, int maxHeight,
                        float cropX = 0.0f, float cropY = 0.0f, int cornerRadius = 0) const;
  // Drops every cached entry and resets the byte counter. Use when the
  // active book / library has changed enough that the cache contents are
  // stale, or as a low-heap escape hatch.
  void clearImageCache() const;
  // CrumBLE #131: expose budget reconciliation publicly so HomeActivity
  // (and other activities) can pre-shrink the cache on transition
  // without nuking it. The cache is normally reconciled only on insert
  // (lookupCachedBitmap path); explicit reconciliation lets activities
  // that anticipate heap pressure shrink the cache proactively.
  void reconcileImageCacheBudgetExt() const { reconcileImageCacheBudget(); }
  // Returns the current budget (post-reconciliation if you've called
  // anything that triggers it). Diagnostic; not for sizing decisions.
  size_t getImageCacheBudget() const { return imageCacheBudget_; }
  size_t getImageCacheBytes() const { return imageCacheBytes_; }
  // Trapezoidal blit used by Flow/iPod-style carousel. Fits the bitmap into a
  // bounding box of width `w` and height `max(hL, hR)` whose top-left is (x, y);
  // each output column has its own height linearly interpolated from hL on the
  // left edge to hR on the right edge, vertically centered in the bbox.
  void drawPerspectiveBitmap(const Bitmap& bitmap, int x, int y, int w, int hL, int hR) const;
  // CrumBLE Phase A perf: cached-bitmap source variant. Reads the 2bpp
  // packed pixels directly out of the CachedBitmap entry (no SD I/O,
  // no readNextRow). Used by Flow's carousel side covers so L/R
  // navigation hits RAM for previously-seen books.
  void drawPerspectiveBitmap(CachedBitmap* handle, int x, int y, int w, int hL, int hR) const;

  // CrumBLE #125: render the perspective-warped bitmap into a user-supplied
  // 1bpp packed buffer instead of the framebuffer. Mirrors the cached
  // overload's geometry exactly so a tile baked here renders identically
  // when blitted back via drawPacked1bpp at the same (w, hL, hR). Used by
  // LyraFlowTheme to pre-bake the 4 side covers of the Flow carousel
  // once at home-entry — every subsequent carousel L/R press then blits
  // those 1bpp tiles instead of re-walking ~70k source pixels per cover.
  //
  // `dst` must be at least ((w + 7) / 8) * max(hL, hR) bytes. The buffer
  // is NOT pre-cleared by this function (only set-bits are written via
  // OR), so the caller must zero-fill before the first render of a tile.
  // Output is always BW: any source pixel value < 3 (non-pure-white) sets
  // the corresponding bit. Independent of the renderer's renderMode.
  void renderPerspectiveBitmapToPacked1bpp(CachedBitmap* handle, int w, int hL, int hR, uint8_t* dst) const;
  // CrumBLE 4.5.5+: Bitmap-source variant. Streams 2bpp rows directly from
  // SD via Bitmap::readNextRow — no imageCache_ dependency, so this works
  // even when the cache budget is 0 (typical for Home after the cover-
  // snapshot pre-alloc has eaten the slack). Used by
  // LyraFlowTheme::prerenderCarouselSideTiles to populate the side-tile
  // cache reliably regardless of cache state. The Bitmap must already have
  // parseHeaders() called; this consumes the row stream so the same Bitmap
  // can only be used once unless rewound externally.
  void renderPerspectiveBitmapToPacked1bpp(const Bitmap& bitmap, int w, int hL, int hR, uint8_t* dst) const;
  // CrumBLE 4.5.5+: dual-tile variant. Bakes the LEFT-perspective and
  // RIGHT-perspective tiles for the same source bitmap in a SINGLE
  // file/row pass. The Flow carousel's prerender needs both shapes per
  // book; rendering them separately would double the SD I/O cost (re-open
  // + re-stream). Each dst buffer must be ((w + 7) / 8) * max(hL_X, hR_X)
  // bytes and zero-filled by the caller before this call.
  void renderPerspectiveBitmapToPacked1bppDual(const Bitmap& bitmap, int w,
                                                int hL_a, int hR_a, uint8_t* dst_a,
                                                int hL_b, int hR_b, uint8_t* dst_b) const;

  // CrumBLE #125: blit a 1bpp packed buffer (MSB-first per byte) to the
  // framebuffer. `srcStride` is the byte count per source row. Each set
  // bit becomes a drawPixel write at the offset (x + col, y + row); 0
  // bits are skipped (transparent overlay). Used together with
  // renderPerspectiveBitmapToPacked1bpp to consume the tile cache.
  void drawPacked1bpp(const uint8_t* src, int srcStride, int x, int y, int w, int h, bool state = true) const;

  // v18.9.9.210 Phase 2: 2bpp packed variants. Same geometry as the 1bpp
  // pair but preserve the source's 4-gray value per pixel (0=black,
  // 3=white). Used by LyraFlowTheme's Phase 2 tile cache so grayscale
  // covers render at their true tone instead of a BW threshold.
  //
  // Dst layout: 2bpp packed MSB-first per byte, 4 pixels per byte,
  // stride = (w + 3) / 4. Caller MUST pre-fill dst with 0xFF (all
  // pixels = 3 = white) -- the walk uses last-write-wins semantics on
  // pixels within colTop..colTop+colH, and pixels outside that range
  // stay at their init value.
  void renderPerspectiveBitmapToPacked2bpp(CachedBitmap* handle, int w, int hL, int hR, uint8_t* dst) const;
  void renderPerspectiveBitmapToPacked2bpp(const Bitmap& bitmap, int w, int hL, int hR, uint8_t* dst) const;

  // Blit a 2bpp packed buffer to the framebuffer. Mirrors the per-plane
  // logic of drawPerspectiveBitmap (BW mode paints val<3, GRAY_MSB
  // paints val=1|2, GRAY_LSB paints val=1). No state parameter -- the
  // pixel color is implied by (val, renderMode).
  void drawPacked2bpp(const uint8_t* src, int srcStride, int x, int y, int w, int h) const;

  // v18.9.9.211 Phase 3: aspect-fit scaled 2bpp packed tile for center
  // covers. Same math as buildScaledBitmap (cropX/cropY, xRatio/yRatio,
  // nearest-neighbor sample) but preserves the source's 4-gray value
  // per pixel into the packed dst instead of thresholding to 1bpp.
  //
  // dst layout: 2bpp packed MSB-first, stride = (dstW + 3) / 4. Caller
  // must pre-fill dst with 0xFF (all white). Buffer size must be at
  // least stride * dstH bytes.
  void renderCachedBitmapToPacked2bpp(CachedBitmap* handle, int dstW, int dstH, uint8_t* dst,
                                       float cropX = 0.0f, float cropY = 0.0f) const;
  void renderBitmapToPacked2bpp(const Bitmap& bitmap, int dstW, int dstH, uint8_t* dst,
                                 float cropX = 0.0f, float cropY = 0.0f) const;

  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer(bool turnOffScreen = false) const;
  // CrumBLE 4.5.5+: experimental ~61 ms B/W refresh for home-nav-style updates.
  // Caller must have done a full FAST/HALF refresh recently to establish a
  // clean panel baseline; see HalDisplay::displayBufferFastLut for the full
  // contract. Use only on activities that opt in (currently HomeActivity).
  void displayBufferFastLut(bool turnOffScreen = false) const;
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore the stored buffer (does NOT free it)
  // True if a compressed BW backup is currently held (either from a prior
  // storeBwBuffer() that hasn't been freed yet, or from another caller's
  // store path leaving its backup around). Callers can use this to decide
  // whether restoreBwBuffer() will produce useful framebuffer content.
  bool hasStoredBwBuffer() const { return bwCompressedBackup != nullptr; }
  // CrumBLE 4.5.7 v18.7: explicitly discard the stored backup and its heap.
  // BW backup can be up to ~32 KB (dense image page). storeBwBuffer callers
  // (drawer, dictionary word-select, grayscale render) don't free after their
  // use because restoreBwBuffer keeps it around for reuse. But before BT
  // enable those bytes are dead weight NimBLE could use. Public so the
  // reader's pre-BT cleanup can drop it explicitly.
  void discardStoredBwBuffer();
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // v18.9.9.70 (ported from crosspoint 05c1e9aa): lend the framebuffer to
  // memory-hungry phases such as section pagination. Nothing may draw/display
  // while it is released. restore returns the buffer white, so callers must
  // redraw the full screen afterward.
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
