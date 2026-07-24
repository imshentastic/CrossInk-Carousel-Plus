#pragma once

#include <HardwareSerial.h>

#include <string>

/*
Define ENABLE_SERIAL_LOG to enable logging
Can be set in platformio.ini build_flags or as a compile definition

Define LOG_LEVEL to control log verbosity:
0 = ERR only
1 = ERR + INF
2 = ERR + INF + DBG
If not defined, defaults to 0

If you have a legitimate need for raw Serial access (e.g., binary data,
special formatting), use the underlying logSerial object directly:
    logSerial.printf("Special case: %d\n", value);
    logSerial.write(binaryData, length);

The logSerial reference (defined below) points to the real Serial object and
won't trigger deprecation warnings.
*/

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

static HWCDC& logSerial = Serial;

void logPrintf(const char* level, const char* origin, const char* format, ...);

#ifdef ENABLE_SERIAL_LOG
#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#endif

#if LOG_LEVEL >= 1
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...)
#endif

#if LOG_LEVEL >= 2
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...)
#endif
#else
#define LOG_DBG(origin, format, ...)
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#endif

std::string getLastLogs();
void clearLastLogs();

// v18.9.9.332: lightweight "last known operation" beacon for crash reports.
// Call SET_CHECKPOINT("some-short-name") at the top of any code path we
// want to correlate with terminate/panic events. The std::set_terminate
// handler in main.cpp reads this and logs it alongside heap-at-terminate,
// so a "bad_alloc during BT enable" or "bad_alloc during section load"
// becomes distinguishable from a "bad_alloc during Home render" in the
// crash log. Storage is a single 48-byte static buffer -- cheap, no
// alloc. Overwrites on every call; only the LATEST checkpoint is
// preserved, which is exactly what we want ("what was in flight when
// we died"). Safe to call from any task.
void setLastCheckpoint(const char* name);
const char* getLastCheckpoint();
// v18.9.9.334: read + clear the RTC-persisted checkpoint from the previous
// boot. Returns nullptr on cold boot / already-consumed / empty. Call once
// early in setup() to surface the last-known operation across ANY reboot
// cause (std::terminate, ESP-IDF panic_abort, Guru Meditation, watchdog).
const char* consumeCheckpointFromPrevBoot();
#define SET_CHECKPOINT(name) setLastCheckpoint(name)
// Validates the RTC log state (magic word + logHead range). Returns true if
// corruption was detected (magic mismatch or logHead out of range), meaning
// logMessages is untrusted garbage. Callers should call clearLastLogs() when
// this returns true so getLastLogs() does not dump corrupt data into crash reports.
bool sanitizeLogHead();

class MySerialImpl : public Print {
 public:
  void begin(unsigned long baud) { logSerial.begin(baud); }

  // Support boolean conversion for compatibility with code like:
  //   if (Serial) or while (!Serial)
  operator bool() const { return logSerial; }

  __attribute__((deprecated("Use LOG_* macro instead"))) size_t printf(const char* format, ...);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
  static MySerialImpl instance;
};

#ifdef Serial
#undef Serial
#endif
#define Serial MySerialImpl::instance
