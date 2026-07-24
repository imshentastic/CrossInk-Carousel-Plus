#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>
#include <string>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Temporarily unload the active SD font without clearing the saved setting.
  /// Call ensureLoaded() later to restore it before reader rendering.
  void releaseLoadedFont(GfxRenderer& renderer);

  /// CrumBLE 4.5.4: load SETTINGS.uiFontFallbackFamily as the UI glyph
  /// fallback. Lives on its own SdCardFontManager (independent of the
  /// reader's primary font) so it stays resident across book opens /
  /// settings activities / FT mode and is registered BEFORE the first
  /// home render. Pass the loaded family pointer to
  /// EpdFontFamily::setUiFallbackFamily so any in-family glyph miss
  /// falls back to this family before returning REPLACEMENT_GLYPH.
  /// No-op if the setting is empty or names a family that's not on SD.
  /// Re-callable -- detects no-op when the active fallback already
  /// matches the setting.
  void ensureFallbackLoaded(GfxRenderer& renderer);

  /// CrumBLE 4.5.4 task #5C: do the actual fallback load work, bypassing
  /// both the suppressed-flag and the lazy-armed early-returns that
  /// ensureFallbackLoaded() applies. Invoked by runLazyFallback() when
  /// the first UI glyph miss fires the one-shot loader. Public so the
  /// global runLazyFallback() helper in the .cpp can reach it; not part
  /// of the documented API surface.
  void performFallbackLoad(GfxRenderer& renderer);

  /// CrumBLE 4.5.4 task #5C: arm a one-shot lazy loader instead of
  /// loading the fallback at boot. The fallback only loads when the
  /// first glyph miss in the UI render path fires (via EpdFontFamily::
  /// uiFallbackFamily()'s lazy hook). Non-CJK users never pay the
  /// ~15-25 KB resident cost; CJK users pay it the first time they
  /// render a non-Latin char (carousel title, book name, etc.).
  /// Pass the renderer that the eventual load will use -- captured so
  /// the loader callback (which is parameterless per the C-callback
  /// signature) has access.
  void registerLazyFallback(GfxRenderer& renderer);

  /// CrumBLE 4.5.4: explicit teardown of the UI fallback. Used by
  /// CrossPointWebServerActivity::onEnter to reclaim the ~15-25 KB
  /// resident fallback payload during FT mode (web server doesn't
  /// need CJK glyph rendering). Pairs with setFallbackSuppressed(true)
  /// so the per-tick poll doesn't immediately re-load it.
  void releaseFallback(GfxRenderer& renderer);

  /// When true, ensureFallbackLoaded() becomes a no-op. Set on FT
  /// entry, cleared on FT exit. The per-tick poll in main.cpp uses
  /// ensureFallbackLoaded(), so without this flag the fallback would
  /// reload one render tick after release.
  void setFallbackSuppressed(bool suppressed) { fallbackSuppressed_ = suppressed; }

  /// CrumBLE 4.5.116: point the UI glyph fallback at the currently-loaded
  /// primary SD font. Reader activities call this on onEnter so any CJK
  /// glyph miss in the reader UI (status bar, chapter label, drawer,
  /// menu) resolves via the reader's own primary font -- zero extra heap.
  /// Renders at the primary's point size (typically 14-20pt), which will
  /// look visibly larger than the surrounding 10-12pt UI text but beats
  /// the tofu boxes the pre-116 release+suppress path produced.
  ///
  /// Returns true if the alias was set. Returns false when no primary is
  /// loaded (user hasn't selected an SD font, or ensureLoaded hasn't run
  /// yet) -- caller should fall back to setFallbackSuppressed(true) +
  /// releaseFallback() so the per-tick poll doesn't clobber this state.
  ///
  /// Interacts with ensureLoaded(): if the primary is unloaded + reloaded
  /// (font change, reindex), the alias is broken and re-established
  /// across the swap so the fallback pointer never dangles.
  bool aliasPrimaryAsFallback(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// v18.9.9.311: reverse-lookup a fontId to (family, pointSize) by
  /// iterating installed SD fonts, peeking each file's contentHash, and
  /// recomputing candidate fontIds until one matches. Used by the
  /// prebake-fontId-rescue path: when a prebake manifest lies about
  /// which font it was baked with (crosspointreader.com pipeline bug
  /// writes sdFontFamilyName="" alongside sections that were actually
  /// baked with a different font), the section-file fingerprint has the
  /// truth. Rescue: find the installed font whose fontId matches, and
  /// swap SETTINGS.sdFontFamilyName to it.
  ///
  /// Returns true on match; fills outFamilyName + outPointSize. Cost:
  /// one small SD read per installed .cpfont file (~160 B each). Rare
  /// path -- only fires on prompt-accept when the manifest is lying.
  bool findFamilyByFontId(int32_t targetFontId, std::string& outFamilyName, uint8_t& outPointSize) const;

  /// Change the reader font size using the active SD family when one is selected.
  bool changeReaderFontSize(bool larger);

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// CrumBLE 4.5.4: return the currently-loaded SD font's point size, or
  /// 0 if no SD font is active. Consumed by /api/reader-render-info so the
  /// browser optimizer can pin the prebake to the device's exact current
  /// size (was previously guessing from fontSize index, which mismatched
  /// the device's actual resolved pt and produced a font fingerprint
  /// mismatch the user had to manually resolve).
  uint8_t currentPrimaryPointSize() const { return manager_.currentPointSize(); }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  // CrumBLE 4.5.4: independent manager for the UI fallback family. Kept
  // separate so reader load/unload of the primary doesn't disturb the
  // fallback registration (the fallback is registered at boot and stays
  // resident across activity transitions / FT mode / book opens). Name
  // cache lets ensureFallbackLoaded short-circuit when the setting
  // hasn't changed since the last load.
  SdCardFontManager fallbackManager_;
  std::string loadedFallbackFamily_;
  uint8_t loadedFallbackPt_ = 0;
  // v18.9.9.144: default true. The UI font fallback (LXGWWenKai) was
  // designed for CJK glyph support in book titles / carousel labels,
  // but CrumBLE isn't shipping CJK support -- most fork users build
  // custom .cpfonts with any needed non-Latin glyphs already embedded
  // in the primary. Keeping the fallback armed cost 5-8 KB whenever
  // it lazy-loaded, and that 5-8 KB was exactly what pushed NimBLE
  // past its heap budget on BT-connected reads.
  //
  // v18.9.9.306: default flipped to false so users can opt into a CJK
  // fallback family via Reader > Font > UI Font Fallback. Users who
  // don't set a family pay nothing (performFallbackLoad no-ops on
  // uiFontFallbackFamily=""). Users who do pay the 15-25 KB tax --
  // per user request they accept per-image suppression under BT+CJK.
  // FT mode still explicitly suppresses via setFallbackSuppressed(true)
  // to reclaim heap; no automatic un-suppress on FT exit yet, so a
  // reboot is required to restore CJK after using FT.
  bool fallbackSuppressed_ = false;
  // CrumBLE 4.5.116: when true, the UI fallback pointer is aliased to
  // the primary SD font's EpdFontFamily (owned by manager_, held in the
  // renderer's font map). ensureFallbackLoaded skips while this is set
  // -- the alias must survive per-tick polling. ensureLoaded preserves
  // it across a primary swap: clear the fallback pointer before the
  // primary unload, re-alias after the new primary loads.
  bool fallbackAliasedToPrimary_ = false;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
