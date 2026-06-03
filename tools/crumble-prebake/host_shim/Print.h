#pragma once

// Minimal Print stub. The firmware's Print class (from Arduino) is the
// base of HalFile + other output sinks. The EPUB pipeline only uses the
// raw `write(const uint8_t*, size_t)` and `write(uint8_t)` overloads, so
// that's all we expose. write(uint8_t) is left pure-virtual to force
// subclasses (HalFile in our shim, plus any others we add later) to
// implement it.

#include <cstddef>
#include <cstdint>
#include <string>

class Print {
 public:
  virtual ~Print() = default;

  // Required for any concrete subclass.
  virtual size_t write(uint8_t b) = 0;

  // Bulk-write -- subclasses MUST override. The original shim had a
  // default loop over write(uint8_t) here, but it was being picked over
  // ContentOpfParser's override at runtime, leading to a stack overflow
  // via repeated single-byte virtual-dispatch loops. Making it pure
  // forces the override to actually take effect (or fails at link).
  virtual size_t write(const uint8_t* buf, size_t size) = 0;
  size_t write(const char* str) {
    if (!str) return 0;
    return write(reinterpret_cast<const uint8_t*>(str), std::strlen(str));
  }
  // INTENTIONALLY no write(const void*, size_t) overload here -- it shadows
  // overload resolution for write(uint8_t*, size_t) calls in firmware code
  // (specifically ZipFile's stream-emit loop), routing them through the
  // default Print::write(uint8_t*, size_t) impl and back through write(uint8_t)
  // until the stack runs out. Keep the void surface in HalFile.cpp where it
  // can't leak into virtual dispatch.

  // Convenience helpers some firmware code calls. printf is the
  // commonly-touched one for diagnostic logging.
  size_t print(const std::string& s) { return write(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
  size_t print(const char* s) { return write(s); }
  size_t println(const std::string& s) {
    size_t n = print(s);
    n += write('\n');
    return n;
  }
};
