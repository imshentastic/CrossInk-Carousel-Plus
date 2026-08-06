#pragma once

// UC8279d panel driver — Xteink X3, newer production run (792x528 B/W).
// UltraChip UC8279d ("d_B" silicon, TFT-module variant) driven in KW mode
// (PSR KW/R=1): 1-bpp, DTM1 = OLD plane, DTM2 = NEW plane, differential
// refresh — the same paradigm as the UC8253 X3 driver, near-identical command
// set. Written from the UC8279d_B 0.1 datasheet (Dec 2025); PENDING HARDWARE
// VALIDATION on a UC8279 X3 unit.
//
// v1 drives the panel with the factory OTP waveforms (PSR REG=0): the 4K MTP
// holds 12 temperature-range LUT sets including per-range frame rate and rail
// voltages, so PWR/PLL/VDCS are left at silicon defaults and every refresh is
// temperature-compensated by the controller itself. That means Full/Half/Fast
// all currently run the same OTP waveform (a full GC-style flash) — correct
// but not fast. Custom register banks (REG=1, commands 0x20-0x24, 7-byte-group
// format ≠ UC8253's 43-byte format) are the follow-up once we can tune LUTs on
// real hardware; inject them via Uc8279Config when that lands.
//
// BUSY_N: low while busy (PON/DRF/POF all flag), same two-phase shape as the
// UC8253 X3 — reuses BusyPolarity::X3TwoPhase and the async start/finish split.

#include "PanelDriver.h"

namespace freeink {

struct Uc8279Config {
  // PSR (0x00) byte 0: RES=00 (TRES overrides), REG=0 (OTP LUTs), KW/R=1 (KW
  // mode), UD=1, SHL=1, SHD_N=1, RST_N=1.
  uint8_t psr0;
  // PSR byte 1: VCMZ=0, TS_AUTO=1 (auto temp sense before booster), TIEG=1,
  // NORG=0, VC_LUTZ=1 (float VCOM after refresh) — datasheet defaults.
  uint8_t psr1;
  // CDI (0x50): VBD[1:0] | DDX[1:0] | CDI[3:0]. 0x97 = border driven white
  // each refresh (KW mode, DDX=01: VBD=10 -> LUTWK 0->1), CDI = 10 hsync.
  // Datasheet default 0xD7 floats the border instead.
  uint8_t cdi;
  // TCON (0x60): S2G/G2S non-overlap, datasheet default 0x22.
  uint8_t tcon;
};

const Uc8279Config& uc8279DefaultConfig();

class Uc8279Driver : public PanelDriver {
 public:
  explicit Uc8279Driver(const Uc8279Config& cfg = uc8279DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  bool supportsAsyncDisplay() const override { return true; }

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

 private:
  void initController(EpdBus& bus);
  void triggerRefresh(EpdBus& bus, bool turnOff);

  const Uc8279Config& _cfg;

  uint16_t _w;
  uint16_t _h;
  uint16_t _wb;
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _oldPlaneSynced = false;
  bool _forceFullSyncNext = false;

  // Async split state (see Uc8253X3Driver for the contract).
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
};

PanelDriver& uc8279Driver();

}  // namespace freeink
