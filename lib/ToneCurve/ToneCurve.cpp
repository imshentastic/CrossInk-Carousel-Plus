#include "ToneCurve.h"

#include <cmath>

namespace {

// CrumBLE 4.6: tone curves are RANGE-COMPRESSION ("clamping"), not contrast
// expansion. The complaint we're addressing is "covers look dark and
// crushed-to-black on e-ink" -- source color covers, mastered for LCD/OLED,
// land mostly in the 0-128 range after grayscale conversion, and 1-bit
// Atkinson dither collapses all of <50 to solid black. Compressing the
// source range to [lo..hi] before dither means the dither has more
// middle-gray values to stipple with -- crushed-black areas become visibly
// gray-stippled, and over-bright whites get visible texture too.
//
// Linear remap: output = lo + (input/255) * (hi - lo).
//
// MILD: gentle compression to 30..225. Source 0 -> 30 (~12% white stipple
// after dither); source 128 -> ~125 (unchanged); source 255 -> 225 (~88%
// white). Subtle softening of extremes, preserves most local contrast.
void fillMildLut(uint8_t lut[256]) {
  constexpr double kLo = 30.0;
  constexpr double kHi = 225.0;
  constexpr double kSpan = kHi - kLo;
  for (int i = 0; i < 256; ++i) {
    const double v = kLo + (i / 255.0) * kSpan;
    const int q = static_cast<int>(v + 0.5);
    lut[i] = static_cast<uint8_t>(q < 0 ? 0 : q > 255 ? 255 : q);
  }
}

// STRONG: aggressive compression to 60..200. Source 0 -> 60 (~24% white
// stipple); source 128 -> ~130 (unchanged); source 255 -> 200 (~78% white).
// Heavily-disperse multi-gray look; dark dust-jacket photos that previously
// rendered as black silhouettes become visibly grayscale-textured covers.
// Cost: max local contrast is reduced (200/60 ratio vs 255/0), so sharp
// graphic-novel art may look slightly washed.
void fillStrongLut(uint8_t lut[256]) {
  constexpr double kLo = 60.0;
  constexpr double kHi = 200.0;
  constexpr double kSpan = kHi - kLo;
  for (int i = 0; i < 256; ++i) {
    const double v = kLo + (i / 255.0) * kSpan;
    const int q = static_cast<int>(v + 0.5);
    lut[i] = static_cast<uint8_t>(q < 0 ? 0 : q > 255 ? 255 : q);
  }
}

}  // namespace

void ToneCurve::buildLut(uint8_t curve, uint8_t outLut[256]) {
  switch (curve) {
    case COVER_TONE_MILD:
      fillMildLut(outLut);
      return;
    case COVER_TONE_STRONG:
      fillStrongLut(outLut);
      return;
    case COVER_TONE_OFF:
    default:
      for (int i = 0; i < 256; ++i) outLut[i] = static_cast<uint8_t>(i);
      return;
  }
}
