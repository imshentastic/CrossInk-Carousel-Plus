#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "activities/Activity.h"

struct WordInfo {
  std::string text;
  std::string lookupText;
  int screenX;
  int screenY;
  int width;
  int rowIndex;

  bool isHyphenatedLineEnd = false;
  int continuationIndex = -1;
  int continuationOf = -1;

  WordInfo(std::string t, int x, int y, int w, int r)
      : text(std::move(t)), screenX(x), screenY(y), width(w), rowIndex(r) {}
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
  bool defOverlay_ = false;
  bool defOverlayLoading_ = false;  // performing lookup; popup drawn empty
  bool defOverlayNotFound_ = false;
  bool defOverlayLowMemory_ = false;
  bool defOverlayCaptureValid_ = false;  // storeBwBuffer succeeded
  std::string defTargetWord_;
  std::vector<std::string> defLines_;
  int defScrollOffset_ = 0;
  int defLinesPerPage_ = 0;
  int defMaxScroll_ = 0;

  void openDefinitionOverlay(const std::string& word);
  void closeDefinitionOverlay();
  void performDefinitionLookup();
  void wrapDefinition(const std::string& definition);
  void renderDefinitionOverlay();

  void extractWords();
  void mergeHyphenatedWords();
  int findClosestWordIndexInRow(int rowIndex, int targetX) const;

  // Build the preview text from the inclusive word range [a, b] in the
  // current page's words vector. Joins raw word text with single spaces
  // and truncates to BOOKMARK_PREVIEW_MAX-1 chars with a trailing "..."
  // if the joined text is longer. Order-agnostic (handles b < a).
  std::string buildPreviewBetween(int a, int b) const;
};