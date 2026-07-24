#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the DS3231 RTC is present on this device
  bool isAvailable() const { return _available; }

  // v18.9.9.304: true when the device has a valid current time -- either
  // via DS3231 (X3) or via SNTP-set system clock (X4 or X3-without-DS3231).
  // Callers that want to display / act on wall-clock time should gate on
  // this instead of isAvailable(); the latter only knows about the external
  // RTC chip and returns false on X4 even after a successful NTP sync.
  bool hasValidTime() const;

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // v18.9.9.80: get current date. dayOfWeek 1-7 (1=Sunday per DS3231 convention),
  // day 1-31, month 1-12, year 2000-2099 (DS3231 stores 00-99 offset from 2000).
  // Returns false if RTC is unavailable.
  bool getDate(uint8_t& dayOfWeek, uint8_t& day, uint8_t& month, uint16_t& year) const;

  // Compact YYYYMMDD stamp for streak/date-anchored persistence. Returns 0 on
  // failure (RTC unavailable). Callers can compare directly to detect a new day.
  uint32_t getYyyymmdd() const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the DS3231 RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
  bool writeDateToRTC(uint8_t dayOfWeek, uint8_t day, uint8_t month, uint16_t year);
};
