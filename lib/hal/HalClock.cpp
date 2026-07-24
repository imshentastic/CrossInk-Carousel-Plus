#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::hasValidTime() const {
  // v18.9.9.304: valid on X3 whenever DS3231 answers, valid on X4 (or any
  // device without DS3231) whenever the ESP32 system clock has been set
  // past the 2020-01-01 threshold (same one ReadingStats uses).
  if (_available) return true;
  const time_t now = time(nullptr);
  return now >= static_cast<time_t>(1577836800);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) {
    // v18.9.9.304: fall back to ESP32 system clock when DS3231 isn't
    // present. Same code path that ReadingStats uses -- once NTP has
    // set the system clock, time() returns real UTC seconds.
    const time_t sys = time(nullptr);
    if (sys < static_cast<time_t>(1577836800)) return false;
    struct tm t;
    if (gmtime_r(&sys, &t) == nullptr) return false;
    hour = static_cast<uint8_t>(t.tm_hour);
    minute = static_cast<uint8_t>(t.tm_min);
    return true;
  }

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

// v18.9.9.80: DS3231 date registers:
//   0x03: Day of week (1-7)  (raw 3-bit; no BCD conversion needed for 1-7)
//   0x04: Day of month       (BCD, 1-31)
//   0x05: Month + century    (BCD 1-12 in low nibble; bit 7 = century since 2000)
//   0x06: Year               (BCD 00-99 offset from 2000, or from 2100 if century bit set)
static constexpr uint8_t DS3231_DOW_REG = 0x03;

bool HalClock::getDate(uint8_t& dayOfWeek, uint8_t& day, uint8_t& month, uint16_t& year) const {
  if (!_available) {
    // v18.9.9.304: fall back to ESP32 system clock, same as getTime.
    const time_t sys = time(nullptr);
    if (sys < static_cast<time_t>(1577836800)) return false;
    struct tm t;
    if (gmtime_r(&sys, &t) == nullptr) return false;
    // struct tm: tm_wday 0=Sunday. DS3231 convention is dayOfWeek 1-7
    // where 1=Sunday, so +1.
    dayOfWeek = static_cast<uint8_t>(t.tm_wday + 1);
    day = static_cast<uint8_t>(t.tm_mday);
    month = static_cast<uint8_t>(t.tm_mon + 1);
    year = static_cast<uint16_t>(1900 + t.tm_year);
    return true;
  }

  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_DOW_REG);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)4);
  if (Wire.available() < 4) return false;

  dayOfWeek = Wire.read() & 0x07;
  day = bcdToDec(Wire.read() & 0x3F);
  const uint8_t rawMonth = Wire.read();
  month = bcdToDec(rawMonth & 0x1F);
  const uint8_t rawYear = Wire.read();
  const uint16_t centuryBase = (rawMonth & 0x80) ? 2100 : 2000;
  year = centuryBase + bcdToDec(rawYear);
  return true;
}

uint32_t HalClock::getYyyymmdd() const {
  uint8_t dow, day, month;
  uint16_t year;
  if (!getDate(dow, day, month, year)) return 0;
  return static_cast<uint32_t>(year) * 10000u + static_cast<uint32_t>(month) * 100u + static_cast<uint32_t>(day);
}

bool HalClock::writeDateToRTC(uint8_t dayOfWeek, uint8_t day, uint8_t month, uint16_t year) {
  if (day < 1 || day > 31 || month < 1 || month > 12 || year < 2000 || year > 2199) return false;
  if (dayOfWeek < 1 || dayOfWeek > 7) return false;
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_DOW_REG);
  Wire.write(dayOfWeek & 0x07);
  Wire.write(decToBcd(day));
  const uint8_t monthByte = decToBcd(month) | (year >= 2100 ? 0x80 : 0x00);
  Wire.write(monthByte);
  Wire.write(decToBcd(year >= 2100 ? (year - 2100) : (year - 2000)));
  return Wire.endTransmission() == 0;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  // v18.9.9.292: allow NTP sync on X4 (no DS3231). The ESP32 system
  // clock persists across deep sleep via the SoC's internal RTC counter
  // until power loss, which is enough for the reading-stats module to
  // date-bucket sessions. We just skip the DS3231 write path below when
  // `_available` is false.
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      // v18.9.9.292: on X4 (no DS3231) the SNTP sync above has already
      // set the ESP32 system clock, which is what ReadingStats reads.
      // Skip the DS3231 writes -- the module isn't present.
      if (!_available) {
        LOG_INF("CLK", "System clock set to %04u-%02u-%02u %02d:%02d:%02d UTC (no DS3231 write)",
                static_cast<uint16_t>(1900 + timeinfo.tm_year),
                static_cast<uint8_t>(timeinfo.tm_mon + 1),
                static_cast<uint8_t>(timeinfo.tm_mday), timeinfo.tm_hour, timeinfo.tm_min,
                timeinfo.tm_sec);
        return true;
      }

      // v18.9.9.80: also write date fields so downstream stats (streaks,
      // time-of-day charts, per-book start/finish dates) have a valid anchor.
      const bool timeOk = writeTimeToRTC(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      // struct tm: tm_wday 0=Sunday, tm_mon 0=Jan, tm_year offset from 1900.
      const uint8_t dow = static_cast<uint8_t>(timeinfo.tm_wday + 1);
      const uint8_t day = static_cast<uint8_t>(timeinfo.tm_mday);
      const uint8_t month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      const uint16_t year = static_cast<uint16_t>(1900 + timeinfo.tm_year);
      const bool dateOk = writeDateToRTC(dow, day, month, year);
      if (timeOk && dateOk) {
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02d:%02d:%02d UTC", year, month, day,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
