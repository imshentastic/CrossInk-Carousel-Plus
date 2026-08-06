#include "XteinkDetect.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

#include <string.h>

#include "nvs.h"

// Two independent capabilities live in this file:
//   * FREEINK_XTEINK_C3 — the X3-vs-X4 I2C fingerprint on SDA=20 / SCL=0. Only
//     safe on the Xteink C3 pinout (on an S3 those pins are USB D+ / boot
//     strap), so it compiles only for the C3 profiles.
//   * FREEINK_XTEINK_DISPLAY_PROBE — the board-agnostic UC81xx display-controller
//     fingerprint. It reads whatever pins the ACTIVE profile carries, so it is
//     safe on the S3 X4 Pro too.
#define FREEINK_XTEINK_C3 (FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3)
#define FREEINK_XTEINK_DISPLAY_PROBE (FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4 || FREEINK_DEVICE_X4PRO)

namespace freeink {

// =============================================================================
// Board-agnostic UC81xx display-controller probe (shared bit-bang core).
// =============================================================================
#if FREEINK_XTEINK_DISPLAY_PROBE

namespace {

struct EpdProbePins {
  int8_t sclk;
  int8_t mosi;  // the controller's bidirectional SDA in half-duplex mode
  int8_t cs;
  int8_t dc;
  int8_t rst;
  int8_t busy;
};

// UC81xx read-capable registers (UC8179 / UC8279d datasheets, identical layout).
constexpr uint8_t UC81XX_CMD_VER = 0x70;  // reserved 0x00, CHIP_VER, LUT_VER[23:0]
constexpr uint8_t UC81XX_CMD_FLG = 0x71;  // status; BUSY_N (D0) = 1 when idle

inline void epdClockDelay() { delayMicroseconds(1); }  // ~500 kHz, timing-safe

void epdWriteByte(const EpdProbePins& p, uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(p.mosi, (b & 0x80) ? HIGH : LOW);
    epdClockDelay();
    digitalWrite(p.sclk, HIGH);
    epdClockDelay();
    digitalWrite(p.sclk, LOW);
    b <<= 1;
  }
}

uint8_t epdReadByte(const EpdProbePins& p) {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    // The controller shifts the next bit out on the SCL falling edge; sample
    // while the clock is low, then pulse.
    epdClockDelay();
    b = static_cast<uint8_t>((b << 1) | (digitalRead(p.mosi) == HIGH ? 1 : 0));
    digitalWrite(p.sclk, HIGH);
    epdClockDelay();
    digitalWrite(p.sclk, LOW);
  }
  return b;
}

// One command + N-byte half-duplex read: command with DC low, then SDA (our
// MOSI) released to input with DC high while the controller drives the reads.
void epdCmdRead(const EpdProbePins& p, uint8_t cmd, uint8_t* out, uint8_t len) {
  pinMode(p.mosi, OUTPUT);
  digitalWrite(p.dc, LOW);
  digitalWrite(p.cs, LOW);
  epdClockDelay();
  epdWriteByte(p, cmd);
  digitalWrite(p.dc, HIGH);
  pinMode(p.mosi, INPUT_PULLUP);
  epdClockDelay();
  for (uint8_t i = 0; i < len; i++) out[i] = epdReadByte(p);
  digitalWrite(p.cs, HIGH);
  pinMode(p.mosi, OUTPUT);
}

// The UC81xx VER signature: a leading reserved 0x00, then a CHIP_VER byte
// (datasheet default 0x03, MTP-programmed so not pinned to that exact value)
// that is neither a floating-low nor a floating-high bus. A released SDA reads
// back all-0x00 or all-0xFF through the pull-up, and neither the UC8253 nor the
// SSD1677 answers 0x70 with this shape (the SSD-family has no such read), so a
// additionally report idle (BUSY_N=1) without being a floating pattern.
//
// Match on the FLG status plus a non-uniform VER. A UC81xx drives 0x71 to a real
// status byte (BUSY_N=D0=1 when idle) and returns a structured VER; an SSD-family
// controller doesn't answer 0x70/0x71 at all, so the half-duplex line floats to a
// uniform level (all 0xFF via the pull-up, or all 0x00). We deliberately do NOT
// require any specific CHIP_VER value: a shipping X4 Pro UC8179 was observed
// returning VER=00 00 01 FF FF (CHIP_VER byte = 0x00) with FLG=0x13 — an earlier
// matcher that required ver[1] != 0 wrongly rejected it.
bool verIsFloating(const uint8_t ver[5]) {
  for (int i = 1; i < 5; i++)
    if (ver[i] != ver[0]) return false;  // any variation => a real, driven response
  return true;                           // all five bytes identical => floating bus
}

bool matchUc81xx(const uint8_t ver[5], uint8_t flg) {
  // FLG must be a real, non-floating status with BUSY_N (bit0) asserted (idle).
  if (flg == 0x00 || flg == 0xFF) return false;
  if ((flg & 0x01) != 0x01) return false;
  // VER must be an actually-driven (non-uniform) pattern, not a floating bus.
  return !verIsFloating(ver);
}

bool runDisplayProbePass(const EpdProbePins& p, uint8_t ver[5], uint8_t* flg) {
  pinMode(p.cs, OUTPUT);
  digitalWrite(p.cs, HIGH);
  pinMode(p.sclk, OUTPUT);
  digitalWrite(p.sclk, LOW);
  pinMode(p.dc, OUTPUT);
  digitalWrite(p.dc, LOW);
  pinMode(p.mosi, OUTPUT);
  if (p.busy >= 0) pinMode(p.busy, INPUT);

  // Hardware reset pulse (RST_N min low width 50 us; give it 1 ms) then wait a
  // fixed settle time. We can't trust BUSY polarity here — the controller (and
  // therefore its idle level) is exactly what we're trying to identify — so we
  // don't gate on BUSY; a flat delay covers every UC81xx power-up. The panel
  // driver's own begin() resets again afterwards, so this leaves no state.
  if (p.rst >= 0) {
    pinMode(p.rst, OUTPUT);
    digitalWrite(p.rst, HIGH);
    delay(2);
    digitalWrite(p.rst, LOW);
    delay(1);
    digitalWrite(p.rst, HIGH);
  }
  delay(30);

  uint8_t flgByte = 0;
  epdCmdRead(p, UC81XX_CMD_FLG, &flgByte, 1);
  epdCmdRead(p, UC81XX_CMD_VER, ver, 5);
  if (flg) *flg = flgByte;
  return matchUc81xx(ver, flgByte);
}

void releaseDisplayPins(const EpdProbePins& p) {
  // Leave everything released. RST_N has an internal pull-up, so INPUT keeps the
  // controller out of reset.
  pinMode(p.sclk, INPUT);
  pinMode(p.mosi, INPUT);
  pinMode(p.cs, INPUT_PULLUP);  // don't leave the panel selected
  pinMode(p.dc, INPUT);
  if (p.rst >= 0) pinMode(p.rst, INPUT);
}

// Two-pass probe with agreement, over an arbitrary pinout. Confirmed only when
// both passes match the UC81xx signature AND agree on the VER bytes — a floating
// bus can't produce the same stable non-trivial pattern twice. Disagreement is
// Inconclusive; both-fail is PrimaryAssumed (the profile's default controller).
DisplayControllerVerdict probeDisplayController(const EpdProbePins& p, uint8_t verBytes[5], uint8_t* flg) {
  uint8_t ver1[5] = {0};
  uint8_t ver2[5] = {0};
  uint8_t flg1 = 0;
  const bool pass1 = runDisplayProbePass(p, ver1, &flg1);
  delay(2);
  const bool pass2 = runDisplayProbePass(p, ver2, nullptr);
  releaseDisplayPins(p);
  if (verBytes) memcpy(verBytes, ver1, 5);
  if (flg) *flg = flg1;
  if (pass1 && pass2 && memcmp(ver1, ver2, 5) == 0) return DisplayControllerVerdict::Uc81xxConfirmed;
  if (!pass1 && !pass2) return DisplayControllerVerdict::PrimaryAssumed;
  return DisplayControllerVerdict::Inconclusive;
}

}  // namespace

DisplayControllerVerdict detectXteinkDisplayController(uint8_t verBytes[5], uint8_t* flg) {
  const auto& d = BoardConfig::ACTIVE.display;
  const EpdProbePins p{d.sclk, d.mosi, d.cs, d.dc, d.rst, d.busy};
  return probeDisplayController(p, verBytes, flg);
}

namespace {

// The OEM records the panel controller per unit in NVS namespace `hw_calib`,
// key `screenType` (u8): 1/0x0B = UC8179, 2/0x0C = UC8279, anything else (incl.
// the default 3) = the shipping SSD-family / UC8253 part. We READ it only for
// diagnostics/cross-reference — NOT for the decision. It is unreliable in the
// field: a full-flash from another unit overwrites this namespace, so it can
// describe the wrong panel entirely. The live bus probe is the ground truth.
// Returns false if NVS has no such namespace/key.
bool readOemScreenType(uint8_t* out) {
  nvs_handle_t h;
  if (nvs_open("hw_calib", NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t v = 0;
  const esp_err_t e = nvs_get_u8(h, "screenType", &v);
  nvs_close(h);
  if (e != ESP_OK) return false;
  *out = v;
  return true;
}

bool screenTypeIsUltraChip(uint8_t st) { return st == 1 || st == 2 || st == 0x0B || st == 0x0C; }

// Run the display-bus probe and report the verdict with a diagnostic log line.
bool probeSaysUltraChip() {
  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  const DisplayControllerVerdict v = detectXteinkDisplayController(ver, &flg);
  if (Serial)
    Serial.printf("[%lu] [XTDET] bus probe VER=%02X %02X %02X %02X %02X FLG=%02X -> %s\n", millis(), ver[0], ver[1],
                  ver[2], ver[3], ver[4], flg,
                  v == DisplayControllerVerdict::Uc81xxConfirmed  ? "UltraChip"
                  : v == DisplayControllerVerdict::PrimaryAssumed ? "default controller"
                                                                  : "inconclusive (default)");
  return v == DisplayControllerVerdict::Uc81xxConfirmed;
}

}  // namespace

bool applyXteinkDisplayController() {
  // Decide from the live display-bus probe — the ground truth. The OEM NVS
  // hw_calib/screenType is read only for diagnostics: it's unreliable in the
  // field (a full-flash from another unit overwrites it, so it can name the wrong
  // panel). Log it — and flag when it disagrees with the probe — but never
  // decide on it.
  uint8_t screenType = 0;
  if (readOemScreenType(&screenType)) {
    if (Serial)
      Serial.printf("[%lu] [XTDET] NVS hw_calib/screenType=%u (%s) [info only]\n", millis(), screenType,
                    screenTypeIsUltraChip(screenType) ? "UltraChip" : "default");
  } else if (Serial) {
    Serial.printf("[%lu] [XTDET] NVS hw_calib/screenType: not set [info only]\n", millis());
  }

  const bool ultraChip = probeSaysUltraChip();
  if (!ultraChip) return false;

  // Promote the profile's default controller to its UltraChip sibling. screenType
  // 1/0x0B (UC8179) pairs with the SSD1677 boards, 2/0x0C (UC8279) with UC8253,
  // so promoting by the profile default lands on the right driver either way.
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677:
      BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8179;
      if (Serial) Serial.printf("[%lu] [XTDET] promoted SSD1677 -> UC8179\n", millis());
      return true;
    case BoardConfig::DisplayController::UC8253:
      BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
      if (Serial) Serial.printf("[%lu] [XTDET] promoted UC8253 -> UC8279\n", millis());
      return true;
    default:
      return false;  // already an UltraChip part (or a non-sibling default)
  }
}

#else  // no probe-capable profile in this build

DisplayControllerVerdict detectXteinkDisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return DisplayControllerVerdict::PrimaryAssumed;
}
bool applyXteinkDisplayController() { return false; }

#endif  // FREEINK_XTEINK_DISPLAY_PROBE

// =============================================================================
// X3-vs-X4 I2C fingerprint (C3-only) + X3 display-controller wrapper.
// =============================================================================
#if !FREEINK_XTEINK_C3

// No C3 Xteink profile in this build, so there is nothing to fingerprint over
// the C3-only I2C bus, and no X3 disambiguation to do.
XteinkVerdict detectXteinkVerdict(uint8_t* score1, uint8_t* score2) {
  if (score1) *score1 = 0;
  if (score2) *score2 = 0;
  return XteinkVerdict::Inconclusive;
}
bool detectXteinkIsX3() { return false; }
X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return X3DisplayVerdict::Uc8253Assumed;
}
bool selectXteinkDevice() { return false; }

#else

namespace {

// X3-only peripherals on the secondary I2C bus (SDA=20, SCL=0).
constexpr int X3_I2C_SDA = 20;
constexpr int X3_I2C_SCL = 0;
constexpr uint32_t X3_I2C_FREQ = 400000;

constexpr uint8_t ADDR_BQ27220 = 0x55;  // fuel gauge
constexpr uint8_t ADDR_DS3231 = 0x68;   // RTC
constexpr uint8_t ADDR_QMI8658 = 0x6B;  // IMU
constexpr uint8_t ADDR_QMI8658_ALT = 0x6A;

constexpr uint8_t BQ27220_SOC_REG = 0x2C;
constexpr uint8_t BQ27220_VOLT_REG = 0x08;
constexpr uint8_t DS3231_SEC_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

bool readReg8(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *out = Wire.read();
  return true;
}

bool readReg16LE(uint8_t addr, uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

// Each probe checks not just for an ACK but for a plausible value, so a stray
// pull-up or floating bus can't masquerade as a present chip.
bool probeBq27220() {
  uint16_t soc = 0;
  uint16_t mv = 0;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) return false;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_VOLT_REG, &mv)) return false;
  return mv >= 2500 && mv <= 5000;
}

bool probeDs3231() {
  uint8_t sec = 0;
  if (!readReg8(ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  return tens <= 5 && ones <= 9;  // valid BCD seconds
}

bool probeQmi8658() {
  uint8_t who = 0;
  if (readReg8(ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  if (readReg8(ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  return false;
}

uint8_t runProbePass() {
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
  const uint8_t score =
      static_cast<uint8_t>(probeBq27220()) + static_cast<uint8_t>(probeDs3231()) + static_cast<uint8_t>(probeQmi8658());
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  return score;
}

}  // namespace

XteinkVerdict detectXteinkVerdict(uint8_t* score1, uint8_t* score2) {
  const uint8_t pass1 = runProbePass();
  delay(2);
  const uint8_t pass2 = runProbePass();
  if (score1) *score1 = pass1;
  if (score2) *score2 = pass2;
  // X3 confirmed only when both passes see at least two of the three chips; the
  // X4 sees zero, so a single stray ACK never flips the result. Anything in
  // between is Inconclusive: callers should run as X4 but may re-probe later.
  if (pass1 >= 2 && pass2 >= 2) return XteinkVerdict::X3Confirmed;
  if (pass1 == 0 && pass2 == 0) return XteinkVerdict::X4Confirmed;
  return XteinkVerdict::Inconclusive;
}

bool detectXteinkIsX3() { return detectXteinkVerdict() == XteinkVerdict::X3Confirmed; }

#if FREEINK_DEVICE_X3

X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  // The X3 uses the shared board-agnostic probe over its display pinout. Both
  // X3 profiles carry the same display pins, so ACTIVE (still the boot default
  // here, before selectDevice) has the right map either way.
  const DisplayControllerVerdict v = detectXteinkDisplayController(verBytes, flg);
  switch (v) {
    case DisplayControllerVerdict::Uc81xxConfirmed: return X3DisplayVerdict::Uc8279Confirmed;
    case DisplayControllerVerdict::PrimaryAssumed: return X3DisplayVerdict::Uc8253Assumed;
    default: return X3DisplayVerdict::Inconclusive;
  }
}

#else  // X4-only C3 build: no X3 profile, nothing to disambiguate.

X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return X3DisplayVerdict::Uc8253Assumed;
}

#endif  // FREEINK_DEVICE_X3

bool selectXteinkDevice() {
  const bool isX3 = detectXteinkIsX3();
  if (!isX3) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
    return false;
  }
  // X3 confirmed: fingerprint which panel controller this production run
  // carries and select the matching sibling profile.
  const bool isUc8279 = detectX3DisplayController() == X3DisplayVerdict::Uc8279Confirmed;
  BoardConfig::selectDevice(isUc8279 ? BoardConfig::Board::XteinkX3Uc8279 : BoardConfig::Board::XteinkX3);
  return true;
}

#endif  // FREEINK_XTEINK_C3

}  // namespace freeink
