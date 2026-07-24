# BT Port Plan — h2zero → freeink SDK BleKeyboardHost

Goal: replace `BluetoothHIDManager` (h2zero/NimBLE-Arduino wrapper, ~1200 LOC) with the freeink-sdk `BleKeyboardHost` singleton (aka `BleHid`), matching crosspoint's architecture.

Anchor: crosspoint uses `src/BleInput.cpp/.h` (121 lines) as a thin `bleinput::` namespace over `BleHid`.
SDK header: `freeink-sdk/libs/network/BleKeyboardHost/include/BleKeyboardHost.h` — already present, no submodule bump needed.

## Phase 1 — Capability enable + BleInput shim (2 h)
- Add `FREEINK_CAP_BLE_HID_HOST=1` to `platformio.ini` build_flags (SDK gates real impl on this).
- Confirm framework NimBLE builds. Our sdkconfig NimBLE flags (`CONFIG_BT_NIMBLE_*`) already reference the framework stack, not h2zero.
- Port `src/BleInput.cpp` + `src/BleInput.h` from `crosspoint/feat-bluetooth`, renamed to match CrumBLE conventions if needed. Keep the `bleinput::` namespace + `kHostName = "CrumBLE"`.
- Wire `bleinput::popNextEvent` into `MappedInputManager` alongside (not replacing) BluetoothHIDManager. Verify BleHid receives events on hardware.

Exit criteria: compile clean, first BT connect from Bluetooth Settings screen goes via `BleHid` and pumps key events. `BluetoothHIDManager` still exists but is bypassed for input.

## Phase 2 — Callsite migration (3 h)
36 files reference BT. Categories:
1. **Input path** (drawer, reader, menus): replace `BluetoothHIDManager::getInstance().popKeyEvent()` → `BleHid.popKey()`.
2. **Lifecycle** (main.cpp, ROA, BluetoothSettingsActivity): replace `enable()/disable()` → `BleHid.begin()/end()`.
3. **Status queries** (`isConnected/isEnabled/pairedName`): 1:1 to BleHid equivalents.
4. **Bond mgmt** (BluetoothSettingsActivity): swap to `BleHid.paired(i)`, `BleHid.forget(addr)`.

Do class-by-class. Test after each activity converts.

## Phase 3 — Remove h2zero dependency (2 h)
- Remove `h2zero/NimBLE-Arduino @ 2.3.6` from `lib_deps`.
- Delete `lib/hal/BluetoothHIDManager.*` (or reduce to a compat header that forwards).
- Verify link: no unresolved NimBLE symbols outside SDK.
- Simulator stub in `src/simulator/sim_stubs/BluetoothHIDManager.h` — replace with a no-op BleHid stub or leave for host builds.

## Phase 4 — Rewire silent-restart logic (2 h)
The BleHid `end()` actually deinits NimBLE (~57 KB reclaimed). This changes:
- **v48 skip-deinit**: obsolete. `BleHid.end()` is safe to call.
- **v49 silent-restart-when-BT-holds-heap**: no longer needed (end() truly frees).
- **v22 pre-flight defrag** (`kStartMinFreeHeap = 56*1024`): match crosspoint's 56 KB reader-context floor, 70 KB explicit-settings floor.
- **Auto-reconnect on reason 520**: BleHid handles this internally via `poll()`.
- **v70 framebuffer lending**: keep — orthogonal to BT.

Delete now-unused silent-restart branches carefully. Preserve chapter-boundary silent-restart-to-defrag (v64) — that's not BT-specific.

## Phase 5 — Testing (2-3 h)
Test matrix:
1. Cold-boot + pair from Bluetooth Settings → connect → read a book with BT page turns.
2. In-reader BT quick-connect from drawer (both PreparedLayout and CustomSettings paths).
3. BT-connected chapter boundary (previously v70's target — must still work).
4. BT disable from drawer → confirm heap returns (~57 KB).
5. Reason-520 early drop → auto-reconnect via BleHid.poll().
6. Sleep/wake with BT paired → auto-reconnect.
7. Multiple bonds → switch between them.

## Phase 6 — Cleanup (1 h)
- Remove `custom_sdkconfig` NimBLE role-disable flags that BleHid may already set differently.
- Update `crumble_version` to 4.5.71 with tag `bt-port-p1` through `bt-port-final`.
- Delete `docs/bt_port_plan.md` when done (or move to `docs/history/`).

## Risk register
- **Framework NimBLE conflicts with our custom_sdkconfig**: current `wpa_supplicant -Werror` issue was resolved for h2zero; may resurface. Mitigation: `pio run -e tiny-bitter -v > log` after Phase 1 build to catch macro redefinitions.
- **Bond migration**: h2zero bonds are stored in NVS under different keys than BleHid. Users lose pairings on upgrade. Mitigation: document as one-time re-pair, or write a migration helper reading old NVS keys and calling BleHid persistBonds.
- **v70 lending compatibility**: `FrameBufferBuildLoan` reads `BluetoothHIDManager::getInstance().isEnabled()`. Update to `BleHid.isRunning()`.
- **Drawer quick-connect UX**: preserves current behavior of showing "Connecting…" popup. Crosspoint's `showConnectingUntilLinked` is the reference — port that too.

## Rollback
Each phase commits separately. If any phase breaks a hardware test, `git revert` and go back to v70 (last known good with h2zero).
