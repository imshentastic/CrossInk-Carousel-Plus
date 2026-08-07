# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## Simulator

- Simulator patches belong in the adjacent `crosspoint-simulator` repo.
- The valid local simulator env in this repo is `simulator`. On `feat/cjk-flash-variant` (worktree CrumBle-cjk) it FAILS pre-existing: the GitHub-fetched simulator package lacks APIs this branch's firmware uses (`HalStorage::readFileWithFallback`, `HalDisplay::setSimulatorOrientation`, `String::reserve`, `freertos/portmacro.h` via RenderLock). Fix belongs in crosspoint-simulator, not here.
- The simulator `PNGdec` stub in `crosspoint-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## Build System

- Editing ANY line inside `custom_sdkconfig` in `platformio.ini` — including comments — changes its MD5, which no longer matches the `# TASMOTA__<hash>` stamp on line 1 of `sdkconfig.defaults`. pioarduino then discards `~/.platformio/packages/framework-arduinoespressif32-libs` and demands a full ESP-IDF rebuild.
- That rebuild CANNOT run from this checkout: `builder/frameworks/espidf.py` aborts with "Detected a whitespace character in project paths", and the path contains spaces (`Coding Exp - CrossInk`). Redirecting `PLATFORMIO_BUILD_DIR` gets past the guard but the compile then fails on the space in the source paths.
- **Treat `custom_sdkconfig` as frozen.** Changing a value also makes pioarduino REGENERATE `sdkconfig.defaults`, and the regenerated config does not build: it pulls in Arduino's RainMaker library, which fails with `esp_rmaker_core.h: No such file or directory`. The committed `sdkconfig.defaults` / `sdkconfig.tiny-bitter` are hand-curated and the regeneration cannot reproduce them. Tune NimBLE/IDF behaviour in code instead.
- Recovery after a failed attempt (it leaves the framework libs replaced with STOCK ones — 5120 stack, `BLE_MAX_ACT=6` — silently undoing every NimBLE heap trim): `git checkout -- platformio.ini sdkconfig.tiny-bitter`, copy the project to a whitespace-free path (`rsync -a --exclude .pio --exclude .git`), run `pio run -e tiny-bitter` there once, then build normally again. The libs live under `~/.platformio/packages`, so the original path works once they carry the right config.
- Always verify after any of this: `grep -E "HOST_TASK_STACK_SIZE|BLE_MAX_ACT=" ~/.platformio/packages/framework-arduinoespressif32-libs/esp32c3/sdkconfig` must read 2560 and 1, not 5120 and 6.

## Misc Repo Gotchas

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
