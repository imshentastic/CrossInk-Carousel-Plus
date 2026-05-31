#!/usr/bin/env bash
set -euo pipefail

# Host-side build of crumble-prebake. Mirrors test/run_cmb_roundtrip_test.sh's
# bare-g++ pattern -- no PlatformIO, no Arduino toolchain. Resolves the repo
# root via git so the script works from any CWD inside the worktree.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BINARY="$BUILD_DIR/crumble-prebake"

mkdir -p "$BUILD_DIR"

# Phase 1 source set. host_shim/ first in include path so its Arduino.h /
# HalStorage.h take precedence over the firmware originals. Firmware-side
# sources get added piecewise as each phase grows.
SOURCES=(
  "$SCRIPT_DIR/src/main.cpp"
  "$SCRIPT_DIR/host_shim/HalStorage.cpp"
  # Firmware-side sources we compile-share. Order doesn't matter for the
  # linker but listing them roughly bottom-up makes the dependency story
  # easier to read at a glance.
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
  # Expat -- the on-device fork lives in lib/expat. Build the same three
  # translation units the firmware does (xmlparse/xmltok/xmlrole).
  "$REPO_ROOT/lib/expat/xmlparse.c"
  "$REPO_ROOT/lib/expat/xmltok.c"
  "$REPO_ROOT/lib/expat/xmlrole.c"
  # Phase 2A: image pipeline (cover -> resized 1bpp BMP).
  # JpegToBmpConverter + PngToBmpConverter are firmware-side wrappers;
  # they delegate to the vendored JPEGDEC + PNGdec decoders below.
  "$REPO_ROOT/lib/JpegToBmpConverter/JpegToBmpConverter.cpp"
  "$REPO_ROOT/lib/PngToBmpConverter/PngToBmpConverter.cpp"
  "$REPO_ROOT/lib/GfxRenderer/BitmapHelpers.cpp"
  # Vendored decoders -- copies of bitbank2/JPEGDEC + bitbank2/PNGdec at the
  # same upstream pins the firmware platformio.ini uses, with the
  # scripts/jpegdec_patches/* applied (DC-write + wild-pointer fixes for
  # progressive grayscale JPEGs). Both decoders already #ifdef cleanly for
  # __MACH__ so they build without modification on macOS host.
  "$SCRIPT_DIR/vendor/JPEGDEC/src/JPEGDEC.cpp"
  "$SCRIPT_DIR/vendor/PNGdec/src/PNGdec.cpp"
  # PNGdec bundles a copy of zlib in its src/ dir for the inflate path used
  # to decompress IDAT chunks. We pull only the chunks PNGdec.cpp actually
  # references (not infback/inflate proper for the cover-decoding use case;
  # add them if a real cover trips an undefined symbol).
  "$SCRIPT_DIR/vendor/PNGdec/src/adler32.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/crc32.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/inffast.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/inflate.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/inftrees.c"
  "$SCRIPT_DIR/vendor/PNGdec/src/zutil.c"
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
  # Phase 2A includes.
  -I "$REPO_ROOT/lib/JpegToBmpConverter"
  -I "$REPO_ROOT/lib/PngToBmpConverter"
  -I "$REPO_ROOT/lib/GfxRenderer"
  -I "$REPO_ROOT/lib/Memory"
  -I "$SCRIPT_DIR/vendor/JPEGDEC/src"
  -I "$SCRIPT_DIR/vendor/PNGdec/src"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  # -pedantic is intentionally omitted: ##__VA_ARGS__ in Logging.h is a
  # GNU extension that's universally accepted but warns under pedantic.
  # The on-device build doesn't enable pedantic either.
  # expat flags must match the firmware build verbatim. XML_GE=0 disables
  # general-entity expansion; XML_CONTEXT_BYTES=1024 sets the parse-error
  # context window. Both are pulled from the on-device platformio.ini.
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  # Phase 2A: force JPEGDEC to take the same C-reference code paths the
  # device uses (ESP32-C3 RV32 defines neither HAS_NEON nor ALLOWS_UNALIGNED).
  # Without this, host arm64 builds take the NEON SIMD IDCT path and
  # produce different pixel values than the device, breaking byte-match.
  # See vendor/JPEGDEC/src/JPEGDEC.h for the corresponding #undef block.
  -DCRUMBLE_PREBAKE_MATCH_DEVICE_DECODE
  # Floating-point determinism: the resize step in JpegToBmpConverter and
  # color/dither math elsewhere occasionally pivot on `float`. Without
  # these flags the host compiler is free to fuse multiply-add (FMA),
  # round in 80-bit extended precision, or reorder operations relative
  # to source order, producing tiny pixel-value drifts that cascade
  # through error-diffusion dither. -ffp-contract=off disables FMA
  # fusion; -fno-fast-math keeps strict IEEE 754 (no -Ofast-style
  # reassociation).
  -ffp-contract=off
  -fno-fast-math
)
# Separate flag set for the C TUs we pull in (expat + uzlib). Same
# overall hygiene, but C-only switches.
CFLAGS=(
  -O2
  -Wall
  -Wextra
  # Match the C++ side's expat config -- both must agree or xmlparse.c
  # rejects the build.
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DCRUMBLE_PREBAKE_MATCH_DEVICE_DECODE
  -ffp-contract=off
  -fno-fast-math
)

CXX="${CXX:-g++}"
# Pick a default C compiler that pairs with the C++ one. The standard
# heuristic strip ${CXX%++} only works for c++ -> cc; g++/clang++ need a
# named lookup.
case "${CXX}" in
  g++*)     CC="${CC:-gcc}";;
  clang++*) CC="${CC:-clang}";;
  c++*)     CC="${CC:-cc}";;
  *)        CC="${CC:-cc}";;
esac

# Split SOURCES into C vs C++ since the toolchains take different flag sets.
CSRCS=()
CXXSRCS=()
for src in "${SOURCES[@]}"; do
  case "$src" in
    *.c) CSRCS+=("$src");;
    *)   CXXSRCS+=("$src");;
  esac
done

echo "[build] compiling ${#CSRCS[@]} C + ${#CXXSRCS[@]} C++ sources"
OBJDIR="$BUILD_DIR/obj"
mkdir -p "$OBJDIR"
OBJS=()
for src in "${CSRCS[@]}"; do
  obj="$OBJDIR/$(echo "$src" | tr '/' '_' | sed 's/.c$/.o/')"
  "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
  OBJS+=("$obj")
done
for src in "${CXXSRCS[@]}"; do
  obj="$OBJDIR/$(echo "$src" | tr '/' '_' | sed 's/.cpp$/.o/')"
  "$CXX" "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
  OBJS+=("$obj")
done

echo "[link]  $BINARY"
"$CXX" "${OBJS[@]}" -o "$BINARY"

echo "[done]  $BINARY"
