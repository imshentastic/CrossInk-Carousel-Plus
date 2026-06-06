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
