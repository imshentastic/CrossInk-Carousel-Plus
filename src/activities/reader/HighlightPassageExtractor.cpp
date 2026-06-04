#include "HighlightPassageExtractor.h"

#include <Epub.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "Epub/Page.h"
#include "Epub/Section.h"

// CrumBLE 4.1 (WIP - skeleton): extract the full text of a highlight
// passage. See header for the heap-safety contract. Pages are loaded
// sequentially via Section::loadPageFromSectionFile so peak memory
// stays bounded by one Page at a time.
//
// LIMITATIONS (documented for the user / for future-me):
//   1. startWord/endWord are page-local indices captured when the
//      highlight was created. If the user has since changed font /
//      font size / margins / orientation, today's pagination won't
//      match the original. The middle of long passages is unambiguous;
//      only the leading / trailing words can drift. Acceptable for
//      v1; a layout-independent character-offset scheme is a v5
//      bookmark-format upgrade.
//   2. Cross-chapter passages (spineIndex != endSpineIndex) walk
//      every intervening section. Each section load is ~200-500ms;
//      a 5-chapter passage costs ~1-2 s on modal open. Cache the
//      result in the modal -- never re-extract on L/R within a
//      single modal session.
//   3. The 4 KB byte cap is enforced inside the inner word-append
//      loop, NOT as a post-hoc truncation. Once we hit the cap, the
//      walk short-circuits and "..." is appended. No extra memory
//      is held beyond the cap.
//
// TODO before merging:
//   - Hook up to current SETTINGS for loadSectionFile fingerprint
//     (font / spacing / margins / orientation / etc.)
//   - Compute viewportWidth/Height from current renderer + margins
//   - Test single-page, multi-page, cross-chapter cases
//   - Test settings-changed-since-creation degradation gracefully

namespace {

// Pull all words from a Page into a flat vector, in reading order.
// Skips images, HR, and any non-text PageElement. The returned vector
// is owned by the caller; the Page can be released afterward.
std::vector<std::string> collectPageWords(const Page& page) {
  std::vector<std::string> out;
  // TODO: walk page.elements (or whatever the accessor is), find each
  // PageLine, pull TextBlock::getWords() into out. Reserve based on a
  // rough estimate (~50 words/page) so we don't realloc.
  out.reserve(80);
  // Pseudocode:
  //   for (const auto& elem : page.getElements()) {
  //     if (auto* line = dynamic_cast<const PageLine*>(elem.get())) {
  //       const auto& words = line->getBlock()->getWords();
  //       for (const auto& word : words) out.push_back(word);
  //     }
  //   }
  return out;
}

// Append a word to out with a single leading space if out is non-empty.
// Returns false if appending would exceed capBytes (out is left at the
// pre-append size). Caller appends "..." separately on overflow.
bool tryAppendWord(std::string& out, const std::string& word, size_t capBytes) {
  const size_t reservedForEllipsis = 4;  // " ..."
  const size_t need = (out.empty() ? 0u : 1u) + word.size();
  if (out.size() + need + reservedForEllipsis > capBytes) {
    return false;
  }
  if (!out.empty()) out += ' ';
  out += word;
  return true;
}

}  // namespace

std::string extractPassageText(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const Bookmark& bookmark,
                               size_t capBytes) {
  if (!epub || capBytes < 16) return {};

  std::string out;
  out.reserve(capBytes);

  // TODO: compute current viewport from renderer + SETTINGS.screenMargin
  // (mirror EpubReaderActivity's snapshot path). Hard-code zero margins
  // here would mismatch the section fingerprint and fail to load.
  const uint16_t viewportWidth = 0;
  const uint16_t viewportHeight = 0;

  // TODO: pass current SETTINGS to loadSectionFile so the fingerprint
  // matches. If it doesn't, the load fails and we fall back to the
  // stored Bookmark.preview (caller handles the empty-string return).

  for (uint16_t spine = bookmark.spineIndex; spine <= bookmark.endSpineIndex; ++spine) {
    auto section = std::make_unique<Section>(epub, spine, renderer);
    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                  SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                  SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled,
                                  SETTINGS.optimizeChapterIndexing != 0)) {
      LOG_INF("HPE", "extract: section %u load failed; bailing", spine);
      return {};
    }

    const float startProgress = (spine == bookmark.spineIndex) ? bookmark.progress : 0.0f;
    const float endProgress = (spine == bookmark.endSpineIndex) ? bookmark.endProgress : 1.0f;
    const uint16_t startPage = static_cast<uint16_t>(startProgress * section->pageCount);
    const uint16_t endPage =
        static_cast<uint16_t>(std::min<float>(endProgress * section->pageCount, section->pageCount - 1));

    bool overflow = false;
    for (uint16_t pg = startPage; pg <= endPage && !overflow; ++pg) {
      section->currentPage = pg;
      auto page = section->loadPageFromSectionFile();
      if (!page) {
        LOG_INF("HPE", "extract: section %u page %u load failed; bailing", spine, pg);
        return out;  // return what we have so far
      }
      const auto words = collectPageWords(*page);
      page.reset();  // release Page before we walk; words vector still owns the text

      const size_t firstIdx = (pg == startPage && spine == bookmark.spineIndex) ? bookmark.startWord : 0;
      const size_t lastIdx = (pg == endPage && spine == bookmark.endSpineIndex)
                                 ? std::min<size_t>(bookmark.endWord, words.size() - 1)
                                 : (words.empty() ? 0 : words.size() - 1);

      for (size_t i = firstIdx; i <= lastIdx && i < words.size(); ++i) {
        if (!tryAppendWord(out, words[i], capBytes)) {
          overflow = true;
          break;
        }
      }
    }

    if (overflow) {
      out += "...";
      break;
    }
  }

  LOG_INF("HPE", "extract: %u B for [spine %u@%.3f w%u -> spine %u@%.3f w%u]", (unsigned)out.size(),
          bookmark.spineIndex, bookmark.progress, bookmark.startWord, bookmark.endSpineIndex, bookmark.endProgress,
          bookmark.endWord);
  return out;
}
