#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  bool settingsChanged = false;
  // CrumBLE: BookSettingsDrawer's Bluetooth entry sets this when the user
  // has no bonded remote and we need to launch the BT settings UI for
  // first-time pairing. The reader's drawer result handler picks it up.
  bool requestBluetoothFlow = false;
  // CrumBLE: BluetoothSettingsActivity sets this when it auto-exited after
  // a successful connect (exitOnSuccessfulConnect=true). The EpubReaderMenu
  // result handler treats it as a signal to also finish itself so the user
  // lands directly back in the book instead of in the reader menu.
  bool autoExitParent = false;
  // CrumBLE: BookSettingsDrawer's BT Quick Connect sets this so the reader
  // can do the connect AFTER any pending re-layout drains, instead of
  // racing the NimBLE handshake against a heap-heavy section rebuild
  // (which OOM'd the connect on the way out). Reader's drawer result
  // handler reads it, finishes any pending re-layout, runs the .pxc
  // manifest-mismatch check (if applicable), then enables and connects.
  bool bleConnectRequested = false;
  // Paired with bleConnectRequested: true when the BT No Images variant
  // was tapped (the reader will arm renderer.setSuppressImages() before
  // the actual connect lands).
  bool bleConnectNoImages = false;
};

struct ChapterResult {
  int spineIndex = 0;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct SyncResult {
  int spineIndex = 0;
  int page = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct BookmarkResult {
  uint16_t spineIndex = 0;
  float progress = 0.0f;
};

struct FileBrowserActionResult {
  int action = -1;
};

// CrumBLE Collections sort picker — value matches CollectionSort enum.
struct SortPickerResult {
  int sortMode = 0;  // 0 = Manual, 1 = TitleAlpha, 2 = DateAddedDesc, 3 = DateAddedAsc
};

struct FilePathResult {
  std::string path;
};

// CrumBLE: result from ChoicePromptActivity. choice == -1 means the user hit
// Back (cancel); choice >= 0 is the index of the picked option.
struct ChoicePromptResult {
  int choice = -1;
};

// CrumBLE: result from RearrangeCollectionsActivity. The orderedIds vector
// is the user's chosen display order (first to last). Cancelled results --
// either an early Back or a partial mark sequence -- are signalled by
// ActivityResult::isCancelled; orderedIds is empty in that case.
struct RearrangeCollectionsResult {
  std::vector<std::string> orderedIds;
};

// CrumBLE: result from DictionaryWordSelectActivity when invoked in
// HighlightRange mode (the reused word-picker). startWordIndex and
// endWordIndex are positions within the page's WordInfo vector
// (already inclusive); previewText is the raw concatenated text of
// the selected words, joined with single spaces, truncated and
// ellipsized to BOOKMARK_PREVIEW_MAX-1 chars (the storage cap).
// isCancelled signals Back-out; the indices are valid otherwise.
struct HighlightRangeResult {
  int startWordIndex = -1;
  int endWordIndex = -1;
  std::string previewText;
};

using ResultVariant = std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult,
                                   IntervalResult, PageResult, SyncResult, NetworkModeResult, FootnoteResult,
                                   BookmarkResult, FileBrowserActionResult, FilePathResult, SortPickerResult,
                                   ChoicePromptResult, RearrangeCollectionsResult, HighlightRangeResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
