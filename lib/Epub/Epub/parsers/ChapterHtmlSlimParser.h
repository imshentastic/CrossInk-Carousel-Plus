#pragma once

#include <HalStorage.h>  // v20 Phase B: parseFile_ member for resumable parse
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  static constexpr uint8_t MAX_SIMPLE_TABLE_COLUMNS = 8;
  static constexpr uint16_t MAX_SIMPLE_TABLE_CELLS = 64;
  static constexpr uint16_t MAX_SIMPLE_TABLE_CELL_WORDS = 160;
  static constexpr uint8_t TABLE_CELL_PADDING = 6;

  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int strikethroughUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  uint8_t extraParagraphSpacing;
  bool forceParagraphIndents;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool guideReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;
  bool lowMemoryImageFallback = false;
  bool lowMemoryAbort = false;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
    bool hasBackgroundBlack = false, backgroundBlack = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  bool effectiveBackgroundBlack = false;

  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    std::vector<std::pair<int, FootnoteEntry>> footnotes;
    bool isHeader = false;
    uint8_t colSpan = 1;
  };

  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool hasHeaderCell = false;
    bool hasDataCell = false;
    uint16_t effectiveColumnCount = 0;
  };

  struct BufferedTable {
    BlockStyle blockStyle;
    std::vector<BufferedTableRow> rows;
    uint16_t maxCols = 0;
    uint16_t totalCells = 0;
    bool unsupported = false;
  };

  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;
  int pendingListMarkerDepth = -1;
  bool currentTableCellIsHeader = false;
  uint8_t currentTableCellColSpan = 1;
  std::unique_ptr<BufferedTable> currentTableBuffer = nullptr;
  std::vector<CssAncestorEntry> ancestorStack_;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  void updateEffectiveInlineStyle();
  bool shouldAbortForLowMemory(const char* stage);
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPartWordBuffer();
  void makePages();
  void emitHorizontalRule(const BlockStyle& blockStyle);
  void finalizeCurrentTableCell();
  void emitBufferedTableAsParagraphs(BufferedTable& table);
  void emitBufferedTableAsFragments(BufferedTable& table);
  void emitCurrentTableBuffer();
  void fallbackCurrentTableBufferToParagraphs(const char* reason);
  void fallbackCurrentTableBufferIfNeeded(const char* stage);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                                 const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                 const uint16_t viewportWidth, const uint16_t viewportHeight,
                                 const bool hyphenationEnabled, const bool bionicReadingEnabled,
                                 const bool guideReadingEnabled,
                                 const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath) {}

  ~ChapterHtmlSlimParser() = default;

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // v20 Phase B (from CrossPoint PR #2452 Smart Indexing): resumable parse
  // for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going/yield; Done: finishParse();
  //                                Error: abortParse(); }
  // Pages emit via completePageFn as they complete during parseStep(), so the
  // caller can stop once enough pages exist and resume on a later tick.
  //
  // CrumBLE preserves parseAndBuildPages as the one-shot wrapper AND keeps
  // every parse-time guard (popup tick, watchdog yield, lowMemoryAbort check,
  // per-iter DBG log). The step methods themselves are cheap — no popup/yield
  // inside — so an incremental caller can schedule its own pacing.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  // Byte progress of the in-flight parse — used by the incremental section
  // builder to estimate a still-building section's total page count.
  // Valid between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }

  void addLineToPage(std::shared_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  bool wasLowMemoryFallbackTriggered() const { return lowMemoryImageFallback; }
  bool wasLowMemoryAbortTriggered() const { return lowMemoryAbort; }

 private:
  // Resumable parse state. parseAndBuildPages() drives these internally; the
  // incremental section builder drives them across render ticks so a large
  // single chapter can yield between pages instead of blocking the UI until
  // the whole thing is laid out. xmlParser_ + parseFile_ stay alive for the
  // parse's lifetime so it can be paused and resumed at buffer boundaries.
  XML_Parser xmlParser_ = nullptr;
  FsFile parseFile_;
  uint32_t parseStartTime_ = 0;
};
