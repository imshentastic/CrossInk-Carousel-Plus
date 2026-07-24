# BT Port Callsite Migration Map — Phase 2

Every `BluetoothHIDManager` API call in the codebase (excluding the manager itself and sim stub), with its Phase 2d/2e target.

Snapshot from Phase 2a enumeration (v18.9.9.72 in progress). Update this file if callsites shift before 2d executes.

## Ownership decisions

- `BluetoothHIDManager` becomes a **thin facade** in Phase 2d — its `enable/disable/isEnabled/isConnected/popKeyEvent` delegate to `bleinput::` / `BleHid`. Public API stays; internals gut.
- `_bleKeyMapResolver`, device profile normalization, debug capture, auto-reconnect/enable-later retry semantics — all move into a new `bleinput::adapter` layer that consumes `BleHid.popKey()` output before the facade returns it.
- Phase 3 deletes the facade entirely and rewrites all callers to `bleinput::` / `BleHid` directly. Doing that in Phase 2 would multiply risk.

## Category A — `isEnabled()` reads (majority of callsites)

**2d target:** rewrite `BluetoothHIDManager::isEnabled()` body to `return BleHid.isRunning();`. All callers unchanged.

Sites:
- `src/activities/Activity.cpp:54` — `auto& bt = ...; ... bt.isEnabled()`-adjacent
- `src/activities/reader/BookSettingsDrawerActivity.cpp:229, 281, 532, 625`
- `src/activities/reader/EpubReaderActivity.cpp:223, 234, 1777, 1868, 1875, 2019, 2051, 2075, 2521, 3068, 3303, 3875, 3888, 4704, 4887, 4978, 5043, 5239, 5342, 5358, 5494, 5593, 5734, 5765`
- `src/activities/reader/EpubReaderMenuActivity.cpp:169, 344`
- `src/activities/settings/FontDownloadActivity.cpp:136`
- `src/activities/network/CrossPointWebServerActivity.cpp:73`
- `src/main.cpp:1633`

## Category B — `isConnected(SETTINGS.bleBondedDeviceAddr)` reads

**2d target:** rewrite `BluetoothHIDManager::isConnected(const char*)` body to `return BleHid.isConnected();` (BleHid tracks one connection at a time; address matching becomes implicit since we only ever ask about the bonded remote).

Sites:
- `EpubReaderActivity.cpp:2053, 2077, 5241, 5595`
- `BookSettingsDrawerActivity.cpp:627`

## Category C — `requestEnableLater()` / `requestDisableLater()`

**2e target:** keep the deferred-request queue in the facade (Phase 4 revisits whether it's still needed once BleHid.end() truly deinits). Facade's `tryEnableIfRequested()` calls `bleinput::ensureStarted()`; the retry give-up window + `_enableGaveUpAlertPending` semantics stay in the facade.

Sites (unchanged in 2d):
- `EpubReaderActivity.cpp:1610, 3138, 3228, 3318, 3329, 3441, 3656, 3906, 4748, 4891, 5342, 5797, 5829`
- `EpubReaderMenuActivity.cpp:407`

## Category D — `enable()` / `disable()` inline

**2d target:** facade delegates to `bleinput::ensureStarted()` / `bleinput::stop()`. Preserve the `HalPowerManager::Lock` scope (bleinput already does this internally).

Sites:
- `main.cpp:1635` (top-level cleanup)
- `BookSettingsDrawerActivity.cpp:639, 1015`
- `EpubReaderActivity.cpp:237, 2507, 3106, 3306, 3891`

## Category E — one-shot alert consume

**2e target:** move `_connectionLostAlertPending`, `_enableGaveUpAlertPending`, `_autoReconnectPending` into the adapter. Reader keeps calling `btMgr.take*()`; facade forwards to adapter.

Sites (unchanged in 2d):
- `EpubReaderActivity.cpp:1750, 2328`

## Category F — `nimbleStateSkippedTeardown()` reads

**Phase 4 obsoletes this.** BleHid.end() truly deinits, so v48's skip-deinit escape hatch goes away. Facade returns `false` unconditionally in 2d; the deletions in Phase 4 clean up the callsites.

Sites:
- `EpubReaderActivity.cpp:2328, 3094`

## Category G — BluetoothSettingsActivity (pairing UI)

**2d target:** biggest rewrite. Currently touches `startScan`, `_discoveredDevices`, `connectToDevice`, `setBondedDevice`. Move to `BleHid.startScan/device(i)/connect(addr)/paired(i)/forget(addr)` directly (bypass facade — this activity fully owns pairing).

Site: `src/activities/settings/BluetoothSettingsActivity.cpp:89` and all methods below it. Estimate 300–500 LOC touched here.

## Category H — explicit connect + input callback wiring

**2d target:** `connectToDevice(SETTINGS.bleBondedDeviceAddr)` → `BleHid.connect(SETTINGS.bleBondedDeviceAddr)`.

Sites:
- `EpubReaderActivity.cpp:2508`

**Input wiring** (setInputCallback, setLearnInputCallback, setButtonInjector, setBleKeyMapResolver, setReaderContextCallback, setButtonActivityNotifier, setDebugCaptureEnabled) — these all remain **on the facade** in 2d. The facade's `processInputEvents()` loop is what drains BleHid's ring queue, applies the device-profile adapter, and dispatches to the input/learn callbacks.

## Adapter layer (new, Phase 2b/2c work)

New file: `src/BleInputAdapter.h/cpp` (or extend `BleInput.cpp`).

Responsibilities:
1. **Poll drain**: read `BleHid.popKey()` events from main loop (or from `btMgr.processInputEvents()`).
2. **Device profile normalization**: apply Free3/GameBrick/Free2 quirks. Currently in `BluetoothHIDManager::parseHIDReport` + `mapKeycodeToButton`. Adapter needs the raw HID report to do profile detection, which BleHid does not expose. **2c blocker**: SDK needs a raw-report observer hook — add `void BleKeyboardHost::setReportObserver(std::function<void(const uint8_t*, size_t)>)` upstream (or patch our submodule).
3. **BleKeyMapResolver**: `bleinput::encodeKey(ev, kind, value)` → `_bleKeyMapResolver(kind, value)` → HalGPIO::BTN_*.
4. **Auto-reconnect + reason-520 alert**: hook BleHid's `onLinkDown` (add observer callback to SDK). If drop within `SETTLE_MS..EARLY_DISCONNECT_MS`, set `_connectionLostAlertPending` + `_autoReconnectPending`.
5. **Enable-later give-up**: adapter tracks `_enableLaterFirstAttemptMs` + burns `kGiveUpAfterMs` budget; sets `_enableGaveUpAlertPending`.

## Phase 4 cleanups after facade is delete

- `nimbleStateSkippedTeardown()` callsites — remove the escape-hatch branch entirely; BleHid.end() truly deinits so no need to silent-restart around stale state.
- v48 skip-deinit code path in the facade — deleted along with the facade.
- v49 silent-restart-when-BT-holds-heap — reader path retunes; the heap floor for BT-enable pre-flight should drop to `bleinput::kStartMinFreeHeap` = 56 KB (from our current 56 KB, coincidentally the same value).
