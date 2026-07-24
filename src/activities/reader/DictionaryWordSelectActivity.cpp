#include "DictionaryWordSelectActivity.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap()
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>  // PrewarmScope (forward-declared in GfxRenderer.h)
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

#include <sstream>

#include "../../SilentRestart.h"
#include "BookmarkStore.h"  // BOOKMARK_PREVIEW_MAX
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

DictionaryWordSelectActivity::DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::unique_ptr<Page> page, int fontId, int marginLeft,
                                                           int marginTop, std::string cachePath, uint8_t orientation,
                                                           std::string nextPageFirstWord, Mode mode)
    : Activity("DictionaryWordSelect", renderer, mappedInput),
      page(std::move(page)),
      fontId(fontId),
      marginLeft(marginLeft),
      marginTop(marginTop),
      cachePath(std::move(cachePath)),
      orientation(orientation),
      nextPageFirstWord(std::move(nextPageFirstWord)),
      mode_(mode) {}

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  extractWords();
  mergeHyphenatedWords();
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  Activity::onExit();

  // v18.9.9.196: user hit Back to leave word-select -- return to reader is
  // a heap-healing event, no need to restart. Cancel any pending deferral.
  if (deferredRestartAtMs_ != 0) {
    LOG_DBG("DICT", "cancelling deferred restart on onExit (returning to reader)");
    deferredRestartAtMs_ = 0;
    deferredRestartWord_.clear();
  }

  // FIX: Aggressively free all resources and defragment heap before returning to the reader
  words.clear();
  words.shrink_to_fit();

  rows.clear();
  rows.shrink_to_fit();

  // Explicitly reset the unique_ptr to free the parsed DOM memory immediately
  page.reset();
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  rows.clear();
  if (!page) return;

  // Pre-allocate vectors to prevent massive heap fragmentation.
  // CrumBLE tuning: reserve(350) is ~22 KB on a fragmented heap with BLE
  // connected (NimBLE ~58 KB, maxAlloc often drops to 7-10 KB), which
  // bad_allocs and reboots the device. Reduced to 80 (~5 KB) -- covers
  // most pages and re-allocates from there for long ones. Exceptions
  // are disabled (-fno-exceptions), so a failing allocation would call
  // __terminate() and reboot. Defense in depth against the LOOKUP
  // entry's heap pre-flight: re-check max contiguous alloc here, and
  // bail out clean if it slipped. Render falls back to showing the
  // page with no highlight (handled by the rows-empty guard).
  constexpr uint32_t EXTRACT_MIN_MAX_ALLOC = 8000;
  if (ESP.getMaxAllocHeap() < EXTRACT_MIN_MAX_ALLOC) {
    LOG_ERR("DICT", "extractWords: maxAlloc=%u below %u; aborting word build",
            ESP.getMaxAllocHeap(), EXTRACT_MIN_MAX_ALLOC);
    return;
  }
  words.reserve(80);
  rows.reserve(20);

  int currentRowIndex = -1;
  int16_t lastY = -1;

  for (const auto& element : page->elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;

    const auto& line = static_cast<const PageLine&>(*element);
    auto block = line.getBlock();
    if (!block) continue;

    const auto& lineWords = block->getWords();
    if (lineWords.empty()) continue;

    const auto& wordXpos = block->getWordXpos();
    const auto& wordStyles = block->getWordStyles();

    int16_t currentY = marginTop + line.yPos;
    if (currentY != lastY) {
      rows.push_back({currentY, {}});
      currentRowIndex++;
      lastY = currentY;
    }

    for (size_t i = 0; i < lineWords.size(); ++i) {
      const std::string raw(lineWords[i].data(), lineWords[i].size());
      int16_t baseX = marginLeft + line.xPos + wordXpos[i];

      std::string prefix = "";
      std::string current_word = "";

      auto emitWord = [&]() {
        if (current_word.empty()) return;

        int pre_w = prefix.empty() ? 0 : renderer.getTextWidth(fontId, prefix.c_str(), wordStyles[i]);
        int word_w = renderer.getTextWidth(fontId, current_word.c_str(), wordStyles[i]);

        std::string lookup = current_word;
        while (!lookup.empty() && lookup.back() == '\'') lookup.pop_back();
        while (!lookup.empty() && lookup.front() == '\'') lookup.erase(0, 1);

        if (!lookup.empty()) {
          int wordIndex = static_cast<int>(words.size());
          words.emplace_back(current_word, baseX + pre_w, currentY, word_w, currentRowIndex);
          // CrumBLE 4.5.5: no per-word lookupText assignment. computeLookup
          // TextAt(wordIndex) reproduces the same strip-apostrophes logic
          // on demand at lookup time. The local `lookup` variable above
          // is still used for the empty-check gate (we don't add an entry
          // for a word that strips down to nothing -- pure punctuation).
          rows[currentRowIndex].wordIndices.push_back(wordIndex);
        }
      };

      for (size_t c = 0; c < raw.length();) {
        unsigned char ch = static_cast<unsigned char>(raw[c]);

        if (ch == 0xE2 && c + 2 < raw.length() && static_cast<unsigned char>(raw[c + 1]) == 0x80) {
          emitWord();
          prefix += current_word + raw.substr(c, 3);
          current_word = "";
          c += 3;
          continue;
        }

        bool isWordChar = (ch > 127 || std::isalnum(ch) || ch == '\'');

        if (isWordChar) {
          current_word += raw[c];
          c++;
        } else {
          emitWord();
          prefix += current_word + raw[c];
          current_word = "";
          c++;
        }
      }
      emitWord();

      if (i == lineWords.size() - 1 && !raw.empty() && raw.back() == '-') {
        if (!words.empty() && words.back().rowIndex == currentRowIndex) {
          words.back().isHyphenatedLineEnd = true;
        }
      }
    }
  }

  if (!words.empty()) {
    // CrumBLE: rows are pushed per Y-line, but wordIndices only gets
    // entries for lookup-able tokens. A row of pure punctuation /
    // styled glyphs leaves wordIndices empty, and starting at row 0
    // with an empty wordIndices vector faults the first render --
    // rows[0].wordIndices[0] derefs OOB on an empty vector. Skip
    // forward to the first row that actually has selectable words.
    currentRow = 0;
    currentWordInRow = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
      if (!rows[r].wordIndices.empty()) {
        currentRow = static_cast<int>(r);
        break;
      }
    }
  }
}

int DictionaryWordSelectActivity::findClosestWordIndexInRow(int rowIndex, int targetX) const {
  if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()) || rows[rowIndex].wordIndices.empty()) {
    return 0;
  }
  const auto& rowWords = rows[rowIndex].wordIndices;
  int bestIndex = 0;
  int minDistance = 99999;
  for (size_t i = 0; i < rowWords.size(); ++i) {
    const auto& word = words[rowWords[i]];
    int wordCenterX = word.screenX + (word.width / 2);
    int distance = std::abs(wordCenterX - targetX);
    if (distance < minDistance) {
      minDistance = distance;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

void DictionaryWordSelectActivity::loop() {
  // v18.9.9.235: auto-recover on Low Memory -- two phases.
  //
  // Phase 1: popup has been shown for POPUP_HOLD_MS so the user can see the
  // "Low memory" message. Close the overlay cleanly (this will restore the
  // BW backup if valid, else requestUpdate to re-render word-select), then
  // arm a deferred restart to give the render task time to finish before
  // the sleep-frame snapshot fires. Sleep-frame captures a CLEAN
  // word-select page (no def-composite), which is small enough for packbits
  // to compress -- the v215 truncation panic class is out of reach.
  //
  // Phase 2: deferred deadline hit -> silentRestartToReaderWithCursorWord.
  // Post-boot's OpenLookupAtWord dispatch (already-wired but previously
  // unused) lands cursor back on the word. One Confirm tap re-lookups on
  // freshly defragmented heap.
  constexpr unsigned long POPUP_HOLD_MS = 1500;
  constexpr unsigned long RENDER_SETTLE_MS = 800;
  if (pendingLowMemoryAutoRestart_ && millis() - lowMemoryPopupShownAtMs_ >= POPUP_HOLD_MS) {
    LOG_INF("DICT", "auto-recover: dismissing Low-memory popup, arming silent-restart-with-cursor-word '%s'",
            lowMemoryRestartWord_.c_str());
    pendingLowMemoryAutoRestart_ = false;
    lowMemoryRestartAtMs_ = millis() + RENDER_SETTLE_MS;
    closeDefinitionOverlay();
    return;
  }
  if (lowMemoryRestartAtMs_ != 0 && millis() >= lowMemoryRestartAtMs_) {
    const std::string word = std::move(lowMemoryRestartWord_);
    const uint32_t chunkStart = lowMemoryRestartChunkStart_;
    lowMemoryRestartAtMs_ = 0;
    lowMemoryRestartChunkStart_ = 0;
    // v18.9.9.249: chunk-transition refuse path arms chunkStart > 0.
    // Fire the chunk-aware variant so post-boot the definition opens
    // directly on the chunk the user was paging into. For wrap-guard /
    // readDef-refuse (chunkStart == 0), fire the cursor-only variant
    // as before -- the user's cursor lands back on the word, one tap
    // reopens the definition at chunk 0 on the fresh heap.
    if (chunkStart != 0u) {
      LOG_INF("DICT",
              "auto-recover: firing silentRestartToReaderWithDefinitionAtChunk('%s', chunkStart=%u)",
              word.c_str(), chunkStart);
      silentRestartToReaderWithDefinitionAtChunk(word.c_str(), chunkStart);
    } else {
      LOG_INF("DICT", "auto-recover: firing silentRestartToReaderWithCursorWord('%s')", word.c_str());
      silentRestartToReaderWithCursorWord(word.c_str());
    }
    return;  // never returns
  }

  // v18.9.9.232: deferred close-guard restart machinery removed. It was the
  // root cause of a recurring class of panic ("BW restore truncated at
  // literal"): silentRestartToReaderWithDefinition's sleep-frame snapshot
  // carried whatever framebuffer state we had into the reboot, post-boot
  // openDefinitionOverlay's storeBwBuffer re-captured that state, packbits
  // silently truncated when it exceeded the 32 KB budget, and the next
  // closeDefinitionOverlay's restoreBwBuffer decoded past the end -> heap
  // poisoning -> reboot loop. The whole "restart to defrag heap on tight
  // dismiss" pattern (v196/v213/v215/v227/v231) is deleted. On tight heap
  // the pre-flight guard in Dictionary::readDefinition (v222) surfaces a
  // "Low memory" popup, user dismisses, and a page-turn defrags manually.
  //
  // The pending-word Phase 1/2 code below is preserved because
  // OpenLookupAtWord silent-restarts (from EpubReaderActivity's lookup
  // pre-flight, not from this file) still use it. Those restarts happen
  // BEFORE the def overlay opens on the target boot, so the sleep-frame
  // carries a clean reader-page framebuffer -- no packbits truncation.

  // CrumBLE 4.4 post-bisect: post-silent-restart restore is two-phase.
  // Phase 1 (first loop tick after onEnter+first render): navigate the
  // cursor to the previously-looked-up word and request a redraw. We
  // can't open the overlay yet because storeBwBuffer would capture the
  // selection screen with the WRONG cursor position (the previous render
  // used the default cursor).
  // Phase 2 (next loop tick, after the render task has redrawn with the
  // correct cursor): open the definition overlay. The capture now lands
  // on the user's pre-restart context: same page, cursor on the word
  // they were looking up.
  if (!pendingDefinitionWord_.empty() && !defOverlay_) {
    if (!pendingDefinitionCursorMoved_) {
      // Phase 1: locate the word + move cursor + redraw.
      // CrumBLE 4.5.5: per-word lookupText was removed -- compare against
      // the on-demand computation. Restore is rare (only after a silent
      // restart during a lookup), so the per-word string allocation here
      // doesn't matter; the per-word extraction-time cost did.
      for (size_t i = 0; i < words.size(); ++i) {
        if (computeLookupTextAt(static_cast<int>(i)) == pendingDefinitionWord_) {
          for (size_t r = 0; r < rows.size(); ++r) {
            for (size_t w = 0; w < rows[r].wordIndices.size(); ++w) {
              if (rows[r].wordIndices[w] == static_cast<int>(i)) {
                currentRow = static_cast<int>(r);
                currentWordInRow = static_cast<int>(w);
                goto cursorMoved;
              }
            }
          }
        }
      }
    cursorMoved:
      pendingDefinitionCursorMoved_ = true;
      // v18.9.9.246: mark session as post-restart so guards downstream
      // avoid the auto-recover -> restart -> refuse -> auto-recover loop
      // when the target word still doesn't fit even on freshly rebooted heap.
      sessionBornFromRestart_ = true;
      requestUpdate();
      return;
    }
    // Phase 2: cursor is rendered into the framebuffer; safe to capture.
    std::string word = std::move(pendingDefinitionWord_);
    pendingDefinitionWord_.clear();
    pendingDefinitionCursorMoved_ = false;
    if (pendingOpenOverlay_) {
      openDefinitionOverlay(word);
    } else {
      // Cursor-only restore (OpenLookupAtWord path). Reset the flag for any
      // subsequent setPendingDefinitionWord call that doesn't pass it.
      pendingOpenOverlay_ = true;
    }
    return;
  }

  // CrumBLE 4.4 post-bisect: when the definition overlay is open, all
  // button input drives the overlay (scroll / dismiss). Word-selection
  // navigation underneath is frozen until the overlay closes.
  if (defOverlay_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      closeDefinitionOverlay();
      return;
    }
    // v18.9.9.198: Left/Right cycle through entries when the word has more
    // than one. v247: also reset the target entry's chunk to 0 since chunks
    // are per-entry.
    if (defEntryStreams_.size() > 1) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Left) && currentEntryIndex_ > 0) {
        currentEntryIndex_--;
        defScrollOffset_ = 0;
        defLines_.clear();
        defLines_.shrink_to_fit();
        loadChunkForCurrentEntry(0u);
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Right) &&
          currentEntryIndex_ + 1 < static_cast<int>(defEntryStreams_.size())) {
        currentEntryIndex_++;
        defScrollOffset_ = 0;
        defLines_.clear();
        defLines_.shrink_to_fit();
        loadChunkForCurrentEntry(0u);
        requestUpdate();
        return;
      }
    }
    // v18.9.9.247: chunk-aware Up/Down. At the last visible line of the
    // current chunk, Down loads the next chunk (if there is more entry
    // remaining). At the first visible line, Up loads the previous chunk
    // (if there's anything before). Within the current chunk, Up/Down
    // just scrolls the pre-wrapped defLines_ as before.
    const EntryStream* es = (currentEntryIndex_ >= 0 &&
                              currentEntryIndex_ < static_cast<int>(defEntryStreams_.size()))
                                ? &defEntryStreams_[currentEntryIndex_]
                                : nullptr;
    const bool hasMoreForward = es && es->bufferRawEnd < es->totalSize;
    const bool hasMoreBackward = es && es->bufferRawStart > 0u;
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (defScrollOffset_ < defMaxScroll_) {
        defScrollOffset_ = std::min(defMaxScroll_, defScrollOffset_ + std::max(1, defLinesPerPage_ - 1));
        requestUpdate();
      } else if (hasMoreForward) {
        LOG_DBG("DICT", "Down at chunk end: loading next chunk from %u", es->bufferRawEnd);
        defScrollOffset_ = 0;
        loadChunkForCurrentEntry(es->bufferRawEnd);
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (defScrollOffset_ > 0) {
        defScrollOffset_ = std::max(0, defScrollOffset_ - std::max(1, defLinesPerPage_ - 1));
        requestUpdate();
      } else if (hasMoreBackward) {
        const uint32_t newStart = es->bufferRawStart > DEF_CHUNK_SIZE
                                       ? es->bufferRawStart - DEF_CHUNK_SIZE
                                       : 0u;
        LOG_DBG("DICT", "Up at chunk start: loading previous chunk from %u", newStart);
        loadChunkForCurrentEntry(newStart);
        defScrollOffset_ = defMaxScroll_;  // start at bottom of previous chunk
        requestUpdate();
      }
      return;
    }
    return;
  }

  if (rows.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  bool selectionChanged = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (currentWordInRow > 0) {
      currentWordInRow--;
      selectionChanged = true;
    } else if (currentRow > 0) {
      currentRow--;
      currentWordInRow = static_cast<int>(rows[currentRow].wordIndices.size()) - 1;
      selectionChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size()) - 1) {
      currentWordInRow++;
      selectionChanged = true;
    } else if (currentRow < static_cast<int>(rows.size()) - 1) {
      currentRow++;
      currentWordInRow = 0;
      selectionChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (currentRow > 0) {
      int currentWordIdx = rows[currentRow].wordIndices[currentWordInRow];
      int targetX = words[currentWordIdx].screenX + (words[currentWordIdx].width / 2);
      currentRow--;
      currentWordInRow = findClosestWordIndexInRow(currentRow, targetX);
      selectionChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (currentRow < static_cast<int>(rows.size()) - 1) {
      int currentWordIdx = rows[currentRow].wordIndices[currentWordInRow];
      int targetX = words[currentWordIdx].screenX + (words[currentWordIdx].width / 2);
      currentRow++;
      currentWordInRow = findClosestWordIndexInRow(currentRow, targetX);
      selectionChanged = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (rows.empty() || currentRow < 0 || currentRow >= static_cast<int>(rows.size()) ||
        rows[currentRow].wordIndices.empty() || currentWordInRow < 0 ||
        currentWordInRow >= static_cast<int>(rows[currentRow].wordIndices.size())) {
      // No selectable word -- ignore the press.
    } else {
      int selectedWordIdx = rows[currentRow].wordIndices[currentWordInRow];

      if (mode_ == Mode::Lookup) {
        // CrumBLE 4.5.5: on-demand lookup-text build (was a per-word
        // WordInfo field; now computed from text + apostrophe strip +
        // hyphenation join). Same result, no per-extraction cost.
        openDefinitionOverlay(computeLookupTextAt(selectedWordIdx));
      } else if (mode_ == Mode::HighlightSingleWord) {
        // One-tap mode for cross-page END pick. Capture some lead-in
        // context BEFORE the picked word so the saved preview reads
        // like a passage ending here rather than a lonely word. The
        // reader pairs this with the start anchor's trailing context
        // to build the final preview.
        ActivityResult result;
        HighlightRangeResult hr;
        hr.startWordIndex = selectedWordIdx;
        hr.endWordIndex = selectedWordIdx;
        // Lead-in window sized to roughly fill the BOOKMARK_PREVIEW_MAX
        // budget on its own; the reader's cross-page join (start trailing
        // + " ... " + end lead-in) will truncate the END half when the
        // combined string exceeds the cap.
        const int leadIn = std::max(0, selectedWordIdx - 14);
        hr.previewText = buildPreviewBetween(leadIn, selectedWordIdx);
        result.data = hr;
        setResult(std::move(result));
        finish();
      } else {
        // HighlightRange (same-page): first Confirm anchors the start;
        // second Confirm finishes with the range. Anchor lives until exit.
        if (highlightAnchorWordIdx_ < 0) {
          highlightAnchorWordIdx_ = selectedWordIdx;
          requestUpdate();
        } else {
          ActivityResult result;
          HighlightRangeResult hr;
          hr.startWordIndex = std::min(highlightAnchorWordIdx_, selectedWordIdx);
          hr.endWordIndex = std::max(highlightAnchorWordIdx_, selectedWordIdx);
          hr.previewText = buildPreviewBetween(hr.startWordIndex, hr.endWordIndex);
          result.data = hr;
          setResult(std::move(result));
          finish();
        }
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    // CrumBLE: in HighlightRange mode with the anchor placed, Back means
    // "hold the start for later" instead of cancel -- emit a result with
    // startWordIndex=anchor and endWordIndex=-1. Reader stores this as
    // pendingHighlightStart_ and surfaces FINISH / CANCEL in the menu.
    // Lookup mode and pre-anchor HighlightRange still treat Back as cancel.
    if (mode_ == Mode::HighlightRange && highlightAnchorWordIdx_ >= 0 &&
        highlightAnchorWordIdx_ < static_cast<int>(words.size())) {
      HighlightRangeResult hr;
      hr.startWordIndex = highlightAnchorWordIdx_;
      hr.endWordIndex = -1;  // signal: anchor only
      // CrumBLE: capture a trailing-context snippet starting at the
      // anchor so the held preview ("Start saved...") and the eventual
      // bookmark preview both read as a passage rather than a single
      // word. Bounded by available words on this page.
      // Trailing window sized to roughly fill the BOOKMARK_PREVIEW_MAX
      // budget on its own. Wider than the lookup-style 8-word context;
      // gives the user a readable passage when reviewing the held start
      // ("Start saved...") and again in the final bookmark preview.
      const int trailingEnd =
          std::min(static_cast<int>(words.size()) - 1, highlightAnchorWordIdx_ + 14);
      hr.previewText = buildPreviewBetween(highlightAnchorWordIdx_, trailingEnd);
      result.data = hr;
    } else {
      result.isCancelled = true;
    }
    setResult(std::move(result));
    finish();
  }

  if (selectionChanged) requestUpdate();
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // CrumBLE 4.4 post-bisect: when the definition overlay is open, restore
  // the captured word-selection screen first, then paint the popup on top.
  // We re-restore on every render so scroll/dismiss redraws don't accumulate
  // stale popup pixels. Capture happened in openDefinitionOverlay before
  // this render fires.
  if (defOverlay_) {
    renderDefinitionOverlay();
    return;
  }
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());

  // CrumBLE 4.2: prewarm the font cache before page->render. The parent
  // EpubReaderActivity::renderContents wraps its own page render in a
  // FontCacheManager::PrewarmScope whose dtor clearCache()'s the SD-font
  // miniData on the way out -- so by the time this activity runs, the
  // SD font has no intervals/glyphs cached. EpdFontFamily::getGlyph (the
  // path drawText uses) only consults findGlyph; it never falls back to
  // the glyphMissHandler / overflow ring buffer (only EpdFont::getGlyph
  // does that). With no prewarm, every codepoint resolves to
  // REPLACEMENT_GLYPH and the overlay renders as a wall of '?'. The
  // built-in compressed-font path didn't notice because its findGlyph
  // works against the always-resident interval table -- only SD fonts
  // are affected.
  //
  // Mirroring the reader's scan-then-prewarm-then-render pattern populates
  // the SD-font miniData for every codepoint on the current page. The scope
  // is held in an std::optional at function scope so it stays alive past
  // page->render -- the HighlightRange reverse-video pass below also calls
  // drawText for the selected words and would otherwise hit the same
  // wall-of-?'s problem. Scope dtor wipes the cache when render() returns
  // so we don't leak cached state past this activity.
  auto* fcm = renderer.getFontCacheManager();
  std::optional<FontCacheManager::PrewarmScope> prewarmScope;
  if (fcm) {
    prewarmScope.emplace(fcm->createPrewarmScope());
    page->renderText(renderer, fontId, marginLeft, marginTop);  // scan pass: records text, doesn't draw
    prewarmScope->endScanAndPrewarm();
  }
  page->render(renderer, fontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());

  // CrumBLE: belt and suspenders -- extractWords now skips empty rows,
  // but if currentRow lands on one anyway (defensive) or words is empty
  // entirely, skip the highlight pass instead of dereferencing OOB.
  if (!rows.empty() && currentRow >= 0 && currentRow < static_cast<int>(rows.size()) &&
      !rows[currentRow].wordIndices.empty() && currentWordInRow >= 0 &&
      currentWordInRow < static_cast<int>(rows[currentRow].wordIndices.size())) {
    int selectedWordIdx = rows[currentRow].wordIndices[currentWordInRow];
    const int lineHeight = renderer.getLineHeight(fontId);

    // Dark-mode-aware highlight color. Selection visuals always show reverse
    // video relative to the page: black box on a white page, white box on a
    // dark page. Without this, in dark mode the black focus reticle and
    // range fill draw black-on-black and the selection becomes invisible.
    const bool highlightInk = ReaderUtils::readerForegroundBlack();
    const bool highlightTextBlack = !highlightInk;

    auto drawSingleWordBox = [&](int index) {
      const WordInfo& word = words[index];
      int boxX = word.screenX;
      int boxY = word.screenY;
      int boxWidth = word.width;

      renderer.fillRect(boxX, boxY + lineHeight + 2, boxWidth, 3, highlightInk);
      renderer.fillRect(boxX, boxY - 3, boxWidth, 1, highlightInk);
      renderer.fillRect(boxX - 3, boxY - 3, 2, lineHeight + 8, highlightInk);
      renderer.fillRect(boxX + boxWidth + 1, boxY - 3, 2, lineHeight + 8, highlightInk);
    };

    // HighlightRange mode + anchor placed: render a filled black box
    // behind every word in the inclusive range [anchor, cursor], then
    // redraw each word in white over the box (reverse video). Skips
    // continuation slots so hyphenated halves aren't double-stamped.
    // The cursor's word-ring is suppressed inside the range -- the
    // reverse-video selection already shows where the cursor sits.
    if (mode_ == Mode::HighlightRange && highlightAnchorWordIdx_ >= 0 &&
        highlightAnchorWordIdx_ < static_cast<int>(words.size())) {
      const int lo = std::min(highlightAnchorWordIdx_, selectedWordIdx);
      const int hi = std::max(highlightAnchorWordIdx_, selectedWordIdx);
      const int padX = 1;
      const int padTop = 2;
      const int padBot = 2;
      // First pass: fill the inter-word gap on the same row so the
      // selection reads as one continuous highlighted block instead of
      // a series of word boxes with white gaps. Two consecutive in-range
      // words on the same rowIndex get a black bridge between them.
      for (int i = lo; i < hi && i < static_cast<int>(words.size()) - 1; ++i) {
        if (words[i].continuationOf != -1) continue;
        if (words[i].rowIndex != words[i + 1].rowIndex) continue;
        const int gapStart = words[i].screenX + words[i].width;
        const int gapEnd = words[i + 1].screenX;
        if (gapEnd > gapStart) {
          renderer.fillRect(gapStart - padX, words[i].screenY - padTop,
                            (gapEnd - gapStart) + padX * 2, lineHeight + padTop + padBot, highlightInk);
        }
      }
      // Second pass: fill the words themselves and redraw in the inverse ink.
      for (int i = lo; i <= hi && i < static_cast<int>(words.size()); ++i) {
        const WordInfo& w = words[i];
        if (w.continuationOf != -1) continue;
        renderer.fillRect(w.screenX - padX, w.screenY - padTop, w.width + padX * 2,
                          lineHeight + padTop + padBot, highlightInk);
        // Reverse-video text: in light mode highlightInk=true (black fill)
        // so text draws white; in dark mode highlightInk=false (white fill)
        // so text draws black. Style defaults to REGULAR since we don't
        // store per-word EpdFontFamily::Style in WordInfo yet.
        renderer.drawText(fontId, w.screenX, w.screenY, w.text.c_str(), highlightTextBlack);
      }
    } else {
      drawSingleWordBox(selectedWordIdx);
      if (words[selectedWordIdx].continuationIndex != -1) {
        drawSingleWordBox(words[selectedWordIdx].continuationIndex);
      }
      if (words[selectedWordIdx].continuationOf != -1) {
        drawSingleWordBox(words[selectedWordIdx].continuationOf);
      }
    }
  }

  // CrumBLE: button hints differ by mode and state.
  //   Lookup        : Cancel / Lookup     /.../ Prev-Next
  //   Range (pre-)  : Cancel / Start      /.../ Prev-Next
  //   Range (held)  : Hold   / End        /.../ Prev-Next
  //                   ^ Back changes meaning once an anchor is placed.
  //   SingleWord    : Cancel / End        /.../ Prev-Next
  const char* btn1Label = tr(STR_CANCEL);
  const char* btn2Label = tr(STR_LOOKUP);
  if (mode_ == Mode::HighlightRange) {
    btn2Label = (highlightAnchorWordIdx_ < 0) ? tr(STR_HIGHLIGHT_START) : tr(STR_HIGHLIGHT_END);
    if (highlightAnchorWordIdx_ >= 0) btn1Label = tr(STR_HIGHLIGHT_HOLD);
  } else if (mode_ == Mode::HighlightSingleWord) {
    btn2Label = tr(STR_HIGHLIGHT_END);
  }
  // CrumBLE: split "Prev/Next" into separate Prev + Next slots so each
  // physical button shows its own hint instead of one button labelled
  // "Prev/Next" and the other left blank.
  const auto labels = mappedInput.mapLabels(btn1Label, btn2Label, tr(STR_PREV), tr(STR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4,
                       /*allowInvertedText=*/false, ReaderUtils::readerDarkModeEnabled());
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string DictionaryWordSelectActivity::buildPreviewBetween(int a, int b) const {
  const int lo = std::min(a, b);
  const int hi = std::max(a, b);
  if (lo < 0 || hi >= static_cast<int>(words.size())) return {};

  std::string out;
  out.reserve(BOOKMARK_PREVIEW_MAX);
  for (int i = lo; i <= hi; ++i) {
    // Skip continuation halves so hyphenated words aren't duplicated.
    if (words[i].continuationOf != -1) continue;
    if (!out.empty()) out += ' ';
    out += words[i].text;
    // Cap early once the string would exceed storage (-1 for NUL,
    // -3 for the ellipsis). Saves cycles on long ranges.
    if (out.size() >= BOOKMARK_PREVIEW_MAX - 4) break;
  }
  if (out.size() > BOOKMARK_PREVIEW_MAX - 1) {
    out.resize(BOOKMARK_PREVIEW_MAX - 4);
    out += "...";
  }
  return out;
}

void DictionaryWordSelectActivity::mergeHyphenatedWords() {
  if (words.empty()) return;
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i].isHyphenatedLineEnd && i + 1 < words.size()) {
      // CrumBLE 4.5.5: lookupText merge removed -- computeLookupTextAt()
      // joins on demand via continuationIndex / continuationOf at lookup
      // time. This loop now only sets the continuation pointers for the
      // render path (drawing the joined-word highlight across the line
      // break) and for the on-demand lookup join.
      words[i].continuationIndex = static_cast<int16_t>(i + 1);
      words[i + 1].continuationOf = static_cast<int16_t>(i);
    }
  }
}

// CrumBLE 4.5.5: compute the dictionary-lookup form of words[idx] on demand.
// Replaces the per-word WordInfo::lookupText field that used to be filled
// during extractWords (saved ~6-16 KB of vector mass on CJK pages). Called
// at most once per user tap from the Lookup path -- the per-page cost goes
// from O(N words) at extraction to O(1) at click.
std::string DictionaryWordSelectActivity::computeLookupTextAt(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(words.size())) return {};
  std::string out = words[idx].text;
  // Hyphenated continuation: join with the next-line half (only the start
  // of a hyphenated pair carries continuationIndex; the continuation half
  // carries continuationOf and is the right operand).
  if (words[idx].continuationIndex >= 0 &&
      words[idx].continuationIndex < static_cast<int16_t>(words.size())) {
    out += words[words[idx].continuationIndex].text;
  } else if (words[idx].continuationOf >= 0 &&
             words[idx].continuationOf < static_cast<int16_t>(words.size())) {
    // Tapped the second half of a hyphenated pair -- prepend the first half
    // so the lookup string matches what the dictionary would have for the
    // full word.
    out = words[words[idx].continuationOf].text + out;
  }
  // Same apostrophe strip extractWords used to do inline.
  while (!out.empty() && out.back() == '\'') out.pop_back();
  while (!out.empty() && out.front() == '\'') out.erase(0, 1);
  return out;
}

// CrumBLE 4.4 post-bisect: inline definition overlay implementation. Replaces
// the prior DictionaryDefinitionActivity push pattern -- now the popup draws
// on top of the captured word-selection screen and dismissal restores the
// underlying frame via the renderer's BW backup (~2-5 KB packbits-compressed).
// User's selection cursor is preserved across the lookup.

void DictionaryWordSelectActivity::openDefinitionOverlay(const std::string& word) {
  // v18.9.9.232: v213 rapid-cycle guard removed. It called
  // silentRestartToReaderWithDefinition on rapid double-tap under tight
  // heap, which fed the sleep-frame-composite panic loop (see loop()'s
  // comment above). Rapid re-tap on tight heap now takes the same benign
  // path as any other open: storeBwBuffer either succeeds (dismiss
  // restores cleanly) or fails (dismiss re-renders word-select), and
  // wrap OOM is caught by Dictionary::readDefinition's pre-flight guard
  // -> "Low memory" popup. No silent-restart path from this file.
  clearSilentRebootContinuationFlag();
  // v18.9.9.240: cancel any pending auto-recover from a previous overlay
  // session. Rapid successive Confirms can layer: overlay A refuses ->
  // auto-recover armed -> user taps word B before Phase 2 fires ->
  // overlay B opens. If B succeeds we don't want A's stale timer to
  // fire mid-render (that's what caused v239's contended-lock white
  // flash: overlay B's wrap was still running when overlay A's phase 2
  // fired). If B also fails, its own wrap guard re-arms with B's word
  // and a fresh timestamp.
  pendingLowMemoryAutoRestart_ = false;
  lowMemoryRestartAtMs_ = 0;
  lowMemoryRestartWord_.clear();
  // v18.9.9.244: pre-open guard removed. It made "every other word" trigger
  // auto-recover on X3+BLE because word-select mode entry itself consumes
  // ~23 KB of maxAlloc, leaving <22 KB (or <16 KB) at lookup time -- an
  // untenable UX regression vs v241, where the guard was absent and users
  // could sustain 6+ lookups before the occasional big-entry crash. The
  // occasional crash-to-Home on genuinely-oversized entries is worse than
  // "constant restart friction" only when it happens; users tolerate
  // rare crashes better than pervasive restarts. Real fix for the crash
  // class is BT-off-by-default (recovers ~20 KB, lets wrap+render fit
  // without needing aggressive guarding).
  defTargetWord_ = word;
  defLines_.clear();
  defLines_.shrink_to_fit();
  // v18.9.9.198: drop any prior lookup's entries before starting a new one.
  defEntryStreams_.clear();
  defEntryStreams_.shrink_to_fit();
  currentEntryIndex_ = 0;
  defScrollOffset_ = 0;
  defOverlayNotFound_ = false;
  defOverlayLowMemory_ = false;
  defOverlayLoading_ = true;
  // Capture the current word-selection screen so we can paint the popup
  // on top without losing the underlying frame. storeBwBuffer compresses
  // with packbits (2-5 KB for a text page), so this is cheap even on a
  // fragmented post-BT heap.
  defOverlayCaptureValid_ = renderer.storeBwBuffer();
  if (!defOverlayCaptureValid_) {
    LOG_INF("DICT", "openDefinitionOverlay: storeBwBuffer failed; overlay will repaint via word-select re-render on dismiss");
  }
  defOverlay_ = true;
  // Render the "looking up" popup state first so the user sees instant feedback,
  // then perform the synchronous lookup, then re-render with the result.
  requestUpdate();
  performDefinitionLookup();
  // v18.9.9.175: reverted v171 post-wrap check and v173 auto-restart. The
  // post-wrap check tripped at post-restart maxAlloc=15K which was actually
  // fine to render. The auto-restart's isContinuingFromSilentReboot guard
  // fired AFTER clearSilentRebootContinuationFlag() at line 636 so it
  // couldn't tell we'd just restarted -- caused infinite restart loop.
  // Restored original flow: user dismisses Low-memory popup on failure and
  // the closeDefinitionOverlay guard handles the silent-restart cleanly.
  defOverlayLoading_ = false;
  requestUpdate();
}

void DictionaryWordSelectActivity::performDefinitionLookup() {
  Dictionary::clearLastRefusedDueToMemory();
  // v18.9.9.247: fetch entry HANDLES (offset+size only) instead of full
  // data. Then loadChunkForCurrentEntry loads the first 3 KB of the first
  // entry on-demand. Multi-entry cycling reloads the target entry's chunk
  // to 0 in the Left/Right handler.
  auto handles = Dictionary::lookupAllHandles(defTargetWord_);
  if (handles.empty()) {
    auto stems = Dictionary::getStemVariants(defTargetWord_);
    for (const auto& stem : stems) {
      handles = Dictionary::lookupAllHandles(stem);
      if (!handles.empty()) {
        defTargetWord_ = stem;
        break;
      }
    }
  }
  Dictionary::freeMemory();
  defEntryStreams_.clear();
  for (const auto& h : handles) {
    defEntryStreams_.push_back(EntryStream{h.offset, h.size, 0u, 0u, {}});
  }
  if (defEntryStreams_.empty()) {
    // v18.9.9.237: distinguish "actually not found" from "readDefinition
    // refused due to memory." The user's field log showed several
    // consecutive lookups landing in the refuse path on tight heap while
    // the popup misleadingly said "Word not found" -- they thought the
    // dict was missing common words. Arm the same auto-recover flow
    // wrapDefinition's guard uses.
    if (Dictionary::wasLastRefusedDueToMemory()) {
      defOverlayLowMemory_ = true;
      if (!sessionBornFromRestart_) {
        LOG_ERR("DICT",
                "performDefinitionLookup: readDefinition refused for memory; arming auto-recover on '%s'",
                defTargetWord_.c_str());
        pendingLowMemoryAutoRestart_ = true;
        lowMemoryPopupShownAtMs_ = millis();
        lowMemoryRestartWord_ = defTargetWord_;
      } else {
        LOG_INF("DICT", "performDefinitionLookup: readDefinition refused post-restart; NOT re-arming (would loop)");
      }
    } else {
      defOverlayNotFound_ = true;
    }
    return;
  }
  // v18.9.9.246: successful lookup clears the post-restart flag so a later
  // heavier word CAN arm auto-recover fresh.
  sessionBornFromRestart_ = false;
  currentEntryIndex_ = 0;
  LookupHistory::addWord(cachePath, defTargetWord_);
  // v18.9.9.247: load the first chunk of the first entry. If load fails
  // (heap tight for even a 3 KB range read), the readDef refuse path in
  // loadChunkForCurrentEntry sets defOverlayLowMemory_ and arms recovery.
  //
  // v18.9.9.249: if the ERA post-boot dispatch handed us a chunk-start
  // offset (from silentRestartToReaderWithDefinitionAtChunk), open at
  // that chunk instead of chunk 0. Clamp to the first entry's totalSize
  // so a bogus offset (word matched a different-length entry after the
  // restart -- unlikely but not impossible) doesn't push us past EOF.
  uint32_t initialChunk = 0u;
  if (pendingDefinitionChunkStart_ != 0u && !defEntryStreams_.empty()) {
    const uint32_t totalSize = defEntryStreams_[0].totalSize;
    if (pendingDefinitionChunkStart_ < totalSize) {
      initialChunk = pendingDefinitionChunkStart_;
      LOG_INF("DICT", "performDefinitionLookup: opening at chunkStart=%u (from post-restart)",
              initialChunk);
    } else {
      LOG_INF("DICT",
              "performDefinitionLookup: pendingDefinitionChunkStart_=%u >= totalSize=%u; using 0",
              pendingDefinitionChunkStart_, totalSize);
    }
    pendingDefinitionChunkStart_ = 0u;
  }
  loadChunkForCurrentEntry(initialChunk);
}

bool DictionaryWordSelectActivity::loadChunkForCurrentEntry(uint32_t chunkStart) {
  if (currentEntryIndex_ < 0 ||
      currentEntryIndex_ >= static_cast<int>(defEntryStreams_.size())) {
    return false;
  }
  EntryStream& es = defEntryStreams_[currentEntryIndex_];
  if (chunkStart >= es.totalSize) return false;
  const uint32_t remaining = es.totalSize - chunkStart;
  const uint32_t bytesToRead = remaining < DEF_CHUNK_SIZE ? remaining : DEF_CHUNK_SIZE;
  // v18.9.9.248: chunk-transition guard. This branch fires when Up/Down at
  // a chunk boundary triggers loading the next/previous chunk. The wrap
  // formula in wrapDefinition predicts ~10 KB for a 3 KB chunk, but the
  // v247 field crash on 'lie' at maxAlloc=15860 showed actual wrap+render
  // peak is closer to ~15 KB on tight heap (wrap itself ~10 KB, then
  // renderer's per-glyph SD loads and drawText temp buffers push another
  // ~5 KB). If we let the load through and wrap runs, the current-chunk
  // defLines_ have already been cleared -- terminate handler then kicks
  // to Home. Guard here BEFORE clearing so if we refuse, the current
  // chunk stays visible and the user can Back out cleanly.
  //
  // Only fires for chunk navigation (chunkStart > 0 OR loading a chunk
  // whose start doesn't match what we already have). For the initial
  // load path (called from performDefinitionLookup / Left+Right entry
  // cycling), the caller has already cleared defLines_ and the standard
  // wrapDefinition guard handles refuse + auto-recover.
  // v18.9.9.250: revert v249's silent-restart-to-chunk. The
  // silentRestartToReaderWithDefinitionAtChunk path auto-opened the
  // definition overlay post-boot, and the pre-restart framebuffer
  // (word-select page + selection cursor + hint text) got carried into
  // the RTC sleep-frame snapshot -- packbits truncated on write, then
  // the reader page's first paint post-boot ran the grayscale AA
  // storeBwBuffer/restoreBwBuffer cycle which decoded past the end of
  // the truncated backup -> heap poisoning -> panic. This is the same
  // class of bug that killed the v215 / v228 close-guard restart
  // machinery; the v246 cursor-word restart survives it only because
  // post-boot lands in cursor-only mode with no overlay auto-open.
  //
  // Behavior for v250: chunk-transition refuse just returns false and
  // keeps the current chunk visible. User Backs out normally when they
  // hit the wall. Initial-load refuse falls through to wrapDefinition's
  // existing wrap-only guard (armed cursor-word restart, same as v247).
  //
  // Threshold lowered 18 KB -> 15 KB: post-tap wrapDefinition itself
  // fragments the heap down to 15-17 KB maxAlloc (observed on tight
  // post-BT-off heap), and 18 KB refused nearly every chunk transition,
  // making the reader unusable for long entries. 15 KB catches the
  // observed crash class (v247 crashed at 15860 during wrap+render of
  // a 3 KB chunk) with minimal room to spare; on tight sessions the
  // second chunk is best-effort.
  if (bytesToRead == DEF_CHUNK_SIZE) {
    constexpr uint32_t CHUNK_WRAP_PLUS_RENDER_MAX_ALLOC = 15u * 1024u;
    const bool isTransition = !es.content.empty() && chunkStart != es.bufferRawStart;
    if (isTransition && ESP.getMaxAllocHeap() < CHUNK_WRAP_PLUS_RENDER_MAX_ALLOC) {
      LOG_INF("DICT",
              "loadChunkForCurrentEntry: refusing chunk-transition to %u (maxAlloc=%u < 15 KB); "
              "staying on current chunk (user Backs out)",
              chunkStart, ESP.getMaxAllocHeap());
      return false;
    }
  }
  std::string content = Dictionary::readDefinitionRange(es.offset, es.totalSize, chunkStart, bytesToRead);
  if (content.empty() && bytesToRead > 0) {
    // Load failed. If Dictionary marked it as memory-refused, arm the same
    // auto-recover as v237's readDef-refuse path -- else treat as IO error.
    if (Dictionary::wasLastRefusedDueToMemory()) {
      defOverlayLowMemory_ = true;
      if (!sessionBornFromRestart_) {
        LOG_ERR("DICT", "loadChunkForCurrentEntry: refused for memory; arming auto-recover on '%s'",
                defTargetWord_.c_str());
        pendingLowMemoryAutoRestart_ = true;
        lowMemoryPopupShownAtMs_ = millis();
        lowMemoryRestartWord_ = defTargetWord_;
      }
    } else {
      defOverlayNotFound_ = true;
    }
    return false;
  }
  es.bufferRawStart = chunkStart;
  es.bufferRawEnd = chunkStart + bytesToRead;  // raw bytes consumed, not post-strip
  es.content = std::move(content);
  wrapDefinition(es.content);
  return true;
}

void DictionaryWordSelectActivity::wrapDefinition(const std::string& definition) {
  // v18.9.9.246: retuned formula from v241/v244's 1.4x + 3K to 1.4x + 6K.
  // v245 BT-off didn't recover enough heap for large entries; v244's formula
  // let 'light' (10.7 KB) proceed past the guard at maxAlloc ~20 KB, wrap
  // allocated ~14 KB, and hit terminate at maxAlloc=4852. Bumping the fixed
  // overhead from 3 KB to 6 KB raises the 'light' threshold to ~21 KB --
  // refuses cleanly instead of crashing. Cost: small entries still fine
  // (10 KB floor unchanged); some medium entries (5-7 KB) refuse where
  // they would have passed on v244 but succeeded barely (crash risk).
  const uint32_t defSize = static_cast<uint32_t>(definition.size());
  const uint32_t wrapNeeded = (defSize * 14u / 10u) + 6u * 1024u;
  const uint32_t threshold = wrapNeeded > 10u * 1024u ? wrapNeeded : 10u * 1024u;
  if (ESP.getMaxAllocHeap() < threshold) {
    LOG_ERR("DICT",
            "wrapDefinition: maxAlloc=%u too low (< %u for defSize=%u); Low-memory popup",
            ESP.getMaxAllocHeap(), threshold, defSize);
    defOverlayLowMemory_ = true;
    // v18.9.9.246: gate auto-recover arming on sessionBornFromRestart_.
    // If this session was already born from a silent-restart-with-cursor,
    // 'light' STILL didn't fit -- don't loop, just show popup and let user
    // Back out to reader. Restored from v242 (was reverted in v244).
    if (!sessionBornFromRestart_) {
      pendingLowMemoryAutoRestart_ = true;
      lowMemoryPopupShownAtMs_ = millis();
      lowMemoryRestartWord_ = defTargetWord_;
    } else {
      LOG_INF("DICT", "wrapDefinition: refused post-restart; NOT re-arming (would loop)");
    }
    return;
  }
  // CrumBLE 4.4 post-bisect: kindle-style bottom-pinned popup. The popup
  // occupies ~45% of screen height at the bottom; the top half of the
  // book page stays visible (via the restored BW buffer) so the user
  // keeps surrounding context while reading the definition.
  const int kPopupHorizMargin = 16;
  const int kPopupBottomMargin = 60;  // leave room for button hints below
  const int kPopupPadding = 12;
  const int popupMaxWidth = renderer.getScreenWidth() - (kPopupHorizMargin * 2) - (kPopupPadding * 2);
  // v18.9.9.236: prewarm SD-font glyphs for every codepoint in the definition
  // BEFORE measurement + wrap. Ported from CrossPoint's
  // DictionaryDefinitionActivity::wrapText.
  //
  // v18.9.9.238: guard with a heap check. Prewarm loads glyphs in a big
  // batch (50-200 B per novel codepoint × 60+ codepoints for a
  // Wiktionary IPA/pronunciation entry = 5-12 KB of new cache slots).
  // On tight heap that was the crash suspect for "lies-kicks-to-Home"
  // (v236 field: maxAlloc dropped 15 KB -> 1.4 KB in 776 ms during
  // lookup). When heap is thin, skip the batch and let wrap fall back
  // to on-demand per-glyph loads -- same total memory, but paced so
  // the allocator can coalesce/evict between allocations.
  if (ESP.getMaxAllocHeap() >= 20 * 1024) {
    renderer.ensureSdCardFontReady(BITTER_12_FONT_ID, definition.c_str(), 0x01 /* REGULAR */);
  } else {
    LOG_INF("DICT", "wrapDefinition: skipping prewarm (maxAlloc=%u < 20 KB); falling back to on-demand glyph loads",
            ESP.getMaxAllocHeap());
  }
  // v18.9.9.248: clear before append. Chunk transitions (Up/Down at
  // chunk boundary) call wrapDefinition on a new chunk without clearing
  // defLines_ at the callsite -- v247 accumulated old + new chunk lines
  // in defLines_, doubling then tripling memory over successive chunk
  // loads and eventually rendering wrong text (still showing chunk 1's
  // first page after Down had loaded chunk 2). Clearing here also
  // shrinks the vector: if the previous chunk had 40 lines and the new
  // chunk has 20, capacity was 40 -- shrink_to_fit keeps peak proportional
  // to current chunk, not the largest chunk seen this session.
  defLines_.clear();
  defLines_.shrink_to_fit();
  defLines_.reserve(20);
  // v18.9.9.241: manual paragraph walk instead of std::stringstream +
  // std::getline. Stringstream's copy-into-internal-buffer approach
  // doubles the definition's transient heap footprint -- pure waste on
  // tight heap. Walk the source directly, extract each paragraph into a
  // single reusable temp string (one alloc, grows in place), feed to
  // wrappedText. Peak transient during wrap: one paragraph, not the
  // whole definition.
  std::string paragraph;
  paragraph.reserve(128);
  const size_t n = definition.size();
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i == n || definition[i] == '\n') {
      if (i == start) {
        defLines_.push_back("");
      } else {
        paragraph.assign(definition, start, i - start);
        auto pLines = renderer.wrappedText(BITTER_12_FONT_ID, paragraph.c_str(), popupMaxWidth, 1000);
        for (auto& line : pLines) defLines_.push_back(std::move(line));
      }
      start = i + 1;
    }
  }
  const int lineHeight = renderer.getLineHeight(BITTER_12_FONT_ID);
  const int titleH = renderer.getLineHeight(BITTER_12_FONT_ID);
  const int popupHeight = (renderer.getScreenHeight() * 45) / 100;  // ~45% of screen
  const int popupInnerHeight = popupHeight - (kPopupPadding * 2) - titleH - 6;
  defLinesPerPage_ = std::max(1, popupInnerHeight / lineHeight);
  defMaxScroll_ = std::max(0, static_cast<int>(defLines_.size()) - defLinesPerPage_);
}

void DictionaryWordSelectActivity::renderDefinitionOverlay() {
  // Restore the captured selection screen so this render lands on top of the
  // user's pre-overlay context. If capture failed (heap too tight for the
  // compressed BW backup), DON'T clearScreen -- that would wipe the
  // underlying selection page to white. Instead, paint the popup over the
  // current framebuffer, which still holds the selection screen from the
  // previous render. The book text stays visible above the popup; the only
  // cost is potential pixel artifacts inside the popup region if the user
  // scrolls (since each scroll paints over the previous popup state without
  // a clean restore). Acceptable -- much better than a white page.
  if (defOverlayCaptureValid_) {
    renderer.restoreBwBuffer();
  }

  // CrumBLE 4.4 post-bisect: kindle-style bottom-pinned popup so the top
  // half of the book page (restored via the BW buffer above) remains
  // visible behind the definition. White fill inside the popup keeps the
  // definition text legible; thick top border draws the eye to the new
  // surface without losing reading context.
  const int kPopupHorizMargin = 16;
  const int kPopupBottomMargin = 60;  // leaves room for button hints below
  const int kPopupPadding = 12;
  const int popupW = renderer.getScreenWidth() - (kPopupHorizMargin * 2);
  const int popupH = (renderer.getScreenHeight() * 45) / 100;
  const int popupX = kPopupHorizMargin;
  const int popupY = renderer.getScreenHeight() - kPopupBottomMargin - popupH;

  // White fill so the definition text is legible; the page text above the
  // popup is preserved by the restoreBwBuffer earlier in this render.
  renderer.fillRect(popupX, popupY, popupW, popupH, false);              // white fill
  renderer.fillRect(popupX, popupY, popupW, 3, true);                    // thicker top border (eye-draw)
  renderer.fillRect(popupX, popupY + popupH - 2, popupW, 2, true);       // bottom
  renderer.fillRect(popupX, popupY, 2, popupH, true);                    // left
  renderer.fillRect(popupX + popupW - 2, popupY, 2, popupH, true);       // right

  const int textX = popupX + kPopupPadding;
  int currentY = popupY + kPopupPadding;

  if (defOverlayLoading_) {
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, tr(STR_LOOKING_UP));
  } else if (defOverlayLowMemory_) {
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, defTargetWord_.c_str(), EpdFontFamily::BOLD);
    currentY += renderer.getLineHeight(BITTER_12_FONT_ID) * 2;
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, "Low memory -- back out");
    currentY += renderer.getLineHeight(BITTER_12_FONT_ID);
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, "and reopen Lookup");
  } else if (defOverlayNotFound_) {
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, defTargetWord_.c_str(), EpdFontFamily::BOLD);
    currentY += renderer.getLineHeight(BITTER_12_FONT_ID) * 2;
    // v18.9.9.233: the word is already drawn bold above, so we don't need
    // to concatenate it after "Word not found" -- pre-v233 that produced
    // no-space runs like "Word not foundlight".
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, tr(STR_WORD_NOT_FOUND));
  } else {
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, defTargetWord_.c_str(), EpdFontFamily::BOLD);
    const int titleH = renderer.getLineHeight(BITTER_12_FONT_ID);
    const int titleWidth = renderer.getTextWidth(BITTER_12_FONT_ID, defTargetWord_.c_str(), EpdFontFamily::BOLD);
    renderer.fillRect(textX, currentY + titleH + 4, titleWidth, 2, true);
    // v18.9.9.198: multi-entry badge "2/3" right-aligned on the title row when
    // the word has >1 entry. Silent on single-entry lookups so the common case
    // stays uncluttered.
    if (defEntryStreams_.size() > 1) {
      char badge[16];
      snprintf(badge, sizeof(badge), "%d/%d", currentEntryIndex_ + 1,
               static_cast<int>(defEntryStreams_.size()));
      const int badgeW = renderer.getTextWidth(BITTER_12_FONT_ID, badge);
      const int overlayRight = popupX + popupW - kPopupPadding;
      renderer.drawText(BITTER_12_FONT_ID, overlayRight - badgeW, currentY, badge);
    }
    currentY += titleH + (kPopupPadding);
    const int lineHeight = renderer.getLineHeight(BITTER_12_FONT_ID);
    const int linesToDraw = std::min(defLinesPerPage_, static_cast<int>(defLines_.size()) - defScrollOffset_);
    for (int i = 0; i < linesToDraw; ++i) {
      renderer.drawText(BITTER_12_FONT_ID, textX, currentY, defLines_[defScrollOffset_ + i].c_str());
      currentY += lineHeight;
    }
    if (defScrollOffset_ > 0) {
      renderer.drawText(BITTER_12_FONT_ID, popupX + popupW - kPopupPadding - 12, popupY + kPopupPadding, "^");
    }
    if (defScrollOffset_ < defMaxScroll_) {
      renderer.drawText(BITTER_12_FONT_ID, popupX + popupW - kPopupPadding - 12,
                        popupY + popupH - kPopupPadding - lineHeight, "v");
    }
  }

  // CrumBLE 4.4 post-bisect: clear the bottom button-hints strip BEFORE drawing
  // the overlay's hints. The word-select activity's full set of hints
  // ("Lookup / Prev / Next" etc.) is still in the framebuffer underneath the
  // popup (the popup is positioned with a 60px bottom margin so the hints
  // area isn't covered). Without the wipe, those old labels remain visible
  // alongside our overlay's "Back" hint.
  //
  // Hardware Up/Down already scroll the definition, so the overlay only needs
  // a Back hint -- the rest of the slots stay blank.
  const int hintsStripH = 50;
  const int hintsStripY = renderer.getScreenHeight() - hintsStripH;
  // Wipe the strip in the current page polarity so dark mode doesn't get a
  // white flash under the hint buttons. The buttons themselves invert via
  // the darkMode arg passed to drawButtonHints below.
  const bool darkMode = ReaderUtils::readerDarkModeEnabled();
  renderer.fillRect(0, hintsStripY, renderer.getScreenWidth(), hintsStripH, darkMode);
  // v18.9.9.296: when the word has multiple StarDict entries (the top-right
  // "1/2" indicator is visible), also show Prev/Next hints on the physical
  // rocker slots. Users otherwise didn't know they could cycle. Hints only
  // appear when there's actually somewhere to move -- Prev grays out at
  // entry 0, Next at the last entry.
  const bool multiEntry = defEntryStreams_.size() > 1;
  const char* prevLabel = (multiEntry && currentEntryIndex_ > 0) ? tr(STR_PREV) : "";
  const char* nextLabel =
      (multiEntry && currentEntryIndex_ + 1 < static_cast<int>(defEntryStreams_.size())) ? tr(STR_NEXT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4,
                       /*allowInvertedText=*/false, darkMode);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void DictionaryWordSelectActivity::closeDefinitionOverlay() {
  defOverlay_ = false;
  defOverlayLoading_ = false;
  defOverlayNotFound_ = false;
  defOverlayLowMemory_ = false;
  defLines_.clear();
  defLines_.shrink_to_fit();
  // v18.9.9.198: release all entries -- they were only kept alive so the user
  // could Left/Right-cycle within this overlay session. Freeing them on close
  // recovers up to ~6× the single-entry byte cost as heap for the next lookup.
  defEntryStreams_.clear();
  defEntryStreams_.shrink_to_fit();
  currentEntryIndex_ = 0;
  defScrollOffset_ = 0;
  // CrumBLE 4.4 post-bisect: aggressive cleanup to keep heap fragmentation
  // from accumulating across consecutive lookups. wrappedText leaves a
  // forest of small std::string allocations even after defLines_ is freed;
  // dropping the font cache + dictionary state forces the heap allocator
  // to coalesce neighbors and recover larger contiguous spans.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  Dictionary::freeMemory();
  // v18.9.9.225: RESTORE ALWAYS. v215's wholesale skip-visual-dismiss
  // path caused heap corruption on X3: pre-restart framebuffer with the
  // def box was carried through reboot by the sleep-frame snapshot
  // pipeline (which also re-seeds the RAM framebuffer post-boot), so
  // post-boot openDefinitionOverlay's storeBwBuffer captured that
  // composite (>32 KB packbits budget -> silent truncation), and the
  // next closeDefinitionOverlay's restoreBwBuffer decoded past the end
  // -> heap poisoning -> panic.
  //
  // v18.9.9.228 tried to split by capture-validity to reintroduce the
  // "hidden restart" only when storeBwBuffer had FAILED at open (thinking
  // no packbits backup could get truncated). v229 crash confirmed that
  // reasoning wrong: the RAM framebuffer state (def box + word-select)
  // still survives reboot regardless of whether WE captured a backup,
  // so post-boot's OWN storeBwBuffer still traps into the >32 KB packbits
  // path and truncates. Reverted here as of v231. The correct fix for
  // the "no longer very hidden" symptom is downstream (see the deferred
  // delay extension below).
  // v18.9.9.232: dismiss is one path now -- restore the packbits capture if
  // we have one, else re-render word-select. No deferred-restart arming.
  // See loop()'s header comment for the crash history that motivated the
  // revert.
  if (defOverlayCaptureValid_) {
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    defOverlayCaptureValid_ = false;
  } else {
    requestUpdate();
  }
  // v18.9.9.242: DO NOT reset sessionBornFromRestart_ here. It persists
  // through this session's overlay open/close cycles so subsequent lookups
  // that also refuse won't infinite-loop the auto-recover. Reset happens
  // only after a SUCCESSFUL lookup (in performDefinitionLookup) or when the
  // activity is destructed and a fresh word-select session starts.
}