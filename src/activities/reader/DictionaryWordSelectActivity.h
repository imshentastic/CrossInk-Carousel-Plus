#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "activities/Activity.h"

struct WordInfo {
  std::string text;
  // CrumBLE 4.5.5: lookupText REMOVED from the struct entirely. Was a
  // second std::string per word (~16 B struct overhead + a heap alloc per
  // word whose stripped content exceeded the SSO buffer -- guaranteed for
  // every CJK word at 3 bytes/char). For a 400-word page that was
  // ~6-16 KB of vector mass. Both readers compute it on demand now via
  // DictionaryWordSelectActivity::computeLookupTextAt(idx) which strips
  // leading/trailing apostrophes from `text` and joins across hyphenation
  // when the entry is a continuation pair. Lookup happens once per user
  // tap (rare); the per-word cost during extractWords disappears.
  //
  // CrumBLE 4.5.5: geometry packed to int16_t. Screen coords never exceed
  // ~1000 px on this hardware; row counts <= a few hundred per page.
  int16_t screenX;
  int16_t screenY;
  int16_t width;
  int16_t rowIndex;

  bool isHyphenatedLineEnd = false;
  int16_t continuationIndex = -1;
  int16_t continuationOf = -1;

  WordInfo(std::string t, int x, int y, int w, int r)
      : text(std::move(t)),
        screenX(static_cast<int16_t>(x)),
        screenY(static_cast<int16_t>(y)),
        width(static_cast<int16_t>(w)),
        rowIndex(static_cast<int16_t>(r)) {}
};

struct RowInfo {
  int16_t y;
  std::vector<int> wordIndices;
};

class DictionaryWordSelectActivity final : public Activity {
 public:
  // CrumBLE: the same word-picking machinery powers two flows:
  //   - Lookup: tap a word, dive into DictionaryDefinitionActivity.
  //   - HighlightRange: tap a word to anchor, navigate to expand the
  //     selection, tap again to finish. Emits a HighlightRangeResult
  //     with both word indices + the joined preview text.
  enum class Mode {
    Lookup,            // tap a word -> definition lookup
    HighlightRange,    // two-tap inclusive range on a single page (phase 2)
    HighlightSingleWord  // one-tap single word; used to pick the END
                       // anchor of a cross-page/cross-chapter highlight
                       // whose START anchor is held in EpubReaderActivity.
                       // Emits HighlightRangeResult with start==end and the
                       // picked word's raw text in previewText.
  };

  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        std::string cachePath, uint8_t orientation, std::string nextPageFirstWord,
                                        Mode mode = Mode::Lookup);

  void onEnter() override;
  void loop() override;
  void onExit() override;
  void render(RenderLock&&) override;

  // CrumBLE 4.4 post-bisect: post-silent-restart restore hook. The reader's
  // OpenDefinition / OpenLookupAtWord post-boot dispatch sets this to the
  // word the user was looking up. On the first loop() tick after extractWords,
  // the activity navigates the cursor to this word (if found on the current
  // page); if openOverlay is true (OpenDefinition path), it then auto-opens
  // the definition popup. If false (OpenLookupAtWord -- dismiss-time restart
  // path), it stops after the cursor navigation so the user resumes on the
  // word free to dismiss or pick another.
  void setPendingDefinitionWord(std::string word, bool openOverlay = true) {
    pendingDefinitionWord_ = std::move(word);
    pendingOpenOverlay_ = openOverlay;
  }
  // v18.9.9.249: paired with setPendingDefinitionWord. Non-zero means the
  // ERA post-boot dispatch consumed a chunk-start offset from the
  // silentRestartToReaderWithDefinitionAtChunk path; the activity's
  // performDefinitionLookup uses it as the initial chunk to load
  // (instead of the hard-coded 0u) so the definition opens on the
  // same chunk the user was trying to page into. 0 = start-of-entry.
  void setPendingDefinitionChunkStart(uint32_t chunkStart) {
    pendingDefinitionChunkStart_ = chunkStart;
  }

 private:
  std::unique_ptr<Page> page;
  int fontId;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  uint8_t orientation;
  std::string nextPageFirstWord;
  Mode mode_;

  std::vector<WordInfo> words;
  std::vector<RowInfo> rows;
  int currentRow = 0;
  int currentWordInRow = 0;

  // HighlightRange mode: word index in `words` of the first Confirm
  // (selection anchor). -1 means start hasn't been picked yet -- cursor
  // navigation moves a single word as in Lookup. Once set, the cursor
  // and the anchor define an inclusive range that's rendered as a
  // contiguous highlight underline rather than the single-word box.
  int highlightAnchorWordIdx_ = -1;

  // CrumBLE 4.4 post-bisect: inline definition overlay (kindle-style).
  // When a Confirm in Lookup mode picks a word, we capture the current
  // selection screen via storeBwBuffer (packbits-compressed 2-5 KB) and
  // draw a centered definition popup on top. Back closes the popup and
  // restoreBwBuffer redraws the selection screen underneath -- no full
  // activity push, no silent restart, the user's selection cursor is
  // preserved across the lookup. Replaces the prior
  // DictionaryDefinitionActivity push pattern (which required a
  // silent-restart for the second-and-later definition on low heap).
  std::string pendingDefinitionWord_;  // set by setPendingDefinitionWord; consumed on first loop() tick
  bool pendingDefinitionCursorMoved_ = false;  // phase gate: false=navigate first, true=now open overlay
  bool pendingOpenOverlay_ = true;  // true: auto-open overlay after cursor move; false: cursor only
  // v18.9.9.249: paired with pendingDefinitionWord_. Non-zero means the
  // ERA post-boot dispatch consumed a chunk-start offset from the
  // silentRestartToReaderWithDefinitionAtChunk path. Consumed once by
  // performDefinitionLookup on the initial chunk load: instead of
  // hardcoded loadChunkForCurrentEntry(0u) we load this offset so the
  // definition opens at the same chunk the user was trying to page
  // into. Cleared after consumption so a subsequent Left/Right cycle
  // still resets to chunk 0.
  uint32_t pendingDefinitionChunkStart_ = 0;
  bool defOverlay_ = false;
  bool defOverlayLoading_ = false;  // performing lookup; popup drawn empty
  bool defOverlayNotFound_ = false;
  bool defOverlayLowMemory_ = false;
  bool defOverlayCaptureValid_ = false;  // storeBwBuffer succeeded
  // v18.9.9.227: sticky flag for the current def-overlay session. Set true in
  // openDefinitionOverlay when isContinuingFromSilentReboot() was TRUE at
  // entry (i.e. this session was auto-opened by post-boot dispatch after a
  // silent-restart-with-definition). closeDefinitionOverlay checks this and
  // refuses to arm ANOTHER deferred restart -- otherwise: dismiss under
  // tight heap -> restart -> re-open on still-tight heap -> dismiss ->
  // restart again = infinite loop (observed on v226 when storeBwBuffer
  // failed at open time). Reset on close.
  bool sessionBornFromRestart_ = false;
  // v18.9.9.235: auto-recover on Low Memory. When wrapDefinition detects
  // the wrap threshold has been crossed, we can't safely render this
  // definition on the current heap. Instead of just showing the popup
  // and asking the user to back out + page-turn manually, arm a
  // silentRestartToReaderWithCursorWord that (a) shows the popup briefly
  // so the user knows what happened, (b) dismisses the def overlay
  // cleanly, (c) waits for the word-select re-render to complete so the
  // sleep-frame snapshot captures a clean (no def-composite) framebuffer,
  // then (d) fires the restart. Post-boot dispatches OpenLookupAtWord
  // which lands the cursor back on this word for a one-tap re-lookup on
  // freshly defragmented heap.
  bool pendingLowMemoryAutoRestart_ = false;      // guard fired, popup showing, waiting to auto-dismiss
  unsigned long lowMemoryPopupShownAtMs_ = 0;     // millis() when popup started showing
  unsigned long lowMemoryRestartAtMs_ = 0;        // deferred deadline for silentRestartToReaderWithCursorWord
  std::string lowMemoryRestartWord_;              // word to re-cursor on post-boot
  // v18.9.9.249: paired with lowMemoryRestartWord_. Non-zero only when the
  // arm-site was the chunk-transition refuse path in loadChunkForCurrentEntry;
  // the loop-driven fire-site then calls silentRestartToReaderWithDefinitionAtChunk
  // (auto-opens overlay at this chunkStart) instead of the cursor-only
  // silentRestartToReaderWithCursorWord (just navigates cursor to word).
  // 0 = the arm-site was a wrap-guard / readDef-refuse -- fire the
  // cursor-word variant as before (v247/v246 UX).
  uint32_t lowMemoryRestartChunkStart_ = 0;
  std::string defTargetWord_;
  // v18.9.9.247: chunked reader state. Each EntryStream references a source
  // entry in the .dict file (offset+totalSize) plus the currently loaded
  // byte range within that entry ([bufferRawStart, bufferRawEnd)) and the
  // HTML-stripped content of that range. Large entries (Wiktionary "light"
  // at 10+ KB) are loaded 3 KB at a time; Down at last visible line loads
  // the next chunk in-place, Up loads the previous. Small entries fit
  // in one chunk (loaded start=0, end=totalSize). Left/Right entry cycling
  // resets the target entry's chunk to 0.
  struct EntryStream {
    uint32_t offset;         // byte offset within .dict file
    uint32_t totalSize;      // total entry size in bytes
    uint32_t bufferRawStart; // start of loaded range (in raw entry bytes)
    uint32_t bufferRawEnd;   // end of loaded range (in raw entry bytes)
    std::string content;     // HTML-stripped bytes for range [bufferRawStart, bufferRawEnd)
  };
  std::vector<EntryStream> defEntryStreams_;
  int currentEntryIndex_ = 0;
  // Chunk size for large entries. 3 KB balances readability (~2 pages) vs
  // heap safety (wrap peak ~2 * 3K + 5K = 11 KB fits at 15 KB maxAlloc).
  static constexpr uint32_t DEF_CHUNK_SIZE = 3u * 1024u;
  // v18.9.9.196: deferred close-guard restart. On close under tight heap, we
  // stash the cursor word + a deadline; loop() fires the silent-restart only
  // if the deadline passes without the user doing anything (page nav, new
  // lookup, back-to-reader). Cancels on activity exit + on new open. This
  // trades v189's instant flash on close for a smoother "close is fine, but
  // if you keep hammering lookups we'll restart" UX.
  unsigned long deferredRestartAtMs_ = 0;  // 0 = no pending; else millis() deadline
  std::string deferredRestartWord_;
  std::vector<std::string> defLines_;
  int defScrollOffset_ = 0;
  int defLinesPerPage_ = 0;
  int defMaxScroll_ = 0;

  void openDefinitionOverlay(const std::string& word);
  void closeDefinitionOverlay();
  void performDefinitionLookup();
  void wrapDefinition(const std::string& definition);
  void renderDefinitionOverlay();
  // v18.9.9.247: (re)load the [chunkStart, chunkStart+DEF_CHUNK_SIZE) byte
  // range of defEntryStreams_[currentEntryIndex_] and re-wrap. Returns
  // true on success. Chunk gets clamped to totalSize at the tail. On
  // heap-refused range read, sets defOverlayLowMemory_ and returns false.
  bool loadChunkForCurrentEntry(uint32_t chunkStart);

  void extractWords();
  void mergeHyphenatedWords();
  int findClosestWordIndexInRow(int rowIndex, int targetX) const;

  // 4.5.5: replaces the per-word WordInfo::lookupText field. Builds the
  // dictionary lookup string for a picked word on demand: strips leading
  // / trailing apostrophes from words[idx].text and joins with the
  // continuation word's text if the entry was hyphenated across a line
  // break. Called from the Lookup path (openDefinitionOverlay,
  // pendingDefinitionWord match) and from buildPreviewBetween where
  // applicable.
  std::string computeLookupTextAt(int idx) const;

  // Build the preview text from the inclusive word range [a, b] in the
  // current page's words vector. Joins raw word text with single spaces
  // and truncates to BOOKMARK_PREVIEW_MAX-1 chars with a trailing "..."
  // if the joined text is longer. Order-agnostic (handles b < a).
  std::string buildPreviewBetween(int a, int b) const;
};