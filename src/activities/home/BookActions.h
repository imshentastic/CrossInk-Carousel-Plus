#pragma once

#include <optional>
#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"

class GfxRenderer;

namespace BookActions {

// CrumBLE 4.2: long-press menu item order, applied uniformly across every
// caller that goes through FileBrowserActionActivity. The order is
// user-driven: low-risk navigation/curation first, destructive deletes last,
// so the most common actions are nearest the focus row and the cursor has
// to travel past benign options before reaching anything that can wipe data.
//
//   1. Add to / remove from collection
//   2. Remove from Recent Books        (variant per caller -- see options)
//   3. Mark as Finished / Unfinished
//   4. Show metadata
//   5. Delete book cache
//   6. Delete
//
// Callers opt items in or out via BookActionMenuOptions. Items that don't
// apply to the path (e.g. ToggleCompleted on a non-EPUB) are silently
// skipped. PinFavorite/UnpinFavorite is the file-browser-only sleep-image
// affordance; it's still appended at the call site after this base set.
struct BookActionMenuOptions {
  bool addToCollection = false;
  bool showMetadata = false;
  // When set, emit the Remove-From-Recents row using the given action enum
  // (RemoveFromRecents vs RemoveFromRecentBooks -- different handlers in
  // the recent-list activity vs the home carousel; same UX label, but the
  // dispatch goes to a different switch case).
  std::optional<FileBrowserAction> removeFromRecents;
};

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const BookActionMenuOptions& options);
// CrumBLE 4.2: returns "Optimized" if the book's cache dir has the v2
// prebake marker file, otherwise "". Callers pass the result as
// FileBrowserActionActivity's `headerRightLabel` so the long-press menu
// header shows "<Book Title>          Optimized" at a glance. Pre-4.2
// bakes have only the legacy manifest and return "" so they don't claim
// optimization status the WASM toolchain at the time couldn't actually
// deliver for SD fonts.
std::string optimizedHeaderLabel(const std::string& fullPath);

// CrumBLE 4.2: best-effort (title, author) resolution for the long-press
// menu's two-line header. Tries the in-memory RecentBooksStore first
// (free, no SD I/O); if the book hasn't been opened recently, falls back
// to opening the EPUB and parsing the OPF metadata (~50-100 ms on cold
// SD). If both fail, title gets the filename and author is empty -- the
// header falls back to its legacy single-line word-wrap layout.
struct BookHeaderText {
  std::string title;   // never empty -- filename used as last-resort fallback
  std::string author;  // empty if no author was discoverable
};
BookHeaderText resolveBookHeaderText(const std::string& fullPath);
bool hasClearableBookCache(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool isEpubCompleted(const std::string& fullPath);
bool toggleEpubCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
void drawToast(const GfxRenderer& renderer, const char* msg);

}  // namespace BookActions
