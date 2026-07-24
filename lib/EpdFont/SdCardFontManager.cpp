#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPointSize,
                                   uint8_t sizeStep) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  const SdCardFontFileInfo* selected = family.selectFile(targetPointSize, sizeStep);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", selected->path.c_str());
    return false;
  }

  if (!font->load(selected->path.c_str())) {
    LOG_ERR("SDMGR", "Failed to load %s", selected->path.c_str());
    delete font;
    return false;
  }

  int fontId = computeFontId(font->contentHash(), family.name.c_str(), selected->pointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, selected->path.c_str());
    delete font;
    return false;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, selected->pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u (target=%u step=%u)", selected->path.c_str(), selected->pointSize,
          fontId, font->styleCount(), targetPointSize, sizeStep);

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);

  // CrumBLE 4.5.4 Shape 3: the UI font fallback is owned by
  // SdCardFontSystem::ensureFallbackLoaded, NOT auto-set here. Reason:
  // previously this load (which the reader calls per-book-open) would
  // overwrite the user's chosen fallback whenever a book opened with a
  // different primary font, leaving the carousel/UI back at '?' on
  // CJK after first book open. Now the explicit setting in
  // SETTINGS.uiFontFallbackFamily is the only thing that touches the
  // global fallback.

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  loadedFontId_ = fontId;
  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  // CrumBLE 4.5.4 Shape 3: don't touch the global UI fallback here.
  // SdCardFontSystem::ensureFallbackLoaded is the only thing allowed to
  // change the fallback pointer. If the user has a non-fallback primary
  // and that primary unloads (book close / settings transition), the
  // fallback must survive untouched.
  //
  // CrumBLE 4.5.4 hotfix: removed renderer.clearSdCardFonts() global
  // wipe. With two managers in play (primary + fallback) each holding
  // their own entries in renderer.sdCardFonts_, a global clear from one
  // manager would orphan the OTHER manager's registration (entries
  // erased from sdCardFonts_ but still present in fontMap), causing the
  // 'Font ID NNN collides with existing font' loop the user hit. The
  // per-fontId removeFont() below already erases this manager's own
  // entries from BOTH maps -- that's all this manager owns.
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
  loadedFontId_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}
