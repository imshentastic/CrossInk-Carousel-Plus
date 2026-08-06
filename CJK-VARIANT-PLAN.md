# CJK Flash-Font Variant (`tiny-cjk`) — Plan

_Branch: `feat/cjk-flash-variant` (based on `feat/dict-casper` @ ba80e2a2, which carries
the CJK leading-punctuation line-break rule this variant needs). Worktree:
`/Users/michaelshen/Desktop/Coding Exp - CrossInk/CrumBle-cjk`._

## Goal

A separate firmware build variant that reads CJK books **with Bluetooth working**,
by compiling CJK glyphs into flash (`.rodata`, memory-mapped, zero heap at draw
time) instead of streaming them from SD with heap caches. This is the proven
CrossLink architecture (icannotttt/crosspoint-chinesetype): they run stock
NimBLE + full CJK on the same ESP32-C3 because CJK costs no RAM.

## Hard constraints (user-set)

1. **MUST fit the stock 6.25 MB slot: 6,553,600 bytes.** SD-card flashable from
   CrossInk/CrossPoint like the main build. No 7.5 MB-only variant.
2. **Keep OPDS, Calibre, QR, KOReader** (measured: only ~63 KB combined — not
   worth the feature loss).
3. Main (`tiny-bitter`) build stays CJK-free and unchanged.

## Measured flash budget (map-file analysis, 4.7.2-era build = 5,354,544 B)

| Item | Bytes |
|---|---|
| Headroom to 6,553,600 | +1,199 K |
| Drop Bitter italic + bolditalic blobs (keep regular + bold, all 4 sizes) | +551 K |
| Drop hyphenation tries except English (de 206K + ru/uk/pl/etc.) | +323 K |
| **CJK data budget** | **≈ 2.07 MB** |

CJK package target (CrossLink per-glyph costs as reference: 2-bit 18pt ≈ 292 B/glyph,
1-bit 10pt ≈ 51 B/glyph):

- 2-bit reading face @ ~16pt, 5,000 common hanzi + ASCII/Latin-1 + CJK punct ≈ **1.2 MB**
- 1-bit UI/fallback face, same subset, small size ≈ **0.26 MB**
- Total ≈ **1.5 MB → ~570 KB margin**. 7,000 glyphs ≈ 2.0 MB — only if margin allows.

## What already exists in our tree (do not rebuild from scratch)

- `lib/EpdFont/EpdFontData.h:119` — `bool is2Bit;` already in the struct (shared
  lineage with CrossLink's base). Verify `GfxRenderer::renderChar` still has the
  2-bit unpack path; CrossLink's is at their `GfxRenderer.cpp:1002-1042` for
  reference (BW pass: any non-white -> black; MSB pass: vals 1,2; LSB: val 1).
- `lib/EpdFont/scripts/` — CJK donor TTFs already present: `LXGWWenKai-Regular.ttf`
  (open license, good for 简体 reading), `HanWangMing-Font.ttf`; plus
  `build-inter-cjk.sh`, `build_combo_font.sh`, `convert-builtin-fonts.sh` from the
  4.5.x CJK era. Check what they emit before writing a new converter.
- Per-character CJK word segmentation (4.5.3, `ChapterHtmlSlimParser.cpp:124+`).
- CJK leading-punctuation rule 避头尾 (this branch, `ParsedText.cpp` — both breakers).
- CJK UI fallback for titles (4.6.0) — extend/reuse the per-glyph fallback for the
  reader path: Latin font selected + hanzi codepoint -> baked CJK face.
- OMIT infra precedent: `OMIT_XLARGE_FONT` / `OMIT_HUGE_FONT` flags; add
  `OMIT_ITALIC_FONTS` and a tries-trim flag (`CJK_VARIANT_EN_HYPHENATION_ONLY`)
  gated in `LanguageRegistry.cpp` includes/table.

## CrossLink findings to copy / avoid (full analysis in session notes 2026-07-26)

COPY: flash-resident glyphs returned by pointer (their `EpdFont.cpp:110-115`);
binary-search interval lookup (already ours); size-menu aliasing for the CJK face
only (all sizes -> one physical face — ours keeps real Bitter sizes for Latin);
bit-depth split (2-bit reading face on a subset, 1-bit fallback).
AVOID: their SD `.epdfont` path (per-glyph SD reads — that's our failed approach);
their `std::list`+`std::advance` text storage; their unguarded chapter-index-while-BLE window.

## Task breakdown (suggested order)

1. **Verify 2-bit render path** end-to-end with a tiny hand-built 2-bit test font
   on X4 (or websim once bootable). If `is2Bit` rotted, port CrossLink's renderChar.
2. **Subset + convert pipeline**: pick frequency list (e.g. Jun Da / BLCU top-5000
   simplified + CJK punctuation + ASCII/Latin-1), extend the existing scripts to emit a
   2-bit `.h` in OUR builtin format (match `EpdFontData` fields; glyph intervals
   sorted; PROGMEM arrays). Deterministic output, committed to repo.
   LXGW WenKai is the suggested reading face (license-clean, screen-optimized).
3. **`[env:tiny-cjk]`** in platformio.ini: extends base like tiny-bitter, plus
   `-DOMIT_ITALIC_FONTS -DCJK_VARIANT=1`, includes the CJK face header in main.cpp
   font registration; CJK sizes alias to the one face; tries trimmed to en.
4. **Reader glyph fallback**: when the selected family lacks a codepoint and the
   CJK face has it, draw from the CJK face (extend the 4.6.0 title-fallback hook to
   the reader draw path). Metrics: line height when mixing = max(latin, cjk) —
   watch the X3 layout-overflow diagnostic (v442) for regressions.
5. **Size gate**: fail the tiny-cjk build > 6,500,000 bytes (release headroom
   margin below the 6,553,600 hard slot limit).
6. **OTA variant-awareness (IMPORTANT)**: `OtaUpdater` matches the
   `crumble-firmware*.bin` asset stem. A cjk-variant device must fetch
   `crumble-firmware-cjk-X.Y.Z.bin`, and a main-variant device must NOT.
   Gate the asset-name match on a compile-time variant tag
   (`CROSSPOINT_FIRMWARE_VARIANT` is already injected — extend the matcher), and
   make the main build reject `-cjk-` assets explicitly.
7. **BT + CJK acceptance test**: X4 + Free3-R + a real Chinese EPUB: open book,
   enable BT via the new stay-in-menu flow, connect, page through image-heavy and
   text chapters, chapter jump, AA passes — watch free heap + maxAlloc in serial.
   Then X3 (viewport overflow diag v442 still armed).
8. Release flow: separate asset `crumble-firmware-cjk-<ver>.bin`, release notes
   list the variant trade-offs (no italics, single CJK size, en-only hyphenation).

## Trade-offs to state in release notes when it ships

- Italic/bold-italic render as regular/bold (variant only).
- CJK renders at one fixed size regardless of the size setting; Latin keeps all sizes.
- Non-English hyphenation dropped in the variant (en kept; CJK needs none).
- Existing SD-font CJK path still present but discouraged with BT.

## Verification gates

- tiny-bitter (main) build byte-identical concerns: variant code must be fully
  flag-gated; run both builds and diff main's size before/after.
- The wasm optimizer shares the layout code — it must NOT include the CJK face
  (browser bakes use SD-font/builtin metrics as configured); check
  `CJK_VARIANT` stays off in tools/crumble-prebake + websim builds.
