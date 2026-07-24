#!/bin/bash
# CrumBLE 4.5.113: rebuild built-in Inter with a CJK Unified Ideographs
# fallback layer so Chinese book titles render as glyphs instead of the
# "??" REPLACEMENT box when UI Font Fallback is set to none.
#
# Layered stack: Inter-Regular (primary Latin/Cyrillic/etc.) + Noto Sans
# CJKsc for the U+4E00-U+9FFF block (all ~20K CJK Unified Ideographs,
# covers Simplified + most Traditional Chinese).
#
# Cost estimate: current inter_12_regular.h is ~400 KB. Adding the full
# CJK block at 12pt/--2bit/--compress should land ~2-3 MB. If that's too
# much flash, switch INCLUDE_RANGES below to the narrower "common chars"
# subset (0x4E00-0x9FA5 is essentially the same size; a real trim needs
# a codepoint list which fontconvert.py currently doesn't ingest).

set -e

cd "$(dirname "$0")"

INTER_DIR="../builtinFonts/source/Inter"
CJK_FONT="../builtinFonts/source/NotoSansCJKsc/NotoSansCJKsc-Regular.otf"
OUT_DIR="../builtinFonts"

# Sanity checks so a missing source doesn't produce silent zero-byte output.
[[ -f "$INTER_DIR/Inter-Regular.ttf" ]] || { echo "MISSING: $INTER_DIR/Inter-Regular.ttf"; exit 1; }
[[ -f "$CJK_FONT" ]] || { echo "MISSING: $CJK_FONT"; exit 1; }

# CJK Unified Ideographs base block. Simplified Chinese chars (GB2312 /
# GB18030 / most modern usage) live in here. If you also want punctuation
# (fullwidth colon, Chinese quotes, etc.), add 0x3000-0x303F and
# 0xFF00-0xFFEF -- they're small (~100 glyphs) but they matter a lot for
# how titles look.
INCLUDE_RANGES=(
  "0x4E00,0x9FFF"   # CJK Unified Ideographs
  "0x3000,0x303F"   # CJK Symbols and Punctuation
  "0xFF00,0xFFEF"   # Halfwidth and Fullwidth Forms
)

build_one() {
  local size="$1"
  local style="$2"          # Regular | Bold
  local style_lc
  style_lc="$(echo "$style" | tr '[:upper:]' '[:lower:]')"
  local name="inter_${size}_${style_lc}"
  local primary="$INTER_DIR/Inter-${style}.ttf"
  local out="$OUT_DIR/${name}.h"
  [[ -f "$primary" ]] || { echo "MISSING: $primary"; return 1; }

  # Two argument families are BOTH required:
  # 1. --additional-intervals X,Y   -- tells fontconvert.py to EMIT these
  #    codepoints into the output at all. Without this the ranges are just
  #    silently ignored no matter which fallback font you list.
  # 2. --font-include-intervals N:X,Y -- restricts font-stack entry N to
  #    only serve those ranges (so Latin from Inter, CJK from Noto).
  local additional_args=()
  local include_args=()
  for r in "${INCLUDE_RANGES[@]}"; do
    additional_args+=(--additional-intervals "${r}")
    include_args+=(--font-include-intervals "1:${r}")
  done

  echo "Building ${name} with CJK fallback..."
  # v18.9.9.113: --2bit --compress. Whole CJK block at 1-bit uncompressed is
  # ~11 MB (would blow past the 7.5 MB app0 partition). Compression on CJK
  # is dramatic because so many glyphs share bitmap fragments (radicals).
  # The reader path already supports 2-bit compressed fonts (Bitter uses
  # it), so mixing 2-bit UI Inter with 1-bit UI Inter should be harmless
  # -- both go through the same EpdFont draw path.
  # fontconvert.py prints stray informational messages ("kerning: N pairs
  # extracted", etc.) to STDOUT before the .h content -- if left in they
  # cause 'kerning does not name a type' compile errors. Discard them by
  # dropping every line before the '/**' comment banner that starts the
  # real output.
  python fontconvert.py "$name" "$size" \
    "$primary" "$CJK_FONT" \
    "${additional_args[@]}" \
    "${include_args[@]}" \
    --2bit --compress \
    2>/dev/null | sed '/^\/\*\*/,$!d' > "$out"

  local bytes
  bytes=$(stat -f%z "$out" 2>/dev/null || stat -c%s "$out")
  printf '  -> %s (%d bytes = %d KB)\n' "$out" "$bytes" "$((bytes / 1024))"
}

# Stage 1: bake CJK into the title-tier font ONLY. If flash headroom
# survives, uncomment the second call to add the small-UI variant too.
build_one 12 Regular
# build_one 10 Regular
# build_one 12 Bold

echo ""
echo "Done. Now build firmware ('pio run -e tiny-bitter') and check the final"
echo "'firmware.bin' size against your 16 MB app partition. Current before-CJK"
echo "firmware.bin is ~6.7 MB; adding CJK to inter_12_regular alone will push"
echo "it up by roughly the header-file size increase (which will print above)."
