#!/usr/bin/env bash
set -euo pipefail

# WASM build of crumble-prebake. Mirrors build.sh but swaps in emscripten
# (em++ / emcc) so the same source tree can compile to a browser-loadable
# .wasm blob for /optimizer integration (task #28).
#
# The CLI build (build.sh) and the WASM build share 100% of the firmware-
# adjacent code -- font tables, parsers, layout, image pipeline. The only
# delta is the platform shim: the CLI uses libcurl + std::filesystem,
# WASM uses a JS-side fetch() + MEMFS. -DCRUMBLE_PREBAKE_WASM gates the
# libcurl path in main.cpp (which is dropped entirely in step 28.3 once
# the entry point is refactored to take render-info JSON as input).
#
# Prereq: emscripten SDK installed at ~/emsdk and activated. Run
#   source ~/emsdk/emsdk_env.sh
# before running this script, or set EMSDK below.

EMSDK="${EMSDK:-$HOME/emsdk}"
if [[ ! -d "$EMSDK/upstream/emscripten" ]]; then
  echo "[build-wasm] emscripten not found at $EMSDK -- install via:"
  echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk"
  echo "  cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest"
  exit 1
fi
EMCC="$EMSDK/upstream/emscripten/emcc"
EMXX="$EMSDK/upstream/emscripten/em++"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-wasm"
OUTPUT="$BUILD_DIR/crumble-prebake.js"

mkdir -p "$BUILD_DIR"

# Same source set as build.sh -- the entire firmware-adjacent surface
# compiles cleanly under emscripten because it's vanilla C/C++ with no
# platform-specific calls outside the host_shim/ layer. Keep this list
# in sync with build.sh; any divergence indicates platform-specific
# code creeping in that should be guarded behind CRUMBLE_PREBAKE_WASM.
SOURCES=(
  "$SCRIPT_DIR/src/main.cpp"
  "$SCRIPT_DIR/host_shim/HalStorage.cpp"
  "$REPO_ROOT/lib/uzlib/src/tinflate.c"
  "$REPO_ROOT/lib/uzlib/src/uzlib_checksums.c"
  "$REPO_ROOT/lib/InflateReader/InflateReader.cpp"
  "$REPO_ROOT/lib/FsHelpers/FsHelpers.cpp"
  "$REPO_ROOT/lib/ZipFile/ZipFile.cpp"
  "$REPO_ROOT/lib/Epub/Epub/BookMetadataCache.cpp"
  "$REPO_ROOT/lib/Epub/Epub/parsers/ContainerParser.cpp"
  "$REPO_ROOT/lib/Epub/Epub/parsers/ContentOpfParser.cpp"
  "$REPO_ROOT/lib/Epub/Epub/parsers/TocNcxParser.cpp"
  "$REPO_ROOT/lib/Epub/Epub/parsers/TocNavParser.cpp"
  "$REPO_ROOT/lib/expat/xmlparse.c"
  "$REPO_ROOT/lib/expat/xmltok.c"
  "$REPO_ROOT/lib/expat/xmlrole.c"
  "$REPO_ROOT/lib/JpegToBmpConverter/JpegToBmpConverter.cpp"
  "$REPO_ROOT/lib/ToneCurve/ToneCurve.cpp"
  "$REPO_ROOT/lib/PngToBmpConverter/PngToBmpConverter.cpp"
  "$REPO_ROOT/lib/GfxRenderer/BitmapHelpers.cpp"
  "$SCRIPT_DIR/vendor/JPEGDEC/src/JPEGDEC.cpp"
  "$SCRIPT_DIR/vendor/PNGdec/src/PNGdec.cpp"
  "$REPO_ROOT/lib/EpdFont/EpdFont.cpp"
  "$REPO_ROOT/lib/EpdFont/EpdFontFamily.cpp"
  "$REPO_ROOT/lib/EpdFont/FontDecompressor.cpp"
  "$REPO_ROOT/lib/EpdFont/SdCardFont.cpp"
  "$REPO_ROOT/lib/EpdFont/SdCardFontRegistry.cpp"
  "$REPO_ROOT/lib/EpdFont/SdCardFontManager.cpp"
  "$REPO_ROOT/lib/Utf8/Utf8.cpp"
  "$SCRIPT_DIR/host_shim/GfxRenderer.cpp"
  "$SCRIPT_DIR/vendor/PNGdec/src/adler32.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/crc32.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/inffast.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/inflate.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/inftrees.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/zutil.c"
  "$REPO_ROOT/lib/Epub/Epub.cpp"
  "$REPO_ROOT/lib/Epub/Epub/Section.cpp"
  "$REPO_ROOT/lib/Epub/Epub/Page.cpp"
  "$REPO_ROOT/lib/Epub/Epub/ParsedText.cpp"
  "$REPO_ROOT/lib/Epub/Epub/htmlEntities.cpp"
  "$REPO_ROOT/lib/Epub/Epub/blocks/TextBlock.cpp"
  "$REPO_ROOT/lib/Epub/Epub/blocks/ImageBlock.cpp"
  "$REPO_ROOT/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"
  "$REPO_ROOT/lib/Epub/Epub/hyphenation/Hyphenator.cpp"
  "$REPO_ROOT/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp"
  "$REPO_ROOT/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp"
  "$REPO_ROOT/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp"
  "$REPO_ROOT/lib/Epub/Epub/css/CssParser.cpp"
  "$REPO_ROOT/lib/Epub/Epub/converters/ImageDecoderFactory.cpp"
  "$REPO_ROOT/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp"
  "$REPO_ROOT/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp"
  "$REPO_ROOT/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp"
)

INCLUDES=(
  -I "$SCRIPT_DIR/host_shim"
  -I "$REPO_ROOT/lib/uzlib/src"
  -I "$REPO_ROOT/lib/InflateReader"
  -I "$REPO_ROOT/lib/FsHelpers"
  -I "$REPO_ROOT/lib/ZipFile"
  -I "$REPO_ROOT/lib/Epub"
  -I "$REPO_ROOT/lib/Epub/Epub"
  -I "$REPO_ROOT/lib/Serialization"
  -I "$REPO_ROOT/lib/XmlParserUtils"
  -I "$REPO_ROOT/lib/expat"
  -I "$REPO_ROOT/lib/JpegToBmpConverter"
  -I "$REPO_ROOT/lib/PngToBmpConverter"
  -I "$REPO_ROOT/lib/ToneCurve"
  -I "$REPO_ROOT/lib/GfxRenderer"
  -I "$REPO_ROOT/lib/Memory"
  -I "$SCRIPT_DIR/vendor/JPEGDEC/src"
  -I "$SCRIPT_DIR/vendor/PNGdec/src"
  -I "$SCRIPT_DIR/vendor/ArduinoJson/src"
  -I "$REPO_ROOT/lib/EpdFont"
  -I "$REPO_ROOT/lib/Utf8"
  -I "$REPO_ROOT/lib/MemoryBudget"
  -I "$REPO_ROOT/src"
)

# Mirror build.sh flags exactly so the WASM build produces deterministic
# output identical to the CLI build for the parts that overlap (font
# measurement, layout, image decode/dither).
CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DNO_SIMD
  -DCRUMBLE_PREBAKE_MATCH_DEVICE_DECODE
  -DCRUMBLE_PREBAKE_WASM
  # Emscripten doesn't auto-define __MACH__ / __LINUX__, so JPEGDEC.h +
  # PNGdec.h fall through to the Arduino branch and miss the memcpy_P /
  # PROGMEM identity shims. Force the LINUX path so both vendored
  # decoders pick up the host-build conventions verbatim.
  -D__LINUX__
  -ffp-contract=off
  -fno-fast-math
)
CFLAGS=(
  -O2
  -Wall
  -Wextra
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DNO_SIMD
  -DCRUMBLE_PREBAKE_MATCH_DEVICE_DECODE
  -DCRUMBLE_PREBAKE_WASM
  -D__LINUX__
  -ffp-contract=off
  -fno-fast-math
)

# Emscripten link flags.
# - MODULARIZE + EXPORT_NAME: produce a factory function, so optimizer.js
#   loads the module on demand (`const mod = await CrumblePrebake({...})`).
# - ALLOW_MEMORY_GROWTH: book.bin + sections may briefly need >32 MB; let
#   the runtime grow rather than presizing for the worst case.
# - INVOKE_RUN=0 + EXIT_RUNTIME=1: don't auto-call main() on module init;
#   JS will explicitly invoke the exported core entry point (28.2).
# - ENVIRONMENT=web,worker,node: web for /optimizer, worker for off-main
#   thread heavy work, node for headless smoke tests.
LDFLAGS=(
  -sMODULARIZE=1
  -sEXPORT_NAME=CrumblePrebake
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=64MB
  -sINVOKE_RUN=0
  # EXIT_RUNTIME=0: the JS caller (optimizer.js) reads output files from
  # MEMFS AFTER callMain() returns, which requires libc functions like
  # strerror to stay reachable. With EXIT_RUNTIME=1 emscripten tears down
  # the runtime as soon as main() exits, and the post-run FS.readFile /
  # FS.unlink walk we do to collect book.bin + sections + thumbs hits
  # "native function called after runtime exit" assertions. Keeping the
  # runtime alive costs a few KB of resident JS state per page session,
  # which is fine -- the module is already cached and reused across
  # multiple book prebakes anyway.
  -sEXIT_RUNTIME=0
  -sENVIRONMENT=web,worker,node
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8,stringToUTF8,UTF8ToString
  -sFORCE_FILESYSTEM=1
)

CSRCS=()
CXXSRCS=()
for src in "${SOURCES[@]}"; do
  case "$src" in
    *.c) CSRCS+=("$src");;
    *)   CXXSRCS+=("$src");;
  esac
done

echo "[build-wasm] compiling ${#CSRCS[@]} C + ${#CXXSRCS[@]} C++ sources via emscripten"
OBJDIR="$BUILD_DIR/obj"
mkdir -p "$OBJDIR"
OBJS=()
for src in "${CSRCS[@]}"; do
  obj="$OBJDIR/$(echo "$src" | tr '/' '_' | sed 's/.c$/.o/')"
  "$EMCC" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
  OBJS+=("$obj")
done
for src in "${CXXSRCS[@]}"; do
  obj="$OBJDIR/$(echo "$src" | tr '/' '_' | sed 's/.cpp$/.o/')"
  "$EMXX" "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
  OBJS+=("$obj")
done

echo "[link-wasm] $OUTPUT"
"$EMXX" "${OBJS[@]}" "${LDFLAGS[@]}" -o "$OUTPUT"

echo "[done-wasm] $OUTPUT ($(du -h "$BUILD_DIR"/crumble-prebake.wasm 2>/dev/null | cut -f1) wasm)"
