# crumble-prebake — off-device EPUB cache prebake CLI

## Goal

Ship a desktop binary that pre-bakes the per-book cache state CrumBLE's
on-device XHTML pipeline writes after first open:

- `book.bin` — BookMetadataCache (spine, TOC, cover href, language,
  cumulative spine sizes).
- `sections/<spineIdx>.bin` — per-chapter pre-laid-out pages, for a
  default font / viewport preset.
- `css_rules.cache` — CssParser rule table for the same preset.
- `thumb_<W>x<H>.bmp` — cover thumbnails at common UI sizes.

The device picks up these files unchanged through the existing
`Epub::load` cache-hit path and `Section::loadSectionFile` cache-hit
path. **No firmware changes are required** — the prebake tool produces
byte-identical artifacts to what the device would generate, just off
the device.

Users with large libraries run the tool once per book on a desktop,
drop the resulting `.crosspoint/` tree onto the SD card, and read with
instant first-open / instant first-chapter-visit. Users who don't
prebake see no regression: they're on the v3.7.2-equivalent on-device
path.

## Non-goals

- `.cmb` format involvement. The `.cmb` runtime path was reverted on
  2026-05-30; see `project_cmb_pivot.md` in user memory.
- Pre-baking `.pxc` page bitmaps. Separate, more invasive feature.
- Multi-preset section files. Start with one preset (device default);
  expand later if users actually change settings often.

## Strategy: share the on-device parser/builder code

Most of the on-device EPUB pipeline (`BookMetadataCache`,
`ContainerParser`, `ContentOpfParser`, `TocNcxParser`, `TocNavParser`,
`CssParser`, `ZipFile`, `Section::createSectionFile`) is mostly portable
C++. The Arduino-coupled surface is concentrated in two places:

1. **File I/O** — Storage / HalFile / FsFile, which talks to the
   device's SD card driver. Replaceable with a host shim that wraps
   `std::fstream`.
2. **Renderer** — GfxRenderer takes a HalDisplay&, uses FreeRTOS
   mutexes, and pulls in the EPD driver. Only needed for **section
   builds** (phase 2); the renderer's text-measurement APIs themselves
   are pure math over portable `EpdFontData` structs.

For each piece of firmware code we want on the host, we either:
- Compile it as-is against a `host_shim/` directory that provides
  drop-in replacements for `<Arduino.h>`, `HalStorage.h`, `HalGPIO.h`,
  `Logging.h`, FreeRTOS primitives. The shims are minimal and
  no-op-where-possible.
- For unavoidable hardware coupling (the rendering pipeline's display
  side), expose a `TextMetricsOnly` wrapper that calls only the pure
  math APIs and stubs the rest.

Tradeoff vs reimplementing parsers from scratch: shared code drifts in
lockstep with the device. If a parser bug is fixed on device, the
prebake tool gets the fix on the next rebuild. If we reimplemented, we'd
silently produce different artifacts after every firmware change.

## Iterative scope

### Phase 1 — `book.bin` only (no renderer)

Walks EPUB ZIP, parses container.xml / content.opf / NCX or NAV,
populates `BookMetadataCache`, writes `book.bin`. Identical to what
`Epub::load`'s slow path does after `bookMetadataCache->load()` fails.

**Dependencies the host shim must support:**
- Storage (exists/open-write/open-read/remove/rename/mkdir)
- HalFile (read/write/seek/position/close)
- Arduino: `millis()`, `delay()`, basic types
- expat (already host-compatible; uses libexpat from system)

**No renderer, no fonts, no CSS parser invocation, no XHTML chapter
parsing.** Tightest possible proof-of-concept.

### Phase 2 — `sections/<n>.bin` (needs renderer)

Drag in the renderer + font code via the stub-HalDisplay /
replace-FreeRTOS approach. Each chapter is parsed via
`ChapterHtmlSlimParser` against a `TextMetricsOnly` renderer that
returns identical line-width measurements to the device, producing
identical section files.

### Phase 3 — `css_rules.cache`

Run the CssParser the same way. Probably trivial once phase 2 is done.

### Phase 4 — `thumb_<W>x<H>.bmp`

Decode + resize cover images using the device's
`JpegToBmpConverter` / `PngToBmpConverter`. Hard-coded common sizes.

## Directory layout

```
tools/crumble-prebake/
  DESIGN.md                         # this file
  build.sh                          # plain g++/clang script; mirrors test/run_cmb_roundtrip_test.sh
  src/
    main.cpp                        # CLI argument parsing + per-book dispatch
    prebake.cpp                     # high-level "prebake one EPUB" logic
    prebake.h
  host_shim/
    Arduino.h                       # millis()/delay()/String/byte/etc.
    HalStorage.h                    # Storage singleton + HalFile class backed by fstream
    HalStorage.cpp
    Logging.h                       # LOG_* macros -> stderr
    freertos_shims.h                # std::mutex replacement for SemaphoreHandle_t
    halgpio_shim.h                  # no-op for any GPIO calls (shouldn't be hit in our subset)
  test/
    run_phase1_test.sh              # round-trip: bake book.bin from a known EPUB, byte-compare against expected
    fixtures/
      small.epub                    # known small EPUB for tests
      expected_book.bin             # golden file
```

## Build

`tools/crumble-prebake/build.sh` runs:

```bash
g++ -std=c++20 -O2 -Wall -Wextra \
    -I host_shim \
    -I <repo-root>/lib/Epub \
    -I <repo-root>/lib/Epub/Epub \
    -I <repo-root>/lib/ZipFile \
    -I <repo-root>/lib/expat \
    -I <repo-root>/lib/FsHelpers \
    -I <repo-root>/lib/Serialization \
    src/*.cpp \
    host_shim/*.cpp \
    <repo-root>/lib/Epub/Epub/BookMetadataCache.cpp \
    <repo-root>/lib/Epub/Epub/parsers/ContainerParser.cpp \
    <repo-root>/lib/Epub/Epub/parsers/ContentOpfParser.cpp \
    <repo-root>/lib/Epub/Epub/parsers/TocNcxParser.cpp \
    <repo-root>/lib/Epub/Epub/parsers/TocNavParser.cpp \
    <repo-root>/lib/ZipFile/ZipFile.cpp \
    <repo-root>/lib/FsHelpers/FsHelpers.cpp \
    <repo-root>/lib/expat/xmlparse.c <repo-root>/lib/expat/xmltok.c <repo-root>/lib/expat/xmlrole.c \
    -lexpat \
    -o build/crumble-prebake
```

(Real script in `build.sh` resolves repo-root via `git rev-parse`,
gracefully handles macOS vs Linux compiler invocation, etc.)

## CLI shape (target for phase 1)

```
crumble-prebake [options] <epub> [<epub> ...]

Options:
  --output-dir <dir>     Write cache state into <dir>/.crosspoint/epub_<hash>/
                         instead of next to each input EPUB.
  --sd-mount <path>      Equivalent to --output-dir <sd-mount>; semantic alias
                         that makes the SD-card workflow self-documenting.
  --check                Skip books whose existing book.bin is fresh against the
                         EPUB's mtime.
  --verbose              Log per-step timing.
```

Phase 1 produces only `book.bin`. Phases 2/3/4 add `sections/*.bin`,
`css_rules.cache`, and `thumb_*.bmp` respectively. The CLI gains
`--book-bin-only` / `--sections-only` / `--thumbs-only` flags once
those phases land.

## Compatibility / versioning

- `BOOK_CACHE_VERSION` is in `lib/Epub/Epub/BookMetadataCache.cpp`.
  The prebake tool reads the same constant from the same source, so
  cross-version drift can't happen as long as the tool is rebuilt
  against the current firmware tree.
- `SECTION_FILE_VERSION` (phase 2) — same arrangement.
- Distribute prebake binaries paired with a firmware release: a
  prebake built against firmware v3.7.3 emits cache state v3.7.3
  understands.

## Phase 2 — cover thumbs (2A) and section files (2C)

Phase 1 status: shipped. Host-emitted `book.bin` is byte-identical
to device output for the same EPUB (sha256 verified end-to-end on a
real device against the same `/Books/...epub` SD path; see commits
`ce872b50` + `8beaacfe`).

### What Phase 2 actually buys

Phase 1 alone skips OPF + TOC parsing on first book-open (~0.5-2s
saved on this hardware). The bigger first-open cost is **per-chapter
layout** — building `sections/N.bin`. That's the 5-15s "Loading"
delay per chapter on fresh books. Eliminating that is the headline
goal for Phase 2.

We split Phase 2 into 2A (cover thumbs) and 2C (sections) because:

1. They have very different complexity profiles. 2A is mostly an
   image-pipeline port. 2C requires porting `GfxRenderer` + font
   measurement + page-break + CSS application — substantially more
   firmware code on the host side.
2. 2A's image pipeline (decode → resize → dither → BMP) is a hard
   prerequisite for 2C anyway — chapter images flow through the same
   dither-to-e-ink path. Doing 2A first surfaces those questions
   early and lets 2C reuse the infrastructure.
3. 2A is shippable on its own; 2C is more useful but bigger.

Phase 2B (raw `toc.ncx` / `toc.nav` cache files) is low-payoff
relative to effort; folded into 2C if convenient, skipped otherwise.

### CLI shape after Phase 2

```
crumble-prebake [options] <epub> [<epub> ...]

Existing (phase 1):
  --output-dir / --sd-mount / --device-path / --check / --verbose / -h

New for phase 2:
  --device <x4|x3>             Target device. Sets viewport (800x480 vs
                               792x528) and the canonical thumb-size set.
                               Default: x4.
  --book-bin-only              Phase-1 behaviour: emit book.bin only.
  --thumbs-only                Emit cover thumbs only.
  --sections-only              Emit sections/*.bin only (requires book.bin
                               to already exist in the output dir; we
                               re-run the OPF parse to get the spine, but
                               don't re-write book.bin).
  --viewport <WxH>             Override device's default viewport. Power
                               users only; --device covers the 99% case.
  --font-id <n>                Override the canonical default font.
                               Power users only.
```

The implicit default (no flag) emits **all** cache state for the
selected `--device`. That matches the optimizer's "build everything"
workflow.

### Per-user device tagging (the optimizer-UI question)

The user marks their device once in the optimizer (X4 or X3). Every
EPUB they bake goes through prebake with that `--device` flag. Result:
one build per book per user, no per-device duplication, output dir
stays small. Section-cache headers fingerprint the viewport so a
mismatched device variant simply gets rebuilt on-board (graceful
degradation, no breakage).

### Settings strategy for sections

Section files bake 12 settings into a header fingerprint: font id,
viewport width/height, line compression, paragraph spacing, paragraph
indents, paragraph alignment, hyphenation, embedded style, image
rendering, bionic reading, guide reading.

We prebake **factory defaults only**. Justification: most users never
touch reader settings (and benefit forever); power users who customize
get the speedup until first change, then back to on-device build until
their next change. A combinatorial matrix of common combos would blow
up output size for diminishing returns. Re-evaluate after telemetry
from actual users (do most users customize a lot?).

### Phase 2A — cover thumbs

**Goal:** emit `thumb_WxH.bmp` and `thumb_WxH_fit.bmp` in
`epub_<hash>/`, byte-identical to device output.

**Canonical thumb-size set (pinned during 2A.1 survey, 2026-05-31):**

The default home theme since v2.1 is `LyraCarousel`. It draws three
covers on home: a focused center book and two side books. EPUB-self-
healing in `HomeActivity` pre-generates thumbnails at the inner (visual
-inset-adjusted) size; XTC reader pre-generates at the outer slot size.
Sizes are theme constants (`src/components/themes/lyra/LyraCarouselTheme.h`),
NOT device-dependent — X4 and X3 generate the same thumb sizes; only
viewport layout differs.

| Variant     | W × H    | Call site                                                | Filename                |
|-------------|----------|----------------------------------------------------------|-------------------------|
| Center inner| 296×468  | `HomeActivity.cpp:648` — self-heal on cover-miss          | `thumb_296x468.bmp`     |
| Side cover  | 200×390  | `LyraCarouselTheme.cpp:335`, `HomeActivity.cpp:652`       | `thumb_200x390.bmp`     |
| Center outer| 340×540  | `XtcReaderActivity.cpp:65` (XTC only)                     | `thumb_340x540.bmp`     |

**Phase 2A MVP scope:** ship `thumb_296x468.bmp` + `thumb_200x390.bmp`
for every EPUB. That covers home-screen rendering for ~90% of users on
first boot with the default theme.

**Out of scope for 2A v1** (add later as separate phases or once
telemetry shows usage):
- XTC center 340×540 — only applies when the user opens an XTC file
- LyraFlow sleep-screen center cover — height computed dynamically
  per screen, so requires `--viewport` awareness
- Bookshelf grid sizes (`RecentBooksGridActivity` — coverWidth_,
  coverHeight_) — computed at runtime from the 2x2/3x3/4x4 grid
  selection; would need either a `--grid 3x3` flag or shipping all
  three variants
- Non-Lyra themes (Minimal home 350×583, Base home 222×370,
  RoundedRaff home 156×260, Lyra basic home 136×226) — only relevant
  if the user has switched away from LyraCarousel
- Adaptive (`_fit.bmp`) variants — generated only by Minimal-style
  themes for adaptive contain, not LyraCarousel

The CLI `--device` flag still selects between X4 and X3, but for 2A
v1 both produce the same three (well, two) thumb files. Device-level
divergence shows up first in Phase 2C section files (viewport-baked
into header).

**Firmware code to pull onto the host:**
- `lib/JpegToBmpConverter/` — JPEG decode (already host-buildable)
- `lib/PngToBmpConverter/` — PNG decode (already host-buildable)
- `lib/Epub/Epub.cpp` — `generateThumbBmpAtSize` / `generateCoverBmp`
  helpers (extract the decode-resize-dither-write pipeline; need to
  trace exact functions)
- Whatever dither table / palette mapping the device uses (probably
  in `GfxRenderer` or a sibling header) — this is the bit most likely
  to bite us; bilinear-resize + Floyd-Steinberg + 4bpp e-ink palette
  is fiddly and any rounding diff produces a 1-byte cmp miss

**Host-shim gaps:**
- The decode libs already build host-side (proven in Phase 1's
  unrelated bring-up). Mainly need to confirm they don't pull in
  Arduino-only types we haven't stubbed.
- BMP writer is small — we may need to mirror it in the host shim
  if it currently lives in a device-only helper.

**CLI integration:** new code path after `prebakeBookBin` returns
ok. For each (W, H) in the canonical set, decode the cover from the
EPUB, resize, dither, write `epub_<hash>/thumb_WxH.bmp` and
`_fit.bmp`.

**Acceptance:** for one EPUB with a cover, every `thumb_*.bmp` file
the device emits has a byte-identical host counterpart (sha256
match, all sizes).

**Risk areas:**
- Dither algorithm determinism. If the device's dither uses a
  per-pass error buffer that's not reset cleanly, we have to match
  that exactly.
- Resize algorithm. Bilinear vs nearest-neighbor vs Lanczos —
  whatever the device uses, we use the same.
- Color space. If the device dithers to a 4-color (red + 3-gray) or
  similar e-ink palette, the palette + mapping has to be identical.

### Phase 2C — section files (revised after 2C.1 survey)

**Survey finding 1 (2026-05-31): scope is much smaller than originally
sketched.** Section files do NOT contain rendered pixel data. They
contain a **paginated block tree with positions**: each `PageLine` is
(xPos, yPos, TextBlock); each `PageImage` is (xPos, yPos, ImageBlock
ref). All `Page::serialize` does is `tryWritePod` the positions + serialize
the block's metadata. No rasterization happens during section build.
That means GfxRenderer's *drawing* surface (drawText, drawLine, fillRect,
drawArc, drawRoundedRect, drawPixel, all the dither variants) is
unused during layout — these can all be no-ops on host. The only thing
that has to match device output is the *measurement* surface.

**Survey finding 2 (2026-05-31): the EPUB optimizer already handles
chapter image decode via `.pxc` bake.** lib/Epub/Epub/blocks/ImageBlock.cpp:23
and lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:929 ("if the optimizer
bundled a pre-rendered .pxc pixel cache..."). So the heaviest part of
chapter rendering on first-open (JPEG decode + dither at fitted size) is
ALREADY off-device when the user has run their EPUB through the optimizer
with the default "Generate .pxc image cache" toggle. Phase 2C's remaining
job is purely **text layout** — font measurement + page-break + anchor LUT.

**Goal (revised):** emit `sections/N.bin` for each spine entry,
byte-identical to device output, given a fixed settings combo and viewport.
ImageBlock references in the output are paths to `.pxc` files; those
files come from the optimizer, not from this CLI.

**Firmware code to pull onto the host:**
- `lib/Epub/Epub/Section.cpp` — orchestrates the section build (write
  header, run parser, write LUT/anchors/paragraph-LUT/li-LUT, patch
  header back with final values)
- `lib/Epub/Epub/Page.cpp` — page object, page-break, serialize
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` — XHTML → block
  tree → pagination (this is the 5-15s on-device cost we're moving)
- `lib/Epub/Epub/css/CssParser` + siblings — CSS rule application
- `lib/Epub/Epub/blocks/TextBlock.cpp` + `ImageBlock.cpp` — block
  hierarchy, serialize (ImageBlock's actual decode path is unused
  during layout; only the metadata path is touched)
- `lib/EpdFont/EpdFont.cpp` + `EpdFontFamily.cpp` + `builtinFonts/` —
  bitmap font tables. The exact same binary fonts that ship in firmware
  have to be linkable on host. `getAdvance(codepoint, styleIdx)` is the
  only EpdFont entry the layout path actually needs.

**GfxRenderer surface we actually need on host** (from 2C.1 grep over
ChapterHtmlSlimParser + TextBlock + ImageBlock + Page):

Measurement (must produce device-identical values):
- `getLineHeight(fontId)`
- `getFontAscenderSize(fontId)`
- `getTextWidth(fontId, text, style)`
- `getTextAdvanceX(fontId, text, style)`
- `getSpaceWidth(fontId, style)`
- `getScreenWidth()` / `getScreenHeight()`

Housekeeping (no-op stubs OK):
- `suppressImages()` → false (host always has heap)
- `markImageRepaintUnsafe()` / `markRenderStarved()` → no-op
- `releaseSdCardFontForLowMemory()` → false (host never under memory pressure)
- `ensureSdCardFontReady()` → no-op (assume font preloaded)

Drawing (no-op stubs OK — section build doesn't rasterize):
- `drawText`, `drawLine`, `drawRect`, `fillRect`, `drawPixel`,
  `drawPixelDither<Color>`, `drawArc`, `drawRoundedRect`, etc.

The entire GfxRenderer.cpp (~2900 lines) is mostly draw paths. The
host stub is probably <150 lines — mostly forwarding the measurement
calls into an `EpdFontFamily` and stubbing the rest.

**Host-shim gaps (revised down — smaller than Phase 1's):**
- GfxRenderer stub: ~150 lines forwarding the 6 measurement methods
  into an EpdFontFamily instance, returning hardcoded screen-w/screen-h
  from `--viewport`, no-op everything else. The text-flow code paths
  in ChapterHtmlSlimParser call only the measurement subset, so a thin
  forwarding stub is enough.
- Glyph metrics determinism: must produce identical advance widths to
  on-device. EpdFont stores advance per glyph as plain integers
  (no float). If both sides use the exact same builtin-fonts data
  AND the same `getAdvance` lookup, host and device should agree by
  construction. Early acceptance on a single chapter validates this.
- Image blocks: NO host-side image pipeline work needed for Phase 2C.
  The optimizer already produces `.pxc` files; the layout pass only
  reads ImageBlock's *metadata* (dimensions, src path) — never decodes.

**CLI integration:** new code path triggered by absence of
`--book-bin-only` / `--thumbs-only`. After Phase 1 + 2A finish, loop
over spine entries (read from book.bin), run
`ChapterHtmlSlimParser` + `Page` build for each, write
`sections/<spineIdx>.bin`.

**Acceptance:** for one EPUB with factory-default settings on `--device x4`,
every `sections/*.bin` file the device emits has a byte-identical host
counterpart.

**Risk areas (in order of likelihood):**
- Font measurement mismatch (highest risk). If glyph advances differ
  by even one pixel anywhere, line breaks shift and the whole file
  diverges. Mitigation: build an acceptance test on the simplest
  possible chapter first (plain text, default font), get that
  byte-identical, then move to richer chapters.
- `buildMaxAlloc` field in the section header (line 84 of
  `Section.cpp`) captures the highest heap allocation seen during
  the on-device build. On host that has no meaning — we'd either
  write 0 (device will treat as "unknown, assume OK") or copy a
  device-measured value if there is one for the same settings combo.
  Open question.
- `imagesSuppressed` bit (line 83) — same situation. On-device this
  records whether the build skipped images due to heap pressure. On
  host we never skip images. Likely write `false`.
- CSS application order. The device applies stylesheets in a
  specific order (manifest order, then per-element inline). Any
  divergence here cascades into different layout.

### Phase 2C.2 status update (2026-05-31)

Completed in this session (commits 9291562d, c7dbbc0e):

1. **All six GfxRenderer measurement methods are implemented**
   (host_shim/GfxRenderer.cpp) -- forward into EpdFontFamily, mirror the
   device's integer fixed-point + fp4 snap chain exactly.
2. **EpdFont host build wired** -- EpdFont.cpp / EpdFontFamily.cpp /
   FontDecompressor.cpp / Utf8.cpp compile-share with device. micros()
   added to Arduino.h shim. Phase 1 book.bin byte-identical regression
   check passes.
3. **MemoryBudget header-only shim works as-is** -- our ESP{} stub
   returns 1 GB free so all hasHeap() checks pass on host.
4. **Full Epub layout chain compiles + links on host**:
     lib/Epub/Epub.cpp
     lib/Epub/Epub/Section.cpp
     lib/Epub/Epub/Page.cpp
     lib/Epub/Epub/ParsedText.cpp
     lib/Epub/Epub/htmlEntities.cpp
     lib/Epub/Epub/blocks/{TextBlock,ImageBlock}.cpp
     lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
     lib/Epub/Epub/hyphenation/{Hyphenator,LanguageRegistry,
                                 LiangHyphenation,HyphenationCommon}.cpp
     lib/Epub/Epub/css/CssParser.cpp
     lib/Epub/Epub/converters/{ImageDecoderFactory,JpegToFramebuffer,
                                PngToFramebuffer,ImageToFramebuffer
                                Decoder}.cpp
5. **host_shim/GfxRenderer.h fully fleshed out** with the enums and
   query methods DirectPixelWriter.h needs to parse (Orientation,
   RenderMode, getDisplayWidth/Height/WidthBytes, getOrientation,
   getRenderMode, getFrameBuffer, isSdCardFont, the
   vector<string> overload of ensureSdCardFontReady).

What's still needed for a first section_*.bin emission:

A. **Builtin font header includes + font construction.** main.cpp needs
   to include the four LexendDeca-14 headers and construct EpdFont +
   EpdFontFamily instances mirroring src/main.cpp:107-112:
       #include <builtinFonts/lexenddeca_14_regular.h>
       #include <builtinFonts/lexenddeca_14_bold.h>
       #include <builtinFonts/lexenddeca_14_italic.h>
       #include <builtinFonts/lexenddeca_14_bolditalic.h>
       EpdFont lexenddeca14RegularFont(&lexenddeca_14_regular);
       ... (3 more)
       EpdFontFamily lexenddeca14FontFamily(&...regular, &...bold,
                                             &...italic, &...bolditalic);
       renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14FontFamily);
   The fontId constant lives in src/fontIds.h
   (LEXENDDECA_14_FONT_ID = 1797004611 for non-OMIT_EMOJI_FONTS builds).
   Verify the headers parse cleanly on host (they're pure data so should
   be a no-op compile).

B. **Epub instance construction.** After Phase 1 emits book.bin, the
   metadata is on disk; construct an Epub for it:
       auto epub = std::make_shared<Epub>(epubPath, cacheDirParent);
       epub->load();  // pulls book.bin back in as the metadata cache
   This loads the spine into Epub's internal state so getSpineItem(N)
   returns paths for each chapter.

C. **Section loop in main.cpp.** For each spine entry, construct a
   Section and call createSectionFile with device-matching settings.
   The settings should mirror what device defaults to -- check
   src/CrossPointSettings.h for the default values (lineCompression,
   paragraphAlignment, imageRendering, etc.) so the section header
   fingerprint matches and the device will reuse the cache instead of
   rebuilding.

D. **Acceptance:** for one spine entry, byte-compare host's
   sections/N.bin against the device's. First chapter is simplest --
   likely just title.xhtml or chapter 1 with plain text.

E. **Expected first-emission divergence sources** (handle in 2C.4):
   - The header's `buildMaxAlloc` field (~12 bytes in) gets the
     ESP.getMaxAllocHeap() snapshot. On host this is 1<<30 -> our value
     will differ from device. Either write 0 (Section::loadSectionFile
     skips the check if it's 0) or override in build flag.
   - The header's `imagesSuppressed` bit. On host with infinite heap we
     never suppress; this is `false` consistently, which matches device
     for any chapter that doesn't hit OOM.
   - CSS application order if the test chapter has any styling.
   - Hyphenator language fallback: setPreferredLanguage("en") for
     English content -- check src/main.cpp for the call pattern.

### Phase 2C.2 handoff (skeleton committed; next-session pick-up)

A skeleton `tools/crumble-prebake/host_shim/GfxRenderer.h` is checked in
with all six measurement methods declared, no-op drawing stubs inline,
and viewport state plumbed via `setViewport(w, h)`. The measurement
method *bodies* (signatures only) are NOT yet written -- they need to
forward into `fontMap[fontId]`'s EpdFontFamily.getAdvance() chain.

The minimum bring-up steps to get a buildable host binary for 2C.3:

1. **Pick the test font**. The session's test EPUB is English Latin
   text. The default reading font (per src/main.cpp's insertFont loop)
   is the `tiny`-env active set: no Teensy (8px), no Itty-Bitty (9px),
   no XLarge (18px), no Huge (20px). Probably bitter_14_regular for
   default size. Cross-check src/CrossPointSettings.cpp for the default
   font preference.

2. **Compile-share EpdFont**. Add to build.sh SOURCES:
     lib/EpdFont/EpdFont.cpp
     lib/EpdFont/EpdFontFamily.cpp
     lib/EpdFont/FontDecompressor.cpp
   And add include path -I "$REPO_ROOT/lib/EpdFont".

3. **Include only the builtin-font headers we need.** The full set is
   139 files (~3-5 MB). For a first byte-match test we need only one
   size's four styles (bitter_14_regular/bold/italic/bolditalic, say).
   They're pure-data header-only PROGMEM tables; should compile straight
   on host.

4. **Implement the six measurement methods in a new
   tools/crumble-prebake/host_shim/GfxRenderer.cpp**, each forwarding
   into the EpdFontFamily for the requested fontId. Mirror the
   logic in the device's GfxRenderer.cpp (line numbers in the survey).

5. **Add no-op MemoryBudget host shim** (lib/MemoryBudget/MemoryBudget.h
   gets #included by Section.cpp; the snapshot() call returns a
   heap-stats struct that's only used for debug logs on device -- the
   host stub can return zeros).

6. **Compile-share the Epub-side layout chain**:
     lib/Epub/Epub/Section.cpp
     lib/Epub/Epub/Page.cpp
     lib/Epub/Epub/blocks/TextBlock.cpp
     lib/Epub/Epub/blocks/ImageBlock.cpp
     lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
     lib/Epub/Epub/css/CssParser.cpp
     lib/Epub/Epub/hyphenation/Hyphenator.cpp (+ siblings)

7. **Try to build.** Each missing-symbol error narrows the remaining
   shim surface. The likely casualties (in order of probability):
     - Bitmap.cpp -- referenced from GfxRenderer.cpp's draw paths; on
       host, our skeleton inlines no-op drawing so Bitmap may not be
       needed at all. If TextBlock or ImageBlock pulls it for some
       coord helper, stub.
     - DirectPixelWriter / ImageDecoderFactory -- ImageBlock.cpp
       imports these; we don't run the decode path during layout but
       the linker still needs the symbols. Stub no-op.
     - SdCardFontManager -- referenced via the SD font fallback path.
       Stub: no SD-resident fonts on host.

8. **Wire Section::createSectionFile call from main.cpp**. After
   Phase 1 emits book.bin, loop the spine entries from BookMetadata
   and call Section.createSectionFile(...) for each. Default settings:
     fontId = (default reading font for --device)
     viewport = X4 800x480 or X3 792x528
     lineCompression = 1.0f (or device default)
     paragraphAlignment = (device default)
     hyphenationEnabled = true
     embeddedStyle = true (use bundled CSS)
     imageRendering = (device default, probably "fit")
     bionicReading = false
     guideReading = false

9. **Byte-compare** first chapter's sections/0.bin against device.
   If different: hex-diff to find first divergence. Likely candidates:
   font advance width difference, page-break position difference,
   CSS rule application order.

### Sequencing

1. **2A scope refinement** — enumerate the canonical thumb-size set
   per device by surveying theme code. Pin in the design.
2. **2A image-pipeline bring-up** — get decode-resize-dither-write
   running on host with empty test data. Verify BMP byte layout
   matches device for a synthetic input.
3. **2A acceptance** — one EPUB, all thumb sizes byte-match device.
   Ship.
4. **2C planning** — survey GfxRenderer dependencies in detail. Map
   which files must come over wholesale vs which can be stubbed.
   Write a "section-build minimal viable" file list. Pin in design.
5. **2C bring-up: text-only chapter** — simplest possible chapter
   (no images, no complex CSS), default settings, get byte-identical
   section file. This is the make-or-break gate for Phase 2C — if
   font measurement diverges here, we have to spend effort on the
   metrics pipeline before going further.
6. **2C bring-up: image-bearing chapter** — pulls 2A image pipeline
   into section build.
7. **2C bring-up: CSS-heavy chapter** — last because CSS bugs are
   the most likely to surface late.
8. **Phase 2 acceptance + PR** — full prebake on the same test EPUB
   we used for Phase 1; every output file byte-matches device. Open
   PR.

### Compatibility / versioning

Same arrangement as Phase 1: `SECTION_FILE_VERSION` lives in the
firmware tree; the prebake tool reads the same constant. Prebake
binary stays paired with a firmware release.

## Open questions

- macOS code-signing for distributed binaries — defer to first release
  cut.
- Should the tool also generate `.pxc` page bitmaps? Separate feature;
  not in this scope.
- Concurrency — the on-device code is single-task. The prebake CLI
  could parallelize across EPUBs, but the per-EPUB pipeline stays
  single-threaded (mirrors device behaviour, reduces drift risk).
- **Phase 2C `buildMaxAlloc` field**: write 0 and accept the device
  may not trust the header's heap estimate? Or import a measured value
  from the on-device build? Decide during 2C bring-up.
- **Phase 2C font determinism**: does `EpdFont` measurement involve
  any floating-point that could differ between host and ESP32 toolchains?
  Early single-chapter acceptance test will answer this.
- **Phase 2A canonical thumb-size set**: pinned during 2A scope
  refinement. Survey of theme code needed.
