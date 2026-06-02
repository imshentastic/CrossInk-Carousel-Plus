#pragma once

// Host-side HalDisplay stub. JpegToBmpConverter + PngToBmpConverter
// pull this in mostly for the ambient `display` global (used by the
// no-arg jpegFileToBmpStream path that infers viewport from runtime
// panel dimensions) and for ESP heap-stat checks reachable through the
// Arduino include chain.
//
// On host we don't have a panel, but the linker still wants the symbol
// because the unused path is part of the same translation unit. So
// provide a tiny stub class + global that report stand-in dimensions
// (matches X4 landscape: 800x480) and pull Arduino.h for ESP.

#include <Arduino.h>  // brings ESP{} shim into scope for heap checks

#include <cstdint>

class HalDisplay {
 public:
  // The on-device class swaps these for portrait cover layout
  // (getDisplayHeight returns the screen's wide dimension). We mirror
  // that swap to keep the same semantics for any path that touches us.
  static uint16_t getDisplayWidth() { return 480; }
  static uint16_t getDisplayHeight() { return 800; }
};

// The on-device firmware exposes a global `display` of type HalDisplay
// (see lib/hal/HalDisplay.cpp). Mirror it here so converter call sites
// that reach for `display.getDisplayWidth()` link cleanly. inline so
// multiple TUs see the same one-definition-rule-compliant symbol.
inline HalDisplay display{};
