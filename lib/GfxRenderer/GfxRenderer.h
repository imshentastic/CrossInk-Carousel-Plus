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
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
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

  // CrumBLE #125: blit a 1bpp packed buffer (MSB-first per byte) to the
  // framebuffer. `srcStride` is the byte count per source row. Each set
  // bit becomes a drawPixel write at the offset (x + col, y + row); 0
  // bits are skipped (transparent overlay). Used together with
  // renderPerspectiveBitmapToPacked1bpp to consume the tile cache.
  void drawPacked1bpp(const uint8_t* src, int srcStride, int x, int y, int w, int h, bool state = true) const;
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
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore the stored buffer (does NOT free it)
  // True if a compressed BW backup is currently held (either from a prior
  // storeBwBuffer() that hasn't been freed yet, or from another caller's
  // store path leaving its backup around). Callers can use this to decide
  // whether restoreBwBuffer() will produce useful framebuffer content.
  bool hasStoredBwBuffer() const { return bwCompressedBackup != nullptr; }
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

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
