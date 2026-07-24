#include "SdCardFontSystem.h"

#include <SdCardFont.h>       // v18.9.9.311: peekContentHash for findFamilyByFontId
#include <SdCardFontManager.h>

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.getSdFontTargetPointSize(), SETTINGS.fontSize)) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

namespace {
// CrumBLE 4.5.4 task #5C: the lazy-loader callback is a parameterless
// function pointer (per EpdFontFamily::LazyFallbackLoader's C-style
// signature for static-init safety). We need a renderer reference to
// actually do the load, so stash a pointer here at registerLazyFallback
// time. Single global, single-instance system; threading isn't a concern
// (renderer is owned by main task that registered it).
GfxRenderer* gLazyRenderer = nullptr;
SdCardFontSystem* gLazySystem = nullptr;
void runLazyFallback() {
  if (gLazyRenderer && gLazySystem) {
    // CrumBLE 4.5.4 fix: call performFallbackLoad, NOT ensureFallbackLoaded.
    // The latter has a lazy-mode early-return guard (returns immediately
    // when fallback==null AND lazy is armed) that would no-op THIS very
    // call -- the loader fires but loads nothing, fallback stays null,
    // and CJK glyphs render as '?'. The performFallbackLoad helper skips
    // both that guard and the suppressed-flag guard, doing the real load.
    gLazySystem->performFallbackLoad(*gLazyRenderer);
  }
}
}  // namespace

void SdCardFontSystem::registerLazyFallback(GfxRenderer& renderer) {
  // v18.9.9.178: UI font fallback was RETIRED. The load cost ~8-9 KB per
  // session on first glyph miss (missing punctuation like em-dash / curly
  // quote / ellipsis triggered a full SD font load), and heap on old
  // NimBLE pools was so tight we couldn't afford it.
  //
  // v18.9.9.288: KEEP-IF-FIT. Post-v281 NimBLE shrink + v285 deinit(false)
  // recovered ~40 KB heap. We can now afford the fallback in healthy-heap
  // sessions. The `performFallbackLoad` gate (kFallbackLoadMinMaxAlloc,
  // bumped to 25 KB below) refuses to load when heap is tight, so tight
  // sessions (post-BT reading, FT upload) still degrade to tofu boxes
  // -- identical behavior to v178+. Healthy sessions (home / reader
  // without BT / post-defrag reader) now get real CJK / em-dash /
  // curly quote / ellipsis rendering.
  //
  // The setting `uiFontFallbackFamily` was previously force-cleared here
  // so user preferences never took effect. That force-clear is removed --
  // whatever the user picked in Settings > Display > UI Font Fallback
  // now sticks and drives the lazy loader.
  gLazyRenderer = &renderer;
  gLazySystem = this;
  if (SETTINGS.uiFontFallbackFamily[0] == '\0') {
    LOG_INF("SDFS", "UI font fallback: no family set (Settings > Display > UI Font Fallback)");
    return;
  }
  EpdFontFamily::setLazyFallbackLoader(runLazyFallback);
  LOG_INF("SDFS", "UI font fallback: lazy loader armed for '%s' (maxAlloc-gated at load time)",
          SETTINGS.uiFontFallbackFamily);
}

void SdCardFontSystem::releaseFallback(GfxRenderer& renderer) {
  EpdFontFamily::setUiFallbackFamily(nullptr);
  fallbackManager_.unloadAll(renderer);
  loadedFallbackFamily_.clear();
  loadedFallbackPt_ = 0;
  // CrumBLE 4.5.116: if a prior alias-to-primary was active, releasing
  // clears its bookkeeping too. The alias never touched fallbackManager_
  // so unloadAll above was a no-op for that state -- but we must clear
  // the flag so a later per-tick ensureFallbackLoaded actually runs the
  // user's real fallback load.
  fallbackAliasedToPrimary_ = false;
  LOG_INF("SDFS", "UI font fallback: released");
  // CrumBLE 4.5.4 fix: re-arm the lazy loader. Without this, the next
  // home render after FT exits has both gUiFallback=null AND gLazyLoader
  // =null (the loader fired once during the pre-FT home render and is
  // one-shot). Result: CJK title text in carousel/collection name
  // surfaces renders as '?' until the user power-cycles the device.
  // Re-arming costs nothing (loader stays armed until the next miss
  // actually fires it again).
  if (SETTINGS.uiFontFallbackFamily[0] != '\0') {
    EpdFontFamily::setLazyFallbackLoader(runLazyFallback);
    LOG_INF("SDFS", "UI font fallback: re-armed lazy loader after release");
  }
}

bool SdCardFontSystem::aliasPrimaryAsFallback(GfxRenderer& renderer) {
  // CrumBLE 4.5.116: point the UI fallback at the primary's EpdFontFamily
  // in the renderer's font map. Zero extra heap (no fallbackManager_
  // load), CJK-in-UI works iff the primary has CJK glyphs. Called by
  // reader activities' onEnter after ensureLoaded has put the primary
  // in place.
  if (manager_.currentFamilyName().empty()) {
    LOG_INF("SDFS", "UI font fallback: alias-to-primary skipped -- no primary loaded");
    return false;
  }
  // Tear down any resident real fallback first -- both to reclaim its
  // ~10 KB and to release the fallback pointer before we reassign it
  // (a dangling pointer to a freed fallbackManager_ entry would crash
  // on the next UI glyph miss).
  EpdFontFamily::setUiFallbackFamily(nullptr);
  fallbackManager_.unloadAll(renderer);
  loadedFallbackFamily_.clear();
  loadedFallbackPt_ = 0;

  const int primaryFontId = manager_.currentFontId();
  auto& fontMap = renderer.getFontMap();
  auto it = fontMap.find(primaryFontId);
  if (it == fontMap.end()) {
    LOG_ERR("SDFS", "UI font fallback: alias-to-primary FAILED -- fontId=%d not in font map",
            primaryFontId);
    fallbackAliasedToPrimary_ = false;
    return false;
  }
  EpdFontFamily::setUiFallbackFamily(&it->second);
  fallbackAliasedToPrimary_ = true;
  LOG_INF("SDFS", "UI font fallback: aliased to primary '%s' @%upt (fontId=%d, 0 KB extra)",
          manager_.currentFamilyName().c_str(), manager_.currentPointSize(), primaryFontId);
  return true;
}

void SdCardFontSystem::ensureFallbackLoaded(GfxRenderer& renderer) {
  // CrumBLE 4.5.4: suppressed during FT mode (web server doesn't need
  // CJK glyph rendering, and the ~15-25 KB resident fallback was the
  // dominant heap-fragmentation contributor on busy devices). Per-tick
  // poll keeps calling this; we just no-op while the flag is set.
  if (fallbackSuppressed_) return;
  // CrumBLE 4.5.116: reader has aliased the primary as fallback -- per-
  // tick poll must not clobber it with the user's real fallback family
  // (which would allocate ~10 KB, defeating the whole point of the alias).
  if (fallbackAliasedToPrimary_) return;
  // CrumBLE 4.5.4 task #5C: lazy-armed mode. The boot path registered
  // a lazy loader instead of loading; the per-tick poll still calls
  // ensureFallbackLoaded but we MUST no-op here so the lazy state
  // survives. The actual load fires on first glyph miss in
  // EpdFontFamily::uiFallbackFamily(), which calls performFallbackLoad
  // directly (bypassing this guard) via runLazyFallback. Per-tick
  // poll path goes through ensureFallbackLoaded and skips while the
  // loader is still armed -- otherwise per-tick would race the lazy
  // trigger and load eagerly, defeating the whole point.
  if (EpdFontFamily::uiFallbackFamily() == nullptr && gLazyRenderer != nullptr) return;
  performFallbackLoad(renderer);
}

// Extracted in 4.5.4: the lazy trigger needs to do the actual load
// WITHOUT the ensureFallbackLoaded early-returns (suppressed flag,
// lazy-armed sentinel) -- those are correct for the per-tick poll
// path but block the real load. Both ensureFallbackLoaded() and
// runLazyFallback() funnel through this body.
void SdCardFontSystem::performFallbackLoad(GfxRenderer& renderer) {
  const char* wanted = SETTINGS.uiFontFallbackFamily;
  const uint8_t wantedPt = SETTINGS.uiFontFallbackPointSize;
  // No-op when family matches AND (wantedPt is auto-0 OR matches exactly).
  // Critical: wantedPt=0 means "auto, smallest available" -- it must NOT
  // be compared literally against the loaded pt (which is the resolved
  // numeric size, e.g. 14). Without the (wantedPt == 0) escape this
  // dirty-check ALWAYS mismatched in the auto case, so the per-tick poll
  // unloaded + reloaded the fallback every render frame, allocated ~15
  // KB transient + fragmented the heap into the ~9 KB maxAlloc ceiling
  // that wedged shelf-resolve (and triggered the xtask-priority-inherit
  // panic under sustained heap pressure).
  if (loadedFallbackFamily_ == wanted && (wantedPt == 0 || loadedFallbackPt_ == wantedPt)) return;

  // Clear FIRST so any active fallback pointer goes null before we touch
  // the underlying EpdFontFamily entry (unloadAll erases the map entry
  // that the static fallback ptr might be pointing at).
  EpdFontFamily::setUiFallbackFamily(nullptr);
  fallbackManager_.unloadAll(renderer);
  loadedFallbackFamily_.clear();
  loadedFallbackPt_ = 0;

  if (wanted[0] == '\0') {
    LOG_INF("SDFS", "UI font fallback: disabled (setting empty)");
    return;
  }
  const auto* family = registry_.findFamily(wanted);
  if (family == nullptr) {
    LOG_INF("SDFS", "UI font fallback: family '%s' not present on SD -- skipping", wanted);
    return;
  }
  // Pick size: if user set uiFontFallbackPointSize and it's present in the
  // family, honor it; otherwise default to the smallest available size
  // (preserves the legacy "match Latin as closely as possible" behavior).
  const auto& sizes = family->availableSizes();
  if (sizes.empty()) {
    LOG_INF("SDFS", "UI font fallback: family '%s' has no sizes -- skipping", wanted);
    return;
  }
  uint8_t fallbackPt = sizes.front();
  if (wantedPt > 0) {
    bool found = false;
    for (const uint8_t s : sizes) {
      if (s == wantedPt) {
        fallbackPt = wantedPt;
        found = true;
        break;
      }
    }
    if (!found) {
      LOG_INF("SDFS", "UI font fallback: requested %upt not in '%s'; using %upt (smallest)", wantedPt, wanted, fallbackPt);
    }
  }
  // CrumBLE 4.5.4 hotfix: if the user set the same family for primary AND
  // fallback, they compute the same fontId -- loadFamily would fail with
  // 'Font ID NNN collides' and we'd loop on the dirty check. Reuse the
  // primary's already-loaded EpdFontFamily directly instead of trying to
  // load a second copy. Only the EXACT same family+pt combo qualifies for
  // reuse; a different size on the same family still falls through to a
  // fresh load (different fontId, no collision).
  if (manager_.currentFamilyName() == wanted && manager_.currentPointSize() == fallbackPt) {
    const int primaryFontId = manager_.getFontId(wanted);
    auto& fontMap = renderer.getFontMap();
    auto it = fontMap.find(primaryFontId);
    if (it != fontMap.end()) {
      EpdFontFamily::setUiFallbackFamily(&it->second);
      loadedFallbackFamily_ = wanted;
      loadedFallbackPt_ = fallbackPt;
      LOG_INF("SDFS", "UI font fallback: aliased to primary '%s' @%upt (fontId=%d)", wanted, fallbackPt, primaryFontId);
      return;
    }
  }
  // v18.9.9.153/154: heap pre-flight. loadFamily allocates a full SdCardFont
  // (~2-8 KB). Under post-BT tight heap, the alloc fails but leaves ~800 B
  // of fragmentation from partial sub-allocs -- same pattern as prewarmStyle
  // (v152). Skip cleanly so downstream PageLine allocs (256 B floor) don't
  // get starved.
  //
  // v18.9.9.154: do NOT re-arm the lazy loader. v153 re-armed and got hit
  // ~5000 times per render (EpdFontFamily::uiFallbackFamily() fires the
  // loader on every glyph miss when fallback is null) -- 3x slower page
  // turns. Match the original loadFamily FAILED path: log, return, done.
  // Fallback stays absent for the session; non-Latin glyphs render as
  // boxes until the user restarts. Same trade-off the existing FAILED
  // path already accepts.
  // v18.9.9.288: bumped from 6144 to 25 KB now that keep-if-fit is live
  // (registerLazyFallback un-retired). 6 KB let the pre-flight pass but
  // the actual load then peaked at ~9 KB + 2-3 KB churn = OOM on tight
  // heap. 25 KB pre-flight leaves ~15 KB post-load, which is comfortable
  // and matches the free-heap-bypass logic elsewhere. Skip cleanly on
  // tight sessions (post-BT reading, FT upload) -- same tofu-glyph
  // fallback the v178-v287 line shipped.
  constexpr uint32_t kFallbackLoadMinMaxAlloc = 25 * 1024;
  const uint32_t maxAllocPre = ESP.getMaxAllocHeap();
  if (maxAllocPre < kFallbackLoadMinMaxAlloc) {
    LOG_INF("SDFS",
            "UI font fallback: loadFamily('%s' @%upt) skipped pre-flight -- "
            "maxAlloc=%u < needed=%u (fallback absent this session; glyphs render as tofu)",
            wanted, fallbackPt, maxAllocPre, kFallbackLoadMinMaxAlloc);
    return;
  }
  if (!fallbackManager_.loadFamily(*family, renderer, fallbackPt, 0)) {
    LOG_ERR("SDFS", "UI font fallback: loadFamily('%s' @%upt) FAILED", wanted, fallbackPt);
    return;
  }
  const int fbFontId = fallbackManager_.currentFontId();
  auto& fontMap = renderer.getFontMap();
  auto it = fontMap.find(fbFontId);
  if (it == fontMap.end()) {
    LOG_ERR("SDFS", "UI font fallback: loaded but family lookup by id=%d returned null", fbFontId);
    fallbackManager_.unloadAll(renderer);
    return;
  }
  EpdFontFamily::setUiFallbackFamily(&it->second);
  loadedFallbackFamily_ = wanted;
  loadedFallbackPt_ = fallbackPt;
  LOG_INF("SDFS", "UI font fallback: '%s' @%upt loaded (fontId=%d)", wanted, fallbackPt, fbFontId);
}

namespace {
// CrumBLE 4.5.116: before any manager_.unloadAll() during an alias-to-
// primary lifetime (reader session), clear the global UI fallback
// pointer so it doesn't dangle at the freed EpdFontFamily entry. Flag
// stays set so re-alias fires after the new primary loads.
void breakAliasPointerIfActive(bool aliased) {
  if (aliased) EpdFontFamily::setUiFallbackFamily(nullptr);
}
}  // namespace

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t targetPointSize = SETTINGS.getSdFontTargetPointSize();
  const uint8_t sizeStep = SETTINGS.fontSize;

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      breakAliasPointerIfActive(fallbackAliasedToPrimary_);
      manager_.unloadAll(renderer);
      // No primary to re-alias to; drop the intent.
      fallbackAliasedToPrimary_ = false;
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      breakAliasPointerIfActive(fallbackAliasedToPrimary_);
      manager_.unloadAll(renderer);
      fallbackAliasedToPrimary_ = false;
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* wantedFile = family->selectFile(targetPointSize, sizeStep);
    uint8_t wantedPt = wantedFile ? wantedFile->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u step %u)%s", wantedFamily, manager_.currentPointSize(),
            wantedPt, targetPointSize, sizeStep, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    breakAliasPointerIfActive(fallbackAliasedToPrimary_);
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, targetPointSize, sizeStep)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
      // CrumBLE 4.5.116: re-establish the primary-alias fallback if the
      // reader still wants it. The new EpdFontFamily lives at a new spot
      // in the renderer's font map, so we look it up fresh and re-set
      // the global fallback pointer.
      if (fallbackAliasedToPrimary_) {
        const int primaryFontId = manager_.currentFontId();
        auto& fontMap = renderer.getFontMap();
        auto it = fontMap.find(primaryFontId);
        if (it != fontMap.end()) {
          EpdFontFamily::setUiFallbackFamily(&it->second);
          LOG_INF("SDFS", "UI font fallback: re-aliased to new primary '%s' @%upt (fontId=%d)",
                  wantedFamily, manager_.currentPointSize(), primaryFontId);
        } else {
          LOG_ERR("SDFS", "UI font fallback: re-alias failed -- new primary fontId=%d not in map", primaryFontId);
          fallbackAliasedToPrimary_ = false;
        }
      }
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      fallbackAliasedToPrimary_ = false;
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    fallbackAliasedToPrimary_ = false;
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
  if (manager_.currentFamilyName().empty()) return;

  const std::string familyName = manager_.currentFamilyName();
  (void)familyName;
  // CrumBLE 4.5.116: same alias-dangling protection as ensureLoaded.
  // Callers (OPDS/KOR/FT pre-network teardown) always pair this with
  // setFallbackSuppressed(true) so no auto-reload attempt will re-alias
  // to a non-loaded primary. Just drop the intent cleanly.
  breakAliasPointerIfActive(fallbackAliasedToPrimary_);
  fallbackAliasedToPrimary_ = false;
  manager_.unloadAll(renderer);
  LOG_DBG("SDFS", "Released SD card font before low-memory operation: %s", familyName.c_str());
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::findFamilyByFontId(int32_t targetFontId, std::string& outFamilyName,
                                          uint8_t& outPointSize) const {
  // See header docstring. Walk every installed family + file, peek at each
  // .cpfont's contentHash (cheap: 160 B max), compute the candidate fontId
  // via the same FNV-1a construction SdCardFontManager::computeFontId uses
  // for live loads. First match wins. Zero-fontId candidates (extremely
  // rare -- FNV collision with the "not found" sentinel that computeFontId
  // remaps to 1) are still checked against target via the remap.
  const auto& families = registry_.getFamilies();
  for (const auto& family : families) {
    for (const auto& file : family.files) {
      uint32_t contentHash = 0;
      if (!SdCardFont::peekContentHash(file.path.c_str(), contentHash)) {
        continue;  // corrupt / bad-format file; skip
      }
      const int candidateId =
          SdCardFontManager::computeFontId(contentHash, family.name.c_str(), file.pointSize);
      if (candidateId == targetFontId) {
        outFamilyName = family.name;
        outPointSize = file.pointSize;
        return true;
      }
    }
  }
  return false;
}

bool SdCardFontSystem::changeReaderFontSize(const bool larger) {
  refreshIfDirty();

  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      const auto sizes = family->availableSizes();
      if (sizes.size() > 1) {
        uint8_t current = SETTINGS.fontSize < sizes.size() ? SETTINGS.fontSize : static_cast<uint8_t>(sizes.size() - 1);
        if (larger) {
          current = static_cast<uint8_t>((current + 1) % sizes.size());
        } else {
          current = current == 0 ? static_cast<uint8_t>(sizes.size() - 1) : static_cast<uint8_t>(current - 1);
        }
        SETTINGS.fontSize = current;
        return true;
      }
    }
  }

  return SETTINGS.changeReaderFontSize(larger);
}
