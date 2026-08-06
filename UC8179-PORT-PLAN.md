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

## Execution record (steps 1-4 done)

### Step 1 — local-patch direction: resolved

The vendored SDK was an unmodified snapshot of upstream **`6ee2d06`** (2026-07-01),
found by matching blob hashes across all 293 upstream commits: 37 of the 42 files
in the six compiled libs matched that tree exactly. The other 5 carried content
never seen in any upstream commit — that is the complete local-patch surface:

| File | Local patch | Direction |
|---|---|---|
| `lut/Uc8253X3Luts.h` | X3 `lut_x3_wb_gc` lead byte `0x54` -> `0x00` (the v321 fix) | **take upstream** — HEAD is byte-identical; only our comment differed |
| `include/FreeInkDisplay.h`, `src/FreeInkDisplay.cpp` | heap framebuffers, `releaseBuffers`/`reallocBuffers`, `syncWriteBufferFromActive`, secondary-buffer release, `cleanupGrayscale*` restore memcpy, `swapBuffers` null guard | **take upstream** — HEAD has all of them, several comment-for-comment identical (they were backports that landed upstream) |
| `driver/Ssd1677Driver.cpp` | initial paint FULL -> HALF | **take upstream** — converged; on X4 (`halfSeqOverride=0xD7`) upstream also resolves to HALF |
| `driver/Ssd1677Driver.h` | `skipInitialResync()` override defusing `_needsInitialFull` | **KEEP — re-applied.** Upstream made `skipInitialResync` a `PanelDriver` virtual and overrides it on X3/UC8179/UC8279 but *not* SSD1677; the base is a no-op, so without this the X4 seamless silent-restart flash returns |

The `Uc8253X3Driver.{h,cpp}` hunks the plan flagged are **entirely upstream
evolution** — the async `displayStart`/`displayFinish` split, the `factoryP1/P2`
reference banks, and a `forcedFullSync` -> `_forceFullSyncNext` fix. Nothing of
ours to lose there.

### Step 2 — strategy A taken

`freeink-sdk/` mirrored to upstream `e6a8048`, excluding `libs/book/` (4 MB
EPUB engine — expat/miniz/libunibreak plus a test TTF — that CrumBLE never
compiles) and the nested VCS/`__pycache__` dirs. Re-applied the one kept hunk.
**Zero firmware call-site changes were needed**; `pio run -e tiny-bitter` built
clean on the first attempt.

Only six SDK libs are actually compiled (`lib_deps`): FreeInkDisplay,
BatteryMonitor, InputManager, SDCardManager, BoardConfig, PowerManager —
FreeInkUI, network and book are vendored but never built, which is why the
"312 diff lines in FreeInkDisplay.h" scoped down to nothing.

### Step 3 — detection: a gap the plan did not anticipate

Copying the drivers is **not** sufficient, and neither is updating the facade.
`selectDriver()` routes on `BoardConfig::ACTIVE.displayController`, which is set
by `XteinkDetect::applyXteinkDisplayController()` — a lib that was **not in our
`lib_deps` and is called from nowhere in our firmware**. Without wiring it, the
UC8179/UC8279 drivers link but can never be selected, so a new-panel unit still
gets a dead display. Added `XteinkDetect` to `lib_deps` and called the probe from
`HalDisplay::begin()`, after the X3/X4 profile is chosen and before
`einkDisplay.begin()`.

Regression safety, verified by reading the probe:
- It promotes **only** on `Uc81xxConfirmed` — two independent passes must both
  match the UC81xx VER/FLG signature *and* agree byte-for-byte. An SSD1677 or
  UC8253 does not answer register `0x70`, so the bus floats and both passes fail
  -> `PrimaryAssumed` -> the profile default stands. Inconclusive also falls back.
- Pins are read from `BoardConfig::ACTIVE` and released to `INPUT` afterwards;
  `EpdBus::begin()` re-runs `SPI.begin()` immediately after.
- SD shares SCLK/MOSI and is already mounted at this point, but
  `HalSpiBus::Lock` is held across all of `HalDisplay::begin()`, so no SD
  transaction can interleave.
- Cost: roughly 70 ms at boot.

### Step 3b — waveform regression audit: clears

The plan's headline worry ("Replace grayscale LUT waveforms with stock 2-frame
set" changing appearance on existing panels) does not apply. That commit
(`0647bfd`), plus `6662faf` "Restore OLD plane baseline after grayscale refresh"
and `7babfab` "Fix e-ink ghosting by tracking previous frame", touch **only
`Uc8179Driver.cpp`** — the new-panel driver.

- **X4 `lut_grayscale`: unchanged.** What actually changed in `Ssd1677Luts.h` is
  that `lut_grayscale_revert` was deleted and its slot repurposed for a Seeed
  Sticky table, plus an additive X4 Pro partial LUT. Neither is our X4.
- **X3 LUT bank: unchanged**, plus additive `factory_p1/p2` reference tables that
  are deliberately not wired up.
- Removing the X4 revert waveform is a no-op in our flows: the reader always
  calls `cleanupGrayscaleWithFrameBuffer()` after `displayGrayBuffer()`, and
  `cleanupGrayscaleBuffers()` (byte-identical old vs new) already cleared
  `_inGrayscaleMode`, so the old `grayscaleRevert()` branch was dead code.
- `POWER_BUTTON_PIN` and the ADC ladder ranges `{3100, 2090, 750}` are unchanged;
  the large InputManager/BoardConfig diffs are clang-format reflow plus X4 Pro
  additions that are not compiled here.

Real behavior deltas that do land on existing panels, and are therefore what
hardware testing is for: the completion wait moved from 1 ms polling to an
ISR edge wait on BUSY (`waitRefreshComplete`) on **both** X4 and X3;
power-up sequencing before grayscale refresh; and the X4 RED-RAM baseline /
boot first-paint fix. The ISR itself checks out against the repo's rules —
`IRAM_ATTR`, `xSemaphoreGiveFromISR` (never `Take`) in the handler, a
polling fallback when the semaphore can't be created, a bounded 20 ms
arm-confirmation, an already-done fast path, and a 30 s timeout.

The async `displayStart`/`displayFinish` split is inert for us: our app never
calls `displayAsync`/`triggerDisplay`, so `display()` runs the blocking path,
which on both drivers is exactly `displayStart()` + `displayFinish()`.

### Step 4 — builds and size

All six envs build clean, no warnings from SDK or probe code.

| env | 4.7.1 | new | delta | vs 6,553,600 SD slot |
|---|---:|---:|---:|---|
| tiny-bitter | 5,352,608 | 5,361,888 | +9,280 | 1,191,712 under |
| tiny | — | 5,361,744 | — | 1,191,856 under |
| tiny-lexend | — | 5,024,864 | — | 1,528,736 under |
| tiny-chareink | — | 5,316,976 | — | 1,236,624 under |
| xlarge | 7,655,840 | 7,665,136 | +9,296 | over — **already over at 4.7.1** |
| no_emoji | 7,235,984 | 7,245,280 | +9,296 | over — **already over at 4.7.1** |

The whole change costs **+9.3 KB**: +7,344 for the SDK sync including both new
drivers, +1,936 for the probe. `xlarge` / `no_emoji` exceed the 6.25 MB SD-flash
slot but are unchanged in that respect — they were 7.66 / 7.24 MB on 4.7.1 and
fit only the 7,864,320-byte app partition (USB flash), which is why the shipping
SD-flashable build is a `tiny-*` variant.

Simulator envs are unaffected: `[env:simulator*]` declares its own `lib_deps`
without the freeink-sdk symlinks and sets `lib_ignore = hal`, so neither the SDK
nor `HalDisplay.cpp` compiles there.

### Step 5 — hardware gate: what actually happened

The plan's premise was wrong in one important way, and hardware is what proved it.

**The newer-X4 problem is not a panel-controller swap.** CrossInk's release notes
split it: 1.4.0.1 (Jul 29) added display-driver detection for newer **X3** units,
while 1.5.0-rc-2 (Aug 1) added "support for latest X4 battery latch on newer
models (without this patch, newer X4's could seem unresponsive unless connected
to USB power)". Upstream's own consumer entry point agrees —
`selectXteinkDevice()` probes the display bus **only on a confirmed X3**; on X4 it
just selects the profile. No shipping firmware probes an X4's panel bus.

Three hardware failures, all caused by this port, all from the same root mistake
(running things at boot that upstream does not run, or runs in a different order):

1. **X4 hang, probe inside `HalDisplay::begin()`** — `applyXteinkDisplayController()`
   was called unconditionally on every device. Verdict was *correct*
   (`VER=FF..FF -> default controller`, SSD1677 kept) but display init never
   completed. Bisected with a `-DCRUMBLE_DISABLE_PANEL_PROBE` build, which booted
   clean and cleared the SDK update + both new drivers.
2. **X4 hang, probe moved before `Storage.begin()`** — same symptom, which
   disproved the "SD was already mounted" theory. The real problem was simply
   that the X4 bus must not be probed at all.
3. **X3 hang in `Storage.begin()`** — `[SD] SD card detected` never printed. Calling
   `selectDevice(XteinkX3)` early switched `sd.powerEnable` from `PIN_UNASSIGNED`
   (X4 default) to GPIO13, so `SDCardManager` began driving the X3 SD rail for the
   first time ever (upstream `2da0700`, unvalidated here). Every prior CrumBLE
   release mounts SD while `ACTIVE` is still `XTEINK_X4`; copying upstream's call
   site into a different init order is not "1:1".

**Shipped state:** no panel-controller probe on either device. X4 gets
`selectDevice(XteinkX4)` for the battery latch only. X3's boot path is byte-for-byte
4.7.1. UC8179/UC8279 drivers compile in but nothing selects them — documented as a
known limitation rather than pretended-to-work.

Verified on hardware: X4 boots and opens books (init waits 2/5/5 ms); X3 boots
past SD mount. Settings log-spam fix verified at `maxAlloc=19444`, below both old
thresholds, with zero spam.

Nothing has been released.

Working tree is uncommitted. The 21 new SDK paths (including
`Uc8179Driver.{h,cpp}` and `Uc8279Driver.{h,cpp}`) are untracked — they compile
because PlatformIO globs the source dir, but they need `git add` before a commit.

## Do not

- Do not ship without the on-device X4 + X3 regression pass. A bad waveform
  change would break every existing user to fix a few new ones.
- Do not fold this into `feat/dict-casper` (dictionary/BT work) or the CJK
  variant branch — it must be releasable on its own.
