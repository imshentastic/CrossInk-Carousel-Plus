#pragma once

// Logging shim that routes the firmware's LOG_* macros to stderr.
// Firmware LOG_DBG / LOG_INF / LOG_ERR take (tag, fmt, ...) and would
// normally go through HardwareSerial. On host they're fprintf to stderr
// with the same tag-prefixed format so prebake output stays grep-able.
//
// The CRUMBLE_PREBAKE_LOG_LEVEL env override lets users dial verbosity
// without recompiling: 0 = ERR only, 1 = INF + ERR, 2 = DBG + INF + ERR.
// Default is 1.

#include <cstdio>
#include <cstdlib>
#include <string>

namespace crumble_log {
inline int level() {
  static int cached = -1;
  if (cached < 0) {
    const char* env = std::getenv("CRUMBLE_PREBAKE_LOG_LEVEL");
    cached = env ? std::atoi(env) : 1;
    if (cached < 0) cached = 0;
    if (cached > 2) cached = 2;
  }
  return cached;
}
}  // namespace crumble_log

#define LOG_ERR(tag, fmt, ...)                              \
  do {                                                      \
    if (crumble_log::level() >= 0) {                        \
      std::fprintf(stderr, "[ERR] [%s] " fmt "\n", tag, ##__VA_ARGS__); \
    }                                                       \
  } while (0)

#define LOG_INF(tag, fmt, ...)                              \
  do {                                                      \
    if (crumble_log::level() >= 1) {                        \
      std::fprintf(stderr, "[INF] [%s] " fmt "\n", tag, ##__VA_ARGS__); \
    }                                                       \
  } while (0)

#define LOG_DBG(tag, fmt, ...)                              \
  do {                                                      \
    if (crumble_log::level() >= 2) {                        \
      std::fprintf(stderr, "[DBG] [%s] " fmt "\n", tag, ##__VA_ARGS__); \
    }                                                       \
  } while (0)

// Some firmware code uses logSerial.printf for binary / raw output --
// stub as a stderr passthrough. Not common in our compiled subset.
struct LogSerialShim {
  template <class... Args>
  void printf(const char* fmt, Args... args) {
    std::fprintf(stderr, fmt, args...);
  }
  template <class... Args>
  void write(Args...) {}
};
inline LogSerialShim logSerial;
