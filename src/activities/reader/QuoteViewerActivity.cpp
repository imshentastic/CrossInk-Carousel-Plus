#include "QuoteViewerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Inter built-in. Compressed-font interval table is always resident, so
// the viewer is immune to the SD-font prewarm-cache wipe that bites
// DictionaryWordSelectActivity / DictionaryDefinitionActivity. We pick
// UI_12 for readability over UI_10 -- a quote viewer should feel
// roomier than a settings row.
constexpr int kBodyFontId = UI_12_FONT_ID;
constexpr int kHeaderFontId = UI_12_FONT_ID;
}  // namespace

QuoteViewerActivity::QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::vector<Bookmark> bookmarks, int initialIndex)
    : Activity("QuoteViewer", renderer, mappedInput),
      bookmarks_(std::move(bookmarks)),
      currentIndex_(initialIndex < 0 ? 0 : initialIndex) {
  if (currentIndex_ >= static_cast<int>(bookmarks_.size())) {
    currentIndex_ = static_cast<int>(bookmarks_.size()) - 1;
  }
}

void QuoteViewerActivity::onEnter() {
  Activity::onEnter();
  if (bookmarks_.empty()) {
    // Nothing to show -- bail back to caller as a cancel so they don't
    // get a blank screen. The list activity shouldn't have launched us
    // with an empty bookmark vector, but defensive guard.
    ActivityResult result;
    result.isCancelled = true;
    result.data = QuoteViewerExitResult{currentIndex_};
    setResult(std::move(result));
    finish();
    return;
  }
  rewrapForCurrent();
  requestUpdate();
}

void QuoteViewerActivity::rewrapForCurrent() {
  wrappedLines_.clear();
  pageOffset_ = 0;

  if (bookmarks_.empty() || currentIndex_ < 0 || currentIndex_ >= static_cast<int>(bookmarks_.size())) return;

  const Bookmark& bm = bookmarks_[currentIndex_];
  // Empty preview = legacy v3-format bookmark (pre-highlight) that never
  // had selected text. Show a placeholder so the viewer still renders
  // chapter context + "no preview available" without crashing.
  const char* preview = !bm.preview.empty() ? bm.preview.c_str() : "[no preview saved for this bookmark]";

  const int sidePad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int contentWidth = renderer.getScreenWidth() - sidePad * 2;
  // Wrap to a large cap; we'll paginate via pageOffset_ below. 1000 lines
  // is wildly more than any 160-char preview will produce, and the wrap
  // helper bails at the cap rather than allocating unbounded.
  wrappedLines_ = renderer.wrappedText(kBodyFontId, preview, contentWidth, 1000);

  lineHeight_ = renderer.getLineHeight(kBodyFontId);

  // Reserve space for the header (chapter + N/M counter) and the button
  // hints row. Body fills whatever's left.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topReserve = metrics.topPadding + renderer.getLineHeight(kHeaderFontId) * 2 + metrics.verticalSpacing * 2;
  const int bottomReserve = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int bodyHeight = renderer.getScreenHeight() - topReserve - bottomReserve;

  linesPerPage_ = (lineHeight_ > 0) ? std::max(1, bodyHeight / lineHeight_) : 1;
}

void QuoteViewerActivity::loop() {
  if (bookmarks_.empty()) return;
  using B = MappedInputManager::Button;

  if (mappedInput.wasReleased(B::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = QuoteViewerExitResult{currentIndex_};
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(B::Confirm)) {
    // Jump to the bookmark location in the book. The list activity
    // forwards this result to its own caller (the reader), which jumps.
    const Bookmark& bm = bookmarks_[currentIndex_];
    BookmarkResult br;
    br.spineIndex = bm.spineIndex;
    br.progress = bm.progress;
    setResult(ActivityResult{br});
    finish();
    return;
  }

  // Up / Down: walk bookmarks. Wrap around to mirror the list activity's
  // ButtonNavigator behavior so the viewer feels like a continuation of
  // the list, not a separate input model.
  const int total = static_cast<int>(bookmarks_.size());
  if (mappedInput.wasReleased(B::Up)) {
    currentIndex_ = (currentIndex_ - 1 + total) % total;
    rewrapForCurrent();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Down)) {
    currentIndex_ = (currentIndex_ + 1) % total;
    rewrapForCurrent();
    requestUpdate();
    return;
  }

  // Left / Right: page through the current quote. No-op when the quote
  // fits on one screen (true for every BOOKMARK_PREVIEW_MAX=160 preview
  // at UI_12; wired up so future longer-quote storage doesn't need to
  // revisit input).
  if (mappedInput.wasReleased(B::Left)) {
    if (pageOffset_ > 0) {
      pageOffset_ = std::max(0, pageOffset_ - linesPerPage_);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(B::Right)) {
    const int maxOffset = std::max(0, static_cast<int>(wrappedLines_.size()) - linesPerPage_);
    if (pageOffset_ < maxOffset) {
      pageOffset_ = std::min(maxOffset, pageOffset_ + linesPerPage_);
      requestUpdate();
    }
    return;
  }
}

void QuoteViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (bookmarks_.empty()) {
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const Bookmark& bm = bookmarks_[currentIndex_];

  // Header row: chapter title left, "N / M" right. Same bold-Inter
  // treatment as elsewhere in the reader UI.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");
  const int headerY = metrics.topPadding + (metrics.headerHeight > 60 ? metrics.batteryBarHeight + 3 : 14);
  const char* chapter = (bm.chapterTitle[0] != '\0') ? bm.chapterTitle : tr(STR_BOOKMARKS);

  char counter[24];
  std::snprintf(counter, sizeof(counter), "%d / %d", currentIndex_ + 1, static_cast<int>(bookmarks_.size()));
  const int counterW = renderer.getTextWidth(kHeaderFontId, counter, EpdFontFamily::REGULAR);
  // Reserve battery-readout slot + the counter + a small gap on the right;
  // truncate the chapter title to whatever's left so it never overlaps
  // the counter.
  constexpr int kBatteryReserve = 90;
  constexpr int kCounterGap = 8;
  const int chapterBudget = std::max(0, pageWidth - sidePad * 2 - kBatteryReserve - counterW - kCounterGap);
  const std::string chapterTrunc = renderer.truncatedText(kHeaderFontId, chapter, chapterBudget);
  renderer.drawText(kHeaderFontId, sidePad, headerY, chapterTrunc.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(kHeaderFontId, pageWidth - sidePad - kBatteryReserve - counterW, headerY, counter, true,
                    EpdFontFamily::REGULAR);

  // Body: paginated wrapped preview. Start a hair below the header so it
  // doesn't crowd the divider line drawHeader paints.
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bodyBottomLimit = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int linesAvailable = static_cast<int>(wrappedLines_.size()) - pageOffset_;
  const int linesToDraw = std::min(linesPerPage_, std::max(0, linesAvailable));
  for (int i = 0; i < linesToDraw; ++i) {
    if (y + lineHeight_ > bodyBottomLimit) break;
    renderer.drawText(kBodyFontId, sidePad, y, wrappedLines_[pageOffset_ + i].c_str(), true, EpdFontFamily::REGULAR);
    y += lineHeight_;
  }

  // Button hints. Show a small "more pages" caret when the quote
  // overflows the current page in either direction so the L/R affordance
  // is discoverable. Up/Down always show because at least one direction
  // is always meaningful (wraparound).
  const bool hasMoreUp = pageOffset_ > 0;
  const bool hasMoreDown = pageOffset_ + linesPerPage_ < static_cast<int>(wrappedLines_.size());
  const char* leftHint = hasMoreUp ? "< Prev page" : "";
  const char* rightHint = hasMoreDown ? "Next page >" : "";
  const auto labels =
      mappedInput.mapLabels(I18N.get(StrId::STR_BACK), I18N.get(StrId::STR_OPEN), leftHint, rightHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
