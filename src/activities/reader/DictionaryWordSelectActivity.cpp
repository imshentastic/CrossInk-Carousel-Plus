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
          words.back().lookupText = lookup;
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
      for (size_t i = 0; i < words.size(); ++i) {
        if (words[i].lookupText == pendingDefinitionWord_) {
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
    if (defMaxScroll_ > 0) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up) && defScrollOffset_ > 0) {
        defScrollOffset_ = std::max(0, defScrollOffset_ - std::max(1, defLinesPerPage_ - 1));
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) && defScrollOffset_ < defMaxScroll_) {
        defScrollOffset_ = std::min(defMaxScroll_, defScrollOffset_ + std::max(1, defLinesPerPage_ - 1));
        requestUpdate();
      }
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
        openDefinitionOverlay(words[selectedWordIdx].lookupText);
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
      std::string merged = words[i].lookupText + words[i + 1].lookupText;
      words[i].lookupText = merged;
      words[i + 1].lookupText = merged;
      words[i].continuationIndex = static_cast<int>(i + 1);
      words[i + 1].continuationOf = static_cast<int>(i);
    }
  }
}

// CrumBLE 4.4 post-bisect: inline definition overlay implementation. Replaces
// the prior DictionaryDefinitionActivity push pattern -- now the popup draws
// on top of the captured word-selection screen and dismissal restores the
// underlying frame via the renderer's BW backup (~2-5 KB packbits-compressed).
// User's selection cursor is preserved across the lookup.

void DictionaryWordSelectActivity::openDefinitionOverlay(const std::string& word) {
  // CrumBLE 4.4 post-bisect: NO preemptive pre-flight gate. We try the lookup
  // first and only silent-restart when wrap actually fails. This keeps
  // common-case lookups fast (no unnecessary restarts) AND lets the silent-
  // restart-with-word recovery path kick in for the genuinely heap-starved case.
  // See wrapDefinition() for the recovery trigger.
  clearSilentRebootContinuationFlag();
  defTargetWord_ = word;
  defLines_.clear();
  defLines_.shrink_to_fit();
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
  defOverlayLoading_ = false;
  requestUpdate();
}

void DictionaryWordSelectActivity::performDefinitionLookup() {
  std::string definition = Dictionary::lookup(defTargetWord_);
  if (definition.empty()) {
    auto stems = Dictionary::getStemVariants(defTargetWord_);
    for (const auto& stem : stems) {
      definition = Dictionary::lookup(stem);
      if (!definition.empty()) {
        defTargetWord_ = stem;
        break;
      }
    }
  }
  Dictionary::freeMemory();
  if (definition.empty()) {
    defOverlayNotFound_ = true;
    return;
  }
  LookupHistory::addWord(cachePath, defTargetWord_);
  wrapDefinition(definition);
}

void DictionaryWordSelectActivity::wrapDefinition(const std::string& definition) {
  // CrumBLE 4.4 post-bisect: NO silent-restart from this path. Heap corruption
  // crashes were repeatedly triggered by the silent-restart-into-LOOKUP-with-
  // word flow (multi_heap_free assert during post-boot dispatch's LOOKUP
  // setup). Instead, show the "Low memory" message and let the user back out
  // through the existing flow: dismiss popup -> back to text-selection ->
  // back to reader -> re-invoke Lookup. The Lookup entry's pre-flight gate
  // (already battle-tested) handles the silent-restart, opening text-selection
  // fresh -- one extra step for the user, but reliably crash-free.
  if (ESP.getMaxAllocHeap() < 10 * 1024) {
    LOG_ERR("DICT",
            "wrapDefinition: maxAlloc=%u too low (< 10 KB); showing Low-memory popup "
            "(user dismisses + re-invokes Lookup to silent-restart cleanly)",
            ESP.getMaxAllocHeap());
    defOverlayLowMemory_ = true;
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
  defLines_.reserve(20);
  std::stringstream ss(definition);
  std::string paragraph;
  while (std::getline(ss, paragraph, '\n')) {
    if (paragraph.empty()) {
      defLines_.push_back("");
      continue;
    }
    auto pLines = renderer.wrappedText(BITTER_12_FONT_ID, paragraph.c_str(), popupMaxWidth, 1000);
    for (auto& line : pLines) defLines_.push_back(std::move(line));
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
    std::string msg = std::string(tr(STR_WORD_NOT_FOUND)) + defTargetWord_;
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, msg.c_str());
  } else {
    renderer.drawText(BITTER_12_FONT_ID, textX, currentY, defTargetWord_.c_str(), EpdFontFamily::BOLD);
    const int titleH = renderer.getLineHeight(BITTER_12_FONT_ID);
    const int titleWidth = renderer.getTextWidth(BITTER_12_FONT_ID, defTargetWord_.c_str(), EpdFontFamily::BOLD);
    renderer.fillRect(textX, currentY + titleH + 4, titleWidth, 2, true);
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
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
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
  // Restore the captured selection screen and FAST-refresh -- the user sees
  // their cursor exactly where it was. If capture was invalid, requestUpdate
  // falls back to the full word-select render.
  if (defOverlayCaptureValid_) {
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    defOverlayCaptureValid_ = false;
  } else {
    requestUpdate();
  }
  // CrumBLE 4.4 post-bisect: post-dismiss silent-restart at a clean
  // checkpoint. closeDefinitionOverlay just finished aggressive cleanup
  // (font cache + dictionary memory freed, overlay state cleared) so heap
  // is in the safest state we can reach without an actual restart. If
  // MaxAlloc is still below a healthy floor, silent-restart with OpenLookup
  // now -- before the user can fire another lookup that would either fail
  // mid-flow (the buggy path) or show "Low memory". Post-boot opens
  // text-selection on a fresh ~115 KB heap; cursor is reset to default
  // (a known trade-off, but predictable and crash-free).
  // CrumBLE 4.4 post-bisect: 13 KB threshold (just above the 10 KB wrap floor
  // with ~3 KB margin). Lower than the original 22 KB to let users get 2-3
  // lookups between restarts instead of one. Trade-off: tighter per-lookup
  // heap, so closer to wrap-fail risk -- if wrap-fail rate climbs, revert
  // to 22 KB or land somewhere between (e.g. 16 KB).
  constexpr uint32_t LOOKUP_POST_DISMISS_MIN_MAX_ALLOC = 13000;
  if (ESP.getMaxAllocHeap() < LOOKUP_POST_DISMISS_MIN_MAX_ALLOC && !isContinuingFromSilentReboot()) {
    LOG_INF("DICT",
            "closeDefinitionOverlay: maxAlloc=%u below %u; silent-restart with OpenLookupAtWord('%s') "
            "to give next lookup a fresh heap AND preserve cursor on this word",
            ESP.getMaxAllocHeap(), LOOKUP_POST_DISMISS_MIN_MAX_ALLOC, defTargetWord_.c_str());
    silentRestartToReaderWithCursorWord(defTargetWord_.c_str());
  }
}