#pragma once

// Reading fonts have generated variants with identical variable names:
//   default: emoji/symbol fallback + PHM CJK fallback
//   OMIT_PHM: emoji/symbol fallback, no PHM CJK
//   OMIT_EMOJI_FONTS: primary fonts only, no emoji and no PHM CJK
//
// Generate the variants with lib/EpdFont/scripts/convert-builtin-fonts.sh.
//
// Per-size guards:
//   OMIT_TEENSY_FONT - excludes 8px (Teensy) reading fonts; used by env:xlarge
//   OMIT_ITTY_BITTY_FONT  - excludes 9px (Itty Bitty) reading fonts; used by env:xlarge
//   OMIT_TINY_FONT   - excludes 10px (Tiny) reading fonts; used by env:xlarge
//   OMIT_SMALL_FONT  - excludes 12px (Small) reading fonts
//   OMIT_MEDIUM_FONT - excludes 14px (Medium) reading fonts
//   OMIT_LARGE_FONT  - excludes 16px (Large) reading fonts
//   OMIT_XLARGE_FONT - excludes 18px (Extra Large) reading fonts; used by env:tiny
//   OMIT_HUGE_FONT   - excludes 20px (Huge) reading fonts; used by all firmware envs except env:xlarge
#ifdef OMIT_EMOJI_FONTS
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/noemoji/name.h>
#elif defined(OMIT_PHM)
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/nophm/name.h>
#else
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/name.h>
#endif

// CrumBLE 4.2.1: OMIT_BITTER_FONT drops the entire Bitter family. Used by
// the tiny-lexend and tiny-chareink variant builds where Lexend or CharEink
// is the only built-in reader font and Bitter is omitted to free OTA-partition
// space. With Bitter omitted, the picker hides the BITTER option and the
// fallback chain in CrossPointSettings.cpp routes to whichever family IS
// compiled in (LEXENDDECA or CHAREINK) via BUILTIN_DEFAULT_FONT_FAMILY.
#ifndef OMIT_BITTER_FONT
#ifndef OMIT_TEENSY_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_8_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_8_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_8_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_8_regular)
#endif
#ifndef OMIT_ITTY_BITTY_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_9_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_9_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_9_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_9_regular)
#endif
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_10_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_10_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_12_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_12_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_12_regular)
#endif
#ifndef OMIT_MEDIUM_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_14_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_14_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_14_regular)
#endif
#ifndef OMIT_LARGE_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_16_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_16_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_16_regular)
#endif
#ifndef OMIT_XLARGE_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_18_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_18_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_18_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_18_regular)
#endif
#ifndef OMIT_HUGE_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_20_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_20_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_20_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_20_regular)
#endif
#endif  // OMIT_BITTER_FONT

// CrumBLE: OMIT_CHAREINK_FONT drops the entire CharEink family. Saves
// ~700-900 KB binary -- used in the OTA-fit "slim" release build so
// the firmware stays under the 6.25 MB legacy partition. The picker
// in SettingsList.h omits the CharEink option in lockstep; users who
// had CharEink selected before this flag turned on fall back to
// Bitter via getFallbackReaderFontIdForFamily.
#ifndef OMIT_CHAREINK_FONT
#ifndef OMIT_TEENSY_FONT
#include BUILTIN_READING_FONT_HEADER(charein_8_bold)
#include BUILTIN_READING_FONT_HEADER(charein_8_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_8_italic)
#include BUILTIN_READING_FONT_HEADER(charein_8_regular)
#endif
#ifndef OMIT_ITTY_BITTY_FONT
#include BUILTIN_READING_FONT_HEADER(charein_9_bold)
#include BUILTIN_READING_FONT_HEADER(charein_9_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_9_italic)
#include BUILTIN_READING_FONT_HEADER(charein_9_regular)
#endif
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(charein_10_bold)
#include BUILTIN_READING_FONT_HEADER(charein_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_10_italic)
#include BUILTIN_READING_FONT_HEADER(charein_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(charein_12_bold)
#include BUILTIN_READING_FONT_HEADER(charein_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_12_italic)
#include BUILTIN_READING_FONT_HEADER(charein_12_regular)
#endif
#ifndef OMIT_MEDIUM_FONT
#include BUILTIN_READING_FONT_HEADER(charein_14_bold)
#include BUILTIN_READING_FONT_HEADER(charein_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_14_italic)
#include BUILTIN_READING_FONT_HEADER(charein_14_regular)
#endif
#ifndef OMIT_LARGE_FONT
#include BUILTIN_READING_FONT_HEADER(charein_16_bold)
#include BUILTIN_READING_FONT_HEADER(charein_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_16_italic)
#include BUILTIN_READING_FONT_HEADER(charein_16_regular)
#endif
#ifndef OMIT_XLARGE_FONT
#include BUILTIN_READING_FONT_HEADER(charein_18_bold)
#include BUILTIN_READING_FONT_HEADER(charein_18_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_18_italic)
#include BUILTIN_READING_FONT_HEADER(charein_18_regular)
#endif
#ifndef OMIT_HUGE_FONT
#include BUILTIN_READING_FONT_HEADER(charein_20_bold)
#include BUILTIN_READING_FONT_HEADER(charein_20_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_20_italic)
#include BUILTIN_READING_FONT_HEADER(charein_20_regular)
#endif
#endif  // OMIT_CHAREINK_FONT

// CrumBLE: OMIT_LEXENDDECA_FONT drops the entire Lexend Deca family. Saves
// ~600 KB binary. Used in the slim "fits 6.25 MB legacy OTA partition"
// build alongside OMIT_CHAREINK_FONT. With both off, only Bitter remains
// as a built-in reading font; Lexend can be installed as an SD-card
// .cpfont, and the picker hides the missing built-in.
#ifndef OMIT_LEXENDDECA_FONT
#ifndef OMIT_TEENSY_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_regular)
#endif
#ifndef OMIT_ITTY_BITTY_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_9_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_9_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_9_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_9_regular)
#endif
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_regular)
#endif
#ifndef OMIT_MEDIUM_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_regular)
#endif
#ifndef OMIT_LARGE_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_regular)
#endif
#ifndef OMIT_XLARGE_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_regular)
#endif
#ifndef OMIT_HUGE_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_regular)
#endif
#endif  // OMIT_LEXENDDECA_FONT

#undef BUILTIN_READING_FONT_HEADER

// UI fonts - no emoji or PHM variants.
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/inter_8_regular.h>

// tiny-cjk variant: flash-resident CJK fallback face (LXGW WenKai, 2-bit,
// UNCOMPRESSED so glyph bitmaps are memory-mapped straight from .rodata with
// zero heap at draw time). GB2312 level 1+2 hanzi + kana + CJK punctuation +
// fullwidth forms; one physical size shared by every reader font size.
// Regenerate with lib/EpdFont/scripts/build-cjk-flash-font.sh.
#ifdef CJK_VARIANT
#include <builtinFonts/cjk_16_regular.h>
#endif
