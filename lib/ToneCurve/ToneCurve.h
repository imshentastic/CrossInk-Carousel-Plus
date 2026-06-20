#pragma once

#include <cstdint>
#include <cstddef>

// CrumBLE 4.6: tone curve adjustment applied to grayscale cover scanlines
// before dithering. Source images mastered for LCD/OLED (gamma ~2.2, high
// contrast) look muddy on e-ink because the panel's effective response is
// much steeper -- source midtones map to ink densities the panel renders as
// dark gray. Lifting midtones via a curve before dither produces a visibly
// brighter cover without losing local detail.
//
// Applied in JpegToBmpConverter + PngToBmpConverter between the grayscale
// scanline and the dither step. Same code path runs in the off-device WASM
// prebake (where most covers go through) and on the firmware-side thumb
// generator (uploads without optimizer).

enum CoverToneCurve : uint8_t {
  COVER_TONE_OFF = 0,     // identity -- back-compat default
  COVER_TONE_MILD = 1,    // gentle midtone lift (gamma 1.5)
  COVER_TONE_STRONG = 2,  // dynamic-range compression (85..200 -> 0..255) + sigmoid contrast
};

class ToneCurve {
 public:
  // Fills a 256-byte LUT for the given curve. Caller owns storage.
  // For COVER_TONE_OFF this still fills the LUT (identity); callers can
  // optionally skip the LUT pass via isNoop() for a small perf win.
  static void buildLut(uint8_t curve, uint8_t outLut[256]);

  // True iff applying this curve is a no-op (just call sites can skip).
  static bool isNoop(uint8_t curve) { return curve == COVER_TONE_OFF; }

  // Apply LUT in-place to a row of grayscale pixels. No-op if lut is nullptr.
  static void applyRow(uint8_t* row, size_t len, const uint8_t* lut) {
    if (!lut) return;
    for (size_t i = 0; i < len; ++i) row[i] = lut[row[i]];
  }
};
