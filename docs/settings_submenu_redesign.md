# Settings Submenu Redesign — CrumBLE port from CrossInk 1.3.1

Goal: port the CrossInk 1.3.1 SUBMENU/nav-stack pattern into CrumBLE, then
redesign the menu tree so the growing settings list collapses into a navigable
hierarchy instead of four flat lists with section headers.

Phase 1 is this inventory: every settings row that currently exists, where it
lives, what it controls. No row gets dropped unless I explicitly mark it as
removed and justify why.

## Current top-level structure

`SettingsActivity` shows four flat lists you swipe between with the top
"category" tab. Inside `STR_CAT_CONTROLS` there are now section headers
(`SECTION_HEADER` rows) which were the predecessor to real submenus.

Categories: `Display | Reader | Controls | System` (in tab order).

Additionally:
- A few rows are persistence-only (category `STR_NONE_OPT`) — they exist in
  the settings registry for JSON I/O but the device UI hides them; their
  toggles live elsewhere.
- A few categories exist purely for the web settings API and surface on the
  device behind an `Action` row that opens a dedicated activity:
  `STR_KOREADER_SYNC`, `STR_CUSTOMISE_STATUS_BAR`.

---

## Display

Source: `src/SettingsList.h` `--- Display ---` block.

| # | StrId / key                         | Type    | Notes                                                              |
| - | ----------------------------------- | ------- | ------------------------------------------------------------------ |
| 1 | `STR_SLEEP_SCREEN` `sleepScreen`    | ENUM    | Dark / Light / Custom / Cover / None / CoverCustom / PageOverlay / ReadingStats / ThemeMinimal / QuickResume |
| 2 | `STR_SLEEP_COVER_MODE` `sleepScreenCoverMode` | ENUM | Fit / Crop                                                  |
| 3 | `STR_SLEEP_COVER_FILTER` `sleepScreenCoverFilter` | ENUM | None / Contrast / Inverted                              |
| 4 | `STR_CYCLE_SCREENSAVER_ON_TAP` `cycleScreensaverOnTap` | TOGGLE | "Cycle Sleep Screen" — off / short pwr / short up/dn / long up/dn (note: yaml may still be a 4-option enum) |
| 5 | `STR_SLEEP_CYCLE_SKIP_GRAYSCALE` `sleepCycleSkipGrayscale` | TOGGLE | Skip grayscale screens when cycling             |
| 6 | `STR_SLEEP_SCREEN_ORDER` `sleepScreenOrder` | ENUM | Random / Alphabetical                                          |
| 7 | `STR_QUICK_RESUME` `quickResumeSleepScreen` | ENUM | Off / After Timeout / Always   (CrumBLE bump from toggle)      |
| 8 | `STR_HIDE_BATTERY` `hideBatteryPercentage` | ENUM | Never / In Reader / Always                                      |
| 9 | `STR_REFRESH_FREQ` `refreshFrequency` | ENUM  | every 1 / 5 / 10 / 15 / 30 pages                                  |
|10 | `STR_UI_THEME` `uiTheme`            | ENUM    | Classic / Lyra / Lyra Extended / Flow / RoundedRaff / Minimal (+ Carousel if compiled in) |
|11 | `STR_RECENT_BOOKS_VIEW` `recentBooksView` | ENUM | List / Grid                                                  |
|12 | `STR_SUNLIGHT_FADING_FIX` `fadingFix` | TOGGLE | "Inverted refresh" anti-fade hack                                |

Hidden (persistence-only, category `STR_NONE_OPT`):
- `STR_BOOKSHELF_LAYOUT` `bookshelfLayout` — 3x3 / 4x4 / 2x2. Toggle lives on the bookshelf "Layout" row.
- `STR_BOOKSHELF_TITLE_PLACEMENT` `bookshelfTitlePlacement` — Bottom / Top. Toggle lives on bookshelf "Title Placement" row.

## Reader

Source: `src/SettingsList.h` `--- Reader ---` block, plus an injected
`ManageFonts` action and a trailing `CustomiseStatusBar` action.

| # | StrId / key                         | Type    | Notes                                                              |
| - | ----------------------------------- | ------- | ------------------------------------------------------------------ |
| 1 | `STR_FONT_FAMILY` `fontFamily`      | ENUM    | Built-in (Lexend Deca / Bitter / CharEink) + any SD-card families  |
| 2 | `STR_FONT_SIZE` `fontSize`          | ENUM    | Tiny / Small / Medium / Large / X-Large / Teensy / Huge / Itty-Bitty (subject to OMIT_*_FONT macros) |
|   | (Action) `STR_MANAGE_FONTS`         | ACTION  | `DownloadFonts` activity                                           |
| 3 | `STR_LINE_SPACING` `lineSpacing`    | ENUM    | Tight / Normal / Wide                                              |
| 4 | `STR_ORIENTATION` `orientation`     | ENUM    | Portrait / Landscape CW / Inverted / Landscape CCW                 |
| 5 | `STR_SCREEN_MARGIN` `screenMargin`  | VALUE   | 5..40 step 5                                                       |
| 6 | `STR_PARA_ALIGNMENT` `paragraphAlignment` | ENUM | Justify / Left / Center / Right / Book S Style                  |
| 7 | `STR_EMBEDDED_STYLE` `embeddedStyle` | TOGGLE | Use EPUB CSS                                                      |
| 8 | `STR_HYPHENATION` `hyphenationEnabled` | TOGGLE |                                                                |
| 9 | `STR_TEXT_AA` `textAntiAliasing`    | TOGGLE  |                                                                    |
|10 | `STR_IMAGES` `imageRendering`       | ENUM    | Display / Placeholder / Suppress                                   |
|11 | `STR_EXTRA_SPACING` `extraParagraphSpacing` | TOGGLE |                                                            |
|12 | `STR_FORCE_PARAGRAPH_INDENTS` `forceParagraphIndents` | TOGGLE |                                                  |
|13 | `STR_BIONIC_READING` `bionicReadingEnabled` | TOGGLE |                                                            |
|14 | `STR_GUIDE_READING` `guideReadingEnabled` | TOGGLE |                                                              |
|   | (Action) `STR_CUSTOMISE_STATUS_BAR` | ACTION  | Opens StatusBar sub-activity (see below)                           |

## Controls

Source: `src/activities/settings/SettingsActivity.cpp` `controlsSettings`
push order (lines ~172-189). Uses `SECTION_HEADER` rows.

### Power Button (section header)
| # | StrId / key                | Type | Notes                                                                |
| - | -------------------------- | ---- | -------------------------------------------------------------------- |
| 1 | `STR_SHORT_PWR_BTN` `shortPwrBtn` | ENUM | 14-option list (+ TiltPageTurn on x3) — sleep / page turn / refresh / change font / toggle guide / bionic / bookmark / sync / mark finished / reading stats / screenshot / cycle page turn / file transfer / ignore |
| 2 | `STR_LONG_PRESS_ACTION` `longPwrBtn` | ENUM | Same list as short power                                       |

### Front Buttons (section header)
| # | StrId / key                | Type   | Notes                                                              |
| - | -------------------------- | ------ | ------------------------------------------------------------------ |
| 3 | `STR_REMAP_FRONT_BUTTONS`  | ACTION | Opens remap UI                                                     |
| 4 | `STR_REMAP_FRONT_BUTTONS_READER` | ACTION | Opens reader-mode remap UI                                   |
| 5 | `STR_ORIENTATION_AWARE` `frontButtonOrientationAware` | ENUM | No / Nav buttons only / All buttons      |
| 6 | `STR_LONG_PRESS_BEHAVIOR` `longPressButtonBehavior` | ENUM | Off / Skip / Orientation                  |
| 7 | `STR_LONG_PRESS_MENU_ACTION` `longPressMenuAction` | ENUM | 14-option list incl. "Book Settings"        |

### Side Buttons (section header)
| # | StrId / key                | Type | Notes                                                                |
| - | -------------------------- | ---- | -------------------------------------------------------------------- |
| 8 | `STR_SIDE_BTN_LAYOUT` `sideButtonLayout` | ENUM | Prev/Next vs Next/Prev                                       |
| 9 | `STR_ORIENTATION_AWARE` `sideButtonOrientationAware` | ENUM | No / Yes                                          |
|10 | `STR_SIDE_BTN_LONG_PRESS` `sideButtonLongPress` | ENUM | Chapter Skip / Change Font Size / Ignore / Orientation       |

### Other (section header, x3 only — only if QMI8658 IMU present)
|11 | `STR_TILT_PAGE_TURN` `tiltPageTurn` | ENUM | Off / Normal / Inverted                                          |

## System

Source: `src/SettingsList.h` `--- System ---` block, plus injected device-only
ACTION rows.

| # | StrId / key                            | Type    | Notes                                                  |
| - | -------------------------------------- | ------- | ------------------------------------------------------ |
| 1 | `STR_TIME_TO_SLEEP` `sleepTimeoutMinutes` | VALUE | min..max minutes                                       |
| 2 | `STR_SHOW_HIDDEN_FILES` `showHiddenFiles` | TOGGLE |                                                     |
| 3 | `STR_REMOVE_READ_FROM_RECENTS` `removeReadBooksFromRecents` | TOGGLE |                                  |
| 4 | `STR_MOVE_FINISHED_TO_READ` `moveFinishedToReadFolder` | TOGGLE |                                          |
| 5 | `STR_SERIES_DETECTION` `seriesDetectionEnabled` | TOGGLE | CrumBLE opt-in                                |
| 6 | `STR_OPTIMIZE_CHAPTER_INDEXING` `optimizeChapterIndexing` | TOGGLE | CrumBLE prebake master switch         |
|   | (Action) `STR_WIFI_NETWORKS`           | ACTION  |                                                        |
|   | (Action) `STR_KOREADER_SYNC`           | ACTION  | Opens sub-activity (see below)                         |
|   | (Action) `STR_OPDS_SERVERS`            | ACTION  |                                                        |
|   | (Action) `STR_CLEAR_READING_CACHE`     | ACTION  |                                                        |
|   | (Action) `STR_CHECK_UPDATES`           | ACTION  |                                                        |
|   | (Action) `STR_SD_FIRMWARE_UPDATE`      | ACTION  |                                                        |
|   | (Action) `STR_LANGUAGE`                | ACTION  |                                                        |

## KOReader Sync (sub-activity)

Settings live in the global list under category `STR_KOREADER_SYNC`. The
device opens these via the "KOReader Sync" action in System. Web settings
exposes them directly.

| # | StrId / key                       | Type           | Notes                              |
| - | --------------------------------- | -------------- | ---------------------------------- |
| 1 | `STR_KOREADER_USERNAME` `koUsername` | DYNAMIC_STRING | KOReaderCredentialStore-backed   |
| 2 | `STR_KOREADER_PASSWORD` `koPassword` | DYNAMIC_STRING |                                  |
| 3 | `STR_SYNC_SERVER_URL` `koServerUrl` | DYNAMIC_STRING |                                   |
| 4 | `STR_DOCUMENT_MATCHING` `koMatchMethod` | DYNAMIC_ENUM | Filename / Binary                |

## Customise Status Bar (sub-activity)

Settings live in the global list under category `STR_CUSTOMISE_STATUS_BAR`.
Reader category surfaces a "Customise Status Bar" action that opens this.

| # | StrId / key                                    | Type    | Notes                                                |
| - | ---------------------------------------------- | ------- | ---------------------------------------------------- |
| 1 | `STR_CHAPTER_PAGE_COUNT` `statusBarChapterPageCount` | TOGGLE |                                                 |
| 2 | `STR_BOOK_PROGRESS_PERCENTAGE` `statusBarBookProgressPercentage` | TOGGLE |                                     |
| 3 | `STR_PROGRESS_BAR` `statusBarProgressBar`      | ENUM    | Book / Chapter / Hide                                |
| 4 | `STR_PROGRESS_BAR_THICKNESS` `statusBarProgressBarThickness` | ENUM | Thin / Medium / Thick                  |
| 5 | `STR_TITLE` `statusBarTitle`                   | ENUM    | Book / Chapter / Hide                                |
| 6 | `STR_BATTERY` `statusBarBattery`               | TOGGLE  |                                                      |
| 7 | `STR_XTC_STATUS_BAR` `xtcStatusBarMode`        | ENUM    | Hide / Bottom / Top                                  |
| 8 | `STR_CLOCK` `statusBarClock`                   | TOGGLE  |                                                      |
| 9 | `STR_CLOCK_UTC_OFFSET` `clockUtcOffsetQ`       | VALUE   | quarter-hour steps, 0..104 biased -48                |
|10 | `STR_CLOCK_FORMAT` `clockFormat`               | ENUM    | 24h / 12h                                            |
|11 | `STR_CLOCK_SYNCED` `clockHasBeenSynced`        | TOGGLE  | NTP debounce reset toggle                            |

---

## Counts at a glance

- Display: 12 visible rows + 2 hidden persistence-only
- Reader: 14 settings + 2 actions (ManageFonts, CustomiseStatusBar)
- Controls: 10-11 rows + 3 section headers (+1 tilt row on x3)
- System: 6 settings + 7 actions (Wi-Fi, KOReader, OPDS, Cache, Updates, SD FW, Language)
- KOReader Sync sub-activity: 4 rows
- Status Bar sub-activity: 11 rows

Total user-facing rows on device: roughly **60-62** (excluding section-header rows).

---

## Final submenu tree (locked)

Top level is a list of six submenus, not the four-tab layout. Each entry opens
its own list activity. Sub-groups within get one more nesting level where
natural.

Locked decisions from review:
- Library is a new top-level (pulls library-management rows out of System).
- Sync & Network is a new top-level (Wi-Fi + KOReader + OPDS coherent triad).
- Status Bar customisation stays under Reader (minimises behavioural change vs the current "Customise Status Bar" action position).
- Reader splits into four sub-groups (Font / Layout / Style / Reading Aids) rather than a flatter two-group layout.

```
Settings
├── Display ▸
│   ├── Sleep Screen ▸          (sleep screen mode, cover Fit/Crop, cover filter, cycle behavior, skip grayscale, order, quick resume)
│   ├── Theme & Layout ▸        (UI theme, Recent Books view)
│   └── General ▸               (Hide Battery, Refresh Frequency, Sunlight Fading Fix)
├── Reader ▸
│   ├── Font ▸                  (Font Family, Font Size, Manage Fonts action)
│   ├── Layout ▸                (Line Spacing, Orientation, Screen Margin, Paragraph Alignment, Extra Spacing, Force Indents)
│   ├── Style ▸                 (Embedded Style, Hyphenation, Text AA, Images)
│   ├── Reading Aids ▸          (Bionic Reading, Guide Reading)
│   └── Customise Status Bar ▸  (all 11 status-bar rows from the existing sub-activity)
├── Controls ▸
│   ├── Power Button ▸          (Short Power, Long Press Power)
│   ├── Front Buttons ▸         (Remap, Remap (Reader), Orientation Aware, Long Press Behavior, Long Press Menu Action)
│   ├── Side Buttons ▸          (Layout, Orientation Aware, Long Press)
│   └── Tilt (x3 only) ▸        (Tilt Page Turn — surfaced only when QMI8658 IMU is present, matching current behaviour)
├── Library ▸
│   ├── Files ▸                 (Show Hidden Files, Remove Read from Recents, Move Finished to /read)
│   ├── Series Detection        (toggle)
│   └── Optimize Chapter Indexing (toggle)
├── Sync & Network ▸
│   ├── Wi-Fi Networks          (action)
│   ├── KOReader Sync ▸         (sub-activity: 4 rows)
│   └── OPDS Servers            (action)
└── System ▸
    ├── Sleep Timeout           (value)
    ├── Language                (action)
    ├── Check for Updates       (action)
    ├── SD Firmware Update      (action)
    └── Clear Reading Cache     (action)
```

Sub-groups of one row (Library > Series Detection, Library > Optimize Chapter Indexing, etc.) are rendered as plain leaf rows, not as submenus — only multi-row groups get the `▸` chevron.

---

## Implementation phasing

1. **Port `SettingType::SUBMENU` + nav-stack from CrossInk 1.3.1** into `SettingInfo` / `SettingsActivity`. Stay backward compatible with the existing flat `currentSettings` pointer pattern; a submenu is just a list with a back-pointer.
2. **Replace the four-tab top-level** with a list of submenu rows. Each tab becomes one top-level submenu.
3. **Reshape each category** per the tree above. Migrate `SECTION_HEADER` rows into proper submenus where they make sense; keep them inline only where the parent submenu is short.
4. **Verify web settings API unchanged.** `SettingsList.h` still produces the flat registered list; only the device-side grouping changes. Categories on the web side (`STR_CAT_*`) keep working as-is — they group rows in the JSON UI, not on device.
5. **Audit translations.** Confirm every category/sub-category string ID exists, add any new ones to `english.yaml`.

---

## Removed / merged rows tracking

Nothing removed yet. If anything is intentionally dropped during the redesign,
log it here with a reason.

| Row | Reason | Replacement |
| --- | ------ | ----------- |
| _(none yet)_ | | |
