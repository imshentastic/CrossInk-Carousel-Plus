#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path.
  //
  // CrumBLE 4.2 Option 2: only eagerly prewarm REGULAR for SD-card fonts.
  // BOLD / ITALIC / BOLDITALIC styles' miniData stays empty; their glyphs
  // load on demand via the overflow ring buffer when drawText resolves
  // them through EpdFontFamily::getGlyphData -> EpdFont::getGlyph ->
  // glyphMissHandler -> SdCardFont::onGlyphMiss.
  //
  // Why: eagerly prewarming all 4 styles allocates ~10-15 KB of miniData
  // per style (intervals + glyph metadata + bitmaps), so a page mixing
  // regular/bold/italic text used to hold ~40-60 KB during the render
  // scope. With NimBLE's 58 KB allocation pass landing in the middle of
  // a heap that's already churning that much per page, BT enable was
  // failing its 67.5 KB free-heap pre-flight. Trimming to REGULAR-only
  // (~14 KB) frees ~30-45 KB of headroom for BT.
  //
  // Trade-off: each bold/italic glyph hit triggers a per-glyph SD read
  // (~1-2 ms) the first time it's needed. Typical book pages have
  // <20 bold/italic words = <100 bold glyphs, so the per-render cost is
  // ~100-200 ms. The overflow ring buffer caches loaded glyphs so
  // repeated re-renders of the same page reuse them at no SD cost.
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // CrumBLE 4.2 Option 2 gate. The lazy-non-REGULAR path is only on when
    // the user has a Bluetooth controller bonded (see
    // EpubReaderActivity::onEnter): they're willing to pay the per-glyph SD
    // read for bold/italic in exchange for enough free heap to survive a
    // BT enable + page reload. Users without a bonded BT controller stay
    // on the original eager-all-styles prewarm.
    constexpr uint8_t kRegularOnlyMask = 0x01;
    const uint8_t sdMask = sdFontLazyNonRegular_ ? (styleMask & kRegularOnlyMask) : styleMask;
    int missed = it->second->prewarm(utf8Text, sdMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X, originalMask=0x%02X, lazy=%d)", missed,
              sdMask, styleMask, sdFontLazyNonRegular_);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  // Reverse iteration is harmless now; the decompressor keeps one retained page slot per style.
  for (int8_t i = 3; i >= 0; i--) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  scanText_ += text;
  if (scanFontId_ < 0) scanFontId_ = fontId;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  scanStyleCounts_[baseStyle] += cpCount;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanText_.clear();
  manager_->scanText_.reserve(2048);  // Pre-allocate to avoid heap fragmentation from repeated concat
  memset(manager_->scanStyleCounts_, 0, sizeof(manager_->scanStyleCounts_));
  manager_->scanFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanText_.empty()) return;

  // Build style bitmask from all styles that appeared during the scan
  uint8_t styleMask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (manager_->scanStyleCounts_[i] > 0) styleMask |= (1 << i);
  }
  if (styleMask == 0) styleMask = 1;  // default to regular

  manager_->prewarmCache(manager_->scanFontId_, manager_->scanText_.c_str(), styleMask);

  // Keep the grown capacity around so the next page can reuse it without
  // another allocate-grow-shrink cycle.
  manager_->scanText_.clear();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanText_ is empty)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
