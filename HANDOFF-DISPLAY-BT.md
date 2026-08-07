# Handoff: two 4.7.3 field bugs (display regression + BT debug crash)

_Branch `fix/display-grayscale-baseline`, cut from `myfork/main` (bdd73996).
Worktree: `/Users/michaelshen/Desktop/Coding Exp - CrossInk/CrumBle-panel`._

Both are reproducible on the user's own X4 — do NOT plan around asking the
original reporters to test. The user flashes and reports back.

---

## BUG 1 — covers vanish / black squares persist (Flow + Lyra carousel)

### Symptoms (two independent reports, both on 4.7.3)

- Reporter A: on the carousel, moving focus down to the icon row makes **all
  book covers disappear**; flicking through the covers again restores them.
  Photo shows an empty cover outline with the icon row half-drawn.
- The user (own X4, Lyra carousel): covers did NOT vanish, but **black squares
  appeared over the icon being focused** (inverted highlight) and **the black
  square persisted after moving off that icon**.

Both reporters say it started with 4.7.3. Nothing like it on 4.7.1.

### Root-cause hypothesis (strong, evidence below)

4.7.3 contains `f8d790b5 sdk: resync vendored freeink-sdk to upstream e6a8048`.
That resync **deleted the X4 grayscale revert LUT**. From the new
`freeink-sdk/libs/display/FreeInkDisplay/src/lut/Ssd1677Luts.h`:

> "No revert LUT: the stock X4 firmware has no revert waveform (verified
> against the OEM binary...). Grayscale exits via RED resync or a single-pass
> HALF clean."

The replacement exit path is `Ssd1677Driver::cleanupGrayscaleBuffers()`
(`src/driver/Ssd1677Driver.cpp:539`):

```cpp
void Ssd1677Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) return;                                   // <-- silent bail
  setRamArea(bus, 0, 0, _w, _h);
  writeRam(bus, CMD_WRITE_RAM_RED, bw, _bufferSize); // RED = next diff baseline
  _inGrayscaleMode = false;
}
```

So after any grayscale render the restored BW frame **is** the differential
baseline for the next fast refresh. If that reseed does not happen (or happens
with a stale/null buffer), the next FAST refresh diffs against the wrong
baseline, which produces exactly this symptom pair:

- stale dark pixels are never computed as "needs clearing" -> **black square persists**
- regions believed already-correct are never painted -> **covers vanish**

Under the OLD SDK the revert waveform physically reset the panel, masking any
baseline mistake. Nothing masks it now, which is why this surfaces at 4.7.3.
The carousel is the worst-hit screen: heaviest grayscale (AA cover art) plus an
inverted-highlight icon row.

### Prime suspect to check FIRST

`freeink-sdk/.../driver/Ssd1677Driver.h:94` carries a CrumBLE-local patch that
was deliberately re-applied during the resync:

```cpp
void skipInitialResync() override { _needsInitialFull = false; }
```

It exists to stop a visible flash on seamless silent-restart (4.5.7). Upstream
overrides `skipInitialResync` on X3/UC8179/UC8279 but **not** on SSD1677 — i.e.
upstream expects the X4 to keep its resync. Under the old SDK resync was not
load-bearing for grayscale exit; under the new one it is part of how grayscale
cleans up. Suppressing it on the X4 is the most likely regression trigger.

**Experiment 1 (cheapest, decisive):** remove that override, build, flash the
user's X4, and check whether the persistent black square stops. Watch for the
side effect it was added to prevent: a flash on silent-restart.

**Experiment 2 (if 1 is not enough):** instrument the Home/carousel grayscale
path. `GfxRenderer::cleanupGrayscaleBuffers` is called at
`lib/GfxRenderer/GfxRenderer.cpp:3573` and `:3585` with `frameBuffer`. Log
whether those are reached on a carousel repaint and whether `frameBuffer` is
non-null. Note `storeBwBuffer()` (`GfxRenderer.cpp:3458`) can FAIL under tight
heap and only logs — a failed snapshot means restore/cleanup runs with the
wrong contents, which is the same class of bug.

**Do not** "fix" this by forcing FULL refreshes on Home; that trades the bug for
a slow, flashy carousel. Fix the baseline.

---

## BUG 2 — BT Debug Capture crashes on button press (nimble_host stack overflow)

### Evidence

User pressed buttons on a connected Free3-R with BT Debug Capture on:

```
[411364] [INF] [BTDBG] addr=bc:7c:95:b5:6a:4c len=3 raw=01 00 00
[411364] [INF] [BT] >>> BUTTON PRESSED: keycode=0x01 <<<
[411364] [INF] [BT] Mapped key 0x01 -> Up/PageBack
[412894] [INF] [BTDBG] ... raw=02 00 00
[412894] [INF] [BT] Mapped key 0x02 -> Down/PageForward
***ERROR*** A stack overflow in task nimble_host has been detected.
```

Next boot: `Previous boot's last checkpoint: bt:enable-entry`, reset=PANIC.

### Root cause

The HID notify callback runs **on the nimble_host task**, and we log from
inside it. That task's stack is deliberately trimmed in `platformio.ini:191`:

```
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=2560
```

(reduced from the 5120 default as part of the NimBLE heap trims). printf-style
`LOG_INF` formatting plus the serial write does not fit in 2560 bytes, so the
debug-capture logging blows the stack. Consistent with it only crashing when
Debug Capture is enabled AND keys are pressed.

Also note the log: `Free: 10728 bytes ... MaxAlloc: 9204` — the device is
extremely heap-tight in that state, which is its own concern but not the crash
cause (a stack overflow is not a heap exhaustion).

### Suggested fix

Do not log from the notify callback. Copy the few bytes into a small
ring/latch and let the main loop emit the `BTDBG` line (the codebase already
does this pattern for virtual-button injection). Alternative/cheaper: raise
`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` only when `ENABLE_BT_DEBUG_MONITOR=1`,
so shipping builds keep the trimmed stack and the heap saving.

Verify: BT Debug Capture on, Free3-R connected, mash buttons for 30 s, no panic.

---

## Ground rules

- Build `pio run -e tiny-bitter`; keep under 6,553,600 bytes (SD-flash promise).
- Copy successful builds to `~/Downloads/crumble-<ver>-<tag>.bin`.
- No emojis, no AI attribution in code/commits/PRs.
- Releases go to `myfork` (imshentastic/CrumBLE), never upstream.
- Bug 1 is a regression in a SHIPPED release affecting every 4.7.3 user on
  Flow/carousel — prioritise it, and expect iterative flash-and-look cycles
  with the user.
