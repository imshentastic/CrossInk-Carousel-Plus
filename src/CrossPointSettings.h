#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <iosfwd>

class CrossPointSettings {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    OVERLAY = 6,
    READING_STATS_SLEEP = 7,
    MINIMAL_SLEEP = 8,
    QUICK_RESUME = 9,
    // v18.9.9.445 (CrossInk parity): MINIMAL_SLEEP + reader-type label
    // + streak text overlay. Requires a valid clock; on X4 we honor
    // ReadingStats::isClockValid() (post-SNTP) rather than X3-only.
    MINIMAL_STATS_SLEEP = 10,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  // Selection order shared by both the on-enter Custom-mode fallback and the
  // deep-sleep tap-to-cycle path. RANDOM = 0 preserves the long-standing
  // tap-to-cycle default for upgrading users; ALPHABETICAL walks /.sleep/
  // in sorted order using a persisted cursor (sleepScreenCycleIndex).
  enum SLEEP_SCREEN_ORDER : uint8_t {
    SLEEP_ORDER_RANDOM = 0,
    SLEEP_ORDER_ALPHABETICAL = 1,
    SLEEP_SCREEN_ORDER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Previous, Next
  // Swapped: Next, Previous
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTON_LAYOUT_COUNT };

  enum FRONT_BUTTON_ORIENTATION_AWARE {
    FRONT_ORIENTATION_AWARE_OFF = 0,
    FRONT_ORIENTATION_AWARE_NAV_BUTTONS = 1,
    FRONT_ORIENTATION_AWARE_ALL_BUTTONS = 2,
    FRONT_ORIENTATION_AWARE_COUNT
  };

  // Side button long-press action options
  enum SIDE_LONG_PRESS {
    SIDE_LONG_CHAPTER_SKIP = 0,
    SIDE_LONG_FONT_SIZE = 1,
    SIDE_LONG_OFF = 2,
    SIDE_LONG_ORIENTATION_CHANGE = 3,
    SIDE_LONG_PRESS_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { LEXENDDECA = 0, BITTER = 1, CHAREINK = 2, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;

  // CrumBLE 4.2.1: per-variant default built-in family. Exactly one of the
  // three families is compiled in by the tiny-{bitter,lexend,chareink}
  // variants; this constant routes fallbacks (stale-preference clamp,
  // getFallbackReaderFontIdForFamily, getReaderFontId collapse) to whichever
  // one IS available. Resolution order: BITTER > LEXENDDECA > CHAREINK
  // (matches v4.2.0 behaviour, which always landed on BITTER as the universal
  // fallback). A compile-time error fires if all three families are omitted.
#if !defined(OMIT_BITTER_FONT)
  static constexpr FONT_FAMILY BUILTIN_DEFAULT_FONT_FAMILY = BITTER;
#elif !defined(OMIT_LEXENDDECA_FONT)
  static constexpr FONT_FAMILY BUILTIN_DEFAULT_FONT_FAMILY = LEXENDDECA;
#elif !defined(OMIT_CHAREINK_FONT)
  static constexpr FONT_FAMILY BUILTIN_DEFAULT_FONT_FAMILY = CHAREINK;
#else
#error "All built-in reader font families omitted (OMIT_BITTER_FONT + OMIT_LEXENDDECA_FONT + OMIT_CHAREINK_FONT). At least one must be compiled in."
#endif
  // Font size options
  enum FONT_SIZE {
    TINY = 0,
    SMALL = 1,
    MEDIUM = 2,
    LARGE = 3,
    EXTRA_LARGE = 4,
    TEENSY = 5,
    HUGE_SIZE = 6,
    ITTY_BITTY = 7,
    FONT_SIZE_COUNT
  };
  enum SD_FONT_SIZE_RANGE {
    SD_FONT_RANGE_TEENSY = 0,
    SD_FONT_RANGE_TINY = 1,
    SD_FONT_RANGE_XLARGE = 2,
    SD_FONT_RANGE_NO_EMOJI = 3,
    SD_FONT_RANGE_ALL = 4,
    SD_FONT_SIZE_RANGE_COUNT
  };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  // CrumBLE 4.4 (ported from CPR-vCodex): Text Darkness setting. Affects the
  // 2-bit grayscale glyph blit -- maps the font's per-pixel AA value (0..3)
  // to which framebuffer plane (MSB, LSB) gets ink. Doesn't touch the 1-bit
  // BW path. Stored as uint8_t (textDarkness) so layouts stay compact.
  //   NORMAL    : CrossInk-style solid text with smooth AA (current behaviour)
  //   LEGACY_BW : Lighter overlay, the pre-CrumBLE 4.4 look
  //   DARK      : Both AA buckets get inked, denser glyphs
  //   EXTRA_DARK: Same as DARK in the current renderer (reserved for future tuning)
  enum TEXT_DARKNESS {
    TEXT_DARKNESS_NORMAL = 0,
    TEXT_DARKNESS_LEGACY_BW = 1,
    TEXT_DARKNESS_DARK = 2,
    TEXT_DARKNESS_EXTRA_DARK = 3,
    TEXT_DARKNESS_COUNT
  };
  // Spacing *between* paragraphs. Three-way enum (the byte field
  // `extraParagraphSpacing` carries 0/1/2). TIGHT keeps the classic-novel
  // text-indent style with no vertical gap; NORMAL is the default block-style
  // paragraph with a lineHeight/2 gap; WIDE doubles that to a full lineHeight.
  // The byte format is unchanged from the legacy bool, so old config files
  // and old section caches with 0/1 round-trip identically.
  enum EXTRA_PARAGRAPH_SPACING { EPS_TIGHT = 0, EPS_NORMAL = 1, EPS_WIDE = 2, EXTRA_PARAGRAPH_SPACING_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_3_MIN = 5,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    TOGGLE_FONT = 4,
    TOGGLE_GUIDE_DOTS = 5,
    TOGGLE_BIONIC_READING = 6,
    TOGGLE_BOOKMARK = 7,
    SYNC_PROGRESS = 8,
    MARK_FINISHED = 9,
    READING_STATS = 10,
    SCREENSHOT = 11,
    CYCLE_PAGE_TURN = 12,
    FILE_TRANSFER = 13,
    TOGGLE_TILT_PAGE_TURN = 14,
    SHORT_PWRBTN_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme. CrumBLE keeps LYRA_FLOW (the Flow carousel) at slot 3 so saved
  // user settings stay valid; ROUNDEDRAFF/LYRA_CAROUSEL/MINIMAL are shifted up
  // by one. Raw values are persisted in settings, so keep these stable.
  enum UI_THEME {
    CLASSIC = 0,
    LYRA = 1,
    LYRA_3_COVERS = 2,
    LYRA_FLOW = 3,
    ROUNDEDRAFF = 4,
    LYRA_CAROUSEL = 5,
    MINIMAL = 6,
    // v18.9.9.461 (CrossInk parity): Dashboard theme. Home + sleep both
    // show cover(s) alongside a stats grid. MVP inherits Minimal's home
    // layout with a distinct dashboard-styled sleep screen; P3b iterates
    // to match upstream's cover-pair + stats-grid composition.
    DASHBOARD = 7,
    UI_THEME_COUNT = 8
  };
  enum RECENT_BOOKS_VIEW { RECENT_BOOKS_LIST = 0, RECENT_BOOKS_GRID = 1, RECENT_BOOKS_VIEW_COUNT };

  // CrumBLE #133: Bookshelf grid layout choice. 4x4 (default) shares
  // the 100x150 cover thumbs with the Flow shelf; 3x3 uses the legacy
  // 123x180 cells from before the Flow-shelf unification (the user
  // preferred that look at 9-cell density); 2x2 uses 220x320 (carousel
  // center cover size) for max cache reuse with carousel + Reading
  // Stats. Toggleable from the BookshelfPicker.
  enum BOOKSHELF_LAYOUT { BOOKSHELF_LAYOUT_3X3 = 0, BOOKSHELF_LAYOUT_4X4 = 1, BOOKSHELF_LAYOUT_2X2 = 2,
                          BOOKSHELF_LAYOUT_COUNT };

  // CrumBLE #133 follow-up: where the selected-book label strip
  // (title / author / read+remaining times) sits relative to the grid.
  // BOTTOM puts it below the books with page dots just above it;
  // TOP swaps it above the books and moves the page dots to the
  // screen bottom. Toggleable from the BookshelfPicker.
  enum BOOKSHELF_TITLE_PLACEMENT { BOOKSHELF_TITLE_PLACEMENT_BOTTOM = 0, BOOKSHELF_TITLE_PLACEMENT_TOP = 1,
                                   BOOKSHELF_TITLE_PLACEMENT_COUNT };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // v18.9.9.24: user-facing tables toggle. TABLES_DISPLAY renders tables as
  // tables (borders + cell layout, uses PageTableFragment elements).
  // TABLES_PARAGRAPHS collapses cell content into paragraph runs at parse
  // time (same path Simple Rendering / Compat mode uses). Useful preemptive
  // memory saver for heavy books before Compat mode auto-engages.
  enum TABLE_RENDERING { TABLES_DISPLAY = 0, TABLES_PARAGRAPHS = 1, TABLE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2, TILT_PAGE_TURN_COUNT };

  // Long-press Confirm (menu button) quick action in reader
  enum LONG_PRESS_MENU_ACTION {
    LONG_MENU_OFF = 0,
    LONG_MENU_SLEEP = 1,
    LONG_MENU_CHANGE_FONT = 2,
    LONG_MENU_TOGGLE_GUIDE_DOTS = 3,
    LONG_MENU_TOGGLE_BIONIC = 4,
    LONG_MENU_TOGGLE_BOOKMARK = 5,
    LONG_MENU_REFRESH_SCREEN = 6,
    LONG_MENU_SYNC_PROGRESS = 7,
    LONG_MENU_MARK_FINISHED = 8,
    LONG_MENU_READING_STATS = 9,
    LONG_MENU_SCREENSHOT = 10,
    LONG_MENU_CYCLE_PAGE_TURN = 11,
    LONG_MENU_FILE_TRANSFER = 12,
    LONG_MENU_BOOK_SETTINGS = 13,
    LONG_MENU_TOGGLE_TILT_PAGE_TURN = 14,
    LONG_MENU_TOGGLE_DARK_MODE = 15,
    LONG_PRESS_MENU_ACTION_COUNT
  };

  // Clipping storage mode
  enum CLIPPING_STORAGE : uint8_t { SINGLE_FILE = 0, PER_BOOK = 1, CLIPPING_STORAGE_COUNT };
  // Clip selector navigation scheme
  enum CLIP_NAV_MODE : uint8_t { LINE_AWARE = 0, WORD_BY_WORD = 1, CLIP_NAV_MODE_COUNT };
  // Annotation underline visibility
  enum ANNOTATION_VISIBILITY : uint8_t { ANNOT_VISIBLE = 0, ANNOT_HIDDEN = 1, ANNOTATION_VISIBILITY_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    // CrumBLE: fast wake regardless of sleep trigger (auto-timeout OR
    // manual power-button). Pairs with sleepScreen = CUSTOM (or any
    // other image-rendering mode) so the user sees their chosen sleep
    // image on the way in AND gets a near-instant wake -- the framebuffer
    // is saved at sleep time and restored on wake via the same
    // BootResume::QuickResume path used by sleepScreen=QUICK_RESUME.
    QUICK_RESUME_ALWAYS = 2,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // While asleep, a brief tap on the power button cycles to a new random
  // image from /.sleep instead of waking the device. On by default in
  // CrumBLE — the cycling sleep screen is one of our headline features
  // and the battery cost (one boot + e-ink half-refresh per cycle) is
  // small enough that opt-out is the right default for new users.
  uint8_t cycleScreensaverOnTap = 1;
  // Persisted cursor for the SLEEP_ORDER_ALPHABETICAL fallback. Advances on
  // each cycle and is taken modulo the current image count, so adds/removes
  // degrade gracefully without needing a reset. Unused when order=Random.
  uint16_t sleepScreenCycleIndex = 0;
  // Selection order applied when the Custom sleep mode falls back to the
  // /.sleep/ rotation (no pinned image, or pinned image missing) and to the
  // deep-sleep tap-to-cycle path. Default RANDOM preserves prior behavior.
  uint8_t sleepScreenOrder = SLEEP_ORDER_RANDOM;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in in-book status bar (X3 has DS3231 RTC and works
  // out of the box; X4 needs an NTP sync each boot).
  uint8_t statusBarClock = 0;
  // v18.9.9.463 (CrossInk parity): show estimated time-left in status bar,
  // computed from stats.avgSecondsPerForwardPage × pagesRemaining. Renders
  // alongside the page-count / progress-percent field when enabled.
  // Default off — needs pace samples to be meaningful, users opt in.
  uint8_t statusBarTimeLeft = 0;
  // v18.9.9.343: separate toggle for the Home header clock. Split from
  // statusBarClock so users can have the in-book clock without paying
  // Home's boot-time NTP sync silent-restart cost (X4 only) -- clock
  // in-book is X3-only anyway; clock on Home matters on both devices.
  uint8_t homeClockShow = 0;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t forceParagraphIndents = 0;
  uint8_t textAntiAliasing = 1;
  // v18.9.9.405: opt-in single-pass page turn. Default OFF preserves the
  // current fast-then-fill two-stage paint (blacks appear quickly, greys
  // fill in ~500 ms later). When ON, the reader skips the initial BW-only
  // display on text pages that are going to get grayscale AA -- the page
  // only appears once the grayscale render is ready, so users see the full
  // AA'd page in one wave with no "black text jumping around then greys
  // settle in" effect. Trade: ~300 ms longer time to first paint.
  uint8_t singlePassPageTurn = 0;
  // Short power button action behaviour
  uint8_t shortPwrBtn = IGNORE;
  // Long power button action behaviour
  uint8_t longPwrBtn = SLEEP;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonOrientationAware = FRONT_ORIENTATION_AWARE_OFF;
  uint8_t sideButtonOrientationAware = 0;
  // Action performed when side buttons are long-pressed in reader
  uint8_t sideButtonLongPress = SIDE_LONG_CHAPTER_SKIP;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader-specific front button remap (overrides system mapping while in reader activities).
  // readerFrontButtonsEnabled = 0 means the reader uses the system mapping above.
  uint8_t readerFrontButtonsEnabled = 0;
  uint8_t readerFrontButtonBack = FRONT_HW_BACK;
  uint8_t readerFrontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t readerFrontButtonLeft = FRONT_HW_LEFT;
  uint8_t readerFrontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings. CrumBLE: when Lexend Deca is OMIT'd from the
  // slim build, default to whichever family is compiled in
  // (BUILTIN_DEFAULT_FONT_FAMILY resolves at compile time: BITTER on
  // tiny-bitter, LEXENDDECA on tiny-lexend, CHAREINK on tiny-chareink).
#ifdef OMIT_LEXENDDECA_FONT
  uint8_t fontFamily = BUILTIN_DEFAULT_FONT_FAMILY;
#else
  uint8_t fontFamily = LEXENDDECA;
#endif
  uint8_t fontSize = MEDIUM;
#if defined(OMIT_EMOJI_FONTS)
  uint8_t sdFontSizeRange = SD_FONT_RANGE_NO_EMOJI;
#elif defined(OMIT_TINY_FONT) && defined(OMIT_SMALL_FONT)
  uint8_t sdFontSizeRange = SD_FONT_RANGE_XLARGE;
#elif defined(OMIT_MEDIUM_FONT) && defined(OMIT_LARGE_FONT) && defined(OMIT_XLARGE_FONT) && defined(OMIT_HUGE_FONT)
  uint8_t sdFontSizeRange = SD_FONT_RANGE_TEENSY;
#else
  uint8_t sdFontSizeRange = SD_FONT_RANGE_TINY;
#endif
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // CrumBLE 4.4: ported from upstream CrossInk v1.3.2. Selective reader-page
  // inversion -- black page background with white text/UI, but EPUB content
  // images render right-side up (no inverted-photo weirdness). Reader-only;
  // menus, file browser, and the sleep screen remain in light mode.
  uint8_t readerDarkMode = 0;
  // CrumBLE 4.4 (ported from CPR-vCodex): Text Darkness, 0=Normal, 1=Legacy
  // BW, 2=Dark, 3=Extra Dark. Sync to GfxRenderer.setTextDarkness() at boot
  // and whenever this field is edited so the next glyph blit uses it.
  uint8_t textDarkness = TEXT_DARKNESS_NORMAL;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // v18.9.4: device-side BT auto-disconnect timeout in minutes. When BT has
  // been idle (no HID input from any connected device) for this long AND BT
  // is currently enabled AND at least one device is connected, disable BT
  // to release ~58 KB of heap. Only device-side -- can't override the
  // remote's own idle power-off. v18.9.5.1: mirror the Time to Sleep UI
  // exactly (range 1-30, step 1, default 10) -- a 0=Never option confused
  // the slider display and the "never" case is close enough to 30 min for
  // most users.
  uint8_t btAutoDisconnectMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // UI Theme. CrumBLE defaults to LYRA_FLOW (3D-perspective book carousel
  // from CrossInk Carousel) — it's the visual centerpiece of the fork
  // and what most users will want first. Anyone preferring the simpler
  // Lyra list can switch in Settings -> Display -> UI Theme.
  uint8_t uiTheme = LYRA_FLOW;
  // Recent Books screen layout
  uint8_t recentBooksView = RECENT_BOOKS_LIST;
  // CrumBLE #133: Bookshelf grid layout choice. Toggled from the
  // BookshelfPicker's "Layout" row. 4x4 is the default -- it shows the
  // most books per page (16) and uses the same 100x150 cell size as
  // the Flow shelf, so the four shelf books are immediate cache hits
  // when the user transitions Home -> Bookshelf.
  uint8_t bookshelfLayout = BOOKSHELF_LAYOUT_4X4;
  // CrumBLE #133 follow-up: selected-book label strip placement (top
  // / bottom of the grid). Bottom is the historical layout.
  uint8_t bookshelfTitlePlacement = BOOKSHELF_TITLE_PLACEMENT_BOTTOM;
  // CrumBLE: the index-backed virtual collections (Recently Added / All Books)
  // are opt-in so a fresh device never runs the whole-SD walk at boot. 0 =
  // hidden from Home. Existing users (who already have a library index) are
  // migrated to 1 on the first boot after this update; fresh installs stay 0.
  uint8_t showRecentlyAddedCollection = 0;
  uint8_t showAllBooksCollection = 0;
  // CrumBLE: opt-in toggles for the completion-derived virtual collections.
  // Finished = books marked complete. New = books that exist on the SD card
  // but have never been opened (sessionCount == 0). Both are hidden by
  // default; turning one ON triggers the library walk plus a per-book
  // BookReadingStats read, which CollectionsStore caches in-memory.
  uint8_t showFinishedCollection = 0;
  uint8_t showNewCollection = 0;
  // Transient (NOT persisted): true until loadSettings() sees either of the two
  // keys above in the JSON. main.cpp uses it to apply the one-time existing-user
  // migration default (index present -> show them), then clears it.
  bool virtualCollectionsDefaultPending = true;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // CrumBLE 4.6: cover thumbnail tone curve. Applied between the grayscale
  // conversion and dither when the device regenerates a cover thumb (i.e.,
  // any cover not pre-baked off-device). E-ink panels render source
  // midtones much darker than LCD/OLED gamma assumes; lifting them via a
  // curve makes the cover noticeably brighter without losing detail.
  //   0 = Off (identity; back-compat default)
  //   1 = Mild (gamma 1.5 -- gentle midtone lift)
  //   2 = Strong (85..200 stretch + sigmoid contrast -- aggressive)
  // See lib/ToneCurve for the exact curves.
  uint8_t coverToneCurve = 0;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t bionicReadingEnabled = 0;
  // Guide Dots - places a middle dot between words to guide the eye
  uint8_t guideReadingEnabled = 0;
  // v18.9.9.78: Stable Page Numbers (KOReader-style, CrossInk-parity). When on,
  // status bar shows "Stable page X of Y" derived from byte position instead of
  // section-local "Page N of M". Divisor is chars-per-page approximation --
  // default 1500 matches CrossInk / KOReader (~230-300 words). Character
  // approximation via UTF-8 byte count is exact for ASCII, drifts ~50-60% for
  // dense CJK; acceptable for a "stable page number" whose point is a stable
  // frame of reference, not literary precision.
  uint8_t showStablePageNumbers = 0;
  uint16_t stablePageChars = 1500;
  // Glyph atlas (v40 section format): when enabled, the reader installs and
  // renders from the prebake'd glyph atlas. Default ON to keep the existing
  // optimized render path. Turn OFF to A/B test the upload-reliability
  // regression hypothesis -- when off, the renderer falls back to the v39
  // embedded glyph subset (or full SD-font glyph fetch if neither exists).
  uint8_t glyphAtlasEnabled = 1;
  // SD card font family name, including optional range suffix (empty = use built-in fontFamily)
  char sdFontFamilyName[64] = "";
  // CrumBLE 4.5.4: SD card font family used as a UI glyph fallback when the
  // primary UI font (built-in Bitter/Lexend/etc.) lacks a codepoint. The
  // typical case is a Latin reader with CJK book titles -- the user's
  // body font might still be Bitter but they pick a CJK font here so
  // titles, settings labels, collection names render correctly. Empty =
  // disabled. Loaded once at boot (before first home render) and stays
  // resident independent of any per-book primary font load, so the
  // carousel paint has the fallback ready.
  char uiFontFallbackFamily[64] = "";
  // CrumBLE 4.5.4: explicit point size for the UI fallback. 0 = auto =
  // smallest available size in the family (the default; matches the
  // original "tiny CJK as possible to avoid Latin/CJK mismatch" intent).
  // Picker lets the user override when the smallest .cpfont still renders
  // bigger than the surrounding 10-12 pt Latin (CJK glyphs cover the full
  // em-square so they look ~20% bigger at the same nominal pt). When set
  // and the requested size isn't in the family, ensureFallbackLoaded
  // falls back to the smallest available and logs a warning.
  uint8_t uiFontFallbackPointSize = 0;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when marked as finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;

  // v18.9.9.441 (CrossInk parity): idle-time threshold for reading-session
  // detection, in units of 10 seconds. Default 30 = 300 s = 5 min. Min 3
  // (30 s), Max 60 (600 s = 10 min). Applied by the reader: no page turn
  // for > threshold ends the current session for stats purposes.
  static constexpr uint8_t READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS = 10;
  static constexpr uint8_t READING_IDLE_TIME_THRESHOLD_UNITS_MIN = 3;
  static constexpr uint8_t READING_IDLE_TIME_THRESHOLD_UNITS_MAX = 60;
  static constexpr uint8_t READING_IDLE_TIME_THRESHOLD_UNITS_DEFAULT = 30;
  uint8_t readingIdleTimeThresholdUnits = READING_IDLE_TIME_THRESHOLD_UNITS_DEFAULT;
  uint32_t getReadingIdleTimeThresholdSeconds() const {
    uint8_t u = readingIdleTimeThresholdUnits;
    if (u < READING_IDLE_TIME_THRESHOLD_UNITS_MIN) u = READING_IDLE_TIME_THRESHOLD_UNITS_DEFAULT;
    if (u > READING_IDLE_TIME_THRESHOLD_UNITS_MAX) u = READING_IDLE_TIME_THRESHOLD_UNITS_MAX;
    return static_cast<uint32_t>(u) * READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
  }

  // v18.9.9.441 (CrossInk parity): automatic daily backup of all-time stats
  // to /.crossink-stats-backup/. Triggered on sleep-entry when clock valid.
  // 0 = disabled, 1 = enabled (default).
  uint8_t autoBackupStats = 1;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // v18.9.9.24: table rendering mode in EPUB reader
  uint8_t tableRendering = TABLES_DISPLAY;
  // Long-press Confirm (menu button) quick action in reader (0 = off)
  uint8_t longPressMenuAction = LONG_MENU_BOOK_SETTINGS;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  // CrumBLE Collections — global gate for series detection. When 0
  // (default), the OPF-parse pass and the shelf series-collapse
  // rendering are both skipped entirely. Off by default because most
  // EPUBs from non-Calibre sources lack series metadata, so the
  // expensive first-time scan would yield little value for those
  // users. Setting → on triggers the lazy scan on next collection
  // visit.
  uint8_t seriesDetectionEnabled = 0;

  // Bluetooth HID page-turner support. When on, the device scans, pairs, and
  // listens to a BLE HID remote and translates its keys into virtual button
  // presses (front buttons, side buttons) via HalGPIO::setVirtualButtonState.
  uint8_t bluetoothEnabled = 0;
  // v18.9.9.343: one-time migration marker. First v343+ boot force-resets
  // bluetoothEnabled to 0 so the ~58 KB BLE-controller heap tax is
  // released for users who had BT on (~58 KB back gives Settings entry
  // headroom and avoids the getSettingsList silent-restart). User can
  // re-enable BT manually via Settings > Bluetooth Setup.
  uint8_t hasAppliedBtOffMigration_v343 = 0;
  // Address (e.g. "AA:BB:CC:DD:EE:FF") and name of the last successfully bonded
  // BLE HID device. Used for auto-reconnect on next boot.
  char bleBondedDeviceAddr[18] = "";
  char bleBondedDeviceName[32] = "";
  // BLE address type (0 = public, 1 = random). Required by NimBLE on reconnect.
  uint8_t bleBondedDeviceAddrType = 0;

  // CrumBLE 4.5.5: rich remote-button mapping (1:1 port of upstream
  // crosspoint-reader feat-bluetooth BleKeyMapEntry). Replaces the old
  // forward/back wizard which only persisted DeviceProfiles' two custom
  // keycodes. Each entry binds a captured remote key (kind/value pair) to
  // one of the local virtual buttons (HalGPIO::BTN_*). 12 slots = more
  // than any realistic page-turner.
  //   keyKind == 0xFF -> unused entry.
  //   keyKind == 0   -> special key (PageUp/PageDown/arrows/etc) [reserved
  //                     for future freeink-style SpecialKey support; right
  //                     now CrumBLE only emits HID-usage keys].
  //   keyKind == 1   -> raw HID usage code (the keycode CrumBLE already
  //                     captures via BluetoothHIDManager's onReport hook).
  //   keyValue       -> the special-key id (kind 0) or usage code (kind 1).
  //   button         -> HalGPIO::BTN_BACK / CONFIRM / LEFT / RIGHT / UP /
  //                     DOWN. 0xFF means the slot is empty even if keyKind
  //                     is set (defensive).
  struct BleKeyMapEntry {
    uint8_t keyKind = 0xFF;
    uint8_t keyValue = 0;
    uint8_t button = 0xFF;
  };
  static constexpr uint8_t BLE_KEY_MAP_CAPACITY = 12;
  BleKeyMapEntry bleKeyMap[BLE_KEY_MAP_CAPACITY] = {};

  // CrumBLE: skip the grayscale LSB/MSB double-pass when cycling through the
  // sleep screensaver. Each grayscale pass triggers an extra ~1-2 s e-ink
  // sweep, so a single BMP with grayscale data takes ~3-4 sweeps to land.
  // For users who tap-cycle rapidly to find a specific image, the BW
  // single-sweep (~500 ms) feels much snappier. Trade-off: the cycled
  // image is rendered in 1-bit BW instead of 4-level grayscale until the
  // user stops cycling and the device deep-sleeps. Default 0 (grayscale
  // pass stays on, matching v3.7.3 behaviour). Only affects the cycle
  // path -- cover sleep, custom sleep, and end-of-book sleep keep the
  // grayscale pass either way.
  //
  // CrumBLE 4.5.4: kept default at 0. Considered flipping to 1 since X4
  // cycling can feel sluggish vs X3, but field testing showed that the
  // sluggish-feeling X4 sessions had grayscale sleep images (where the
  // 4-level rendering is the entire reason to use the image), and X3
  // sessions with snappy cycling typically used B/W sleep images
  // already. Users with grayscale-heavy collections would regress on a
  // default-on flip; they can opt in via the Settings toggle if they
  // prefer the speed-over-fidelity trade.
  uint8_t sleepCycleSkipGrayscale = 0;

  // CrumBLE prebake — master switch for the off-device chapter-index optimizer.
  // When 0 (default), the device behaves exactly like stock 3.7.3: only
  // sections/*.bin is consulted on cache load, and no prebake-manifest.json
  // lookup runs at book open.
  // When 1, the device also reads sections-prebake/*.bin as a fallback,
  // checks the prebake JSON manifest fingerprint on book open, and
  // prompts the user with "Use prepared layout?" when their current
  // SETTINGS don't match the prepared cache. The lazy background
  // extractor (which converts a pending zip drop into sections-prebake/
  // entries) also only runs when this is on. Per-book opt-in: works
  // alongside the existing /upload file manager flow so users who haven't
  // run the optimizer on a particular book see no change in behavior.
  //
  // CrumBLE 4.1.x line briefly defaulted this OFF as a crisis workaround
  // for a slim-binary prebake-mismatch crash that produced unexpected
  // heap pressure / mismatch-prompt UX on devices whose existing prebake
  // artifacts were generated by a build with different font hashes.
  // CrumBLE 4.2: default back to ON. 4.2 fixes the underlying mismatch
  // crash (drops Lexend Deca built-in instead of emoji glyphs, and adds
  // the host_shim SD-font measurement fast-path so WASM-baked layouts
  // byte-match device). Existing users get auto-migrated to ON via the
  // 4.2 prebake-migration marker in setup(); they can still toggle it
  // off from Settings -> Library -> Optimize Chapter Indexing if they
  // prefer the slower live-parse path.
  uint8_t optimizeChapterIndexing = 1;

  // v18.9.9.172: when 1, the indexing popup shows "Indexing page X of ~Y" once
  // pageCount > 0. When 0, just "Indexing..." + animated dots (v55 behavior).
  // v18.9.9.164: default flipped to 0. User feedback: the page-count form
  // makes long indexes feel slower (you watch the number crawl toward the
  // estimate). The classic "Indexing..." form takes the same wall-clock time
  // but reads as patient rather than laboring. Users who want the count can
  // opt in from Settings.
  uint8_t showIndexingPageCount = 0;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  static constexpr uint16_t POWER_BUTTON_WAKE_SHORT_MS = 10;
  static constexpr uint16_t POWER_BUTTON_LONG_PRESS_MS = 400;
  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = 30;
  static constexpr uint8_t SD_FONT_MAX_SIZE_STEPS = 8;

  uint16_t getPowerButtonWakeDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? POWER_BUTTON_WAKE_SHORT_MS
                                                                    : POWER_BUTTON_LONG_PRESS_MS;
  }

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const { return getPowerButtonWakeDuration(); }
  uint16_t getPowerButtonLongPressDuration() const { return POWER_BUTTON_LONG_PRESS_MS; }
  static uint8_t getActiveReaderFontSizeCount();
  static uint8_t getStoredReaderFontSize(FONT_SIZE size);
  static uint8_t getReaderFontPointSize(FONT_SIZE size);
  static uint8_t getSdFontRangePointSize(uint8_t range, uint8_t step);
  static bool isSdFontPointSizeAllowedForRange(uint8_t pointSize, uint8_t range);
  FONT_SIZE getEffectiveReaderFontSize() const;
  uint8_t getSdFontTargetPointSize() const;
  bool changeReaderFontSize(bool larger);
  int getReaderFontId() const;

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(FsFile& file, bool count_only = false) const;

  // v18.9.9.363: saveToFile() now marks-dirty + returns immediately.
  // Actual disk write happens on debounce elapsed (kSaveDebounceMs = 5s
  // of no new mutations) via retryDeferredSaveIfNeeded() OR at critical
  // exit points via flushIfDirtyNow(). Rationale: SD write failures under
  // contention were causing settings to revert after reboot; batching
  // reduces SD write pressure by ~10x for typical UI setting bursts.
  bool saveToFile() const;
  // Force any pending debounced write to disk NOW. Called from critical
  // exit paths (silent-restart, sleep entry, deep sleep) so the pending
  // change lands before the boundary. No-op if nothing is dirty.
  bool flushIfDirtyNow() const;
  static bool hasDeferredSave();
  // v18.9.9.325: mark save deferred without triggering an inline write.
  static void markSaveDeferred();
  void retryDeferredSaveIfNeeded() const;
  bool loadFromFile();

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static void validateReaderFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);
  static uint8_t sleepScreenStorageToMode(uint8_t storedValue);
  static uint8_t sleepScreenModeToStorage(uint8_t mode);
#ifdef SIMULATOR
  static bool verifySleepTimeoutMigrationContract();
  static bool verifySleepScreenMigrationContract();
#endif

 private:
  bool loadFromBinaryFile();
  bool migrateLanguageBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
