#include "DictionaryDefinitionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <sstream>

#include "../../SilentRestart.h"  // CrumBLE 4.4: silent-restart on second-lookup OOM
#include "CrossPointSettings.h"
#include "DictionarySuggestionsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

DictionaryDefinitionActivity::DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::string word, std::string cachePath, int fontId)
    : Activity("DictionaryDefinition", renderer, mappedInput),
      targetWord(std::move(word)),
      cachePath(std::move(cachePath)),
      fontId(fontId) {}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
  performLookup();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  wrappedLines.clear();
  wrappedLines.shrink_to_fit();
  definition.clear();
  definition.shrink_to_fit();
}

void DictionaryDefinitionActivity::performLookup() {
  definition = Dictionary::lookup(targetWord);

  if (definition.empty()) {
    auto stems = Dictionary::getStemVariants(targetWord);
    for (const auto& stem : stems) {
      definition = Dictionary::lookup(stem);
      if (!definition.empty()) {
        targetWord = stem;
        break;
      }
    }
  }

  // FIX: As soon as we have the definition, immediately return the RAM to the system!
  Dictionary::freeMemory();

  if (definition.empty()) {
    notFound = true;
  } else {
    LookupHistory::addWord(cachePath, targetWord);
    wrapText();
  }

  isLoading = false;
  requestUpdate();
}

void DictionaryDefinitionActivity::wrapText() {
  wrappedLines.clear();

  // CrumBLE 4.4 task #51: pre-flight the wrap. Each wrapped line is a
  // std::string (~24 bytes header) and renderer.wrappedText() returns
  // a vector<string> that we then insert into wrappedLines. Both
  // allocations need contiguous heap; under post-Lookup heap pressure
  // the insert bad_allocs -> __terminate -> panic-reboot. Bail
  // gracefully with a placeholder line instead: user sees "Low memory"
  // rather than a device crash. They can press Back and re-open Lookup
  // later when the heap has recovered.
  //
  // Heap budget reference (measured via [HEAP] checkpoints, 2026-06-12):
  //   Boot entry (silent restart):  ~115 KB MaxAlloc -- fresh heap.
  //   End of setup() (pre-reader):   ~86 KB MaxAlloc -- after display +
  //                                  fonts + library scan.
  //   Reader fully loaded (first
  //   page rendered):                ~60 KB MaxAlloc -- after EPUB parse
  //                                  + first chapter build + first page.
  //   Lookup activity pushed +
  //   dictionary entry parsed:      ~30-50 KB MaxAlloc -- where wrapText
  //                                  enters. Under 8 KB the next insert
  //                                  is the crash, so that's the gate.
  //
  // CrumBLE 4.4 task #4 follow-up: under 2-bit MEDIUM atlas + a few
  // scroll/drawer round-trips, post-Lookup heap was measured at ~19 KB
  // maxAlloc -- passes the old 8 KB gate, but per-paragraph wrappedText
  // calls fragment the heap fast enough that one of the small insert()
  // grows still bad_allocs. Raise to 18 KB so the "Low memory" branch
  // triggers BEFORE we walk into the wrap loop on a heap that's already
  // doomed. A long definition typically holds ~12-15 KB of wrappedLines
  // strings plus a wrappedText scratch vector during the call -- 18 KB
  // gives a ~3 KB cushion against the next-page fragmentation step.
  if (ESP.getMaxAllocHeap() < 18 * 1024) {
    // CrumBLE 4.4 post-bisect: when the user looks up a second word (or
    // any word after the first), the dictionary cache + per-paragraph
    // wrappedText allocations have fragmented heap below the safe wrap
    // floor. Instead of showing the "Low memory" dead-end, silent-restart
    // with OpenLookup queued -- post-boot dispatch reopens the word-
    // select activity on a fresh ~115 KB heap. Skip if we're already
    // post-recovery (avoid restart loop on a genuinely starved heap).
    if (!isContinuingFromSilentReboot()) {
      LOG_INF("DICT",
              "wrapText: maxAlloc=%u too low (< 18 KB); triggering silent restart with OpenDefinition('%s')",
              ESP.getMaxAllocHeap(), targetWord.c_str());
      silentRestartToReaderWithDefinition(targetWord.c_str());
      return;
    }
    LOG_ERR("DICT", "wrapText: maxAlloc=%u too low for safe wrap even after silent restart; showing fallback line",
            ESP.getMaxAllocHeap());
    clearSilentRebootContinuationFlag();
    wrappedLines.reserve(1);
    wrappedLines.push_back("Low memory -- close other activities and try again");
    lineHeight = renderer.getLineHeight(BITTER_12_FONT_ID);
    linesPerPage = 1;
    maxScroll = 0;
    return;
  }
  // Heap is acceptable; clear continuation flag so future ops can restart.
  clearSilentRebootContinuationFlag();

  // FIX: Pre-allocate memory to avoid heap fragmentation during line wrapping
  wrappedLines.reserve(50);

  if (definition.empty()) return;

  const int margin = 20;
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  // CrumBLE 4.4 task #6: wrap + render the definition body in the
  // built-in UI font instead of the reader's SD font. The SD-font path
  // required a PrewarmScope to populate miniData for every codepoint in
  // the definition (chapter atlas doesn't cover dictionary vocabulary),
  // which fragmented heap and produced visible '?' for any glyph that
  // failed to prewarm under post-Lookup heap pressure. UI font ships
  // with full Latin coverage, allocates zero per-render miniData, and
  // is what the rest of the dictionary UI (title, hints, "Not found"
  // message) was already using -- visual consistency, not a regression.
  std::stringstream ss(definition);
  std::string paragraph;
  while (std::getline(ss, paragraph, '\n')) {
    if (paragraph.empty()) {
      wrappedLines.push_back("");
      continue;
    }

    auto pLines = renderer.wrappedText(BITTER_12_FONT_ID, paragraph.c_str(), maxWidth, 1000);
    wrappedLines.insert(wrappedLines.end(), pLines.begin(), pLines.end());
  }

  lineHeight = renderer.getLineHeight(BITTER_12_FONT_ID);

  const int titleH = renderer.getLineHeight(BITTER_12_FONT_ID);
  const int startY = margin + titleH + (margin * 2);

  const int bottomMarginForHints = 55;
  const int availableHeight = renderer.getScreenHeight() - startY - bottomMarginForHints;

  linesPerPage = availableHeight / lineHeight;
  maxScroll = std::max(0, static_cast<int>(wrappedLines.size()) - linesPerPage);
}

void DictionaryDefinitionActivity::loop() {
  if (isLoading) return;

  bool needsUpdate = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (scrollOffset > 0) {
      scrollOffset = std::max(0, scrollOffset - linesPerPage + 1);
      needsUpdate = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (scrollOffset < maxScroll) {
      scrollOffset = std::min(maxScroll, scrollOffset + linesPerPage - 1);
      needsUpdate = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (notFound) {
      // If word not found, pressing Confirm opens the Suggestions list
      startActivityForResult(
          std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, targetWord, cachePath),
          [this](const ActivityResult& result) {
            // When returning from suggestions, we finish the definition view too
            ActivityResult fwResult;
            setResult(std::move(fwResult));
            finish();
          });
      return;
    } else {
      ActivityResult result;
      setResult(std::move(result));
      finish();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    setResult(std::move(result));
    finish();
  }

  if (needsUpdate) requestUpdate();
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  // CrumBLE 4.4 post-bisect: on the post-silent-restart "Looking up..." render
  // (isLoading=true while performLookup's Dictionary::lookup is doing SD reads),
  // skip painting entirely. The panel is still showing the user's pre-restart
  // content (book page) because we restored the framebuffer at boot, and that's
  // a better visual placeholder than a "Looking up..." overlay flashing across
  // it. Once the lookup completes, performLookup sets isLoading=false and calls
  // requestUpdate, which falls through to the normal render path below with the
  // full definition.
  if (isLoading && isContinuingFromSilentReboot()) {
    return;
  }

  renderer.clearScreen();

  const int margin = 20;

  // CrumBLE 4.4 task #6: dictionary body renders entirely in the UI
  // font now. The PrewarmScope dance that lived here -- a scan pass to
  // teach FontCacheManager which codepoints to load from the SD .cpfont
  // followed by endScanAndPrewarm and a dtor-time clearCache -- is
  // gone: UI_12 is a built-in compressed font with full Latin coverage
  // and no per-render miniData allocation. Drops ~5-8 KB of heap
  // pressure off every lookup and removes the dependency on chapter
  // atlas coverage that produced '?' glyphs and mashed-word spacing
  // before this rewrite. fontId stays in the ctor signature so callers
  // don't break, but it's no longer read.

  int currentY = margin;

  if (isLoading) {
    renderer.drawText(BITTER_12_FONT_ID, margin, currentY, tr(STR_LOOKING_UP));
  } else if (notFound) {
    std::string notFoundMsg = std::string(tr(STR_WORD_NOT_FOUND)) + targetWord;
    renderer.drawText(BITTER_12_FONT_ID, margin, currentY, notFoundMsg.c_str());

    currentY += renderer.getLineHeight(BITTER_12_FONT_ID) * 2;
    renderer.drawText(BITTER_12_FONT_ID, margin, currentY, tr(STR_PRESS_CONFIRM_SUGGESTIONS));
  } else {
    renderer.drawText(BITTER_12_FONT_ID, margin, currentY, targetWord.c_str(), EpdFontFamily::BOLD);

    int titleWidth = renderer.getTextWidth(BITTER_12_FONT_ID, targetWord.c_str(), EpdFontFamily::BOLD);
    int titleH = renderer.getLineHeight(BITTER_12_FONT_ID);
    renderer.fillRect(margin, currentY + titleH + 4, titleWidth, 3, true);

    currentY += titleH + (margin * 2);

    int linesToDraw = std::min(linesPerPage, static_cast<int>(wrappedLines.size()) - scrollOffset);
    for (int i = 0; i < linesToDraw; ++i) {
      renderer.drawText(BITTER_12_FONT_ID, margin, currentY, wrappedLines[scrollOffset + i].c_str());
      currentY += lineHeight;
    }

    if (scrollOffset > 0) {
      renderer.drawText(BITTER_12_FONT_ID, renderer.getScreenWidth() - margin - 20, margin * 2, "^");
    }
    if (scrollOffset < maxScroll) {
      renderer.drawText(BITTER_12_FONT_ID, renderer.getScreenWidth() - margin - 20, renderer.getScreenHeight() - 60, "v");
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_SCROLL), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}