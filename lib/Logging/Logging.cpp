#include "Logging.h"

#include <string>

#ifdef SIMULATOR
#include <Arduino.h>

MySerialImpl MySerialImpl::instance;

size_t MySerialImpl::write(uint8_t b) { return logSerial.write(b); }
size_t MySerialImpl::write(const uint8_t* buffer, size_t size) { return logSerial.write(buffer, size); }
void MySerialImpl::flush() { logSerial.flush(); }
#endif

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// (0..MAX_LOG_LINES-1) by chance even though logMessages is garbage. The magic
// value is only set by clearLastLogs(), so its absence means the buffer was
// never properly initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // If the magic is wrong or logHead is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      const size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      output.append(logMessages[idx], len);
    }
  }
  return output;
}

// Checks whether the RTC log state is consistent: rtcLogMagic must equal
// LOG_RTC_MAGIC and logHead must be in 0..MAX_LOG_LINES-1. Returns true if
// corruption is detected, in which case rtcLogMagic is still invalid and
// logMessages may contain garbage. Callers (e.g. HalSystem::begin on the
// panic-reboot path) must call clearLastLogs() after a true result to fully
// reinitialize the ring buffer and stamp the magic before getLastLogs() is used.
bool sanitizeLogHead() {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}

// v18.9.9.332: last-known-operation beacon. See Logging.h for design notes.
// v18.9.9.334: storage moved to RTC_NOINIT so it survives ANY crash type,
// not just std::terminate. ESP-IDF panics (heap-poisoning asserts, Guru
// Meditation faults, wpa_supplicant null derefs) bypass our C++ terminate
// handler and go through the framework's panic_abort path -- but they
// still preserve RTC_NOINIT memory across the auto-reboot. On next boot,
// consumeCheckpointFromPrevBoot() reads and clears it so setup() can log
// what the previous boot was doing when it died.
constexpr uint32_t kCheckpointMagic = 0xC1EC1004u;
RTC_NOINIT_ATTR uint32_t gCheckpointMagic;
RTC_NOINIT_ATTR char gLastCheckpoint[48];

void setLastCheckpoint(const char* name) {
  if (gCheckpointMagic != kCheckpointMagic) {
    gCheckpointMagic = kCheckpointMagic;
    gLastCheckpoint[0] = '\0';
  }
  if (!name) { gLastCheckpoint[0] = '\0'; return; }
  // strncpy + explicit NUL: no allocation, safe from any task.
  size_t i = 0;
  while (i < sizeof(gLastCheckpoint) - 1 && name[i]) {
    gLastCheckpoint[i] = name[i];
    ++i;
  }
  gLastCheckpoint[i] = '\0';
}
const char* getLastCheckpoint() {
  if (gCheckpointMagic != kCheckpointMagic) return "(uninit)";
  return gLastCheckpoint[0] ? gLastCheckpoint : "(none)";
}

// Called once in setup() after boot-reset diagnostic. Reads the RTC-persisted
// checkpoint from the previous boot (if any) so setup() can log it. Then
// clears it so a clean boot doesn't keep re-logging stale data. Returns
// nullptr when RTC is uninitialised (cold boot) or already consumed.
const char* consumeCheckpointFromPrevBoot() {
  if (gCheckpointMagic != kCheckpointMagic) return nullptr;
  if (gLastCheckpoint[0] == '\0') return nullptr;
  // Copy to static buffer so caller can log after clear (though our caller
  // typically logs inline; belt-and-suspenders in case anyone extends).
  static char sPrev[48];
  strncpy(sPrev, gLastCheckpoint, sizeof(sPrev) - 1);
  sPrev[sizeof(sPrev) - 1] = '\0';
  gLastCheckpoint[0] = '\0';
  return sPrev;
}
