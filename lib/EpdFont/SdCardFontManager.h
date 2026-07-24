#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the selected font file. Four-size families map the reader size step
  // onto the sorted file list; other counts fall back to closest point size.
  // Only one .cpfont file is loaded; other sizes remain on disk. This keeps
  // resident interval + kern/ligature tables to one size's worth of memory.
  // Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPointSize, uint8_t sizeStep);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded (closest match to targetPtSize).
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

  // CrumBLE 4.5.4: fontId of the currently-loaded family. 0 if nothing
  // loaded. Used by SdCardFontSystem::ensureFallbackLoaded to look up
  // the EpdFontFamily entry in the renderer's font map after a successful
  // load, then register it as the UI glyph fallback.
  int currentFontId() const { return loadedFontId_; }

  // CrumBLE 4.2: exposed for the off-device prebake WASM, which needs to
  // compute the exact same fontId the device will derive at reader-open
  // time so the manifest's baked-in fontId matches the runtime fontId
  // and the prebake's sections/.pxc both pass the fingerprint check.
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  // CrumBLE 4.5.4: shadow of loaded_.back().fontId, mirrored at load /
  // unload so accessors don't need to crack the vector layout.
  int loadedFontId_ = 0;
  std::vector<LoadedFont> loaded_;
};
