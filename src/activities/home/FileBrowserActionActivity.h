#pragma once

#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

enum class FileBrowserAction : int {
  Delete = 0,
  PinFavorite = 1,
  UnpinFavorite = 2,
  DeleteCache = 3,
  ToggleCompleted = 4,
  // CrumBLE Collections (phase 2): opens a checkbox-list picker for the
  // book against ALL collections, with a "+ New collection..." option.
  // Replaces the phase-1 AddToFavorites/RemoveFromFavorites pair.
  AddToCollection = 5,
  // CrumBLE: long-press action menu on the home carousel / shelf shows this
  // so the user can clear a book off the Recent Books list without having
  // to clear its read state in the file browser. Only meaningful from the
  // home screen; the file browser doesn't expose it.
  RemoveFromRecentBooks = 7,
  // CrumBLE: long-press the shelf header (collection tab) on the Flow
  // theme to invoke this. Walks SD card looking for newly-added books
  // and refreshes the LibraryIndex so they appear in Recently Added /
  // All Books. Only surfaced from the home shelf header — not the file
  // browser or per-book action menu.
  RescanLibrary = 8,
  // CrumBLE: also on the shelf-header action menu. Opens a
  // SortPickerActivity for the active collection. Hidden for the
  // "Recently Added" virtual collection since its sort is intrinsic.
  SortBy = 9,
  // CrumBLE: shelf-header item that flips the collapseSeries flag
  // on the active collection. Label updates to reflect current
  // state ("Series collapse: ON" vs "...: OFF").
  ToggleCollapseSeries = 10,
  // CrumBLE debug: opens a viewer listing the book's OPF metadata
  // (title, author, language, series name + index). Useful when
  // series collapse isn't grouping books as expected — lets the user
  // verify whether the calibre:series tag is actually present in the
  // EPUB.
  ShowMetadata = 11,
  // CrumBLE: shelf-header items for user-collection management.
  // Rename opens the keyboard with the current name; Delete prompts
  // for confirmation then removes the collection (books on disk
  // untouched). Both hidden for virtual collections; Delete hidden
  // for the seeded Favorites.
  RenameCollection = 12,
  DeleteCollection = 13,
  // CrumBLE: shelf-header item available on ANY collection. Opens
  // the keyboard for a name then creates a new user collection and
  // switches active to it. A faster path than long-pressing a book
  // → "Add to collection..." → "+ New collection...". Useful from
  // virtual collections (Recently Added / All Books) where the
  // user-collection management items are otherwise hidden.
  CreateNewCollectionFromHeader = 14,
  // CrumBLE: shelf-header item on user collections. Opens a multi-
  // select picker over Recent Books so the user can add several
  // books to the active collection in one pass instead of long-
  // pressing each book individually.
  AddBooksToActiveCollection = 15,
  // CrossInk 1.3 recent-books long-press action (List/Grid views). Distinct
  // from our home RemoveFromRecentBooks=7; assigned a non-colliding value.
  RemoveFromRecents = 16,
  // CrumBLE: shelf-header toggles to opt into the index-backed virtual
  // collections (Recently Added / All Books). They're hidden by default so a
  // fresh device never runs the whole-SD walk at boot; turning one ON prompts
  // to scan the library first. Label flips Show/Hide with current state.
  ToggleShowRecentlyAdded = 17,
  ToggleShowAllBooks = 18,
  // CrumBLE: shelf-header toggles for the completion-derived virtual
  // collections. Finished is books the user marked complete; New is
  // books that exist in the library but have never been opened. Like the
  // other virtuals above, they're opt-in -- turning one ON triggers the
  // library walk and an additional pass over each book's BookReadingStats.
  ToggleShowFinished = 19,
  ToggleShowNew = 20,
  // CrumBLE: opens RearrangeCollectionsActivity. User taps Confirm on each
  // collection in the order they want them displayed; the new order is
  // saved and becomes the L/R cycle on Home.
  RearrangeCollections = 21,
  // CrumBLE: flips the active collection's twoRowShelf flag. Two-row
  // layout shows ~12 smaller covers per page; one-row shows 4. Persisted
  // on collections.json for user collections; virtuals reseed each begin.
  ToggleTwoRowShelf = 22,
  // CrumBLE: surfaces on the file browser's long-press menu when the
  // selected entry is a folder. Walks the folder recursively for book
  // files, creates a new user collection named after the folder
  // (deduped via CollectionsStore::disambiguateName), bulk-adds every
  // discovered book, and makes the collection active before returning
  // to Home. Skipped (with a toast) if the folder contains no books.
  MakeCollectionFromFolder = 23,
};

class FileBrowserActionActivity final : public Activity {
 public:
  struct MenuItem {
    FileBrowserAction action;
    StrId labelId;
    // CrumBLE: optional right-justified value (rendered after the label using
    // the existing drawList rowValue slot). Used by the shelf-header menu's
    // Show/Hide toggles to make state visible at a glance --
    //   "Recently Added                              Show"
    //   "All Books                                   Hide"
    // -- instead of baking the verb into the label ("Show Recently Added" /
    // "Hide Recently Added") which crowds the text and is less scannable
    // when there are several toggles in a row.
    std::string rightValue;
    // CrumBLE: optional getter that overrides `rightValue` per render. Lets
    // an item show a value that can change WHILE the menu is open (e.g. a
    // toggle that's wired up to inlineToggle below and re-evaluated each
    // frame to reflect the new state without exiting + re-entering the menu).
    std::function<std::string()> rightValueGetter;
    // CrumBLE: when set, Confirm on this row invokes the callback instead
    // of closing the menu. The menu stays open with selectorIndex
    // preserved; the row's rightValueGetter is re-evaluated on the next
    // paint so the user sees the new state. Used for "stay-in-menu"
    // toggles whose effect on Home is deferred until the menu closes
    // (e.g. Rows: One <-> Two flips the per-collection setting but the
    // shelf re-layout is held until Back).
    std::function<void()> inlineToggle;
  };

  FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                            std::vector<MenuItem> items, bool ignoreInitialConfirmRelease = false,
                            std::string headerRightLabel = {})
      : Activity("FileBrowserAction", renderer, mappedInput),
        title(std::move(title)),
        headerRightLabel(std::move(headerRightLabel)),
        items(std::move(items)),
        ignoreConfirmRelease(ignoreInitialConfirmRelease) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  std::string title;
  // CrumBLE: optional right-justified secondary label rendered in the header
  // row next to the title (e.g. the current sort mode on the shelf-header
  // action menu, so "Favorites    Title (A-Z)"). Empty = nothing drawn.
  std::string headerRightLabel;
  std::vector<MenuItem> items;
  int selectedIndex = 0;
  bool ignoreConfirmRelease = false;
};
