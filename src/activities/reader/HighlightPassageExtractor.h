#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "BookmarkStore.h"

class GfxRenderer;

// CrumBLE 4.1: extract the full text of a highlight passage from EPUB
// section files. Used by the upcoming Quote Modal so the user can read
// the entire highlighted passage without diving back into the book.
//
// The Bookmark struct stores enough to reconstruct the passage:
//   (spineIndex, progress, startWord) - start anchor
//   (endSpineIndex, endProgress, endWord) - end anchor (== start for v3 records)
//
// extractPassageText() opens the section file(s) for the spineIndex
// range, paginates to the start/end pages, and walks the word stream
// from startWord through endWord, joining with single spaces.
//
// Heap-safety contract:
//   - Output string is reserve()'d to the cap upfront. No incremental
//     growth, no realloc churn.
//   - For cross-page highlights, pages are loaded and released
//     sequentially (one Page<> alive at a time) so peak memory stays
//     bounded by the largest single Page<>.
//   - Hard byte cap (default 4 KB). Passages beyond the cap end with
//     "..." and the caller can render a small "[passage truncated]"
//     footer. 4 KB is ~700 words / 1-2 minutes of reading -- well
//     beyond any reasonable user highlight.
//   - Single-chapter highlights (spineIndex == endSpineIndex) take
//     the fast path. Cross-chapter highlights load each intervening
//     section in order.
//
// Returns an empty string on extraction failure (section file missing,
// fingerprint mismatch with current reader settings, out-of-range
// word indices). Callers should fall back to the stored
// Bookmark.preview text in that case.
std::string extractPassageText(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const Bookmark& bookmark,
                               size_t capBytes = 4096);
