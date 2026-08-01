# UC8179 / new-panel support — plan

_Branch `fix/uc8179-panel`, based on main (7812054b = crumble-v4.7.1).
Worktree: `/Users/michaelshen/Desktop/Coding Exp - CrossInk/CrumBle-panel`._

## Why this is urgent

Xteink has switched newer X3/X4 units to a different display controller.
CrossInk shipped v1.4.0.1 (2026-07-29) purely to handle it — their words:
"newer panels do NOT work with Crossink prior to this release... If you flash an
older version, your display might freeze up or otherwise be unusable."

Our vendored `freeink-sdk` supports SSD1677, UC8253-X3, ED2208-M5,
UC8253-Murphy, IT8951E. Upstream `Free-Ink/freeink-sdk` HEAD (e6a8048) adds
**Uc8179Driver** and **Uc8279Driver**, plus probe-based controller detection.
We have neither driver, so a new-panel user flashing CrumBLE gets a dead display.

Severity: **soft-brick, recoverable** — the ESP32-C3 boots fine and USB download
mode needs no display, so a USB reflash recovers it. Worse for USB-locked units
(README already warns those users; the unlocker officially supports only
CrossPoint/CrossInk).

## Key technical finding (do not skip)

`Uc8179Driver` overrides the **new async virtuals** that our vendored facade
does not yet call:

- `supportsAsyncDisplay() -> true`, `displayStart()`, `displayFinish()`
  (added to upstream `PanelDriver.h`; ours has `requestResync`/`skipInitialResync`
  at lines 90-91 but NOT the async split — the upstream header adds ~44 lines of
  additive virtuals with defaults).

So **copying the two driver files alone is not enough**: our older
`FreeInkDisplay.cpp` never calls `displayStart/displayFinish`, so the driver
would silently run through the blocking `display()` fallback. It might work,
but it is not the configuration upstream tests.

## Divergence measured (ours vs upstream HEAD)

| Area | Status |
|---|---|
| `driver/Uc8179Driver.{h,cpp}` | missing (117 + 351 lines upstream) |
| `driver/Uc8279Driver.{h,cpp}` | missing |
| `include/FreeInkDisplay.h` | 312 diff lines |
| `src/FreeInkDisplay.cpp`, `bus/EpdBus.{h,cpp}`, `driver/PanelDriver.h` | differ |
| `driver/Uc8253X3Driver.cpp` / `lut/Uc8253X3Luts.h` | 58 / 64 diff lines — **check direction before overwriting** (may contain our v321 X3 wb_gc fix) |
| `driver/Ssd1677Driver.*`, `lut/Ssd1677Luts.h` | differ |
| hardware libs (BatteryMonitor, BoardConfig, FrontlightManager, Imu, InputManager) | differ |
| new upstream libs (`book`, `ImageToneMapper`, `MemoryManager`, lucide icons) | absent here |

Our SDK-touching commits look like upstream pins (PR numbers #2588/#2563/#2475),
i.e. the vendored tree is close to an upstream snapshot — the local-patch
surface may be small. **Verify before assuming.**

## Two strategies

**A. Wholesale update the vendored SDK to upstream HEAD, re-apply local patches.**
Pros: this is the exact configuration CrossInk 1.4.0.1/1.5.0-rc ship and field-test
on BOTH panel types; UC8179 runs in its intended async mode; picks up X3 SD-card
power control + IMU fallback for free. Cons: facade/EpdBus API changed
(`FreeInkDisplay.h` 312 diff lines) so our firmware call sites may need fixes;
must not lose the X3 wb_gc fix.
*Recommended — try this first.*

**B. Surgical: copy Uc8179Driver + upstream `PanelDriver.h` + minimal detection.**
Pros: least disturbance to working panels. Cons: driver runs degraded (no async
split) unless the facade is also updated, which is most of strategy A anyway.
*Fallback if A's API break is unmanageable.*

## Steps

1. Determine local-patch direction: for `Uc8253X3Driver.cpp` + `Uc8253X3Luts.h`,
   diff ours vs upstream and decide per hunk whether it is our fix (keep) or
   upstream evolution (take). v321 "X3 wb_gc byte fix" is the known one.
   `git log --all --oneline -S lut_x3_wb_gc` in the parent repo may find it.
2. Attempt strategy A: replace `freeink-sdk/` with upstream HEAD, re-apply the
   kept hunks, then build `pio run -e tiny-bitter` and fix call sites.
3. Confirm detection: understand how the facade probes the bus and picks the
   driver (upstream commit "Fix display controller detection to rely on bus
   probe"). Make sure an EXISTING X4 (SSD1677) and X3 (UC8253) still resolve to
   their current drivers — this is the regression risk.
4. Build all shipping envs (`tiny-bitter` primary) and confirm size still under
   6,553,600 bytes for the SD-flash promise.
5. **Hardware gate (user):** flash the user's X4 and X3. Verify boot logo, page
   turns, FAST/HALF/FULL refresh, grayscale/AA images, sleep screen. Any
   ghosting/inversion change vs 4.7.1 is a regression — the new LUT/waveform
   commits upstream ("Replace grayscale LUT waveforms with stock 2-frame set")
   could alter appearance on existing panels.
6. Ship as a patch release with a CrossInk-style warning banner. Consider naming
   it 4.7.2 (or 4.7.1.1). We cannot detect who owns which panel, so the notes
   must tell new-device owners to use this build.
7. Follow-up (separate): stop the vendored SDK silently freezing — pin it as a
   real dependency or add "check upstream freeink-sdk for new commits" to the
   release checklist. Third frozen-dependency incident this cycle (prebake wasm,
   sim shim, SDK) and the only one with hardware consequences.

## Do not

- Do not ship without the on-device X4 + X3 regression pass. A bad waveform
  change would break every existing user to fix a few new ones.
- Do not fold this into `feat/dict-casper` (dictionary/BT work) or the CJK
  variant branch — it must be releasable on its own.
