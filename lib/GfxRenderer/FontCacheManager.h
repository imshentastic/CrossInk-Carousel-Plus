#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // CrumBLE 4.2 Option 2: when true, prewarmCache trims SD-card font styleMask
  // to REGULAR (0x01). BOLD/ITALIC/BOLDITALIC glyphs load on demand through
  // EpdFontFamily::getGlyphData -> EpdFont::getGlyph -> glyphMissHandler ->
  // SdCardFont::onGlyphMiss (and cache in the per-style overflow ring buffer).
  // Used by EpubReaderActivity to switch behaviour based on whether the user
  // has a Bluetooth controller bonded: if they don't, eager prewarm of all 4
  // styles is strictly better for reading speed; if they do, the lazy path
  // leaves the post-BT-enable heap headroom needed for the page reload.
  void setSdFontLazyNonRegular(bool lazy) { sdFontLazyNonRegular_ = lazy; }
  bool sdFontLazyNonRegular() const { return sdFontLazyNonRegular_; }

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;
  std::string scanText_;
  uint32_t scanStyleCounts_[4] = {};
  int scanFontId_ = -1;

  // CrumBLE 4.2 Option 2 gate. Default false = original behaviour (eager
  // prewarm of all SD-font styles in styleMask). EpubReaderActivity flips
  // this to true when a Bluetooth controller is bonded so the post-BT
  // heap budget can fit the page DOM reload.
  bool sdFontLazyNonRegular_ = false;
};
