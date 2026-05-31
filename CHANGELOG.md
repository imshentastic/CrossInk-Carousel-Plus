# Changelog

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
