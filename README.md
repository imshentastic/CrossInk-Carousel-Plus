<div align="center">

# CrumBLE

<p align="center">
  <img width="440" height="591" alt="Screenshot 2026-05-31 at 11 45 56 PM" src="https://github.com/user-attachments/assets/855dbae2-8ce5-4d63-a6a9-15081ffbbb94" />
</p>

**A personal fork of [CrossInk](https://github.com/uxjulia/CrossInk) for the Xteink X4 — adds full BLE support, dictionary lookup, quote highlighting, an EPUB optimizer with instant-chapter-turn pre-cache, on-demand sleep-screen cycling, customized Collections, Bookshelf display, and a quick-settings overlay drawer! Supports 23 languages and custom fonts.**

</div>

> **Runs on the Xteink X4 (X3 in development) ** **Back up your device before flashing.** See [Install firmware](#install-firmware) below.

---

## What CrumBLE adds

CrumBLE sits on top of CrossInk and CrossInk Carousel's feature set — see the [CrossInk Carousel README](https://github.com/chintanvajariya/CrossInk-Carousel#whats-different-from-crossink) and [CrossInk's docs](https://github.com/uxjulia/CrossInk) for those features. The sections below cover what's distinct to this fork.

As of v3.0.0, CrumBLE is rebased onto **CrossInk 1.3** (a fresh upstream base rather than back-porting), so it also inherits 1.3's additions — SD-card font sizes, Quick Resume, the Minimal sleep screen, low-memory OPDS handling, and more.


### Collections

Not a fan of always digging into file explorer to find your books? Want to group your books in a display on the home screen that makes sense? Fully customizable collection system that helps you access, organize, and display your books regardless of your file structure.

- **Default collections**: **All Books**, **Recently Added**, **Unopened**, and **Finished**
- **User collections** Make your own and add any books of your choosing
- **Per-collection sort** (A–Z / Z–A / Author A–Z / Author Z–A / Date Added), persisted in `collections.json`
- Optional **series collapse** that folds same-series books into one spine glyph on the shelf; tapping the spine opens a mini-picker of the series members (in Beta)

<p align="center">
   <img src="https://github.com/imshentastic/CrumBLE/releases/download/readme-assets/04-add-remove-books.gif" alt="Add / Remove Books picker" width="280"/>
</p>

### Bookshelf grid

Browse the active collection as a 2×2, 3×3, or 4×4 grid of cover thumbnails instead of cycling through the carousel. Pick the grid size from the Bookshelf collection picker's Layout row.

- **Bookshelf** entry on the home icon bar opens the grid over your current collection
- **Short-press** the carousel header (collection title) opens the same grid
- **Long-press** the Bookshelf icon brings up a full-screen picker to switch collections without leaving the grid
- Cover thumbs are pre-cached at exact cell dimensions, so revisits don't flash a "Loading" popup looking for thumbs that already exist

<p align="center">
  <img width="281" height="378" alt="Screenshot 2026-05-31 at 11 46 30 PM" src="https://github.com/user-attachments/assets/680af84c-3f53-4f01-978c-b86ed9a1dbb9" />
  <img width="281" height="378" alt="Screenshot 2026-05-31 at 11 46 37 PM" src="https://github.com/user-attachments/assets/b458be1b-ed94-46f8-965c-0dd0fd528019" />
  <img width="281" height="457" alt="Screenshot 2026-05-31 at 11 49 10 PM" src="https://github.com/user-attachments/assets/de66eb4c-5e09-4110-b72e-ddad5afe5cd0" />
</p>

### EPUB optimizer + pre-cache (instant chapter turns)

Books normally index their chapter structure on first open and re-index when you change reading settings. CrumBLE lets your computer do the heavy lifting *off-device* during File Transfer instead — your X4/X3 just unpacks a pre-built section/page cache.

- **Upload modal**: an **Optimize EPUB** master toggle (and an underlying **Pre-Cache for fast chapter turns** sub-toggle, default ON) runs the optimizer in your browser before the upload. Adds ~40 s per book in exchange for instant chapter turns and ~4 s cold book opens.
- **Already on the SD card?** Select EPUBs in the file manager and use the **Optimize Selected** action to bake them in place without re-uploading.
- The pre-cache locks in your current reader settings (font, font size, margin, image rendering, orientation) — if you change one mid-read, the reader prompts to switch back to the baked layout or fall through to live indexing.

The previous Bluetooth-image-cache (`.pxc`) and chapter-text flattening features are still included as part of the optimizer's pre-cache pipeline.

### Bluetooth remote page-turner

Pairing is done from WITHIN A BOOK ONLY! Click on the "Confirm" button while inside a book to open the reader menu. Navigate to Bluetooth and follow the instructions there to pair a BT HID remote (e.g. an [IINE GameBrick](https://www.amazon.com/dp/B0CK4DNQM4) or Free2) and use it as a wireless page-turner. BLE auto-disables when you exit the book to keep heap pressure off the parser, so you will need to reconnect again when you enter a new book.

Bluetooth will always be a challenge for this device, but I'm trying to bring the capability forth without disabling WiFi, full image/css disabling, etc. There are still some books (and I expect most manga/comics) that BLE will not work for, but I'm continuing to optimize. The optimizer's pre-cache pipeline also pre-renders images to per-device .pxc, see EPUB optimizer above

- **BT Quick Connect**: Located at the top of the [Global Book Settings drawer](#global-book-settings-drawer) for one-step re-connect to your last bonded remote without re-navigating the menu tree (long-press confirm when inside book). Once connected, the option becomes **BT Disconnect**. When I read, I open my book, open the drawer, press up twice and use the BT Quick Connect button to fast pair with my controller.

- **BT No Images Quick Connect**: Located at the top of the [Global Book Settings drawer](#global-book-settings-drawer), this one-tap drawer action will temporarily suppress image decoding at render time (uses placeholder border) while BT is connected. It will automatically revert back once BT is disconnected, so just another option to have without any long-term commitment.

<p align="center">
  <img width="270" height="480" alt="IMG_9994" src="https://github.com/user-attachments/assets/6b0f1e34-2aae-4186-9229-6eb501cef5f1" />
</p>
Shout-out to [thedrunkpenguin](https://github.com/thedrunkpenguin/crosspoint-reader-ble/) for his BLE changes which I learned much from and added some memory changes to make it all fit.

### On-demand sleep-screen cycling

A new display setting — **Tap Power While Asleep to Cycle** — lets you flip through your `/.sleep` images without fully waking the device. A brief power-button tap picks a fresh random image and re-enters deep sleep. Off by default (each cycle costs a boot + e-ink half-refresh worth of battery). You can also change the sort from 'random' to 'ordered'

`.png` sleep images (with transparency) are also supported in **Custom** mode, not just BMP Page Overlay. Transparent regions compose over the clean last reader page, so a translucent PNG sleep screen reveals the page of the most recently accessed book underneath.

Note: Quick Resume mode is now supported, and overlays the brand cookie logo on the bottom-left of your sleep frame so you can tell at a glance whether the device is in Quick Resume vs. fully off.

<p align="center">
  <img src="https://github.com/imshentastic/CrumBLE/releases/download/readme-assets/05-sleep-cycle.gif" alt="Sleep screen cycling" width="280"/>
</p>

### Dictionary lookup

Drop a StarDict (`.ifo` / `.idx` / `.dict`) into `/dict/` on the SD card, open any book, and tap-select a word from the in-book menu's **Lookup** entry to see the definition without leaving the page. A per-book lookup history surfaces under **Looked-Up Words** so you can re-find a definition without re-tapping the word.

Bluetooth auto-disables during lookup to free heap for the dictionary index and reconnects on exit, so you can read with a BT remote and still hop into a definition when needed.

Ported from [SEEK reader](https://github.com/seek-reader/seek) with full credit — couldn't ask for a better implementation to start from.

### Quote highlighting

A bookmark is now optionally a *passage*, not just a page marker.

- Open **Add Highlight** from the in-book menu, tap the first word of the passage, navigate to the last word (same page or many pages later), tap to finish
- The bookmark list shows the highlighted quote as a preview on each row; **Confirm** jumps straight to where the highlight starts in the book
- Cross-page highlights can be "held" — tap the start word, hit Back ("HOLD"), turn pages, then come back via **Finish Highlight** in the menu when you're at the end word

Export all highlights for a book to a plain-text file from the Bookmarks sub-menu's **Export to text file**.

### Global Book Settings drawer

Long-press the menu button inside a book to pop up a bottom-drawer quick-settings panel. Every reader setting — font, size, hyphenation, bionic, line spacing, paragraph alignment, image rendering — is one tap away with e-ink fast refresh so toggles feel snappy. Closing the drawer re-flows the page only if a setting actually changed; a no-op visit skips the re-layout.

Architecture adapted from [inx by Dave Allie](https://github.com/obijuankenobiii/inx) (MIT).

<p align="center">
  <img src="./docs/images/crumble/06-book-settings-drawer.png" alt="Global Book Settings drawer" width="280"/>
</p>

### Settings menu redesign

The settings UI is reorganized into six nested submenus instead of the previous four-tab flat list:

- **Display** — Sleep Screen, Theme & Layout, General
- **Reader** — Font, Layout, Style, Reading Aids, Customise Status Bar
- **Controls** — Power Button, Front Buttons, Side Buttons
- **Library** — Files, Series Detection, Optimize Chapter Indexing
- **Sync & Network** — Wi-Fi Networks, KOReader Sync, OPDS Servers
- **System** — Sleep Timeout, Language, Check for Updates, SD Firmware Update, Clear Reading Cache

Back from any nested submenu pops one level up; back from the root exits to Home.

---

## Other improvements

- **Faster home + book open** — Faster book opening by deferring non-critical reader setup (settings cache, .pxc manifest parse, font glyph prewarm) to after the first page actually paints. In-RAM cover bitmap cache wired across the Flow carousel and Bookshelf grid so navigation hits memory instead of re-decoding from SD on every cell.
- **Reading Stats redesign** — Still in progress, but some changes already made with larger covers and the multi-book totals when global stats exist.
- **Reading time accuracy** — deep-sleep commit path flushes the active session so power-off never loses minutes. The 10-second minimum-session floor was dropped; very short sessions count too. Idempotent re-commit prevents double-counting. Ported from [aalu's reading-stats fix](https://github.com/aaludon/crosspoint-reader-aalu) (MIT).
- **Persistent cursor recall** — leaving home for Settings / File Browser / Bookshelf and coming back puts the cursor back where you left it on each side (carousel + shelf + menu row), instead of resetting to index 0.
- **Recents auto-heal** — if a foreign firmware writes an incompatible recent.json shape between CrumBLE boots, the first home visit walks per-book stats.bin sidecars and rebuilds the carousel from them (sorted newest first).
- **PNG previews in the file browser** — `.png` files render as a preview over the last book page; you can also set any PNG as a sleep image.
- **Carousel ghosting fixes** on the Lyra Flow theme — max-size cover-slot clear before each paint, thinned selection border, dropped the always-on inner frame so successive scrolls don't leave outline residue, and the side covers no longer clip at the screen edges.
- **PackBits-compressed BW backup** for the grayscale AA pass — a single 16–32 KB bounded buffer replaces the chunked 12 × 4 KB lazy allocation, dropping the fragmentation pressure that made grayscale fail when BLE was active.
- **Auto-retry on chapter-layout abort** — if the parser trips the low-heap floor with BLE consuming its ~58 KB share, CrumBLE silently drops BLE, retries the layout with the recovered headroom, and lets the existing auto-reconnect logic re-pair on your next remote press.
- **Glyph buffer pre-grown at every BT-enable site** so the font scratch's high-water mark is allocated BEFORE NimBLE eats heap, preventing the mid-page-turn allocation failures that used to drop the BT link on text-heavy chapters.
- **Large-library + home stability** (v3.0.x) — streaming library index that survives big libraries, crash-proofed series detection, a Lyra Carousel heap-race crash fix, cover-thumbnail revalidation so a single book can't get stuck on a placeholder, and transparent-PNG sleep screens that reliably show the clean last book page.

For the full changelog, see [CHANGELOG.md](./CHANGELOG.md).

---

## Languages

**UI translations (23 languages)** — the menus, buttons, and prompts are translated. Missing strings fall back to English automatically.

Belarusian, Catalan, Czech, Danish, Dutch, English, Finnish, French, German, Hungarian, Italian, Kazakh, Lithuanian, Polish, Portuguese, Romanian, Russian, Slovenian, Spanish, Swedish, Turkish, Ukrainian, Vietnamese.

**Hyphenation dictionaries (9 languages)** — used by the EPUB renderer to insert soft hyphens at language-correct break points so justified text doesn't leave huge gaps.

English, French, German, Italian, Polish, Russian, Spanish, Swedish, Ukrainian.

**Bundled reader fonts** — Bitter, Charein, Lexend Deca — each in regular / bold / italic / bold-italic at four sizes (10, 12, 14, 16 pt). For other fonts, drop a `.cpfont` file in `/fonts` on the SD card and it shows up in the reader's font picker. Generate your .cpfont from .ttf font families at https://crosspointreader.com/fonts


---

## Lineage

```
CrossPoint Reader  →  CrossInk (uxjulia)  →  CrossInk Carousel (chintanvajariya)  →  CrumBLE
```

- **[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** — the further-upstream foundation. Most reader features (fonts, BookSettings, sleep screens, web UI) come from here.
- **[CrossInk](https://github.com/uxjulia/CrossInk)** — uxjulia's fork. UI polish, localization, additional reader fonts. CrumBLE is rebased onto CrossInk 1.3 as of v3.0.0.
- **[CrossInk Carousel](https://github.com/chintanvajariya/CrossInk-Carousel)** — chintanvajariya's fork. Adds the Flow theme (3D book carousel), 3×3 Recent Books grid, and the multi-book Reading Stats redesign.
- **CrumBLE** — this fork. Adds BLE, Collections, sleep-screen cycling, and the in-book quick-settings drawer.

---

## Install firmware

1. Download `crumble-firmware.bin` from the [Releases](https://github.com/imshentastic/CrumBLE/releases) page (includes Bluetooth page-turner support).
2. Connect your Xteink X4 / X3 via USB-C and wake / unlock the device.
3. Go to https://crosspointreader.com/#flash-tools, select your device, choose **Custom .bin**, pick the file you downloaded, and click **Flash**.

To revert to official Xteink firmware, flash the latest stock build from the same page.

---

## USB-locked devices

Some Xteink units sold through third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory. If your device is locked, you'll need the **Xteink Unlocker** at https://crosspointreader.com/#unlock-tool before you can flash CrumBLE.

**You do not need the unlocker if you bought directly from xteink.com** — those units aren't locked.

**Critical warning:** The unlocker officially supports only CrossPoint and CrossInk firmwares. Flashing a non-supported firmware on a USB-locked device can permanently brick it or leave it stuck on that firmware with no recovery path. **CrumBLE does support OTA**, but verify the boot logo appears after first flash before assuming you have an out. If in doubt, flash CrossInk first, confirm OTA works, then OTA-upgrade to CrumBLE.

---

## Build from source

```bash
# Tiny build (BLE-capable, default partition)
pio run -e tiny

# Flash to a connected device
pio run -e tiny -t upload

# Simulator (desktop, for UI iteration)
pio run -e simulator        # X4 panel (800x480)
pio run -e simulator_x3     # X3 panel (792x528)
.pio/build/simulator/program
```

See [docs/contributing/getting-started.md](./docs/contributing/getting-started.md) for the full development setup.

---

## License

Inherits the upstream MIT license. Third-party code attribution lives in [CHANGELOG.md](./CHANGELOG.md) per-feature.
