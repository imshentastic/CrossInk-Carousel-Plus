#!/bin/bash
# tiny-cjk variant: build the flash-resident CJK fallback face.
#
# Emits ../builtinFonts/cjk_14_regular.h -- a 2-bit, UNCOMPRESSED (memory-
# mapped, zero heap at draw time) LXGW WenKai face at 14pt. Its metrics match
# Bitter 14 exactly (advanceY 35, ascender 28, descender -8), so mixed CJK /
# Latin lines need no extra leading and hanzi sit at the same visual size as
# the surrounding text. Covers:
#   - GB2312 level 1+2 hanzi (6763 chars, from cjk_subset.txt)
#   - CJK Symbols and Punctuation (0x3000-0x303F, incl. U+3007)
#   - Hiragana + Katakana (0x3040-0x30FF; LXGW WenKai covers kana)
#   - Halfwidth and Fullwidth Forms (0xFF00-0xFFEF)
#
# Deliberately NOT --compress: compressed builtin fonts decompress glyph
# groups into heap at draw time (FontDecompressor). The whole point of this
# variant is CJK with ZERO heap cost so NimBLE can coexist. Deliberately
# --no-kern-liga: hanzi are monospaced-advance and the GPOS class scan is
# O(n^2) in glyph count. --no-default-intervals: this face is consulted only
# as a glyph-miss fallback behind Bitter, so Latin glyphs would be dead
# bytes.
#
# Output is deterministic for a given LXGWWenKai-Regular.ttf + cjk_subset.txt
# + fontconvert.py; the generated header is committed to the repo.

set -e

cd "$(dirname "$0")"

CJK_FONT="LXGWWenKai-Regular.ttf"
SUBSET="cjk_subset.txt"
OUT="../builtinFonts/cjk_14_regular.h"

[[ -f "$CJK_FONT" ]] || { echo "MISSING: $CJK_FONT"; exit 1; }
[[ -f "$SUBSET" ]] || { echo "MISSING: $SUBSET (run gen_cjk_subset.py)"; exit 1; }

# fontconvert.py prints informational lines to stderr, but keep the
# build-inter-cjk.sh belt-and-suspenders sed that drops anything before the
# '/**' banner in case a dependency writes to stdout.
python3 fontconvert.py cjk_14_regular 14 "$CJK_FONT" \
  --codepoints-file "$SUBSET" \
  --no-default-intervals \
  --no-kern-liga \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0x3040,0x309F \
  --additional-intervals 0x30A0,0x30FF \
  --additional-intervals 0xFF00,0xFFEF \
  --2bit \
  | sed '/^\/\*\*/,$!d' > "$OUT"

bytes=$(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT")
printf '%s (%d bytes = %d KB header text)\n' "$OUT" "$bytes" "$((bytes / 1024))"
