#pragma once

// Host-side GfxRenderer shim for Phase 2C (section file build).
//
// IMPORTANT: this is a SKELETON. None of the methods are implemented yet
// beyond no-op stubs that satisfy the linker. The measurement methods
// (getTextWidth / getTextAdvanceX / getSpaceWidth / getFontAscenderSize /
// getLineHeight) MUST be made faithful to device output before any
// section_*.bin output will byte-match.
//
// The plan recorded in DESIGN.md: forward each measurement call into an
// EpdFontFamily instance that's been preloaded with the same font data
// the firmware uses on device. The vendored builtin-font headers under
// lib/EpdFont/builtinFonts/*.h are pure-data and should compile on host
// without modification; getAdvance(codepoint, styleIdx) returns integer
// advance widths that are deterministic by construction (no float math).
//
// Drawing methods (drawText, drawLine, fillRect, dither variants, etc.)
// stay as no-ops permanently -- section_*.bin stores block trees with
// (xPos, yPos), not rasterized pixels, so the layout pass never rasters.
//
// Next session pick-up plan:
//   1. Add forward declarations / forwarding bodies for the six
//      measurement methods listed above, calling into fontMap[fontId]
//      with the codepoint stream.
//   2. Wire EpdFontFamily host-build: extend tools/crumble-prebake/build.sh
//      to include lib/EpdFont/EpdFont.cpp + EpdFontFamily.cpp + the
//      builtin-font .h headers the test EPUB needs (start with the
//      default reading font -- probably bitter_14 or lexenddeca_14;
//      see src/fontIds.h for active fontId hash constants).
//   3. Add main.cpp scaffolding that calls insertFont() the same way
//      src/main.cpp does for each (fontFamily, fontId) pair.
//   4. Compile-share lib/Epub/Epub/Section.cpp + Page.cpp + blocks/.
//   5. Try to build. Each linker error narrows the remaining shim
//      surface. Likely casualties: MemoryBudget, Hyphenator (already in
//      lib/Epub/), CssParser (lib/Epub/Epub/css/), ChapterHtmlSlimParser
//      (lib/Epub/Epub/parsers/).
//   6. First byte-match milestone: a single text-only chapter, default
//      settings, --device x4. If glyph metrics match, byte-identical
//      section file is achievable. If they diverge, that's where the
//      next investigation focuses.

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class GfxRenderer {
 public:
  // Enums + types pulled in by DirectPixelWriter.h (via ImageBlock.cpp).
  // The layout pass doesn't touch any of these at runtime, but they have
  // to be reachable through the GfxRenderer namespace for the header to
  // parse + the file to compile.
  enum Orientation {
    Portrait,
    LandscapeClockwise,
    PortraitInverted,
    LandscapeCounterClockwise,
  };
  enum RenderMode { BW, GRAYSCALE_MSB, GRAYSCALE_LSB };

  GfxRenderer() = default;
  ~GfxRenderer() = default;

  // --- Population (called from main.cpp during startup, mirroring
  // src/main.cpp's insertFont() loop) ---
  void insertFont(int fontId, EpdFontFamily font) {
    fontMap.emplace(fontId, std::move(font));
  }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }

  // --- Viewport (set from CLI --device / --viewport) ---
  void setViewport(int w, int h) { screenWidth_ = w; screenHeight_ = h; }
  int getScreenWidth() const { return screenWidth_; }
  int getScreenHeight() const { return screenHeight_; }
  // Physical display geometry queries used by DirectPixelWriter. Layout
  // pass doesn't actually use these (no rasterization), but ImageBlock
  // metadata reads might call them defensively. Match the viewport.
  int getDisplayWidth() const { return screenWidth_; }
  int getDisplayHeight() const { return screenHeight_; }
  int getDisplayWidthBytes() const { return (screenWidth_ + 7) / 8; }
  Orientation getOrientation() const { return LandscapeClockwise; }
  RenderMode getRenderMode() const { return BW; }
  // Framebuffer pointer is only consumed by drawing code that we no-op.
  // Returning nullptr is safe because the layout chain doesn't deref it.
  uint8_t* getFrameBuffer() const { return nullptr; }

  // --- Measurement (MUST produce device-identical values; not yet wired) ---
  int getTextWidth(int fontId, const char* text,
                   EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp,
                      EpdFontFamily::Style style) const;
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp,
                 EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;

  // --- Memory / image-suppression (no-op on host; we always have heap) ---
  bool suppressImages() const { return false; }
  void setSuppressImages(bool) {}
  void markImageRepaintUnsafe() const {}
  void markRenderStarved() const {}
  bool releaseSdCardFontForLowMemory(int) const { return false; }
  // Both ensureSdCardFontReady overloads (single string + vector<string>)
  // are no-ops on host -- we never use SD-resident fonts.
  void ensureSdCardFontReady(int, const char*, uint8_t) const {}
  void ensureSdCardFontReady(int, const std::vector<std::string>&, bool, uint8_t) const {}
  // No SD fonts on host -> no font is an SD font.
  bool isSdCardFont(int) const { return false; }

  // --- Drawing (permanent no-ops -- section build doesn't rasterize) ---
  // We declare these inline-no-op so the linker is satisfied when
  // ChapterHtmlSlimParser / TextBlock / ImageBlock reach for them
  // during layout. The actual device-side drawing paths are unused.
  void drawPixel(int, int, bool) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style) const {}
  void drawLine(int, int, int, int, bool) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void drawRect(int, int, int, int, bool) const {}
  void fillRect(int, int, int, int, bool) const {}
  // ... add more no-op draw stubs as the linker complains about them.

 private:
  std::map<int, EpdFontFamily> fontMap;
  int screenWidth_ = 800;   // X4 landscape default; --viewport overrides
  int screenHeight_ = 480;
};
