#!/usr/bin/env bash
# Build a bundled .cpfont from a primary TTF plus one or two fallback TTFs.
# The primary owns the extended-Latin ranges; fallback #1 owns CJK; fallback #2
# (optional) owns Hangul. Runtime treats the merged font as a single family so
# there is no extra resident heap cost (unlike a per-glyph runtime fallback).
#
# Usage from a folder containing the TTFs:
#   ./build_combo_font.sh PRIMARY.ttf FALLBACK1.ttf [FALLBACK2.ttf]
#
# Options (all optional):
#   -n NAME         Family name written into the cpfont + used as SD dir
#                   (default: derived from PRIMARY basename, e.g. Bitter+LXGW)
#   -s "12,14,16"   Point sizes to render (default: 14)
#   -o DIR          Output directory (default: ./output)
#
# The Unicode ranges each source is responsible for are baked into this
# script -- override by editing PRIMARY_RANGES / FALLBACK1_RANGES /
# FALLBACK2_RANGES below.
#
# Example:
#   cd ~/fonts
#   ./build_combo_font.sh Bitter-Regular.ttf LXGWWenKai-Regular.ttf
#   # produces ./output/Bitter+LXGW/Bitter+LXGW_14.cpfont
#   # Drop into /fonts/Bitter+LXGW/ on the device SD card.
#
# Requirements (one-time):
#   pip install fonttools

set -euo pipefail

# --- Defaults ---
SIZES="14"
OUTPUT_DIR="./output"
FONT_NAME=""  # empty = auto-derive after arg parsing

# --- Unicode range assignments per source ---
# Edit these lists if you want a different split. Uses fontTools' --unicodes
# format ("U+HHHH-HHHH" comma-separated).
PRIMARY_RANGES="U+0020-024F,U+2000-206F,U+2070-20CF,U+2200-22FF,U+FB00-FB06"
FALLBACK1_RANGES="U+3000-303F,U+3040-309F,U+30A0-30FF,U+4E00-9FFF,U+F900-FAFF,U+FF00-FFEF"
FALLBACK2_RANGES="U+AC00-D7AF,U+1100-11FF,U+3130-318F"

# Interval preset names for fontconvert_sdcard.py (must cover the union of
# the range assignments above). Keep in sync with the ranges. `cjk` covers
# the FALLBACK1_RANGES set; `hangul` covers FALLBACK2_RANGES.
INTERVAL_PRIMARY="reading"
INTERVAL_FALLBACK1="cjk"
INTERVAL_FALLBACK2="hangul"

# --- Parse options ---
usage() {
  sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
}
while getopts "n:s:o:h" opt; do
  case "$opt" in
    n) FONT_NAME="$OPTARG" ;;
    s) SIZES="$OPTARG" ;;
    o) OUTPUT_DIR="$OPTARG" ;;
    h|*) usage ;;
  esac
done
shift $((OPTIND - 1))

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "Error: expected 2 or 3 TTF arguments, got $#" >&2
  usage
fi
PRIMARY_TTF="$1"
FALLBACK1_TTF="$2"
FALLBACK2_TTF="${3:-}"

for f in "$PRIMARY_TTF" "$FALLBACK1_TTF" ${FALLBACK2_TTF:+"$FALLBACK2_TTF"}; do
  [ -f "$f" ] || { echo "Error: TTF not found: $f" >&2; exit 1; }
done

# Auto-derive font name from primary + fallback basenames if not supplied.
# e.g. Bitter-Regular.ttf + LXGWWenKai-Regular.ttf -> Bitter+LXGW
if [ -z "$FONT_NAME" ]; then
  pname=$(basename "$PRIMARY_TTF" .ttf | sed -E 's/-(Regular|Bold|Italic|BoldItalic)$//i')
  f1name=$(basename "$FALLBACK1_TTF" .ttf | sed -E 's/-(Regular|Bold|Italic|BoldItalic)$//i')
  # Trim family suffix "WenKai" -> LXGW etc. to keep the auto-name short.
  f1short=$(echo "$f1name" | sed -E 's/(WenKai|SansCJK|SerifCJK)$//i')
  FONT_NAME="${pname}+${f1short:-$f1name}"
  if [ -n "$FALLBACK2_TTF" ]; then
    f2name=$(basename "$FALLBACK2_TTF" .ttf | sed -E 's/-(Regular|Bold|Italic|BoldItalic)$//i')
    FONT_NAME="${FONT_NAME}+${f2name}"
  fi
fi

# Locate the sibling converter script -- this script lives next to it in
# lib/EpdFont/scripts/, but the user runs it from an arbitrary TTF folder.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONVERTER="$SCRIPT_DIR/fontconvert_sdcard.py"
[ -f "$CONVERTER" ] || { echo "Error: converter not found at $CONVERTER" >&2; exit 1; }

command -v pyftsubset >/dev/null 2>&1 || {
  echo "Error: pyftsubset not found. Install with: pip install fonttools" >&2
  exit 1
}
# pyftmerge intentionally NOT required: it crashes on many table types
# (VarStore, FeatureVariations, various int/NotImplemented type mismatches)
# even after aggressive drop-tables. We use an inline Python merger instead
# that only touches glyf/cmap/hmtx/loca -- the tables the SD-card renderer
# actually consumes.

WORK_DIR="$(mktemp -d -t combofont.XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Building '$FONT_NAME' from:"
echo "    primary:   $PRIMARY_TTF  ($INTERVAL_PRIMARY ranges)"
echo "    fallback1: $FALLBACK1_TTF  ($INTERVAL_FALLBACK1 ranges)"
[ -n "$FALLBACK2_TTF" ] && echo "    fallback2: $FALLBACK2_TTF  ($INTERVAL_FALLBACK2 ranges)"
echo "    sizes: $SIZES"
echo "    output: $OUTPUT_DIR/$FONT_NAME/"
echo

# flatten_vf <src.ttf> <dst.ttf>
# If src is a variable font (has fvar), instance it to the default location
# so pyftmerge never sees a VarStore (which it crashes on with "VarStore has
# no attribute mergeMap"). Otherwise, copy src through unchanged.
flatten_vf() {
  local src="$1" dst="$2"
  python3 - "$src" "$dst" <<'PY'
import sys
from fontTools.ttLib import TTFont
src, dst = sys.argv[1], sys.argv[2]
font = TTFont(src)
if "fvar" in font:
    from fontTools.varLib.instancer import instantiateVariableFont
    # Empty axis dict -> instance at each axis default = static Regular.
    inst = instantiateVariableFont(font, {}, inplace=False)
    inst.save(dst)
else:
    font.save(dst)
PY
}

echo "==> Flattening any variable fonts to static instances..."
flatten_vf "$PRIMARY_TTF"   "$WORK_DIR/primary-static.ttf"
flatten_vf "$FALLBACK1_TTF" "$WORK_DIR/fb1-static.ttf"
if [ -n "$FALLBACK2_TTF" ]; then
  flatten_vf "$FALLBACK2_TTF" "$WORK_DIR/fb2-static.ttf"
fi

echo "==> Subsetting sources to their assigned ranges..."
# --drop-tables+ strips whatever variation infra pyftmerge still can't
# handle after instancing (belt-and-suspenders in case a static input still
# ships an ItemVarStore inside GDEF), plus mort (Apple metamorphosis;
# unsubsettable). Safe: fontconvert_sdcard.py consumes static glyph
# outlines only.
SUBSET_COMMON=(--drop-tables+=HVAR,MVAR,VVAR,STAT,fvar,gvar,cvar,avar,mort,GDEF,GSUB,GPOS,BASE,JSTF,MATH --no-hinting --desubroutinize)
pyftsubset "$WORK_DIR/primary-static.ttf" --unicodes="$PRIMARY_RANGES"   "${SUBSET_COMMON[@]}" --output-file="$WORK_DIR/primary.ttf"
pyftsubset "$WORK_DIR/fb1-static.ttf"     --unicodes="$FALLBACK1_RANGES" "${SUBSET_COMMON[@]}" --output-file="$WORK_DIR/fb1.ttf"
MERGE_INPUTS=("$WORK_DIR/primary.ttf" "$WORK_DIR/fb1.ttf")

INTERVAL_LIST="$INTERVAL_PRIMARY,$INTERVAL_FALLBACK1"
if [ -n "$FALLBACK2_TTF" ]; then
  pyftsubset "$WORK_DIR/fb2-static.ttf" --unicodes="$FALLBACK2_RANGES" "${SUBSET_COMMON[@]}" --output-file="$WORK_DIR/fb2.ttf"
  MERGE_INPUTS+=("$WORK_DIR/fb2.ttf")
  INTERVAL_LIST="$INTERVAL_LIST,$INTERVAL_FALLBACK2"
fi

echo "==> Merging TTFs (primary base + fallback glyphs for un-covered codepoints)..."
# Inline Python merger. pyftmerge is too fragile (see comment above); we
# roll our own by taking the primary as the base and copying glyf/hmtx +
# cmap entries from each fallback for codepoints the primary doesn't
# already cover. First-wins on overlap. Only touches the tables the
# SD-card renderer needs, so layout-table shape mismatches are moot.
MERGED_TTF="$WORK_DIR/${FONT_NAME}-Regular.ttf"
python3 - "$MERGED_TTF" "${MERGE_INPUTS[@]}" <<'PY'
import sys
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph

out_path = sys.argv[1]
primary_path = sys.argv[2]
fallback_paths = sys.argv[3:]

base = TTFont(primary_path)
base_glyf = base["glyf"]
base_hmtx = base["hmtx"]
base_cmap = base["cmap"]
# Best cmap sub-table for adding new BMP codepoints. If no unicode cmap
# exists we cannot proceed sensibly.
best_cmap = base_cmap.getBestCmap()
if best_cmap is None:
    print("primary has no unicode cmap", file=sys.stderr)
    sys.exit(1)
# Find a writable unicode sub-table to add new mappings to. Prefer a
# format-4 BMP table (platform 3, encoding 1) which every renderer reads.
target_sub = None
for sub in base_cmap.tables:
    if sub.isUnicode() and sub.format == 4:
        target_sub = sub
        break
if target_sub is None:
    for sub in base_cmap.tables:
        if sub.isUnicode():
            target_sub = sub
            break
if target_sub is None:
    print("primary has no writable unicode cmap sub-table", file=sys.stderr)
    sys.exit(1)

covered = set(best_cmap.keys())
added = 0

for fb_path in fallback_paths:
    fb = TTFont(fb_path)
    fb_cmap = fb["cmap"].getBestCmap() or {}
    fb_glyf = fb["glyf"]
    fb_hmtx = fb["hmtx"]
    for codepoint, glyph_name in fb_cmap.items():
        if codepoint in covered:
            continue
        if glyph_name not in fb_glyf:
            continue
        # Pull the source glyph object. Composite glyphs reference other
        # glyphs by name; those referenced components may not exist in
        # the primary, so we skip composites for safety. Simple glyphs
        # copy cleanly.
        src_glyph = fb_glyf[glyph_name]
        if src_glyph.isComposite():
            # Try to decompose to simple outlines via the pen protocol.
            # If any component is missing, skip this glyph.
            try:
                from fontTools.pens.recordingPen import DecomposingRecordingPen
                pen = DecomposingRecordingPen(fb_glyf)
                src_glyph.draw(pen, fb_glyf)
                # Rebuild as a simple glyph using ttGlyphPen.
                from fontTools.pens.ttGlyphPen import TTGlyphPen
                simple_pen = TTGlyphPen(None)
                pen.replay(simple_pen)
                src_glyph = simple_pen.glyph()
            except Exception:
                continue
        # Unique glyph name in the primary. Collisions would silently
        # overwrite a base glyph, so prefix from the fallback.
        new_name = glyph_name
        n = 0
        while new_name in base_glyf:
            n += 1
            new_name = f"fb_{n:04d}_{glyph_name}"
        base_glyf[new_name] = src_glyph
        base_hmtx[new_name] = fb_hmtx[glyph_name]
        target_sub.cmap[codepoint] = new_name
        # keep best_cmap in sync so subsequent fallbacks don't re-add
        best_cmap[codepoint] = new_name
        covered.add(codepoint)
        added += 1

# Bump maxp glyph count so fontTools writes the extended glyf/loca tables
# correctly. maxp reads glyf on save so this normally auto-updates, but
# force a recompute to be safe.
base["maxp"].numGlyphs = len(base_glyf.glyphs)
base.save(out_path)
print(f"    merged: {added} new glyphs added from {len(fallback_paths)} fallback(s)")
PY

echo "==> Running fontconvert_sdcard.py (intervals: $INTERVAL_LIST, sizes: $SIZES)..."
mkdir -p "$OUTPUT_DIR"
python3 "$CONVERTER" "$WORK_DIR/${FONT_NAME}-Regular.ttf" \
  --intervals "$INTERVAL_LIST" \
  --sizes "$SIZES" \
  --name "$FONT_NAME" \
  --output-dir "$OUTPUT_DIR"

echo
echo "==> Done. Drop the contents of:"
echo "        $OUTPUT_DIR/$FONT_NAME/"
echo "    into the device SD card at:"
echo "        /fonts/$FONT_NAME/"
echo "    then pick '$FONT_NAME' in Settings -> Reader -> Font -> Font Family."
