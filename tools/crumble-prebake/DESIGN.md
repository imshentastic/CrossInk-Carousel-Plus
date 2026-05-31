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

### Phase 2C — section files

**Goal:** emit `sections/N.bin` for each spine entry, byte-identical
to device output, given a fixed settings combo and viewport.

**Firmware code to pull onto the host:**
- `lib/Epub/Epub/Section.cpp` — orchestrates the section build
- `lib/Epub/Epub/Page.cpp` — page object, page-break logic
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser` — XHTML → block tree
- `lib/Epub/Epub/css/CssParser` + sibling files — CSS application
- `lib/Epub/Epub/blocks/TextBlock` / `ImageBlock` — block hierarchy,
  layout, draw stubs
- `lib/GfxRenderer/` — **the big one.** Font measurement, glyph
  metrics, line-breaking. The device pulls glyph advance widths out
  of `EpdFont`; we need that same data on host.
- `lib/EpdFont/` — bitmap font tables. The exact same binary fonts
  that ship in firmware have to be linkable on host (or we pull the
  metrics out of them and embed the table).

**Host-shim gaps (bigger than Phase 1's):**
- `GfxRenderer` is built around a 1bpp/2bpp framebuffer abstraction;
  needs a no-op draw stub on host (we measure but don't actually
  rasterize). The text-flow code paths must work without touching
  any pixel buffer.
- Glyph metrics: must produce identical advance widths to on-device.
  Floating-point determinism is a risk if any kerning / fractional
  advance is involved — early acceptance test on a single chapter
  will catch this fast.
- Image blocks: depend on Phase 2A's pipeline being landed and
  tested. Sequencing matters here.

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
