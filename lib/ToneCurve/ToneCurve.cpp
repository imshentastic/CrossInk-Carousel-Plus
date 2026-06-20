#include "ToneCurve.h"

#include <cmath>

namespace {

// MILD: gamma 1.5 -- out = pow(in/255, 1/1.5) * 255. Lifts midtones a bit
// (a source 128 maps to ~167) without crushing extremes. Safe default for
// most cover types including comics/manga.
void fillMildLut(uint8_t lut[256]) {
  constexpr double kInvGamma = 1.0 / 1.5;
  for (int i = 0; i < 256; ++i) {
    const double v = std::pow(i / 255.0, kInvGamma) * 255.0;
    const int q = static_cast<int>(v + 0.5);
    lut[i] = static_cast<uint8_t>(q < 0 ? 0 : q > 255 ? 255 : q);
  }
}

// STRONG: two-stage curve mirroring the user's Photoshop sleep-image trick.
// Stage 1 -- compress source dynamic range so detail in deep blacks (<85)
// and bright whites (>200) doesn't compete with the midtones we care about
// on e-ink. Inputs <85 clip to 0; inputs >200 clip to 255; the 85..200
// band gets stretched across 0..255.
// Stage 2 -- sigmoid contrast (S-curve) re-spreads the now-compressed
// midtones around their new center to restore local contrast.
// Net effect: noticeably brighter midtones with preserved edge contrast.
void fillStrongLut(uint8_t lut[256]) {
  constexpr int kInLo = 85;
  constexpr int kInHi = 200;
  constexpr double kRange = kInHi - kInLo;
  constexpr double kCenter = 128.0;
  constexpr double kSlope = 0.025;  // tuned: sharper than 0.02, gentler than 0.04

  for (int i = 0; i < 256; ++i) {
    // Stage 1: linear stretch of [85..200] -> [0..255]
    double stretched;
    if (i <= kInLo) {
      stretched = 0.0;
    } else if (i >= kInHi) {
      stretched = 255.0;
    } else {
      stretched = (i - kInLo) * 255.0 / kRange;
    }

    // Stage 2: sigmoid S-curve around midpoint
    const double s = 255.0 / (1.0 + std::exp(-(stretched - kCenter) * kSlope));

    const int q = static_cast<int>(s + 0.5);
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
