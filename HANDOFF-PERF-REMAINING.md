# Handoff: remaining navigation-performance work

_Continues the perf pass on `feat/dict-casper`. Items 1-5 are done and committed
there; what follows is measured, not guessed._

## Why this exists

Users report CrumBLE navigation and button response feel slow. Instrumentation
added in `c7cb7113` (`PERF` log tag) turned that into numbers on a real X4.

Headline: a navigation "loading moment" is a **deliberate reboot**. When heap is
fragmented, entering Settings or exiting to Home silent-restarts to defragment,
so the pause the user feels is a full boot.

Measured on the user's X4 (43 books, 35 sleep images):

```
boot->ready 6887 ms (silentRestart=1 target=0 lean=0)   <- Home-target restart
boot->ready 3300 ms (silentRestart=0 ...)               <- cold boot
```

Breakdown of the 6.9 s, from log timestamps:

| Stage | Cost |
|---|---|
| init -> CollectionsStore loaded | ~0.75 s |
| e-ink refresh | 1.71 s |
| `/sleep` bake scan (35 images) | ~1.8 s -> **FIXED** in `5f467975` |
| library SD walk (43 files) | ~1.5 s -> **ITEM A below** |
| shelf render (`shelf=1277`) | 1.28 s -> **ITEM B below** |
| final panel | 0.57 s |

Other measurements worth keeping:
- `onEnter Home` 1197-1744 ms, `RecentBooksGrid` 1392 ms, `BookStats` 1943 ms,
  `Sleep` 2718 ms.
- `rebuildSettingsLists cost: free 73452->54256 (-19196)` — the Settings entry
  peak, i.e. what trips the restart gate in the first place.
- Steady-state panel time is ~570 ms per fast refresh, ~1800 ms full. That is
  e-ink physics, not our code — do not chase it.

## Already done (do not redo)

| Commit | Change |
|---|---|
| `c7cb7113` | BT auto-reconnect backoff (was freezing the loop 2-3 s per button press when a bonded remote is off) + `PERF` instrumentation |
| `9ca01478` | Settings added to `isLibraryLightBoot` — Settings restarts skip the library load. Also made Rebuild Author Keys call the idempotent `begin()` first, or it would clear an empty index and save it over the user's real one |
| `5f467975` | Sleep-bake scan verdict cache (the ~1.8 s win) |
| `13cd9a96` | Lost-setting fix: lend the framebuffer to the settings write on silent restart instead of dropping the change |

A separate session is already rewriting the settings JSON write to stream
(removing the 20 KB peak). Do not duplicate that.

---

## ITEM A — library SD walk on every restart (~1.5 s)

`LibraryIndex::ensureWalked()` (`src/LibraryIndex.cpp:240`) is gated by
`walkPerformed`, a **plain RAM bool**. It resets on every reboot, so each silent
restart re-walks the card even though the walk it just did is still valid.
Triggered indirectly via `CollectionsStore.cpp:320`.

Intended fix: persist "already walked" across a *silent restart only* (RTC-backed,
same pattern as the other silent-restart state in `SilentRestart.h`), honoured
only when `isContinuingFromSilentReboot()` is true. A cold boot or power-cycle
must always walk.

**The danger — audit before writing code.** The walk is how new books are
discovered. File Transfer adds books AND is itself a silent-restart target, so a
naive flag makes newly uploaded books invisible until a power cycle. That is a
much worse bug than 1.5 s of boot. Enumerate every path that can change the book
set (FT upload/delete, web file manager, deferred delete in `main.cpp`, SD swap)
and clear the flag on each. If that set cannot be enumerated confidently, prefer
a cheap directory signature over a flag.

## ITEM B — first shelf paint is 3-4x slower than later ones

`shelf=1277` on the restart boot versus `shelf=286..443` on subsequent renders
(see `RPROF slow-render stages` lines). The first paint does cover work the later
ones skip. This is where Duet's "input-first shelves" idea (task #171) applies:
paint placeholders first, hydrate covers at yield points so navigation stays
responsive. Compare against our existing v316 shelf-covers fast-path gate and the
v209 `CoverTiles` pre-baked tiles before adding anything new.

## ITEM C — shrink the Settings entry peak (fires fewer restarts)

`rebuildSettingsLists` costs ~19 KB, and `SettingsActivity::onEnter`'s pre-flight
(`src/activities/settings/SettingsActivity.cpp:486`) restarts when free < 45 KB or
maxAlloc < 30 KB. Main already halved this once by moving rows out of
`allSettings` instead of copying (see the v4.7.3 comment at
`SettingsActivity.cpp:171`). Remaining ideas: build only the submenu being shown
rather than the whole tree; avoid materialising the flat list twice. Every KB
here removes restarts entirely, which beats making restarts faster.

## ITEM D — refresh-mode audit (HOLD)

Was item 5 of the original plan. **Do not start** while the
`fix/display-grayscale-baseline` session is live — it is fixing a grayscale
baseline regression on the same panel path, and two threads changing refresh
behaviour at once makes both un-diagnosable. Revisit after that lands.

---

## Bugs the same log surfaced (separate from perf)

1. `[BMC] getSpineCumulativeSize index 8507 out of range` (twice) — bogus spine
   index while opening the Recent Books grid.
2. `[RBGA] OOM: grid snapshot (48000 bytes)` plus `NoIndex gen failed` /
   `Heavy gen also failed` cover generation at ~54 KB free — the grid asks for a
   full 48 KB framebuffer snapshot it cannot get.
3. Device rebooted at checkpoint `cps:serialize` — same crash class the 4.6.1
   floor was raised for; still reachable. The streaming-JSON work may remove it.

## Ground rules

- Build `pio run -e tiny-bitter`; stay under 6,553,600 bytes (SD-flash promise).
- Copy builds to `~/Downloads/crumble-<ver>-<tag>.bin`.
- Re-measure with the `PERF` lines after each change — that is the point.
- No emojis, no AI attribution in code/commits/PRs.
