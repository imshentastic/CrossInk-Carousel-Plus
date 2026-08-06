# Changelog

## [Unreleased]

## [crumble-v4.7.3] - 2026-08-06

Single firmware: `crumble-v4.7.3-tiny-bitter.bin` (Bitter built-in, fits the stock 6.25 MB slot for SD-card flashing).

### Added
- **Newer X3 units are now detected.** Xteink changed the X3's display controller (UC8253 to UC8279) in units shipped from around late July 2026, and firmware built for the old one leaves those panels unusable. CrumBLE now works out which controller a unit carries and loads the matching driver. The check runs once, on the first boot after installing this version — that boot restarts itself part-way through, which is normal and happens only that once; the answer is remembered from then on. Existing X3s keep the driver they run today.

### Fixed
- **Holding the power button switches the device off instead of restarting it.** On a device that had been running a while, powering off could save your settings, run out of memory mid-write, and reboot to the home screen — so it took a second press to actually turn off. The save now steps aside when memory is too tight for it to complete. In that rare case a settings change made just before powering off may not be kept, which is better than the restart.

## [crumble-v4.7.2] - 2026-08-05

Single firmware: `crumble-v4.7.2-tiny-bitter.bin` (Bitter built-in, fits the stock 6.25 MB slot for SD-card flashing).

### Added
- **Newer X4 units now work on battery.** Some newer X4s seemed dead unless plugged into USB, because that hardware revision does not self-latch its battery MOSFET. CrumBLE now asserts the latch (GPIO13) at boot, matching CrossInk 1.5.0-rc-2. Older X4s self-latch through a pull, so this is a no-op for them.

### Changed
- **Vendored `freeink-sdk` updated to upstream `e6a8048`** (154 commits, from the 2026-07-01 snapshot shipped since 4.6.0). For existing panels this brings: the refresh-completion wait now sleeps on the BUSY interrupt edge instead of polling on a 1 ms tick (X4 joins X3, which already did this), power-up sequencing before a grayscale refresh, and an X4 RED-plane baseline fix. The X4 grayscale waveform table and the X3 LUT bank are byte-for-byte unchanged. The X3 grayscale `wb_gc` fix CrumBLE has carried since v4.6.0 is now upstream's own value, so it survives the update unmodified. The update also compiles in upstream's UC8179 and UC8279 drivers for the newer UltraChip panel controllers — see the known limitation below.

### Fixed
- **Opening a book no longer floods the log with deferred-save errors.** Two heap thresholds disagreed: the settings-save retry admitted a write whenever `maxAlloc` cleared 20 KB, but the re-check immediately before serializing rejected anything under 28 KB. In that 20–28 KB band — easily reached while a book is open — every main-loop tick ran the directory/filesystem work, failed the floor, logged an error, and tried again, hundreds of times a second. Both now use the same floor, so the save defers quietly and retries once there is genuinely room. The save behaviour itself is unchanged; only the futile retry loop is gone.

### Known limitations
- **Newer X3 units are not supported.** Xteink changed the X3's display controller (UC8253 to UC8279) in units shipped from roughly late July 2026, and CrumBLE cannot yet tell the two apart. CrossInk 1.4.0.1+ and CrossPoint 1.5.0-rc-4+ both support these panels — use one of those if your X3 display does not come up. The UC8279 driver ships in this build, but nothing selects it: the controller probe upstream provides was tried in four boot positions across both an X4 and an X3, and every one of them left the panel or the SD card wedged so the device never finished booting. Shipping detection that bricks the boot on an older X3 would be worse than not shipping it. This does not affect the X4 — the newer-X4 problem was the battery latch above, not the display controller.

### Fixed
- **UI font fallback no longer taxes the reader, and reader UI keeps CJK coverage for free when the primary is CJK-capable.** All three reader activities (EPUB, TXT, XTC) release the standalone UI glyph fallback family on entry and re-arm it on exit, matching the pattern that File Transfer, OPDS, and KOReader auth already use. The fallback exists to fill glyph misses in carousel/shelf/settings labels; book text renders through the primary SD font, which already carries whatever glyphs the book itself needs. Previously the fallback stayed resident through the reading session (~10 KB scattered across the heap) and starved contiguous allocations the reader depends on (page DOM 25-40 KB, CSS rule-table grow, BT font subset). Users can now leave the "UI Font Fallback" setting on and still have full contiguous heap while reading. On top of the release-and-suppress path, if the reader's primary SD font is loaded on entry (the normal case when the user has picked an SD font family like Bitter-LXGWWenKai), the reader points the UI fallback at the primary's already-loaded `EpdFontFamily` -- zero extra heap, and CJK glyph misses in reader UI (title bar, chapter label, progress bar, drawer, in-reader menu) render through the primary. Rendered at the primary's point size, so CJK reads visibly larger than surrounding 10-12pt UI text, but far better than tofu. The alias survives font-change / reindex within the reader (ensureLoaded breaks the fallback pointer before unloading the primary and re-establishes it after the new primary loads).
- **Progressive JPEG EPUB covers now render smoothly in generated BMP cover assets.** The cover/thumbnail BMP path already detected progressive JPEGs and forced the required 1/8 JPEGDEC decode, but it still upscaled that reduced grid with blocky sampling. `JpegToBmpConverter` now uses a progressive-only bilinear smoothing pass before dithering when those covers are enlarged for home thumbnails and sleep covers, matching the higher-quality behavior already used in the framebuffer renderer while keeping memory bounded to a small line buffer.

## [crumble-v4.2.1] - 2026-06-06

Single firmware: `crumble-firmware-4.2.1.bin` (Bitter built-in, slim, fits the 6.25 MB OTA partition). The Lexend Deca and CharEink families remain installable as SD-card `.cpfont` bundles; the Bluetooth + SD-card font architectural fix (per-section glyph subset baking) is on the v4.3 roadmap.

### Fixed
- **WASM serving no longer mismatches MIME on every book upload.** `CrossPointWebServer::sendBufferGzip` was substituting an empty `text/html` body under low-heap conditions for *all* MIME types, which broke `WebAssembly.compile`'s strict MIME check. The substitution now fires only for `text/html` content; non-HTML responses (including `application/wasm`) return 503 if free heap is below 6 KB or stream from PROGMEM normally otherwise. Symptom before fix: "Incorrect response MIME type" on every book upload + optimize action.
- **Bluetooth + SD-card font (.cpfont) book combo.** Connecting a Bluetooth remote on a book using an SD-card custom font crashed inside `TextBlock::deserialize` (`vector<string>::resize` → bad_alloc → terminate, because the firmware is `-fno-exceptions`). Multiple defenses landed:
  - Activity-level Page DOM cache in `EpubReaderActivity` — steady-state re-renders (status bar refresh, drawer toggle, BT enable popup, focus changes) now reuse the previously-deserialized page DOM instead of allocating a fresh 25-40 KB each frame.
  - 25 KB pre-flight in `Section::loadPageFromSectionFile` — refuses with a logged error and lets the activity retry/recover instead of bad_alloc-terminating when contiguous heap is below the deserialize threshold.
  - Option 2 lazy non-REGULAR SD-font prewarm: when a Bluetooth controller is bonded, `FontCacheManager::prewarmCache` only eagerly loads REGULAR-style glyphs (~14 KB); BOLD / ITALIC / BOLDITALIC styles load on-demand through the overflow ring buffer. Trades ~100-200 ms per fresh page for ~30-45 KB of heap headroom for NimBLE.
  - BT-connect block releases page DOM cache + reader-settings cache before NimBLE init, and explicitly warms the page DOM cache during the post-enable / pre-subscribe window so post-connect re-renders are cache hits.
  - The three-variant build above is the real fix for the common case; these defenses keep the SD-font path graceful for users on the small cohort using a font outside the three built-ins.
- **File Transfer auto-recovers from low heap** instead of dead-ending at a "Reboot the device" alert. When `CrossPointWebServerActivity::onEnter` pre-flight fails because BT/SD-font residue left maxAlloc below threshold, it now silent-restarts directly into FT (~3-5 s loading popup, lands in fresh FT). A loop guard (sentinel mode hint = 99) prevents infinite restart if the pre-flight is still failing after the recovery boot (falls back to the original alert in that genuine-failure case).
- **`bookmarks.bin` v5 — bookmark preview expanded from 160 → 1024 bytes.** Multi-page quote viewer needs the full quote context; the old 160-byte cap truncated meaningful highlights mid-sentence. v4 files load forward-compatibly (read with the legacy 160-byte ceiling, transparently re-saved as v5 on next mutation).
- **Bookmark list scroll lag fixed.** Collapsed-row preview rendering ran `wrappedText` over the full 1024-char preview to find the first ~3 lines; now caps the scan at 200 chars (3-4 lines worth) so scrolling stays interactive on long preview lists.
- **"Sort by" → Author no longer crashes the device.** The per-book `Epub::load` loop in `CollectionsStore::applySort`'s AuthorAlpha branch ran without yielding to FreeRTOS and without per-book heap pre-flight, so on larger collections it either tripped the IDLE-task watchdog (5 s default → panic + reboot) or OOM-terminated when a heavy book hit a tight-heap state. The loop now yields every 8 books, BREAKS (rather than skips) when maxAllocHeap drops below a 30 KB threshold (remaining books sort to the end like no-author books), and double-yields after completion to give the heap consolidator time to coalesce. A parallel pre-flight in `resolveShelfEntries` skips the series-collapse path when post-sort heap is below 25 KB (books render un-grouped on that frame; the grouped view returns on the next render). Other sort modes were already heap-light and unchanged.
- **Bookmark viewing no longer crashes** on heap-tight devices. A v4.2.1 regression: the V5 1024-byte preview field made each in-memory `Bookmark` struct ~1092 bytes, so opening a bookmark list on a book with 50+ highlights triggered a 55 KB contiguous allocation when the activity copied the bookmarks vector by value. After a BT/SD-font session (heap fragmented to ~12 KB max contiguous), this OOM-terminated inside `std::make_unique`. The in-memory representation is now `std::string preview`, sizing to actual content (24 bytes base + the actual preview length). On-disk V5 format unchanged; the 1024-byte field is just the write cap.
- **AuthorAlpha sort now uses a persistent per-book author key cache** instead of loading each EPUB transiently. The v4.2.1 first-attempt's break-on-low-heap + yield pattern wasn't enough on field hardware — per-book loads still fragmented maxAlloc to ~12 KB which crashed `resolveShelfEntries` post-sort. Library index format bumped v1 → v2 with a new `authorKey` field per entry. Cache is populated lazily by `ensureWalked` (heap-aware, BREAKs cleanly under pressure) and proactively by `EpubReaderActivity::onEnter`. AuthorAlpha sort is now O(N) cache lookups with zero Epub loads. First boot after upgrade does a one-time population pass; subsequent boots load the v2 index with all authors pre-cached. v1 indices on disk load cleanly (empty authors, upgraded on next walk); v2 indices on v4.2.0 firmware force a rescan (same downgrade policy as section file v38 → v39).
- **Quote Viewer button hints trimmed** from `< Prev page` / `Next page >` to `Prev page` / `Next page`. Angle-bracket decorations overflowed the hint container at typical sidebar widths; the dpad position next to each label already implies direction.
- **Shelf no longer renders empty until the next click after sorting** on a heap-fragmented device. The v4.2.1 `resolveShelfEntries` heap pre-flight (added so the shelf-build can't bad_alloc under tight heap) was returning an empty vector that `HomeActivity::cachedShelfEntries` then committed as a "valid" cache entry — so subsequent renders hit the cache and returned empty, until something else invalidated the cache (an arbitrary click). `CollectionsStore` now exposes a `lastResolveHitHeapPressure()` signal that the caller uses to skip the cache commit and schedule one immediate retry via `requestUpdate()`. By the next frame, transient allocations from the previous render have freed and the rebuild succeeds. Retry counter capped at 5 to avoid infinite render loops on a permanently-stuck heap.

### Added
- **Bookmark Quote Viewer.** Pressing Confirm on a bookmark in the bookmark list now opens a dedicated viewer activity. Up/Down walks between bookmarks (wraps around at top/bottom), Left/Right pages through long quotes, Confirm jumps to that location in the book, Back returns to the bookmark list with cursor preserved. Uses the built-in UI_12 font for the viewer chrome so the activity itself doesn't depend on SD-font availability.
- **Multi-page quote capture** for highlights that span 100+ words across page breaks. The FINISH_HIGHLIGHT path now walks intervening pages within the same chapter under RenderLock to capture text across the page boundary, instead of capping at "12 words before … and 3 words after" from the start/end anchors alone.

### Changed
- **BT Quick Connect ordering swapped.** In the in-reader settings drawer (BookSettingsDrawerActivity), regular `BT Quick Connect` now sits *above* `BT No Images Quick Connect`. The image-suppressed variant is a fallback for image-heavy books that starve renders under NimBLE pressure, so it makes more sense as the second option.
- **`OMIT_BITTER_FONT` build flag added**, mirroring the existing `OMIT_LEXENDDECA_FONT` and `OMIT_CHAREINK_FONT`. Tied to a new `CrossPointSettings::BUILTIN_DEFAULT_FONT_FAMILY` compile-time constant (resolution order: Bitter > Lexend > CharEink) that every fallback path now chains through, so the tiny-lexend / tiny-chareink variants gracefully clamp stale Bitter preferences to whichever font ships in the binary.
- **`platformio.ini` env restructure.** Old `[env:tiny]` kept as a backward-compat alias for `pio run -e tiny` workflows. Three new canonical envs: `[env:tiny-bitter]`, `[env:tiny-lexend]`, `[env:tiny-chareink]`. Each variant's `CROSSPOINT_FIRMWARE_VARIANT` string carries the font name so the device's About screen and File Transfer banner show which font this binary ships with.

### Known limitations
- **Bluetooth + SD-card font (.cpfont) books may still fail render after NimBLE connect on this hardware.** NimBLE consumes ~50-68 KB during init and scatter-allocates across the heap, dropping contiguous heap to ~13 KB — well below the 25 KB Page DOM allocation threshold. The v4.2.1 defenses above make this graceful (page cache held when possible, page-load pre-flight catches the failure cleanly), but the SD-font path cannot reliably support an active NimBLE host on the ESP32-C3 heap budget. **Workaround:** flash a `tiny-bitter` / `tiny-lexend` / `tiny-chareink` variant matching your preferred font and use the built-in font instead of the SD .cpfont. Per-section glyph subset baking — which embeds each section's glyph data into its `sections-prebake/*.bin` so the SD-font runtime overhead drops out — is on the v4.3 roadmap as the architectural fix.

## [crumble-v4.2.0] - 2026-06-05

### Fixed
- **SD-card font prebake produces device-identical layouts.** The WASM-based `crumble-prebake` ran without an SD-card font measurement path -- ParsedText checked `renderer.isSdCardFont(fontId)` and always got false because the host_shim GfxRenderer hardcoded the answer, so `ensureSdCardFontReady` never fired and `getTextAdvanceX` fell through to `EpdFontFamily::findGlyph()` (which always returns nullptr on an SD font; intervalCount is 0 by design). Every word measured at width 0 → jumbled layout, every section's fingerprint mismatched even though the .cpfont parsed cleanly. The host_shim now wires the full SD-font lifecycle (`sdCardFonts_` map, `registerSdCardFont`, `ensureSdCardFontReady` → `buildAdvanceTable`, advance-table fast-path in `getTextWidth` / `getTextAdvanceX` / `getSpaceWidth`) -- byte-matching the device's runtime measurement path.
- **"Use prepared layout?" actually restores SD font settings.** Both the prebake-mismatch prompt and the BT-connect prompt restored SETTINGS from the manifest but never reloaded the SD-font manager, so `getReaderFontId()` kept returning the stale boot-time fontId and every section fingerprint failed → "use prepared layout, but indexing every chapter" symptom. Both handlers now call `sdFontSystem.ensureLoaded(renderer)` after the SETTINGS restore. The BT path additionally restores font fields (`fontFamily`/`fontSize`/`sdFontSizeRange`/`sdFontFamilyName`) when the manifest carries them (gated on `mm.fontId != 0` so old manifests don't clobber the user's current font with zeroed values).
- **Optimized-reading prebake no longer corrupts on-page text layout on the slim OTA binary.** 4.0.0/4.1.0/4.1.1 slim builds (the only variant that fits the 6.25 MB legacy OTA partition) shipped with `OMIT_EMOJI_FONTS`, which silently swaps the built-in font binaries for `noemoji` variants. The browser-side prebake optimizer (`crumble-prebake.wasm`) bakes glyph positions using full-fat font metrics; the device then renders with noemoji metrics, and the mismatch produced "text stuck at the top of the page" symptoms and (with prior SD-card cache fragmentation in play) intermittent crashes on chapter load. 4.2 keeps the emoji/symbol/CJK fallback glyphs and pays the resulting ~470 KB by dropping the Lexend Deca + CharEink built-in families instead. Slim binary now 5.97 MB, well within the 6.25 MB OTA budget.
- **`optimizeChapterIndexing` re-enabled by default for everyone.** 4.1.0/4.1.1 shipped default OFF as a crisis workaround for the slim-binary crash. With that crash now fixed, the setting defaults to ON for fresh installs, and existing users (whose NVS still carries the 4.1.x crisis value) are auto-migrated to ON via a one-shot marker file (`/.crosspoint/migrated_42_prebake`). Anyone who actively prefers live-parse can still toggle it off from Settings → Library → Optimize Chapter Indexing.
- **`/api/reader-render-info` reports the correct device.** The endpoint was hardcoded to `device: "X4"` which broke prebake on X3 — the .pxc manifest viewport math used the wrong screen geometry (X4's 800×480 instead of X3's 792×528) and per-section fingerprint checks failed on otherwise-identical settings. Now reports based on `gpio.deviceIsX3()`.
- **CSS rule-grow safety margin** loosened slightly to stop X3 OOM-crashes on margins-toggled-tight EPUBs (~10 KB less contiguous heap than X4 at the same boot point).
- **File Transfer heap gate** is now device-aware: 45 KB on X4, 32 KB on X3. The X3's larger frame buffer + e-ink controller buffers left fresh-boot devices stuck above the 45 KB threshold; secondary per-serve guards already catch the failure mode the higher threshold guarded against.

### Added
- **Read-only "Optimized" inspector in the long-press menu.** Long-press a book that's been pre-baked → the menu header shows the title, author, and an `Optimized ⚡` badge right-justified on the second line. Navigate UP from the first menu item to focus the badge; Confirm opens a read-only viewer showing the 15 reader settings baked into `prebake-manifest.json` (Font + step/range, Orientation, Screen margin, Line spacing, Paragraph alignment, Hyphenation, Embedded style, Bionic / Guide reading, Image rendering, Viewport, Font ID hash). Back returns to the menu. Useful for troubleshooting "use prepared layout, but indexing every chapter" — eyeball the saved fontId / size combo and cross-reference with the in-reader settings drawer without restoring.
- **`prebake-v2.marker`** sentinel file written alongside `prebake-manifest.json`. The File Transfer "Pre-cached" badge and the on-device "Optimized" header label both gate on this marker so they only flaunt status for 4.2+ bakes (which actually deliver device-matching layouts for SD fonts). Pre-4.2 bakes still load via `tryLoadPrebakeManifest` for the "Use prepared layout?" prompt — they just no longer claim a status the toolchain at the time couldn't deliver.
- **`/api/builtin-fonts`** endpoint reports which built-in font families the device's current firmware actually ships, so the optimizer preflight modal can hide families that were stripped at build time (CharEink and/or Lexend Deca). Older firmware that lacks this endpoint continues to work via a hardcoded fallback in the optimizer JS.
- **`/api/font-file?family=X&size=Y`** endpoint serves raw `.cpfont` bytes so the WASM optimizer can MEMFS-mount them for SD-font prebakes. Combined with the WASM host_shim SD-font fast-path, the browser bake now hashes against the same `(contentHash, family, pointSize)` triple the device computes at open time.
- **"Lookup" is always visible in the in-book menu**, even when no StarDict dictionary is installed. Selecting Lookup without a dictionary now shows an info screen pointing the user at the GitHub releases for the bundled `.ifo/.idx/.dict` set, instead of silently hiding the feature.
- **Animated `...` beacon during the one-time dictionary index build.** Cycles 1→4 dots throttled to ≥2.5 s between redraws (~3-4 s overhead on a 20 s scan) so the user can see the device is alive without doubling the scan time.

### Changed
- **Long-press menu canonical order** applied uniformly across all 5 entry points (file browser, home carousel, recent-books list + grid, bookmarks home):
  1. Add to / remove from collection
  2. Remove from Recent Books
  3. Mark as Finished / Unfinished
  4. Show metadata
  5. Delete book cache
  6. Delete
  Benign navigation/curation up top, destructive deletes last — the cursor has to travel past safe options before reaching anything that can wipe data.
- **Long-press menu header now shows title + author on two lines** (bold title, regular author) when the book's metadata is available, instead of word-wrapping the filename. Falls back to filename for non-book entries (PDFs, sleep images, etc.) with single-line layout. Resolution chain: in-memory `RECENT_BOOKS` → OPF parse (~50-100 ms) → filename last-resort.
- **Lexend Deca + CharEink no longer bundled in the slim built-in font set.** Both families remain installable as SD-card `.cpfont` bundles (`CrumBLE-LexendDeca-SDfont.zip`, `CrumBLE-ChareInk-SDfont.zip`). The device-side font picker hides them in the slim build; users with `LEXENDDECA` or `CHAREINK` saved as their preference are silently clamped to Bitter at boot.
- **Bitter** becomes the default reading font for fresh installs on the slim binary.

## [crumble-v4.1.0] - 2026-06-04

### Added
- **OTA updates now pull from CrumBLE's own GitHub releases** (`imshentastic/CrumBLE`) instead of upstream CrossInk-Carousel. Once you're on v4.1.0, `Settings → Check for Updates` finds every future CrumBLE release automatically — **this is the last manual flash you'll ever need**.
  - The version comparison uses `CRUMBLE_VERSION` (4.1.0, 4.1.1, 4.2.0, ...) instead of upstream's `CROSSINK_VERSION` (only bumped when CrumBLE rebases onto a new CrossInk).
  - The asset matcher refuses any `crumble-firmware-X.Y.Z-full-needs-USB-flash.bin` variant — those binaries are intentionally too large for the legacy 6.25 MB OTA partition and would brick anyone who tried to OTA them. Users who want CharEink built-in continue to flash the full binary manually via crosspointreader.com/#flash-tools.
  - The OTA Update activity shows `CrumBLE X.Y.Z → 4.1.0` (stripped of the `crumble-v` tag prefix) so the UI reads cleanly.

### Changed
- Version bump 4.0.1 → 4.1.0 because "OTA actually works now" is a meaningful new capability worth a minor-version bump.

### Fixed
- **Optimizer preflight modal now includes SD-card fonts.** Users with a custom `.cpfont` family (e.g. the CharEink SD-card bundle, or any community font dropped into `/fonts/` on the SD card) can now confirm or change their font in the "Lock in reader settings?" dialog that fires before each EPUB optimization. Previously the dropdown hardcoded only the three built-in families (Lexend Deca / Bitter / CharEink) and silently dropped SD selections. The fix pulls the family list from `/api/fonts` at modal open and routes the saved selection through `sdFontFamilyName` when an SD font is picked.
- **Chapter prebake is always on.** The `Optimize Chapter Indexing` toggle that lived under Settings → Library was a development-era opt-in left over from the prebake feature's bring-up. With the rest of the prebake stack working end-to-end, leaving it default-off meant users who uploaded books through the optimizer never actually saw the benefit (no manifest detection, no fast chapter turns, no "switch back to prepared layout?" prompt). The toggle is now removed from the UI; the underlying field is force-migrated from 0 → 1 on settings load so existing devices that had explicitly turned it off pick up the new behavior automatically. Books without a prebake manifest still open normally — the prebake path is a fallback, not a requirement.
- **Chapter prebake uploads no longer fail with `HTTP 400 Access denied to protected path`.** The optimizer writes per-book prebake artifacts to `/.crosspoint/epub_<hash>/sections-prebake/` on the SD card. Because that path's leading `.crosspoint` segment starts with a dot, the `isProtectedPath` check (which blocks `.`-prefixed segments when `showHiddenFiles` is off, the default) was rejecting every single prebake upload. The check now whitelists `/.crosspoint/` as the device-managed cache directory — it stays writable regardless of the `showHiddenFiles` setting. Symptoms before the fix: "Chapter prebake done: 0/214 files in 58.8s" + a wall of `PRE-FAIL` log lines.

## [crumble-v4.0.1] - 2026-06-04

### Changed
- **Two firmware variants ship side by side**: `crumble-firmware-4.0.1.bin` (slim, 6.08 MB, drops the CharEink built-in font, fits the 6.25 MB legacy OTA partition) and `crumble-firmware-4.0.1-full-needs-USB-flash.bin` (full, 6.85 MB, keeps all three built-in fonts but requires USB flash). Users who want CharEink without the partition pain can flash the slim build and install CharEink as a custom SD-card font via `CrumBLE-ChareInk-SDfont.zip`.
- Version bump from 4.0.0 → 4.0.1 is purely a clarity marker. The 4.0.0 release was re-uploaded multiple times during a debugging session (with WASM dropped, with WASM re-added, slim variant added) and users who downloaded mid-iteration have different bits all labelled "4.0.0". 4.0.1 is the canonical settled artifact.

### Fixed
- Settled the `crumble-prebake.{js,wasm}` embedding decision: ALWAYS embedded so the EPUB optimizer's prebake step works offline. A previous iteration of 4.0.0 stripped the WASM to keep the binary smaller, but the optimizer then silently failed the prebake step (book uploaded fine, but no instant chapter turns + a misleading "FAILED" log entry).
- Build flag `OMIT_CHAREINK_FONT` added so the slim binary drops the CharEink family cleanly (picker hides the option, font ID lookup falls back to Bitter for users with a stale CHAREINK preference).

## [crumble-v4.0.0] - 2026-06-04

### Added
- **Dictionary support (StarDict)**: ported from SEEK reader. Drop a StarDict `.ifo`/`.idx`/`.dict` set into `/dict/` on the SD card and tap-select any word in a book to look it up. The reader auto-disables BT during the lookup to free heap for the dictionary index, then reconnects on exit.
- **Quote highlighting**: word-range selection. Long-press to mark a start word, navigate (same page or across pages/chapters), tap to finish. The bookmark list shows quote previews on each row; Confirm jumps straight to the bookmark location.
- **EPUB optimizer pre-cache**: every EPUB uploaded over File Transfer can optionally be pre-baked into the device's section/page format. First-time book open is ~4 s and chapter turns are instant (no mid-read indexing). Adds ~40 s per upload; the toggle defaults ON in the Upload modal and can be skipped per-book. An "Optimize Selected" button optimizes EPUBs already on the SD card without re-uploading.
- **Collections system enhancements**: rename collections, rearrange book order within a collection, build a collection from a folder by long-pressing Select in File Explorer.
- **Bookshelf grid layout**: full-page 2x2, 3x3, or 4x4 cover tiles. Configurable per-collection.
- **Quick Resume cookie logo**: bottom-left overlay on the sleep frame when Quick Resume is wired up. Replaces the previous diagonal-dots loading icon.
- **Cycle Sleep Screen on Tap**: short/long-press buttons to flip between custom sleep images while the device is asleep, without unlocking.
- **Series Detection** (opt-in): scans OPF metadata to group books by series.

### Changed
- **Settings UI redesigned into nested submenus**. The previous four-tab flat list is now six top-level groups: Display, Reader, Controls, Library, Sync & Network, System. Each opens its own sub-screen. Reader splits into Font / Layout / Style / Reading Aids / Customise Status Bar. Inspired by but not directly ported from CrossInk 1.3; the tree was designed specifically for CrumBLE's settings surface.
- **In-book menu reorganized into three line-divided sections**: quick actions (Footnotes, Lookup, Add Highlight, Reading Stats, Auto Page Turn), navigate + customise (Select Chapter, Go to %, Sync Progress, Reader Options, Controls, Bookmarks ▸, Bluetooth), and output (Display QR, Screenshot, Mark Finished, Delete Cache). Bookmarks opens an inline sub-screen with Add Highlight + View / Export / Clear. Add Bookmark and Go Home removed; Orientation moved into Reader Options.
- **Global Book Settings drawer**: BT Quick Connect now sits at the top of the drawer instead of after the Reader settings.
- **Web UI rebranded** from CrossInk to CrumBLE (page titles, header, footer). Upstream CrossInk version still shown as a sync-point reference.
- HOLD highlight preview captures ~14 words of trailing/leading context on each side instead of a single word, so the saved bookmark preview reads as a passage.

### Fixed
- **File Transfer no longer freezes on entry under heap pressure**. Several recovery paths added: BT controller is fully released (not just disabled) before the FT activity claims the heap; web server low-heap guard auto-restarts the FT activity (with the previous AP/STA mode preserved) instead of leaving the device wedged; the page-serve handler refuses-and-reloads if free heap drops below a safe floor.
- **Wake-button long-press no longer fires during boot**. The wake-hold release edge used to persist into the first activity tick, briefly triggering whatever long-press action was bound to the power button. Now absorbed before the activity starts.
- "Could not save progress" no longer repeats after backward navigation through chapters.
- Ghost page-number bug fixed.

### Known limitations
- The web /settings page reports "settings unavailable" in 4.0; use the device-side Settings UI (the new nested submenus). All other web-UI features (File Manager, WiFi, OPDS, Fonts) work normally.

## [crumble-v3.7.3] - 2026-05-31

### Fixed
- **File Transfer no longer freezes mid-session on libraries past ~30 books.** A handful of users hit a wedge where tapping into File Transfer worked, the web UI rendered, and then the back tap stopped responding — only a forced reboot recovered. Root cause was heap **fragmentation**, not raw exhaustion: total free heap looked healthy (~14 KB) but the largest contiguous block dropped to ~6 KB, well below the 8-16 KB chunks WiFi + AsyncWebServer + the activity-back-redraw allocator want. The per-book metadata stores (`LibraryIndex`, `SeriesIndex`) held their fields as individual `std::string`s, so an N-book library produced N (or N×3) small heap allocations scattered across the heap; `releaseMemory()` freed them but the slots stayed scattered between other longer-lived allocations and the allocator couldn't coalesce them back into a single large hole. Both stores now pool their strings into a single contiguous `std::vector<char>` (each entry holds a `uint32_t` offset). One big allocation instead of N small ones; releaseMemory returns ONE large hole. Measured impact on the same 32-book library that was freezing: post-HTML-page-served max-contiguous-alloc went from 6 KB → 23 KB, and **Min Free over the whole File Transfer session went from ~1-2 KB (on the OOM cliff) to ~14.7 KB** — a 7-10x safety margin. Safe library size for File Transfer should now be roughly 90-150 books before hitting the same threshold the old layout hit at 32.
- Cover for boxed-set books (e.g. Game of Thrones 5-Book Boxed Set) sometimes loaded fine in the Home carousel but rendered the text placeholder in Collections. The thumb-failed marker was global per book — one transient memory-pressure failure during a multi-book Collections grid load (e.g. 9 covers in flight at 3x3) permanently poisoned the book against ALL future thumb-gen attempts at any size, even though Carousel-sized thumbs had been cached earlier and worked fine. Markers are now size-scoped (`thumb_failed_v3_<W>x<H>.marker`); failing at one cell size doesn't block regeneration at another. The v2 → v3 suffix bump also invalidates every existing global marker, giving every book a fresh chance to thumbnail at current cell sizes.
- Long boxed-set titles overflowed the cover-placeholder cell with wrapped small-bold text instead of reading as a clean "missing cover" hint. Capped at 4 lines, matching the existing MinimalTheme convention.
- In a 2x2 / 3x3 / 4x4 Bookshelf with a partial last row (e.g. 3 books in 2x2, 5 in 3x3), pressing DOWN from the top-right cell did nothing — the same-column slot in the next row had no book, so the navigator skipped the move and fell through to next-page wrap. Now snaps to the rightmost cell of the partial row, so DOWN feels like a real move every time.

## [crumble-v3.7.2] - 2026-05-30

### Added
- **Make collection from folder**: long-press a folder in the file browser to open a menu with "Make collection from folder" (next to the existing "Delete"). Walks the folder recursively for `.epub` / `.xtc` / `.txt` / `.md` / `.markdown` files (same rules as the library scan, 8 levels deep, skips `XTcache` and dot-prefixed entries), creates a user collection named after the folder, bulk-adds every book in one save, makes the collection active, and returns to Home. Fast path for turning curated SD subdirectories into shelves without long-pressing each book.

### Changed
- Collection names are now disambiguated on create / rename. Picking "Sci-Fi" when "Sci-Fi" already exists (whether as a user collection or a virtual one — Favorites / All Books / Recently Added / Finished / Unopened) auto-suffixes the new name with the smallest unused " (N)" instead of silently allowing two indistinguishable entries on the shelf header. Case-sensitive — "Sci-Fi" and "sci-fi" remain treated as distinct.

## [crumble-v3.7.1] - 2026-05-30

### Added
- **Sleep Screen Order**: new Display setting (Random / Alphabetical) shared by both the Custom-mode fallback (no pinned image) and the deep-sleep tap-to-cycle path. Random (default) preserves prior behavior — anti-recent-repeat random pick from `/.sleep/`. Alphabetical walks `/.sleep/` in sorted order using a persisted cursor that survives reboot, so curated collections rotate in deterministic order.

### Fixed
- Transparent PNG sleep images now compose over the last reader page even when sleeping from Home/Settings, not only when sleeping from inside a reader. The `/.crosspoint/last_reader_page.bin` snapshot is already written on reader-to-home exit, but `composePngOverReaderPage` was gating its use on the current activity being a reader, so non-reader sleep entries dropped the cached page and showed the PNG over a blank background. Cache restoration now relies on the snapshot file's own existence check — safe because the file is only ever written from reader contexts, never from Home/Settings, so it can't surface a stale non-book background.

## [crumble-v3.7.0] - 2026-05-30

### Added
- **Bookshelf grid Layout option**: 3x3, 4x4 (default), or 2x2. Toggle from the "Layout" row at the bottom of the Bookshelf collection picker (hold Back inside the grid to open). The setting persists across reboots. 4x4 shares its 100x150 cell size with the Flow shelf so transitioning Home -> Bookshelf hits a warm thumbnail cache. 3x3 uses bigger 130x190 cells with generous spacing; 2x2 uses 220x320 (shares the cache with the carousel center cover + Reading Stats).
- **Title Placement option**: the focused-book label strip (title / author / read+remaining times) can now sit above the books OR below them. Pairs with the page-dot indicator: above-mode anchors the dots to the screen bottom; below-mode stacks dots just above the strip. Toggle from "Title Placement" in the Bookshelf collection picker, below the Layout row.
- Bookshelf grid cover loading is much more resilient on cold collections. Generation uses the same fast `epub.generateThumbBmpNoIndex` path the Flow shelf uses (content.opf-only parse, no spine/TOC index build) with a heavy `epub.load + generateThumbBmp` fallback for stubborn books. The 48 KB framebuffer snapshot and the in-RAM image cache are freed before gen so the JPG/PNG decoder + scaled-BMP write buffer has up to 112 KB more contiguous heap — what used to leave half a 16-cell page on placeholder now renders fully.

### Fixed
- Books stuck on placeholder covers from prior builds. The "thumb_failed" marker filename was bumped (suffix `_v2`) so markers set by older builds — which sometimes mis-fired on transient heap-OOM gen failures during 16-book sequential gens at 4x4 / 220x320 at 2x2 — are silently ignored. Combined with the new heap-pressure-relief before `loadPageCovers` (snapshot + image cache freed) and the NoIndex + heavy fallback gen pair, transient failures are rare and only permanent failures (no cover image / unsupported cover format) get marked.
- Bookshelf page-dot indicator clipping the bottom row's progress bar in 2x2 mode. The 2x2 inter-row geometry now leaves a clean gap; the dot size shrinks from 8 px to 6 px in 2x2 to keep the bar clear without changing the dot Y position.
- Carousel left/right navigation rebuilt the perspective side covers from scratch every press. Side tiles are now pre-rendered to a packed 1bpp cache and blitted on subsequent presses, eliminating ~70k per-side pixel walks per navigation.
- Returning to Home from the reader sometimes left the cursor on the wrong icon-bar entry. Cursor recall order is now: caller-set entry > saved cursor from the previous visit > opened-book highlight > default; the reader clears the saved cursor on entry so a re-open uses the freshly-opened book as the recall target.

### Changed
- 4x4 is now the default Bookshelf layout (was 3x3). Existing users keep whatever layout they had set; new installs land on 4x4.
- Bookshelf cells use a unified double-stroke selection ring (3 px inner + 1 px outer with a gap) that matches the carousel center cover and the Reading Stats main cover. 2x2 gets a slightly tighter ring (4 px outer extent vs 6 px) so it clears the inter-row gap on the wider covers.
- LyraTheme header is more compact (52 px tall vs 84 px) which gives Bookshelf and Reading Stats more vertical room for cells / cover.
- Reading Stats cover sized to 220x320 to match the carousel center cover. Opening Stats from a focused carousel book is now an immediate cache hit instead of a re-decode. Stats are cached per book during the All Books navigation filter pass so cycling through the list doesn't re-read `stats.bin` for every step.
- Bookshelf and Flow shelf both detect "same page, focused-cell changed only" state on a press and restore a framebuffer snapshot + repaint just the selection ring + title strip, instead of a full clearScreen + redraw. Per-press cost is O(1) in steady state.

## [crumble-v3.6.0] - 2026-05-29

### Added
- Phase 1 fast book open from Home. The reader's non-critical onEnter work (settings cache build, .pxc manifest parse, font glyph buffer prewarm) now runs after the first reader page has actually painted, instead of blocking the tap-to-first-pixel path. Felt as ~30-50 ms snappier on every book open.
- The in-RAM cover bitmap cache (introduced in v3.5 but inert until now) is wired up across the Flow theme carousel center cover, the four perspective side covers, and the Bookshelf grid. Navigating through the carousel and into the Bookshelf hits memory instead of re-decoding from SD on every cell.

### Fixed
- Bluetooth remote stays connected more reliably on mixed text/image books. The reader's glyph decompression buffer is pre-grown at every Bluetooth-enable site (drawer Quick Connect, reader menu BT toggle, Bluetooth settings) so the buffer's high-water mark is allocated BEFORE NimBLE eats heap, instead of fighting for contiguous heap mid-page-turn.

### Changed
- Renderer perf hacks ported from rhythmerc/crosspoint-reader: opaque-path fast path for the cached-bitmap blit, corner-skip during blit (replaces a per-pixel post-mask), the 1px asymmetric drawRect-with-lineWidth fix, and a fast path for fillRoundedRect when cornerRadius is 0. Wired in at the carousel center cover and the Bookshelf grid cells.
- Bluetooth indicator removed from the reader status bar. The always-dotted-when-enabled variant misled users when the remote disconnected; the connection-state-driven variant introduced a perf regression in the status-bar repaint path. Bluetooth state remains visible through the "Connecting Bluetooth..." popup and the in-reader Quick Connect / Disconnect drawer actions.
- System-wide glyph fallback (added in v3.5 but never released in a tagged build) reverted. It let codepoints missing from your reader font route through Inter (the UI font) before becoming tofu, but pushed heap over the cliff on image-heavy books while a Bluetooth remote was linked. Net effect: rare codepoints in book content (uncommon diacritics, Cyrillic on an otherwise-Latin font, math/special punctuation) render as tofu instead of via Inter, but image-heavy + Bluetooth reading is stable again.

## [crumble-v3.4.0] - 2026-05-27

### Added
- Two new opt-in auto-generated collections: **Finished** (books you've marked complete) and **Unopened** (books in your library that have never been opened in the reader). Toggle them on from the long-press menu on the collection header.
- **Rearrange** action in the long-press menu on the collection header. Tap Confirm on each collection in your desired order; Confirm reads "Mark 1", "Mark 2", ..., and the Back button reads "Undo" mid-flow so you can roll back a misclick. On the final mark, Home returns with the new L/R cycle order and the first collection active. Persists across reboots.
- Persistent "Connecting Bluetooth..." popup during BT Quick Connect, spanning the NimBLE init and GATT handshake so the page doesn't sit unchanged for several seconds without feedback.

### Fixed
- Folders named `XTcache` (case-insensitive) are now skipped during the library walk, so any files the companion XT reader parks there don't appear in Recently Added, All Books, or Unopened.

### Changed
- The collection header's long-press menu is reordered around what users actually do: "+ New collection", "Sort by", "Rearrange", then the four Show/Hide toggles (All Books, Recently Added, Unopened, Finished), then "Rescan library". Each Show/Hide row uses the same right-justified inverting toggle style as the main Settings menu, so the current state is scannable at a glance.

## [crumble-v3.3.0] - 2026-05-27

### Added
- The web optimizer pre-renders each EPUB image to a per-device pixel cache (`.pxc`) at the device's exact screen viewport and emits a small manifest of the settings the bake was made against. Image-heavy chapters now render over Bluetooth without thrashing the link or needing the JPEG decoder.
- When opening a baked book with different font, margin, image rendering, or orientation than the bake assumed, the reader now prompts on Quick Connect: switch back to the baked layout, keep your settings and reflow, or cancel. Previously it would silently rebuild under heap pressure.
- Bluetooth status icon in the reader's status bar (to the right of the battery), with side dots when a remote is currently linked.
- New optimizer Advanced toggle (default on) for the `.pxc` image bake, so users who never read with a Bluetooth remote can skip it and keep the EPUB smaller.
- Author shown under each book in the Flow carousel.

### Fixed
- Spurious "Bluetooth couldn't stay connected" alert on every first connect. The bonding/encryption renegotiation that happens in the first few seconds is no longer treated as a real disconnect; genuine heap-pressure drops in the rest of the early window still surface as before.
- RoundedRaff theme: "Continue Reading" was listed twice in the home menu, shifting every other action by one slot — selecting "File Transfer" opened Settings, and the last menu item became a silent no-op. Now lists once and actions land on the right item.
- Flow carousel center book had a wide white background that visually cut into the adjacent covers. The white frame is now sized to the cover itself, not the whole slot, and the side covers regain the strip you could see in older releases.
- Flow carousel side covers are no longer clipped at the screen edges.
- Bluetooth status icon now has visible breathing room from the battery number.

### Changed
- Book Settings drawer (and Reader Options) always show the full settings list now, even with a Bluetooth remote linked. Toggling a font, margin, or other layout setting silently drops the link around the chapter rebuild and restores it after, instead of presenting a "Bluetooth is on, turn it off first" prompt on every toggle. Settings are cached at book open so the drawer remains responsive even when heap is tight.
- The drawer's "BT Quick Connect" entry becomes "BT Disconnect" while a remote is already linked.
- BT Quick Connect now resolves the baked-layout manifest mismatch BEFORE running the chapter rebuild, so the rebuild matches whatever you picked instead of running once with the wrong layout and rebuilding again.
- Selection border around the focused Flow carousel book uses rounded corners with a slightly tighter radius, matching the cover thumb.

## [crumble-v3.2.0] - 2026-05-25

### Added
- Preview PNG images straight from the file browser (they previously failed to open and bounced back to Home), and set a PNG as a sleep screen image.
- Show the loading popup immediately when opening a book, so a tap registers right away even while the cover decodes or the first chapter indexes.

### Fixed
- Text now keeps rendering with a Bluetooth page-turner connected: the glyph decompression buffer is held across pages instead of being reallocated on every render, which previously starved under Bluetooth heap fragmentation and dropped the link mid-chapter.
- Bluetooth now survives image-heavy chapter boundaries: after a low-memory chapter rebuild the remote reconnects on its own once the page is safe to repaint (its images are cached to the on-device pixel cache), instead of thrashing connect/disconnect and leaving the remote off for the rest of the book.

### Changed
- Trimmed NimBLE host buffers (a single page-turner doesn't need the default multi-connection pools) to free heap for the reader's glyph and image buffers while Bluetooth is connected.

## [crumble-v3.1.0] - 2026-05-25

### Added
- Added "BT No Images Quick Connect" to the reader's Book Settings drawer: connect a Bluetooth page-turner on image-heavy books by skipping image decode (images show as placeholder boxes) so the link stays up. Images return automatically when Bluetooth disconnects, and reopening or rebooting the book restores them.
- Added a "Browse files to add a book" action to Add/Remove Books so a book can be added to a collection directly from a folder.
- Added an optimizer toggle (web file-transfer page) to store chapter text uncompressed for smoother Bluetooth reading.
- Made Recently Added and All Books opt-in virtual collections and moved library indexing off boot for a faster startup.

### Fixed
- Hardened Bluetooth reading on image-heavy books: keep the link usable, recover dropped images, gate anti-aliasing by available memory, fall back gracefully on cold chapter loads, and serialize NimBLE teardown with the render task.
- Suppressed the brief half-drawn-glyphs frame that could flash during the Bluetooth connect handshake, without dropping links on books that read fine.
- Deferred settings saves when heap is too low to build the settings JSON safely (previously a rare panic-reboot under Bluetooth memory pressure); the save is retried automatically once memory recovers.
- Recovered wedged book caches with a best-effort cleanup, and added clear guidance ("SD may need a disk repair") when a cache or page genuinely can't be cleared instead of failing silently.
- Improved large-library reliability: find books with long names at the SD root, give Recently Added a stable order, reclaim heap so Flow shelf covers generate reliably, and full-refresh on entering Home to clear transition ghosting.
- Improved web file-transfer reliability: lazy-load the zip and optimizer scripts to avoid running the device out of memory, bound the streaming-send timeout, and free the active SD font to stop WiFi hangs.
- Showed the sleep screen on auto-sleep timeout (no stuck "Going to sleep"), and cached a full-screen sleep image so it restores under low/fragmented heap.

### Changed
- Removed the redundant "Download Font Size Range" picker from reader settings (the shipped font sizes already determine it).
- Captioned collection shelf books with their metadata title and author, falling back to the filename.
- Moved the Bluetooth-friendly chapters toggle out of Advanced and clarified its labels.

## [v1.3.0] - 2026-05-21

### Added
- Added Back/Cancel support while downloading books from OPDS catalogs.
- Added a Recent Books long-press menu in both List and Grid views with delete, cache delete, completion, and remove-from-recents actions.
- Added a Minimal sleep screen option that shows the current book cover and reading progress on a dark background.
- Added more detailed WiFi connection debug logs for scans, selected networks, status changes, disconnect reasons, and timeouts.
- Added a 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.
- Added an in-reader confirmation message when a shortcut turns tilt-to-turn on or off.

### Fixed
- Fixed WiFi and OPDS connection-flow edge cases so manual Settings connections show the connected status first, copied or corrupted saved-password files are rejected before use, OPDS retries show loading before requests, and large OPDS feeds fail safely under low memory instead of rebooting.
- Fixed reader and Home UI polish issues, including landscape status-bar settings, missing Vietnamese labels, File Browser and Lyra Carousel icon alignment, cover thumbnail artifacts, and duplicate Home progress/stat loading.
- Fixed EPUB cache and low-memory handling by using stable cache folder keys, migrating older cache folders where possible, rebuilding stale section caches, laying out very long text blocks earlier, streaming table fallback content when heap is tight, and clarifying the warning text.
- Fixed sleep-entry, network, and SD-card font download reliability issues by reusing cached sleep-screen assets, idling OPDS pages normally after load, putting the X3 tilt sensor back to sleep outside the reader, disabling WiFi power saving during transfers, reducing WebDAV stack usage, tolerating longer stalls, retrying interrupted font files, and freeing active reader fonts when needed.
- Fixed remaining reader service edge cases, including an XTC chapter selector crash on memory-constrained builds, SD-card font size selection, SD-card font-size shortcuts skipping manually installed sizes, and KOReader Sync login compatibility with self-hosted servers that return valid JSON on success.

### Changed
- Modified upstream "page-as-sleep" behavior into a new `Sleep Screen > Quick Resume` option, which also keeps `Quick Resume on Timeout` on, and renamed the timeout-only toggle.
- Improved reader and browser menu behavior by moving the Footnotes shortcut above Select Chapter, wrapping long book titles in action menus, and reducing progress-screen repaint work during OPDS and SD font downloads.

## [v1.2.11.1] - 2026-05-15

### Changed
- Removed Medium font size from `xlarge` build to get it below the size limit

### Fixed
- Included Lyra Carousel by activating the build flag `DCROSSINK_ENABLE_LYRA_CAROUSEL=1`
---
## [v1.2.11] - 2026-05-14

### Added
- Added new personal theme: "Minimal"
- Added a custom sleep timer picker so `Time to Sleep` can be set from 1 to 30 minutes instead of cycling fixed presets.
- Added an in-reader Controls shortcut so you can customize your buttons without leaving the book.
- Added bookmark cleanup shortcuts: hold Select on a bookmark to delete it, or hold Open on a book in Bookmarks to clear that book's bookmark list.
- Added a confirmation message after deleting a book's cache from the reader or File Browser.
- Added a File Browser long-press action for deleting an EPUB or XTC book's cache
- Added a downloaded-font size range setting so SD-card fonts can use compact, default, or large point-size sets.
- Added a File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed
- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.
- Split SD-card font descriptions and supported languages into separate lines in the font download screen.

### Fixed
- Fixed inline EPUB images disappearing in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- Fixed the SD-card font picker reopening immediately after selecting a font from Settings > Reader > Font Family.
- Fixed in-reader font-size changes for SD card fonts not working
- Fixed in-reader SD-card font changes not always rebuilding the current EPUB page layout.

## [v1.2.10] - 2026-05-11

### Added
- Added a `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid.
- Added more flexible reader controls, including orientation-aware front/side button settings, nav-only or all-button front inversion, tilt page turn shortcuts, and side-button long-press rotation actions.
- Added a per-session auto page turn interval picker with values from 5 to 120 seconds.
- Added a file-browser Home/Back long-press action for toggling hidden files and folders.
- Added EPUB rendering and diagnostics improvements, including visible `<hr>` separators and heap logs around section rebuilds, image extraction, page serialization, and sleep-cache rebuilds.
- Added reader font coverage for block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation (PR #104).
- Added simulator tools for testing sleep/wake behavior and smoke-testing common screens and EPUB reader menus.

### Changed
- Reduced Controls settings section spacing so the grouped controls fit better on X3 screens.
- Made front reader long-press actions trigger when the hold delay is reached while normal page turns still trigger on release.
- Used the fast EPUB spine/TOC indexing path for books with 300+ spine entries so heavily split books build `book.bin` faster on first open.
- Allowed the web file manager and WebDAV to browse dot-prefixed hidden files when hidden files are enabled, matching the device file browser.

### Fixed
- Fixed reader button and shortcut behavior, including X3 power-button wake filtering, folder delete long-press timing, and WiFi scan/connect screens that could not be exited while work was in progress.
- Fixed RoundedRaff home-menu, keyboard, and button-hint rendering issues so Settings remains reachable and compact labels no longer overlap or disappear.
- Fixed font and glyph handling by reducing persistent SD-card font advance-cache memory, releasing optional font caches before image extraction only when heap is tight, and showing a visible replacement symbol when compact UI fonts lack `U+FFFD`.
- Fixed KOReader Sync authentication diagnostics and an in-reader sync crash, including clearer handling when a server or proxy returns non-JSON content.
- Fixed EPUB text rendering for redactions, whitespace-only XHTML text nodes, simple black CSS span backgrounds, list bullets in `<li><p>...</p></li>` items, and very long base64-like text runs.
- Fixed EPUB image, thumbnail, and section-rebuild stability so image-heavy chapters use less temporary memory, scale images more reliably, avoid stale dimensions, and suppress optional image work earlier under heap pressure.
- Fixed EPUB low-memory and cache safety by skipping optional next-chapter indexing and sleep-page cache rebuilds when heap is tight, failing safely with a malformed-book warning and Home exit path, rebuilding incompatible fork-written caches, and handling low-memory CSS parsing, truncated SD writes, invalid serialized strings, and failed temp-cache promotion.
- Fixed a Home crash after clearing reading cache by skipping optional EPUB thumbnail rebuilds when the source EPUB cache is missing.
- Fixed reader prewarm behavior by skipping image decoding, keeping mixed-style font glyphs cached together, and avoiding section rebuilds for render-quality-only option changes.
- Fixed concurrent render/storage crashes by serializing `GfxRenderer` scratch-buffer access, shared SPI bus access, and failed SPI lock cleanup.
- Fixed Recent Books, EPUB/XTC thumbnail caches, deleted-folder metadata, and XTC cover scaling so cached book data stays in sync and grid covers fill their slots correctly.
- Fixed simulator build configuration so SDL2 and simulator-provided network/OTA shims compile cleanly.
---
## [v1.2.9.1] - 2026-05-03

### Changed
- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed
- Fix power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions. Those that had short-press power button to act as sleep saw unstable behavior previously. This should be fixed now
- Fix a potential crash when using `Go to %` in EPUBs
- Fix a potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid
