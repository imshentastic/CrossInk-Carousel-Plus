#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, UNDERLINE = 4, STRIKETHROUGH = 8 };
  struct GlyphData {
    const EpdFontData* fontData;
    const EpdGlyph* glyph;
  };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  GlyphData findGlyphData(uint32_t cp, Style style = REGULAR) const;
  GlyphData getGlyphData(uint32_t cp, Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  uint32_t getFallbackCodepoint(uint32_t cp, Style style = REGULAR) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;

  // CrumBLE 4.3: per-section embedded glyph subset. When the active Section
  // has installed a glyph subset block, the reader sets a non-null pointer
  // for each style that was prebaked in. findGlyphData consults these
  // pointers FIRST -- if a codepoint is covered by the embedded subset,
  // the glyph is returned directly from the in-RAM block with zero SD
  // reads + zero SdCardFont miniData allocation. Codepoints NOT in the
  // embedded subset (user notes, dictionary overlays, etc.) fall through
  // to the existing interval-search + glyphMissHandler path.
  //
  // Lifetime: the pointed-at EpdFontData is owned by Section::embeddedStyles_.
  // The reader calls setEmbeddedGlyphData() before rendering a section and
  // clearEmbeddedGlyphData() after (or when switching sections). Reentering
  // a section re-installs (idempotent).
  //
  // Thread-safety: single-threaded render path; no contention.
  void setEmbeddedGlyphData(const EpdFontData* regularData, const EpdFontData* boldData,
                             const EpdFontData* italicData, const EpdFontData* boldItalicData) {
    embeddedDataByStyle_[REGULAR] = regularData;
    embeddedDataByStyle_[BOLD] = boldData;
    embeddedDataByStyle_[ITALIC] = italicData;
    embeddedDataByStyle_[BOLD_ITALIC] = boldItalicData;
  }
  void clearEmbeddedGlyphData() {
    for (int i = 0; i < 4; ++i) embeddedDataByStyle_[i] = nullptr;
  }
  bool hasEmbeddedGlyphData() const {
    for (int i = 0; i < 4; ++i)
      if (embeddedDataByStyle_[i] != nullptr) return true;
    return false;
  }

  // CrumBLE 4.5.4 UI font fallback. A single SD-card family can be
  // registered as the fallback for all built-in UI fonts. When any
  // EpdFontFamily's glyph lookup fails (in-family + typography
  // substitutions all returned nothing), getGlyphData() consults the
  // registered fallback BEFORE falling back to REPLACEMENT_GLYPH ('?').
  // Lets a Latin built-in UI font render CJK book titles, settings labels,
  // collection names, etc. when the user has loaded a CJK SD font for
  // reading. The fallback never replaces glyphs the primary font already
  // covers -- only fills in misses.
  //
  // Owner is SdCardFontSystem -- it calls setUiFallbackFamily() with the
  // currently-loaded SD family on load, and nullptr on release. The
  // pointer must outlive any rendering that might touch it; on font
  // unload the system MUST clear this before deallocating the family.
  static void setUiFallbackFamily(const EpdFontFamily* family);
  static const EpdFontFamily* uiFallbackFamily();

  // CrumBLE 4.5.4 task #5C: lazy-load hook for the UI fallback. When
  // set AND uiFallbackFamily_ is null, the next glyph-miss in a UI
  // render path triggers the loader callback, which loads + registers
  // the SD-card font and sets uiFallbackFamily_. Subsequent renders
  // use the resident pointer normally -- the hook fires exactly once.
  // Purpose: avoid the ~15-25 KB resident cost at boot for users who
  // never render CJK content (per field report: heap fragmentation
  // at FT entry was dominated by the pre-loaded LXGW @14pt fallback).
  // Owner is SdCardFontSystem; callback runs on the render task so
  // it must not block on user input.
  using LazyFallbackLoader = void (*)();
  static void setLazyFallbackLoader(LazyFallbackLoader loader);

  // CrumBLE 4.5.4 task #5C+: built-in fallback. Last-resort lookup after
  // BOTH the primary font AND the UI fallback miss. Points at a built-in
  // family with broad coverage (Bitter for Latin). Used when an SD CJK
  // font is the primary -- LXGW etc. often ship CJK-only, so Latin /
  // digits / punctuation in book text would render as '?' without this
  // fallback. Owner is main.cpp / setupDisplayAndFonts which installs it
  // alongside the font registration. Set once at boot, never cleared.
  static void setBuiltInFallbackFamily(const EpdFontFamily* family);
  static const EpdFontFamily* builtInFallbackFamily();

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;
  // CrumBLE 4.3: see setEmbeddedGlyphData() above. Index = Style enum value
  // (0=REGULAR, 1=BOLD, 2=ITALIC, 3=BOLD_ITALIC). nullptr when no embedded
  // subset is installed for that style; in that case the glyph lookup uses
  // the regular EpdFont* interval table + miss-handler path.
  const EpdFontData* embeddedDataByStyle_[4] = {nullptr, nullptr, nullptr, nullptr};

  const EpdFont* getFont(Style style) const;
};
