#include "Uc8279Driver.h"

#include <BoardConfig.h>

namespace freeink {
namespace {
// UC8279d command set (UC8279d_B 0.1 datasheet, command table pp. 8-11).
constexpr uint8_t CMD_PANEL_SETTING = 0x00;      // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;          // POF
constexpr uint8_t CMD_POWER_ON = 0x04;           // PON
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;         // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;               // OLD plane in KW mode
constexpr uint8_t CMD_DATA_STOP = 0x11;          // DSP
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;    // DRF
constexpr uint8_t CMD_DTM2 = 0x13;               // NEW plane in KW mode
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50; // CDI
constexpr uint8_t CMD_TCON = 0x60;               // TCON
constexpr uint8_t CMD_RESOLUTION = 0x61;         // TRES
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;  // GSST
}  // namespace

const Uc8279Config& uc8279DefaultConfig() {
  static const Uc8279Config cfg = {
      0x1F,  // PSR0: RES=00, REG=0 (OTP LUTs), KW/R=1, UD=1, SHL=1, SHD_N=1, RST_N=1
      0x0D,  // PSR1: TS_AUTO=1, TIEG=1, VC_LUTZ=1 (datasheet default)
      0x97,  // CDI: border white (VBD=10), DDX=01, interval 10 hsync
      0x22,  // TCON: datasheet default
  };
  return cfg;
}

// Resolution comes from the active BoardProfile (XTEINK_X3_UC8279), selected
// by the boot-time controller probe before begin() constructs this singleton.
Uc8279Driver::Uc8279Driver(const Uc8279Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8279Driver::spiHz() const {
  // UC8279 serial write timing is rated to 20 MHz, same as UC8253.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8279Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Uc8279Driver::triggerRefresh(EpdBus& bus, bool turnOff) {
  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279_DRF");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279_POF");
    _isScreenOn = false;
  }
}

void Uc8279Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);
  // TRES 792x528. Byte layout per datasheet: HRES[9:8], HRES[7:3] (low 3 bits
  // zero — horizontal resolution is 8-pixel granular), VRES[9:8], VRES[7:0].
  // NOTE (hardware validation): the UC8253 X3 init programs VRES=600 (the OEM
  // scans the full gate count with 528 rows bonded); if the UC8279 panel shows
  // a vertical offset or compressed image, try 0x02/0x58 here instead.
  bus.cmd(CMD_RESOLUTION);
  bus.data(0x03);
  bus.data(0x18);
  bus.data(0x02);
  bus.data(0x10);
  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdi);
  bus.cmd(CMD_TCON);
  bus.data(_cfg.tcon);
  // PWR/PLL/VDCS deliberately untouched: with REG=0 the MTP temperature-range
  // tables carry per-range frame rate and VGHL/VSH/VSL/VCOM_DC settings, and
  // TS_AUTO senses temperature before every booster enable.

  // Fill both planes white so the first differential diffs against white, not
  // stale SRAM (same rationale as the UC8253 X3 driver).
  bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.fillPlane(CMD_DTM2, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);

  _isScreenOn = false;
}

void Uc8279Driver::begin(EpdBus& bus) {
  bus.reset(50);
  _oldPlaneSynced = false;
  _forceFullSyncNext = false;
  initController(bus);
}

void Uc8279Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

bool Uc8279Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  const bool doFullSync = (mode == RefreshMode::Full) || !_oldPlaneSynced || _forceFullSyncNext;

  if (doFullSync) {
    // Absolute write from a white OLD baseline; the OTP waveform drives every
    // pixel to target regardless of history.
    bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }
  // Differential path: DTM1 already holds the displayed frame from the last
  // displayFinish() sync, so only writing NEW is needed either way.
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);

  if (!_isScreenOn || doFullSync) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8279_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY_N dropped LOW) before returning, so
  // displayFinish() only rides out the completion edge.
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8279Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8279_DRF");
  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279_POF");
    _isScreenOn = false;
  }

  // Sync the OLD plane with the just-displayed frame for the next differential.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  _oldPlaneSynced = true;
  _forceFullSyncNext = false;
}

void Uc8279Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;  // no conditioning passes on the OTP waveform path
  _forceFullSyncNext = true;
}

void Uc8279Driver::skipInitialResync() { _oldPlaneSynced = true; }

void Uc8279Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// Per-board config injection, same idiom as the other drivers: define
// `const Uc8279Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_UC8279_CONFIG=yourConfig.
#ifdef FREEINK_UC8279_CONFIG
const Uc8279Config& FREEINK_UC8279_CONFIG();
static const Uc8279Config& uc8279ActiveConfig() { return FREEINK_UC8279_CONFIG(); }
#else
static const Uc8279Config& uc8279ActiveConfig() { return uc8279DefaultConfig(); }
#endif

PanelDriver& uc8279Driver() {
  static Uc8279Driver instance(uc8279ActiveConfig());
  return instance;
}

}  // namespace freeink
