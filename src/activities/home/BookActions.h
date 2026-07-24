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
  // v18.9.9.170: emit the "Refresh cover" row. Home long-press sets this so
  // users can manually regenerate a placeholder thumbnail without waiting
  // for a full library re-walk.
  bool refreshCover = false;
  // v18.9.9.356: emit the "Retry failed covers" row (global sweep, mirrors
  // Settings > Utility). Home carousel and shelf book long-press set this
  // so users can retry stuck placeholders without navigating to Settings.
  bool retryFailedCovers = false;
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

// Boot-time helper: execute a file or directory delete that was deferred
// (path stashed in RTC NOINIT) from FileBrowserActivity when heap was too
// fragmented to safely run inline. Called from setup() after Storage / index
// stores are up but before the activity dispatch, so the delete runs on a
// fresh ~85 KB heap with no concurrent SD traffic. Includes the recursive
// metadata sweep for directories. Best-effort: on any single-file failure,
// logs and continues; only the silent-restart was guaranteed -- not the
// outcome of the delete itself.
void executeDeferredDelete(const std::string& fullPath, bool isDir);
// CrumBLE 4.5.5+: same idea as executeDeferredDelete, but accepts the raw
// PendingDeleteAction code from main.cpp so the boot path can route file /
// directory / book-cache-clear operations through one entry point. Boot
// reads pendingDeleteIsDir (now an action enum) and passes it here.
//   0 = single file delete
//   1 = directory delete (recursive metadata sweep)
//   2 = EPUB cache clear (Epub::clearCache)
void executeDeferredOperation(const std::string& fullPath, uint8_t action);
bool isEpubCompleted(const std::string& fullPath);
bool toggleEpubCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
void drawToast(const GfxRenderer& renderer, const char* msg);

// v18.9.6.2: Simple Rendering sidecar helpers. Presence of the sidecar
// file in the book's cache directory means the reader opens the book with
// tables collapsed to paragraphs, images suppressed, embedded style
// skipped, bionic + guide reading off. Set automatically by the reader's
// escalation path (v18.9.6.1) OR manually via the long-press menu here.
bool hasSimpleRenderingSidecar(const std::string& fullPath);
// Returns the new state (true = enabled after the toggle).
bool toggleSimpleRenderingSidecar(const std::string& fullPath);

}  // namespace BookActions
