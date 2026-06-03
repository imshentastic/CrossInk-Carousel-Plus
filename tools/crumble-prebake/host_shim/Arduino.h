#pragma once

// Minimal Arduino.h stub for the off-device crumble-prebake CLI build.
// Provides just enough surface for the EPUB pipeline code we compile-share
// with firmware. Definitely NOT a faithful Arduino-ESP32 port -- only the
// pieces the parsers / BookMetadataCache / ZipFile actually touch.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// Arduino's String class -- aliased to std::string. The few firmware-side
// helpers that depend on Arduino-String-specific behaviour (Stream parse,
// reserve patterns) are stubbed individually where needed.
using String = std::string;

// Arduino byte type.
using byte = uint8_t;

// Arduino timing helpers. millis() and micros() are monotonic since
// process start; delay() is a no-op on host (we never need it for prebake).
inline uint32_t millis() {
  static const auto t0 = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count());
}
inline uint32_t micros() {
  static const auto t0 = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count());
}
inline void delay(uint32_t) {}
inline void yield() {}

// Arduino "F()" PROGMEM macro -- identity passthrough on host.
#ifndef F
#define F(x) (x)
#endif

// Intentionally NOT shimming Arduino's min(a,b) / max(a,b) macros. They
// collide with std::numeric_limits<T>::max() in the firmware-side sources
// we compile-share (BookMetadataCache.cpp hits this directly). The
// firmware sources reach for std::min / std::max everywhere relevant,
// so removing the Arduino-style macros is harmless on host.

// ESP-IDF heap-stat shim. The on-device code logs free/maxAlloc to help
// diagnose OOM; on host we report a huge fake value so nothing-cares-about-
// this branches stay benign.
struct EspShim {
  static uint32_t getFreeHeap() { return 1u << 30; }
  static uint32_t getMaxAllocHeap() { return 1u << 30; }
};
inline EspShim ESP{};
