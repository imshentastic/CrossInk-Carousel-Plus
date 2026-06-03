#include "LyraFlowTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/chart.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {
constexpr int centerCoverWidth = 220;
constexpr int centerCoverHeight = 320;
constexpr int sideCoverWidth = 66;     // 30% of center
constexpr int sideInnerHeight = 288;   // 90% of center — taller edge (toward middle)
constexpr int sideOuterHeight = 256;   // 80% of center — shorter edge (away from middle)
constexpr int bookCornerRadius = 6;

// Menu visuals — kept in sync with LyraTheme's anonymous-namespace constants
// so the Flow override looks identical to the parent's button menu.
constexpr int menuTileCornerRadius = 6;
constexpr int menuTilePadding = 8;
constexpr int menuIconSize = 32;

// Same lookup as LyraTheme's iconForName(icon, 32). Duplicated here because
// that helper is file-local to LyraTheme.cpp.
const uint8_t* lyraFlowMenuIcon(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Chart:
      return ChartIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    default:
      return nullptr;
  }
}

// Erase pixels outside a rounded-corner mask so a rectangular bitmap blit
// looks like a rounded book cover. Same trick as the reference FlowTheme.
void cutRoundedCorners(GfxRenderer& renderer, int x, int y, int w, int h, int r) {
  const int rSq = r * r;
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      const int distSq = (r - dx) * (r - dx) + (r - dy) * (r - dy);
      if (distSq > rSq) {
        renderer.drawPixel(x + dx, y + dy, false);
        renderer.drawPixel(x + w - 1 - dx, y + dy, false);
        renderer.drawPixel(x + w - 1 - dx, y + h - 1 - dy, false);
        renderer.drawPixel(x + dx, y + h - 1 - dy, false);
      }
    }
  }
}
}  // namespace

void LyraFlowTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                        int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                        bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                        const BookReadingStats* stats, float progressPercent) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int pageWidth = renderer.getScreenWidth();
  // CrumBLE: cover starts 58 px below the carousel rect's top. (Tried +50
  // for an 8 px up-shift but the carousel ended up crowding the status
  // bar at the top; restored to +58 along with kTitleTopY = -12.)
  const int centerY = rect.y + 58;
  const int centerX = pageWidth / 2;
  const int count = static_cast<int>(recentBooks.size());

  // selectorIndex >= count means HomeActivity has navigated past the books and
  // is highlighting a menu item; in that case we keep the carousel visible but
  // drop the selection border. HomeActivity may encode the preferred center as
  // (count + lastBookIndex) so the carousel keeps the user's place when they
  // pop into the menu. Decode if in range, otherwise fall back to book 0.
  const bool hasSelection = (selectorIndex >= 0 && selectorIndex < count);
  int curIdx = 0;
  if (hasSelection) {
    curIdx = selectorIndex;
  } else {
    const int decoded = selectorIndex - count;
    if (decoded >= 0 && decoded < count) curIdx = decoded;
  }

  // The carousel chrome (header date, footer hints, etc.) is drawn by
  // HomeActivity, not by us. We have nothing static to cache here, so just
  // honor the buffer-snapshot protocol the same way the other themes do.
  if (bufferRestored) {
    coverRendered = true;
    coverBufferStored = true;
  } else {
    coverRendered = true;
    coverBufferStored = storeCoverBuffer();
  }

  // --- Side covers (perspective-projected, drawn outside-in so the center
  //     can land cleanly on top of any near-book overlap) ---
  auto drawStackedCover = [&](int idx, bool isLeft, bool isFar) {
    const int hL = isLeft ? sideInnerHeight : sideOuterHeight;
    const int hR = isLeft ? sideOuterHeight : sideInnerHeight;
    const int hMax = std::max(hL, hR);
    // CrumBLE: looser positioning -- side covers sit further out so
    // each adjacent book shows more of its content. The (separately-
    // tuned) 7 px white border around the center cover gives a clean
    // framed look between center and adjacent books.
    const int drawX = isLeft ? (isFar ? 24 : 74) : (isFar ? 390 : 340);
    const int drawY = centerY + (centerCoverHeight / 2) - (hMax / 2);

    // CrumBLE #125: tile-cache fast path. The 4 side covers in the Flow
    // carousel share only 2 unique perspective shapes per book (left and
    // right; near/far on the same side use the same shape, only drawX
    // differs). prerenderCarouselSideTiles() bakes both shapes once at
    // home entry, so every carousel L/R press here is a pure 1bpp blit
    // instead of a ~70k-source-pixel perspective walk per cover. Falls
    // through to the perspective-render path below when the tile cache
    // is cold (first frame before prerender completes) or the book's
    // tile failed to allocate (low heap during prerender).
    bool drawn = false;
    const std::string& tileKey = recentBooks[idx].path;
    auto tileIt = sideTileCache_.find(tileKey);
    if (tileIt != sideTileCache_.end()) {
      const PerspectiveTile& tile = isLeft ? tileIt->second.left : tileIt->second.right;
      if (tile.pixels && tile.width > 0 && tile.height > 0) {
        // OR-style blit: pre-clear the bbox to opaque white so any
        // previous side cover that was here doesn't bleed through.
        renderer.fillRect(drawX, drawY, sideCoverWidth, hMax, false);
        const int dstStride = (tile.width + 7) / 8;
        renderer.drawPacked1bpp(tile.pixels.get(), dstStride, drawX, drawY, tile.width, tile.height);
        drawn = true;
      }
    }

    const std::string coverPath =
        drawn ? std::string{} : UITheme::getCoverThumbPath(recentBooks[idx].coverBmpPath, centerCoverHeight);
    if (!drawn && !coverPath.empty()) {
      // CrumBLE Phase A perf: try the in-RAM cache first. The Cached
      // overload reads 2bpp packed pixels directly out of the cache
      // entry, no SD I/O. Cache miss / budget-rejected -> fall back to
      // direct SD-streamed drawPerspectiveBitmap.
      GfxRenderer::CachedBitmap* handle = renderer.lookupCachedBitmap(coverPath);
      int srcW = 0, srcH = 0;
      if (renderer.getCachedBitmapDimensions(handle, &srcW, &srcH) && srcW > 0 && srcH > 0) {
        // drawPerspectiveBitmap is OR-style (only writes black), so any
        // white area of the cover would show through to whatever side
        // cover was drawn beneath us. Pre-clear the bbox to opaque white.
        renderer.fillRect(drawX, drawY, sideCoverWidth, hMax, false);
        renderer.drawPerspectiveBitmap(handle, drawX, drawY, sideCoverWidth, hL, hR);
        drawn = true;
      } else {
        FsFile file;
        if (Storage.openFileForRead("HOME", coverPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            renderer.fillRect(drawX, drawY, sideCoverWidth, hMax, false);
            renderer.drawPerspectiveBitmap(bitmap, drawX, drawY, sideCoverWidth, hL, hR);
            drawn = true;
          }
          file.close();
        }
      }
    }
    if (!drawn) {
      // Solid-black placeholder silhouette so the carousel still has shape.
      renderer.fillRect(drawX, drawY, sideCoverWidth, hMax, true);
      return;  // outline would be invisible against solid black anyway
    }
    // 2px trapezoidal outline matching the perspective shape -- traces
    // the cover content rather than a bounding box. The trapezoid is
    // column-centered vertically inside the (sideCoverWidth × hMax)
    // bbox.
    const int topL = (hMax - hL) / 2;
    const int topR = (hMax - hR) / 2;
    const int botL = topL + hL - 1;
    const int botR = topR + hR - 1;
    const int rightX = drawX + sideCoverWidth - 1;
    renderer.drawLine(drawX, drawY + topL, rightX, drawY + topR, 2, true);    // top edge (slanted)
    renderer.drawLine(drawX, drawY + botL, rightX, drawY + botR, 2, true);    // bottom edge (slanted)
    // Verticals use fillRect, not drawLine -- drawLine ignores its thickness
    // arg for purely vertical strokes (x1 == x2).
    constexpr int verticalEdgeWidth = 2;
    renderer.fillRect(drawX, drawY + topL, verticalEdgeWidth, hL, true);                    // left edge
    renderer.fillRect(rightX - verticalEdgeWidth + 1, drawY + topR, verticalEdgeWidth, hR,  // right edge
                      true);
    // The bottom slant's perpendicular thickness leaks pixels into the two
    // rows starting just below the bbox bottom. Wipe rows hMax+1 / hMax+2.
    renderer.fillRect(drawX, drawY + hMax + 1, sideCoverWidth, 2, false);
  };

  const int idx2 = (curIdx + count - 1) % count;  // left-near
  const int idx3 = (curIdx + count - 2) % count;  // left-far
  const int idx4 = (curIdx + 1) % count;          // right-near
  const int idx5 = (curIdx + 2) % count;          // right-far

  // Variables the footer (progress bar) and cover chrome depend on. We
  // peek the CENTER cover's true dimensions on EVERY render — even in the
  // skipCarouselCoverLoads fast path — so the footer width stays stable.
  // If we left these at the default 220 while skipping but recomputed them
  // narrower on the next full render, the progress bar would visibly start
  // wider then shrink when navigating (it tracks actualCoverWidth below).
  // parseHeaders() only reads the small BMP header; the expensive work
  // (scaling + drawing all 5 covers) stays gated behind the skip flag.
  int actualCoverWidth = centerCoverWidth;
  int actualCoverHeight = centerCoverHeight;

  // --- Center cover. Peek the bitmap dimensions first so the slot, outline,
  //     selection border, AND footer/progress-bar width match the cover's
  //     true aspect ratio (otherwise drawBitmap aspect-fits but our 220×320
  //     chrome leaves a white sliver for narrower covers, e.g. 1720×2600
  //     which is taller than 220:320). ---
  // CrumBLE Phase A perf: the in-RAM cache lookup happens immediately
  // before the draw call below -- NOT here. The side-cover loop below
  // also lookups + may evict cache entries under budget pressure, so a
  // CachedBitmap* held across that loop is a dangling-pointer trap.
  // Dimensions come from a one-shot header probe of the cover file
  // (cheap: BMP header only, no row data).
  const std::string cp = UITheme::getCoverThumbPath(recentBooks[curIdx].coverBmpPath, centerCoverHeight);
  int centerSrcW = 0, centerSrcH = 0;
  bool centerParsed = false;
  // CrumBLE #124: cached header dims short-circuit the SD probe when the
  // center book is unchanged across renders -- previously every carousel
  // L/R press paid a Storage.openFileForRead + Bitmap::parseHeaders even
  // though both side covers and the center are byte-identical to the last
  // frame. Footer width still recomputes identically because we feed it
  // the same centerSrcW/H values from the cache.
  if (!cp.empty() && cp == lastCenterCoverPath_ && lastCenterCoverSrcW_ > 0 && lastCenterCoverSrcH_ > 0) {
    centerSrcW = lastCenterCoverSrcW_;
    centerSrcH = lastCenterCoverSrcH_;
    centerParsed = true;
  } else if (!cp.empty()) {
    FsFile probeFile;
    if (Storage.openFileForRead("HOME", cp, probeFile)) {
      Bitmap probe(probeFile);
      if (probe.parseHeaders() == BmpReaderError::Ok && probe.getWidth() > 0 && probe.getHeight() > 0) {
        centerSrcW = probe.getWidth();
        centerSrcH = probe.getHeight();
        centerParsed = true;
        lastCenterCoverPath_ = cp;
        lastCenterCoverSrcW_ = centerSrcW;
        lastCenterCoverSrcH_ = centerSrcH;
      }
      probeFile.close();
    }
  }
  if (centerParsed) {
    const float fitScale = std::min(static_cast<float>(centerCoverWidth) / static_cast<float>(centerSrcW),
                                    static_cast<float>(centerCoverHeight) / static_cast<float>(centerSrcH));
    actualCoverWidth = std::min(centerCoverWidth, static_cast<int>(std::round(centerSrcW * fitScale)));
    actualCoverHeight = std::min(centerCoverHeight, static_cast<int>(std::round(centerSrcH * fitScale)));
  }

  int cX = centerX - actualCoverWidth / 2;
  int actualY = centerY + (centerCoverHeight - actualCoverHeight) / 2;

  // Skip the expensive 5-BMP cover paint when HomeActivity tells us the
  // framebuffer was just restored with these same covers. Saves ~80% of
  // drawRecentBookCover's cost on every "L/R on shelf/menu" type input
  // where the carousel doesn't visually change. The center-cover header
  // peek above still runs so the footer geometry stays consistent.
  if (!skipCarouselCoverLoads) {
    if (count >= 5) drawStackedCover(idx3, true, true);
    if (count >= 4) drawStackedCover(idx5, false, true);
    if (count >= 2) drawStackedCover(idx2, true, false);
    if (count >= 3) drawStackedCover(idx4, false, false);

    // Clear behind the cover so side-cover overlap doesn't bleed
    // through. CrumBLE used to clear the FULL slot (centerCoverWidth +
    // 6) regardless of the actual cover width -- meant a narrower cover
    // left a wide band of white extending ~13 px past the side covers'
    // inner edge, visibly "cutting into" the adjacent books. Now we
    // clear actual cover dims + a 7 px white "frame" border per user
    // preference: a small visible gap between the center cover and the
    // adjacent books reads as intentional matting, not a clipped book.
    constexpr int kSelBorderPad = 7;
    const int clearX = cX - kSelBorderPad;
    const int clearY = actualY - kSelBorderPad;
    const int clearW = actualCoverWidth + 2 * kSelBorderPad;
    const int clearH = actualCoverHeight + 2 * kSelBorderPad;
    renderer.fillRect(clearX, clearY, clearW, clearH, false);

    if (centerParsed) {
      // Lookup HERE (not earlier): the side-cover draws above also
      // touch the cache and may evict, so any handle obtained before
      // the side loop would be dangling. Re-lookup gives a fresh,
      // promoted entry (and the side-cover loads have already biased
      // LRU toward eviction of older entries, so this lookup is what
      // pins the center for the next render).
      GfxRenderer::CachedBitmap* centerHandle = cp.empty() ? nullptr : renderer.lookupCachedBitmap(cp);
      if (centerHandle) {
        // rhythmerc perf hack #62016fba: Opaque=true writes both inks (the
        // surrounding fillRect just painted white substrate, so we don't
        // need the cover to leave white pixels visible). bookCornerRadius
        // arg makes the blit skip the four corner triangles, which
        // replaces the per-pixel cutRoundedCorners loop below for the
        // cached path. Fallback path still pays the cutRoundedCorners cost
        // since drawBitmap doesn't have the corner-skip arg yet.
        renderer.drawCachedBitmap<true>(centerHandle, cX, actualY, actualCoverWidth, actualCoverHeight,
                                        0.0f, 0.0f, bookCornerRadius);
      } else {
        // Budget-tight fall through: stream the cover directly. Open the
        // file fresh here -- we deliberately closed the probe file above
        // to keep file-handle pressure bounded across the side-cover loop.
        FsFile fallbackFile;
        if (Storage.openFileForRead("HOME", cp, fallbackFile)) {
          Bitmap centerFallbackBitmap(fallbackFile);
          if (centerFallbackBitmap.parseHeaders() == BmpReaderError::Ok) {
            renderer.drawBitmap(centerFallbackBitmap, cX, actualY, actualCoverWidth, actualCoverHeight);
          }
          fallbackFile.close();
        }
        cutRoundedCorners(renderer, cX, actualY, actualCoverWidth, actualCoverHeight, bookCornerRadius);
      }
    } else {
    // Placeholder: black lower-2/3 with the cover icon centered just below
    // the divider, then the wrapped book title in white below the icon.
    // Mirrors LyraCarouselTheme's fallback so un-thumbnailed books still
    // look like intentional cards instead of half-blank slots.
    renderer.fillRoundedRect(cX, actualY + actualCoverHeight / 3, actualCoverWidth, 2 * actualCoverHeight / 3,
                             bookCornerRadius, false, false, true, true, Color::Black);

    constexpr int kFallbackTitlePadX = 14;
    constexpr int kFallbackTitlePadBottom = 14;
    constexpr int kFallbackIconGap = 10;
    const int iconX = cX + actualCoverWidth / 2 - 16;
    const int iconY = actualY + actualCoverHeight / 3 + 14;
    renderer.drawIcon(CoverIcon, iconX, iconY, 32, 32);

    const int titleY = iconY + 32 + kFallbackIconGap;
    const int titleW = actualCoverWidth - kFallbackTitlePadX * 2;
    const int titleH = actualY + actualCoverHeight - kFallbackTitlePadBottom - titleY;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxLines = std::clamp(titleH / std::max(1, lineHeight), 1, 4);
    // Fall back to the filename (sans extension) when the EPUB had no title
    // metadata — same logic the Flow theme uses for the heading above.
    std::string title = recentBooks[curIdx].title;
    if (title.empty()) {
      title = recentBooks[curIdx].path;
      const size_t lastSlash = title.find_last_of('/');
      if (lastSlash != std::string::npos) title = title.substr(lastSlash + 1);
      const size_t lastDot = title.find_last_of('.');
      if (lastDot != std::string::npos && lastDot > 0) title = title.substr(0, lastDot);
    }
    const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, title.c_str(), titleW, maxLines, EpdFontFamily::BOLD);
    const int blockH = lineHeight * static_cast<int>(titleLines.size());
    int lineY = titleY + std::max(0, (titleH - blockH) / 2);
    for (const auto& line : titleLines) {
      const int lineW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
      // White text on the black backdrop — drawText's `black` param is the
      // pixel state to write, so `false` puts down white pixels.
      renderer.drawText(UI_10_FONT_ID, cX + (actualCoverWidth - lineW) / 2, lineY, line.c_str(), false,
                        EpdFontFamily::BOLD);
      lineY += lineHeight;
    }
  }
    // Inner cover outline removed — cutRoundedCorners has already
    // shaped the bitmap's corners to white, and the selection border
    // (below, when hasSelection) is sufficient framing for the
    // focused book. Dropping the always-on stroke eliminates the
    // last visible source of corner-hook artifacts.

  }  // end of if (!skipCarouselCoverLoads)

  // CrumBLE #125: reconcile the selection border AFTER the cover-block
  // guard. Previously this draw lived inside `if (!skipCarouselCoverLoads)`
  // which meant any focus-only change (e.g. Down from carousel to shelf
  // header — same center book, just hasSelection flipping from true to
  // false) cache-missed the carousel-skip path because the only way to
  // remove the border was a full repaint. Now the border is its own
  // toggle:
  //   - Full repaint (!skipCarouselCoverLoads): the cover-clear block
  //     above just wiped the frame to white, so we always need to
  //     repaint the border on top if hasSelection is true.
  //   - Skip-covers fast path (skipCarouselCoverLoads): the framebuffer
  //     restore brought back the previous frame's border state. If
  //     hasSelection still matches what we drew, nothing to do. If it
  //     differs, draw the border in black (appearing) or in white
  //     (erasing — works because the 7 px frame around the cover is
  //     white substrate from the prior frame's cover-clear).
  // Tracked in lastDrawnSelectionBorder_ on the theme; HomeActivity's
  // canSkipCovers gate only checks center book identity, leaving the
  // border concern fully to the theme.
  // CrumBLE: double-ring selection mirrors RecentBooksGridActivity's
  // cell focus ring (inner 3 px ring at +4 outset, outer 1 px ring at
  // +6 outset, 2 px gap between). Gives the same "layered 3D" look the
  // user sees on Bookshelf cells. Fits inside the 7 px white frame
  // already cleared around the center cover. Corner radius bumped by
  // each outset so the rings stay concentric with the cover's
  // bookCornerRadius=6.
  constexpr int kSelectionPadding = 4;
  constexpr int kSelectionGap = 2;
  constexpr int kSelectionOuterInset = kSelectionPadding + kSelectionGap;
  const int innerX = cX - kSelectionPadding;
  const int innerY = actualY - kSelectionPadding;
  const int innerW = actualCoverWidth + 2 * kSelectionPadding;
  const int innerH = actualCoverHeight + 2 * kSelectionPadding;
  const int outerX = cX - kSelectionOuterInset;
  const int outerY = actualY - kSelectionOuterInset;
  const int outerW = actualCoverWidth + 2 * kSelectionOuterInset;
  const int outerH = actualCoverHeight + 2 * kSelectionOuterInset;
  auto drawSelectionRings = [&](bool inkBlack) {
    renderer.drawRoundedRect(innerX, innerY, innerW, innerH, 3, bookCornerRadius + kSelectionPadding, inkBlack);
    renderer.drawRoundedRect(outerX, outerY, outerW, outerH, 1, bookCornerRadius + kSelectionOuterInset, inkBlack);
  };
  const int currentBorder = hasSelection ? 1 : 0;
  if (!skipCarouselCoverLoads) {
    // Full repaint path: the cover-clear block above wiped the ring
    // pixels to white. Re-draw the rings iff they should be present.
    if (hasSelection) {
      drawSelectionRings(true);
    }
  } else if (currentBorder != lastDrawnSelectionBorder_) {
    // Fast path with focus change: toggle BOTH rings by redrawing with
    // the appropriate ink. state=true paints black (draw); state=false
    // paints white (erase, since the surrounding frame is white from
    // the prior frame's cover-clear).
    drawSelectionRings(hasSelection);
  }
  lastDrawnSelectionBorder_ = currentBorder;

  // One-shot reset so the next render starts from a known default.
  skipCarouselCoverLoads = false;

  // --- Title above the center cover (filename, no extension) ---
  std::string filename = recentBooks[curIdx].title.empty() ? recentBooks[curIdx].path : recentBooks[curIdx].title;
  if (recentBooks[curIdx].title.empty()) {
    const size_t lastSlash = filename.find_last_of('/');
    if (lastSlash != std::string::npos) filename = filename.substr(lastSlash + 1);
    const size_t lastDot = filename.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) filename = filename.substr(0, lastDot);
  }

  const std::string truncatedTitle =
      renderer.truncatedText(UI_12_FONT_ID, filename.c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, truncatedTitle.c_str(), EpdFontFamily::BOLD);
  // Pre-clear the title + author strip. HomeActivity now snapshots the
  // framebuffer at end-of-render and restores it at the start of the next
  // render (lets the shelf paint be skipped when its state is unchanged).
  // The restored buffer has the PRIOR render's title/author in it — if the
  // new title is shorter or differently-positioned, the leftover characters
  // would show through.
  constexpr int kAuthorFontId = UI_10_FONT_ID;
  constexpr int kTitleAuthorGap = 2;
  // Title sits at rect.y - 12 (reverted from the -20 up-shift). Paired
  // with centerY = rect.y + 58 above; gap from author baseline to cover
  // top stays ~36 px.
  constexpr int kTitleTopY = -12;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineH = renderer.getLineHeight(kAuthorFontId);
  const int titleAuthorStripHeight = titleLineH + kTitleAuthorGap + authorLineH + 4;
  renderer.fillRect(0, rect.y + kTitleTopY, pageWidth, titleAuthorStripHeight, false);
  renderer.drawText(UI_12_FONT_ID, centerX - titleWidth / 2, rect.y + kTitleTopY, truncatedTitle.c_str(), true,
                    EpdFontFamily::BOLD);

  // --- Author line directly under the title (above the cover). ---
  const std::string& authorRaw = recentBooks[curIdx].author;
  if (!authorRaw.empty()) {
    const std::string truncatedAuthor =
        renderer.truncatedText(kAuthorFontId, authorRaw.c_str(), pageWidth - 40, EpdFontFamily::REGULAR);
    const int aw = renderer.getTextWidth(kAuthorFontId, truncatedAuthor.c_str(), EpdFontFamily::REGULAR);
    const int authorY = rect.y + kTitleTopY + titleLineH + kTitleAuthorGap;
    renderer.drawText(kAuthorFontId, centerX - aw / 2, authorY, truncatedAuthor.c_str(), true,
                      EpdFontFamily::REGULAR);
  }

  // --- Reading-progress footer below the center cover. Modelled on the
  //     LyraCarousel footer: a 5-px dithered-track progress bar across the
  //     cover width, with elapsed-time on the bottom-left and remaining-
  //     time on the bottom-right ("XhYm left" / "Nm left"). ---
  constexpr int kFooterFontId = UI_10_FONT_ID;
  constexpr int kFooterTopGap = 8;
  constexpr int kFooterProgressBarHeight = 5;
  constexpr int kFooterBarToLabelGap = 2;

  const int footerWidth = actualCoverWidth;
  const int footerX = cX;
  int infoY = centerY + centerCoverHeight + kFooterTopGap;

  const bool hasStats = (stats != nullptr && stats->sessionCount > 0);
  const bool hasProgress = progressPercent >= 0.0f;

  // Pre-clear the footer region (bar + label row beneath it). Clears
  // the FULL page width -- not just the current cover's width -- because
  // adjacent books in the carousel can have different aspect ratios.
  const int footerStripHeight = kFooterProgressBarHeight + kFooterBarToLabelGap + renderer.getLineHeight(kFooterFontId) + 4;
  renderer.fillRect(0, infoY, renderer.getScreenWidth(), footerStripHeight, false);

  if (hasProgress) {
    const float clampedProgress = std::clamp(progressPercent, 0.0f, 100.0f);
    const int filledWidth = std::clamp(static_cast<int>((clampedProgress / 100.0f) * footerWidth), 0, footerWidth);
    renderer.fillRectDither(footerX, infoY, footerWidth, kFooterProgressBarHeight, Color::LightGray);
    if (filledWidth > 0) {
      renderer.fillRect(footerX, infoY, filledWidth, kFooterProgressBarHeight, true);
    }

    const int labelRowY = infoY + kFooterProgressBarHeight + kFooterBarToLabelGap;
    if (hasStats) {
      char buf[24];
      const uint32_t seconds = stats->totalReadingSeconds;
      if (seconds < 60) {
        snprintf(buf, sizeof(buf), "<1m");
      } else if (seconds < 3600) {
        snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>(seconds / 60));
      } else {
        snprintf(buf, sizeof(buf), "%uh %um", static_cast<unsigned>(seconds / 3600),
                 static_cast<unsigned>((seconds % 3600) / 60));
      }
      renderer.drawText(kFooterFontId, footerX, labelRowY, buf, true, EpdFontFamily::REGULAR);
    }

    // Remaining time replaces the legacy percentage display. Needs both
    // a reading-time history AND non-zero progress; skipped below 1%
    // progress so we don't show wildly inflated estimates from rounding
    // noise on freshly-opened books. >= 1h is rounded to the nearest hour
    // (so 1h 8m -> 1h, 1h 30m -> 2h) -- the granular "1h 30m left" feels
    // misleadingly precise for what's a linear extrapolation from average
    // reading speed; under 1h we keep "Nm left" to be useful near the end.
    char remainingBuf[24] = "";
    if (hasStats && clampedProgress >= 1.0f) {
      const uint32_t remaining =
          static_cast<uint32_t>(static_cast<float>(stats->totalReadingSeconds) *
                                (100.0f - clampedProgress) / clampedProgress);
      if (remaining < 60) {
        snprintf(remainingBuf, sizeof(remainingBuf), "<1m left");
      } else if (remaining < 3600) {
        snprintf(remainingBuf, sizeof(remainingBuf), "%um left", static_cast<unsigned>(remaining / 60));
      } else {
        const unsigned hours = static_cast<unsigned>((remaining + 1800) / 3600);  // round-nearest
        snprintf(remainingBuf, sizeof(remainingBuf), "%uh left", hours);
      }
    }
    if (remainingBuf[0]) {
      const int rw = renderer.getTextWidth(kFooterFontId, remainingBuf, EpdFontFamily::REGULAR);
      renderer.drawText(kFooterFontId, footerX + footerWidth - rw, labelRowY, remainingBuf, true,
                        EpdFontFamily::REGULAR);
    }
  } else if (hasStats) {
    // Edge case: stats exist but progress doesn't (e.g. book just opened
    // for the first time, no last-read marker yet). Fall back to time-only
    // on a single line, where the bar would have sat.
    char buf[24];
    const uint32_t seconds = stats->totalReadingSeconds;
    if (seconds < 60) {
      snprintf(buf, sizeof(buf), "<1m");
    } else if (seconds < 3600) {
      snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>(seconds / 60));
    } else {
      snprintf(buf, sizeof(buf), "%uh %um", static_cast<unsigned>(seconds / 3600),
               static_cast<unsigned>((seconds % 3600) / 60));
    }
    renderer.drawText(kFooterFontId, footerX, infoY, buf, true, EpdFontFamily::REGULAR);
  }
}

LyraFlowTheme::ShelfLayout LyraFlowTheme::shelfLayoutFor(int rowCount) {
  // 1-row default: 4 large covers across 480 px (16 + 4*100 + 3*16 + 16 = 480).
  // 2-row option:  5 covers per row at 60x90 (preserves 2:3), 30 px column
  // gap and 8 px gap between rows. 5 cells (vs the original 6) felt less
  // cluttered without leaving "too much whitespace" -- the wider gap eats
  // the side slack instead of compressing the books. 10 covers per page
  // also paints noticeably faster than 12 since rendering scales with the
  // visible cell count.
  ShelfLayout L;
  if (rowCount >= 2) {
    L.cellWidth = 60;
    L.cellHeight = 90;
    L.cellsPerRow = 5;
    L.cellGap = 30;
    L.rowGap = 4;  // tightened from 8 -> 4 so the two rows feel like a tighter block
    L.rowCount = 2;
    L.drawShadows = false;  // shadows feel like noise at this cell size
    // tab(18) + tabGap(12) + row1(90) + rowGap(4) + row2(90) = 214
    L.stripHeight = 214;
  } else {
    L.cellWidth = 100;
    L.cellHeight = 150;
    L.cellsPerRow = 4;
    L.cellGap = 16;
    L.rowGap = 0;
    L.rowCount = 1;
    L.drawShadows = true;
    // tab(18) + tabGap(12) + row(150) = 180
    L.stripHeight = 180;
  }
  return L;
}

void LyraFlowTheme::drawBookshelfStrip(GfxRenderer& renderer, Rect rect, const char* collectionName,
                                       const std::vector<std::string>& coverPaths, int selectedSpineIndex,
                                       int scrollOffset, bool headerFocused, bool hasMultipleCollections,
                                       const char* focusedBookTitle,
                                       const std::vector<int>* seriesMemberCounts, const char* focusedBookAuthor,
                                       int rowCount, const std::vector<std::string>* cellTitles) const {
  // Vertical layout (top → bottom):
  //   [focused book title]  — drawn ABOVE rect.y so the rest of the layout
  //                           doesn't shift when focused vs unfocused
  //   [collection tab]      — collection name centered, bold when focused
  //   [thumbnail row]       — fixed-cell-size cover thumbnails, aspect-fill
  //                           with crop (same pattern as Recent Books grid)
  //
  // Cell aspect is ~2:3 (book-cover proportion) so a row of thumbnails
  // reads as a row of actual books standing up rather than abstract bars.
  // Tab is the "Favorites" / collection-name heading rendered above the
  // book row. kTabBottomGap is the vertical padding between the heading
  // baseline and the top of the cover row — user feedback iter 4 asked
  // for more breathing room there so the heading doesn't crowd the row.
  constexpr int kTabHeight = 18;
  constexpr int kTabBottomGap = 12;
  // 1-row default: 4 thumbs across at 480 px wide: 2*16 (side pad) +
  // 4*100 (cells) + 3*16 (gaps) = 480 exactly. Dropped from 5 cells @
  // 84x126 to 4 cells @ 100x150 to fill the empty band below the
  // carousel that the smaller cells left behind. Aspect still ~2:3
  // book-cover; renderer can blit 1:1 because loadShelfCovers caches
  // BMPs at exactly this size.
  // 2-row option: 6 cells @ 60x90 per row (same 2:3 aspect, half height),
  // 8 px row gap between the two rows. Strip height becomes 218 px (vs
  // the 1-row 180 px) -- accommodated by HomeActivity's strip placement.
  const ShelfLayout layout = shelfLayoutFor(rowCount);
  const int kCellWidth = layout.cellWidth;
  const int kCellHeight = layout.cellHeight;
  const int kCellGap = layout.cellGap;
  const int kRowGap = layout.rowGap;  // 8 in 2-row, 0 in 1-row (no second row to space)
  const bool drawShadows = layout.drawShadows;
  constexpr int kSidePad = 16;

  const int bookCount = static_cast<int>(coverPaths.size());

  // Pre-clear the entire shelf zone (tab + cell row + shadow strip below
  // + focused-title strip below that). HomeActivity's end-of-render
  // snapshot means the prior frame's pixels (different focus ring,
  // different title text, wider arrows) would otherwise show through.
  // Extends `rect.height` by 40 px to wipe the shadow band (6 px) PLUS
  // the focused-title strip below it (one full UI_10 line height) with
  // generous padding so descenders/ascenders never leak past.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height + 56, false);

  const int tabY = rect.y;
  // Tab font is one step up from the row's prior 8px (SMALL_FONT_ID) to
  // 10px (UI_10_FONT_ID) so the collection name is readable from a glance
  // without dominating the strip vs. the cover thumbs.
  constexpr int kTabFontId = UI_10_FONT_ID;
  if (collectionName != nullptr && *collectionName != '\0') {
    // Bold + flanking left/right arrow glyphs when the header itself is
    // the focus, so the user has an obvious visual cue that L/R cycles
    // collections (and not, say, books within a row).
    const auto style = headerFocused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int nw = renderer.getTextWidth(kTabFontId, collectionName, style);
    const int nameX = rect.x + (rect.width - nw) / 2;
    renderer.drawText(kTabFontId, nameX, tabY, collectionName, true, style);

    if (headerFocused && hasMultipleCollections) {
      // Triangle hints flanking the name. Drawn as filled polygons so
      // they read as clear arrowheads at 10px font scale. Vertically
      // centered on the cap-height of the name.
      const int lineH = renderer.getLineHeight(kTabFontId);
      const int cy = tabY + lineH / 2;
      // Bumped from 4 → 7 px. The 4-px triangles read as decoration
      // more than navigation hints; 7 px is large enough that the
      // user reads them as "press L/R" at a glance.
      constexpr int kArrowSize = 7;
      constexpr int kArrowGap = 8;
      // Left ◀
      const int lTip = nameX - kArrowGap - kArrowSize;
      const int lBase = nameX - kArrowGap;
      const int lXs[3] = {lTip, lBase, lBase};
      const int lYs[3] = {cy, cy - kArrowSize, cy + kArrowSize};
      renderer.fillPolygon(lXs, lYs, 3, true);
      // Right ▶
      const int rTip = nameX + nw + kArrowGap + kArrowSize;
      const int rBase = nameX + nw + kArrowGap;
      const int rXs[3] = {rTip, rBase, rBase};
      const int rYs[3] = {cy, cy - kArrowSize, cy + kArrowSize};
      renderer.fillPolygon(rXs, rYs, 3, true);
    }
  }

  if (bookCount <= 0) return;

  const int cellsPerRow = layout.cellsPerRow;
  const int rows = layout.rowCount;
  const int visiblePerPage = cellsPerRow * rows;
  const int cellTotalW = kCellWidth + kCellGap;
  const int shelfRowY = tabY + kTabHeight + kTabBottomGap;
  // Total visible book slots this paint = min(perPage, remaining in collection).
  const int actualDrawn = std::min(visiblePerPage, bookCount - scrollOffset);
  // CrumBLE: when the row is partial (fewer books than cellsPerRow),
  // center the actual cell run inside the strip instead of anchoring
  // as if the row were full. Mirrors RecentBooksGridActivity's
  // partial-row centering for visual consistency between Bookshelf
  // grid and Collections shelf. Flow shelf is hard-pinned to 1-row so
  // we don't need to worry about per-row offsets in a 2-row layout.
  const int actualRowCells = std::min(cellsPerRow, std::max(0, actualDrawn));
  const int rowDrawW = (actualRowCells > 0)
                           ? actualRowCells * kCellWidth + (actualRowCells - 1) * kCellGap
                           : (cellsPerRow * kCellWidth + (cellsPerRow - 1) * kCellGap);
  const int rowStartX = rect.x + (rect.width - rowDrawW) / 2;
  // The bottom-edge scroll-arrow Y anchors to the centre of the LAST row
  // (so the arrow sits visually beside the cells regardless of 1- vs 2-row).
  const int lastRowMidY = shelfRowY + (rows - 1) * (kCellHeight + kRowGap) + kCellHeight / 2;

  // Page-stack depth cue: draw two dithered light-gray strips only on the
  // right and bottom of each cell (not behind the cover — that part was
  // wasted ink in the previous L-shape implementation). kShadowInset
  // pulls the strips in from the top-right and bottom-left corners so
  // the protrusion fades at the corners — same as how page edges aren't
  // visible at the corners of a real bound book.
  constexpr int kShadowDepth = 6;   // how far the page stack protrudes (px)
  constexpr int kShadowInset = 3;   // corner inset along the cover edge

  // Width of the dark spine glyph drawn LEFT of a series cell's cover.
  // The cover itself stays kCellWidth wide; the spine adds visual mass
  // on the left to signal "this is multiple books bound together".
  constexpr int kSeriesSpineWidth = 6;

  for (int i = 0; i < actualDrawn; ++i) {
    const int spineIdx = scrollOffset + i;
    // Row-major iteration: i = 0..cellsPerRow-1 fills top row, cellsPerRow..
    // 2*cellsPerRow-1 fills bottom row. Matches the user's "last book of
    // row 1 is followed by first book of row 2" mental model.
    const int row = i / cellsPerRow;
    const int col = i % cellsPerRow;
    const int x = rowStartX + col * cellTotalW;
    const int y = shelfRowY + row * (kCellHeight + kRowGap);
    const bool isSeries = seriesMemberCounts != nullptr && spineIdx < static_cast<int>(seriesMemberCounts->size()) &&
                          (*seriesMemberCounts)[spineIdx] >= 2;

    // Series spine: a solid black bar to the left of the cover. Reads
    // as a stack of books leaning against the front cover.
    if (isSeries) {
      renderer.fillRect(x - kSeriesSpineWidth - 1, y, kSeriesSpineWidth, kCellHeight, true);
    }

    if (drawShadows) {
      // Right edge: pages stacked along the fore-edge of the book.
      renderer.fillRectDither(x + kCellWidth, y + kShadowInset, kShadowDepth, kCellHeight - kShadowInset, Color::LightGray);
      // Bottom edge: pages along the tail of the book.
      renderer.fillRectDither(x + kShadowInset, y + kCellHeight, kCellWidth - kShadowInset, kShadowDepth, Color::LightGray);
      // Corner block where the two strips meet — fills the gap so the L
      // looks contiguous rather than missing its outer corner.
      renderer.fillRectDither(x + kCellWidth, y + kCellHeight, kShadowDepth, kShadowDepth, Color::LightGray);
    }

    bool drewThumb = false;
    if (spineIdx < static_cast<int>(coverPaths.size()) && !coverPaths[spineIdx].empty()) {
      const std::string& cellPath = coverPaths[spineIdx];
      // CrumBLE Phase A perf: try the in-RAM cache first. Cache hit blits
      // a pre-scaled 1bpp buffer (~1 ms). Cache miss (budget tight under
      // BLE / low heap) falls back to direct drawBitmap so the cell
      // still renders.
      GfxRenderer::CachedBitmap* handle = renderer.lookupCachedBitmap(cellPath);
      int srcW = 0, srcH = 0;
      const bool cacheHit =
          renderer.getCachedBitmapDimensions(handle, &srcW, &srcH) && srcW > 0 && srcH > 0;
      auto applyAspectFillAndDrawCached = [&](int sw, int sh) {
        const float srcRatio = static_cast<float>(sw) / static_cast<float>(sh);
        const float targetRatio = static_cast<float>(kCellWidth) / static_cast<float>(kCellHeight);
        float cropX = 0.0f;
        float cropY = 0.0f;
        if (srcRatio > targetRatio) {
          cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
        } else if (srcRatio < targetRatio) {
          cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
        }
        renderer.drawCachedBitmap(handle, x, y, kCellWidth, kCellHeight, cropX, cropY);
      };
      if (cacheHit) {
        applyAspectFillAndDrawCached(srcW, srcH);
        drewThumb = true;
      } else {
        // Cache miss (low-heap budget or file open in cache failed):
        // fall back to direct SD-streamed drawBitmap so the cell still
        // renders. Bitmap can't be reassigned (deleted copy ctor) so we
        // do all the work inside this branch.
        FsFile file;
        if (Storage.openFileForRead("LFT", cellPath, file)) {
          Bitmap fallback(file);
          if (fallback.parseHeaders() == BmpReaderError::Ok && fallback.getWidth() > 0 && fallback.getHeight() > 0) {
            const float srcRatio =
                static_cast<float>(fallback.getWidth()) / static_cast<float>(fallback.getHeight());
            const float targetRatio = static_cast<float>(kCellWidth) / static_cast<float>(kCellHeight);
            float cropX = 0.0f;
            float cropY = 0.0f;
            if (srcRatio > targetRatio) {
              cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
            } else if (srcRatio < targetRatio) {
              cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
            }
            renderer.drawBitmap(fallback, x, y, kCellWidth, kCellHeight, cropX, cropY);
            drewThumb = true;
          }
          file.close();
        }
      }
    }
    if (!drewThumb) {
      // CrumBLE: placeholder shows the centered, wrapped book title --
      // mirrors RecentBooksGridActivity's placeholder + the carousel
      // fallback. Reads as an intentional card with the book name
      // showing through, instead of a generic icon. Falls back to the
      // CoverIcon when no title was passed (cellTitles == nullptr) or
      // the entry is empty (series cell, missing metadata).
      renderer.drawRoundedRect(x, y, kCellWidth, kCellHeight, 1, 3, true);
      const std::string* cellTitle =
          (cellTitles != nullptr && spineIdx < static_cast<int>(cellTitles->size()) && !(*cellTitles)[spineIdx].empty())
              ? &(*cellTitles)[spineIdx]
              : nullptr;
      if (cellTitle != nullptr) {
        constexpr int kPlaceholderPadX = 4;
        // Cap title wrapping so a long boxed-set title (e.g. "George R. R.
        // Martin's A Game of Thrones 5-Book Boxed Set") doesn't fill the
        // entire placeholder cell with wrapped text. 4 lines matches the
        // MinimalTheme placeholder convention and leaves visual room for
        // the cell to read as "missing cover, here's a hint of the title"
        // rather than a wall of small bold text. wrappedText auto-
        // ellipsizes on the last line when the title overflows the cap.
        constexpr int kPlaceholderMaxLines = 4;
        const auto titleLines = renderer.wrappedText(SMALL_FONT_ID, cellTitle->c_str(),
                                                      kCellWidth - 2 * kPlaceholderPadX, kPlaceholderMaxLines,
                                                      EpdFontFamily::BOLD);
        const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
        const int blockH = static_cast<int>(titleLines.size()) * lineH;
        int textY = y + (kCellHeight - blockH) / 2;
        for (const auto& line : titleLines) {
          const int lineW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
          renderer.drawText(SMALL_FONT_ID, x + (kCellWidth - lineW) / 2, textY, line.c_str(), true,
                            EpdFontFamily::BOLD);
          textY += lineH;
        }
      } else {
        constexpr int kIconSize = 24;
        const int iconX = x + (kCellWidth - kIconSize) / 2;
        const int iconY = y + (kCellHeight - kIconSize) / 2;
        renderer.drawIcon(CoverIcon, iconX, iconY, kIconSize, kIconSize);
      }
    }

    if (spineIdx == selectedSpineIndex) {
      // Focus ring outside the cell so the cover itself isn't covered.
      renderer.drawRoundedRect(x - 3, y - 3, kCellWidth + 6, kCellHeight + 6, 2, 5, true);
    }
  }

  if (bookCount > visiblePerPage) {
    // Anchor the page-flip arrows vertically halfway down the strip (between
    // the two rows in 2-row mode) so they remain readable as "L/R = paginate"
    // hints regardless of which row the cursor is on.
    const int triY = shelfRowY + ((rows - 1) * (kCellHeight + kRowGap) + kCellHeight) / 2;
    (void)lastRowMidY;
    constexpr int triSize = 4;
    if (scrollOffset > 0) {
      const int tx = rect.x + kSidePad / 2;
      const int triXs[3] = {tx + triSize, tx - 1, tx + triSize};
      const int triYs[3] = {triY - triSize, triY, triY + triSize};
      renderer.fillPolygon(triXs, triYs, 3, true);
    }
    if (scrollOffset + visiblePerPage < bookCount) {
      const int tx = rect.x + rect.width - kSidePad / 2;
      const int triXs[3] = {tx - triSize, tx + 1, tx - triSize};
      const int triYs[3] = {triY - triSize, triY, triY + triSize};
      renderer.fillPolygon(triXs, triYs, 3, true);
    }
  }

  // Focused book title, centered below the cell row. Same affordance as
  // the carousel showing the active book name under its cover. Anchored
  // a few px below the shadow's extent so it doesn't crowd the cells.
  // The strip rect is sized to JUST the tab + cells, so this drops into
  // the small breathing-room band between the strip and the icon bar.
  //
  // CrumBLE: dropped the author second-line from this function. The author
  // (when a book is focused) is now rendered into the icon bar's label
  // band by drawButtonMenu via focusedBookAuthorForLabel -- HomeActivity
  // sets that pointer when a shelf book is focused. The previous author
  // position here was being wiped by drawButtonMenu's pre-render clear,
  // making it invisible.
  if (selectedSpineIndex >= 0 && focusedBookTitle != nullptr && *focusedBookTitle != '\0') {
    constexpr int kTitleFontId = UI_10_FONT_ID;
    constexpr int kTitleTopGap = 0;
    // Anchor under the LAST row (whichever it is in 1- vs 2-row mode) so
    // the title doesn't get tucked between the two rows in 2-row layout.
    const int lastRowBottomY = shelfRowY + (rows - 1) * (kCellHeight + kRowGap) + kCellHeight;
    const int titleY = lastRowBottomY + kShadowDepth + kTitleTopGap;
    const auto truncated = renderer.truncatedText(kTitleFontId, focusedBookTitle, rect.width - 2 * kSidePad);
    const int tw = renderer.getTextWidth(kTitleFontId, truncated.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(kTitleFontId, rect.x + (rect.width - tw) / 2, titleY, truncated.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
  (void)focusedBookAuthor;  // intentionally unused; consumed by drawButtonMenu via focusedBookAuthorForLabel
}

void LyraFlowTheme::drawBookshelfStripFocusUpdate(GfxRenderer& renderer, Rect rect, int prevFocusedSpine,
                                                  int newFocusedSpine, int scrollOffset, int bookCount,
                                                  const char* focusedBookTitle,
                                                  const std::vector<int>* seriesMemberCounts, int rowCount) const {
  // CrumBLE #125: layout constants MUST match drawBookshelfStrip exactly --
  // we're patching pixels into a framebuffer that was painted by it. Any
  // drift here would leave the focus ring offset from the cells. If
  // drawBookshelfStrip is restructured, update both in lockstep (or
  // factor a shared layout helper).
  const ShelfLayout layout = shelfLayoutFor(rowCount);
  const int kCellWidth = layout.cellWidth;
  const int kCellHeight = layout.cellHeight;
  const int kCellGap = layout.cellGap;
  const int kRowGap = layout.rowGap;
  const bool drawShadows = layout.drawShadows;
  constexpr int kSidePad = 16;
  constexpr int kTabHeight = 18;
  constexpr int kTabBottomGap = 12;
  constexpr int kShadowDepth = 6;
  constexpr int kShadowInset = 3;
  constexpr int kSeriesSpineWidth = 6;
  constexpr int kRingOutset = 3;  // ring sits 3 px outside the cell
  constexpr int kRingStroke = 2;
  constexpr int kRingRadius = 5;

  const int cellsPerRow = layout.cellsPerRow;
  const int rows = layout.rowCount;
  const int visiblePerPage = cellsPerRow * rows;
  const int cellTotalW = kCellWidth + kCellGap;
  const int tabY = rect.y;
  const int shelfRowY = tabY + kTabHeight + kTabBottomGap;
  // CrumBLE: match drawBookshelfStrip's partial-row centering so the
  // focus-only path puts the ring on the SAME pixels the full paint
  // used. Without this, focus updates on a < cellsPerRow collection
  // would draw the ring offset from the cell it visually frames.
  const int actualDrawn = std::min(visiblePerPage, std::max(0, bookCount - scrollOffset));
  const int actualRowCells = std::min(cellsPerRow, std::max(0, actualDrawn));
  const int rowDrawW = (actualRowCells > 0)
                           ? actualRowCells * kCellWidth + (actualRowCells - 1) * kCellGap
                           : (cellsPerRow * kCellWidth + (cellsPerRow - 1) * kCellGap);
  const int rowStartX = rect.x + (rect.width - rowDrawW) / 2;

  auto cellOriginForSpine = [&](int spineIdx, int& outX, int& outY) -> bool {
    const int i = spineIdx - scrollOffset;
    if (i < 0 || i >= visiblePerPage) return false;
    const int row = i / cellsPerRow;
    const int col = i % cellsPerRow;
    outX = rowStartX + col * cellTotalW;
    outY = shelfRowY + row * (kCellHeight + kRowGap);
    return true;
  };

  // --- Erase the OLD focus ring (if it was visible). The ring strokes
  //     sit in a 3 px frame around the cell with a 2 px stroke width.
  //     The TOP and LEFT strokes are on the page-white substrate (no
  //     shadow there) -- white fillRects erase cleanly. The RIGHT and
  //     BOTTOM strokes overlap with the dithered shadow strips; we
  //     re-paint those shadow strips (which overwrites the affected
  //     ring stroke pixels with the correct dither) instead of trying
  //     to white-fill them. Series cells additionally have a solid
  //     black spine at x-7..x-1; the LEFT ring stroke at x-3..x-1
  //     would have overdrawn its right 3 px, so restore the spine
  //     after erasing the ring.
  if (prevFocusedSpine >= 0) {
    int x = 0, y = 0;
    if (cellOriginForSpine(prevFocusedSpine, x, y)) {
      // TOP stroke (entirely on white substrate)
      renderer.fillRect(x - kRingOutset, y - kRingOutset, kCellWidth + 2 * kRingOutset, kRingStroke, false);
      // LEFT stroke (on white substrate; may clip a series spine -- restored below)
      renderer.fillRect(x - kRingOutset, y - kRingOutset, kRingStroke, kCellHeight + 2 * kRingOutset, false);

      const bool prevIsSeries = seriesMemberCounts != nullptr &&
                                prevFocusedSpine < static_cast<int>(seriesMemberCounts->size()) &&
                                (*seriesMemberCounts)[prevFocusedSpine] >= 2;

      if (drawShadows) {
        // Re-dither the shadow strips. These cover the right + bottom
        // ring strokes (which overlap with the inner edge of the shadow
        // at the inset). Same calls as the full-strip draw.
        renderer.fillRectDither(x + kCellWidth, y + kShadowInset, kShadowDepth, kCellHeight - kShadowInset,
                                Color::LightGray);
        renderer.fillRectDither(x + kShadowInset, y + kCellHeight, kCellWidth - kShadowInset, kShadowDepth,
                                Color::LightGray);
        renderer.fillRectDither(x + kCellWidth, y + kCellHeight, kShadowDepth, kShadowDepth, Color::LightGray);
        // The shadow's inner inset (top kShadowInset px on the right
        // strip, left kShadowInset px on the bottom strip) doesn't have
        // dither -- it sits on the page-white substrate. White-fill
        // those small slivers to wipe the corresponding ring stroke
        // pixels.
        renderer.fillRect(x + kCellWidth + 1, y - kRingOutset, kRingStroke, kShadowInset + kRingOutset, false);
        renderer.fillRect(x - kRingOutset, y + kCellHeight + 1, kShadowInset + kRingOutset, kRingStroke, false);
      } else {
        // No shadow this layout -- just white-fill the right + bottom strokes.
        renderer.fillRect(x + kCellWidth + 1, y - kRingOutset, kRingStroke, kCellHeight + 2 * kRingOutset, false);
        renderer.fillRect(x - kRingOutset, y + kCellHeight + 1, kCellWidth + 2 * kRingOutset, kRingStroke, false);
      }

      if (prevIsSeries) {
        renderer.fillRect(x - kSeriesSpineWidth - 1, y, kSeriesSpineWidth, kCellHeight, true);
      }
    }
  }

  // --- Draw the NEW focus ring (if a cell is newly focused). Same call
  //     the full-strip draw uses, so geometry matches pixel-perfectly.
  if (newFocusedSpine >= 0) {
    int x = 0, y = 0;
    if (cellOriginForSpine(newFocusedSpine, x, y)) {
      renderer.drawRoundedRect(x - kRingOutset, y - kRingOutset, kCellWidth + 2 * kRingOutset,
                               kCellHeight + 2 * kRingOutset, kRingStroke, kRingRadius, true);
    }
  }

  // --- Re-render the focused-book title strip below the cells. The
  //     framebuffer-restored pixels carry the OLD title text; if the
  //     new title is shorter, leftover characters would show through.
  //     Clear a generous band and redraw centered.
  constexpr int kTitleFontId = UI_10_FONT_ID;
  constexpr int kTitleTopGap = 0;
  const int lastRowBottomY = shelfRowY + (rows - 1) * (kCellHeight + kRowGap) + kCellHeight;
  const int titleY = lastRowBottomY + kShadowDepth + kTitleTopGap;
  // Clear the full strip width across the title's vertical band (line
  // height + a few px of safety for ascenders/descenders).
  const int titleLineH = renderer.getLineHeight(kTitleFontId);
  renderer.fillRect(rect.x, titleY - 2, rect.width, titleLineH + 4, false);
  if (newFocusedSpine >= 0 && focusedBookTitle != nullptr && *focusedBookTitle != '\0') {
    const auto truncated = renderer.truncatedText(kTitleFontId, focusedBookTitle, rect.width - 2 * kSidePad);
    const int tw = renderer.getTextWidth(kTitleFontId, truncated.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(kTitleFontId, rect.x + (rect.width - tw) / 2, titleY, truncated.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
  (void)bookCount;  // future-proof for visiblePerPage / scroll-arrow logic
}

void LyraFlowTheme::prerenderCarouselSideTiles(GfxRenderer& renderer,
                                                const std::vector<RecentBook>& recentBooks) const {
  // Always rebuild from scratch -- the recentBooks vector may have
  // reordered (just-read book promotion) or shrunk (delete). Tiles are
  // keyed by book PATH so the table is stable across reorderings, but
  // we drop stale entries here for tidiness.
  sideTileCache_.clear();

  // Same hL/hR convention drawStackedCover uses. Left side: taller on
  // left edge, shorter on right edge. Right side is the mirror. Tiles
  // are rendered at the natural hMax (288) so they cover the trapezoidal
  // bounding box; positions inside the box with colH < hMax are simply
  // unset bits in the tile.
  const int hL_left = sideInnerHeight;
  const int hR_left = sideOuterHeight;
  const int hL_right = sideOuterHeight;
  const int hR_right = sideInnerHeight;
  const int hMax = std::max(sideInnerHeight, sideOuterHeight);
  const int dstStride = (sideCoverWidth + 7) / 8;
  const size_t tileBytes = static_cast<size_t>(dstStride) * static_cast<size_t>(hMax);

  for (const auto& book : recentBooks) {
    if (book.path.empty()) continue;
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, centerCoverHeight);
    if (coverPath.empty()) continue;
    // Triggers SD load + cache populate if not already resident. Same
    // call drawStackedCover would do on first render anyway -- we just
    // do it up front so the per-press path stays pure RAM.
    GfxRenderer::CachedBitmap* handle = renderer.lookupCachedBitmap(coverPath);
    int srcW = 0, srcH = 0;
    if (!renderer.getCachedBitmapDimensions(handle, &srcW, &srcH) || srcW <= 0 || srcH <= 0) {
      continue;  // skip; drawStackedCover will fall through to perspective-render
    }

    BookSideTiles& tiles = sideTileCache_[book.path];

    // Left-perspective tile (used for both left-near and left-far drawX).
    tiles.left.pixels.reset(new (std::nothrow) uint8_t[tileBytes]);
    if (tiles.left.pixels) {
      std::memset(tiles.left.pixels.get(), 0, tileBytes);
      renderer.renderPerspectiveBitmapToPacked1bpp(handle, sideCoverWidth, hL_left, hR_left, tiles.left.pixels.get());
      tiles.left.width = sideCoverWidth;
      tiles.left.height = hMax;
    }

    // Right-perspective tile (mirror of left).
    tiles.right.pixels.reset(new (std::nothrow) uint8_t[tileBytes]);
    if (tiles.right.pixels) {
      std::memset(tiles.right.pixels.get(), 0, tileBytes);
      renderer.renderPerspectiveBitmapToPacked1bpp(handle, sideCoverWidth, hL_right, hR_right,
                                                    tiles.right.pixels.get());
      tiles.right.width = sideCoverWidth;
      tiles.right.height = hMax;
    }
  }
}

void LyraFlowTheme::clearCarouselSideTiles() const { sideTileCache_.clear(); }

void LyraFlowTheme::drawButtonMenu(GfxRenderer& renderer, Rect /*rect*/, int buttonCount, int selectedIndex,
                                   const std::function<std::string(int index)>& buttonLabel,
                                   const std::function<UIIcon(int index)>& rowIcon) const {
  // Bottom-anchored horizontal icon bar, modelled on LyraCarouselTheme.
  // The selected icon gets a rounded black highlight and its label is
  // centered just above the row. The passed `rect` is ignored — the bar
  // always anchors to the screen bottom regardless of where the home
  // carousel placed its menu area.
  if (buttonCount <= 0) return;

  constexpr int kMenuIconSize = 32;
  constexpr int kMenuIconPad = 14;          // → tile height = 14+32+14 = 60
  constexpr int kHighlightPad = 7;          // ring of padding around selected icon
  constexpr int kHighlightCorner = 6;
  constexpr int kMenuLabelTopGap = 3;       // gap between label and icon row
  constexpr int kMenuLabelBottomGap = 4;    // gap below label baseline
  constexpr int kMenuRowDrop = 31;          // pushes the bar closer to the screen bottom

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int hintsH = LyraFlowMetrics::values.buttonHintsHeight;
  const int tileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
  const int tileW = screenW / buttonCount;
  const int labelLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowY = screenH - hintsH - tileH - kMenuLabelTopGap - labelLineHeight - kMenuLabelBottomGap + kMenuRowDrop;
  const int labelY = rowY - kMenuLabelTopGap - labelLineHeight;

  // Wipe the bar's vertical span so any prior render of the previous list
  // layout doesn't bleed through under the new tighter geometry.
  renderer.fillRect(0, labelY, screenW, screenH - hintsH - labelY, false);

  for (int i = 0; i < buttonCount; ++i) {
    const int tileX = i * tileW;
    const int iconX = tileX + (tileW - kMenuIconSize) / 2;
    const int iconY = rowY + kMenuIconPad;
    const bool selected = (selectedIndex == i);

    if (selected) {
      const int highlightSize = kMenuIconSize + 2 * kHighlightPad;
      const int highlightY = rowY + (tileH - highlightSize) / 2;
      renderer.fillRoundedRect(iconX - kHighlightPad, highlightY, highlightSize, highlightSize, kHighlightCorner,
                               Color::Black);
    }

    if (rowIcon != nullptr) {
      const UIIcon icon = rowIcon(i);
      if (icon == UIIcon::BookmarkIcon) {
        // Status-bar bookmark ribbon shape, drawn in the same slot as a
        // regular icon. Mirrors the Flow list version, just centered in
        // the tile instead of left-aligned.
        constexpr int ribbonWidth = 16;
        constexpr int ribbonHeight = 22;
        constexpr int notchSize = 6;
        const int ribbonX = iconX + (kMenuIconSize - ribbonWidth) / 2;
        const int ribbonY = iconY + (kMenuIconSize - ribbonHeight) / 2;
        const int centerX = ribbonX + ribbonWidth / 2;
        const int polyX[5] = {ribbonX, ribbonX + ribbonWidth, ribbonX + ribbonWidth, centerX, ribbonX};
        const int polyY[5] = {ribbonY, ribbonY, ribbonY + ribbonHeight, ribbonY + ribbonHeight - notchSize,
                              ribbonY + ribbonHeight};
        renderer.fillPolygon(polyX, polyY, 5, !selected);
      } else {
        const uint8_t* bmp = LyraTheme::iconForName(icon, kMenuIconSize);
        if (bmp != nullptr) {
          if (selected) {
            renderer.drawIconInverted(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
          } else {
            renderer.drawIcon(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
          }
        }
      }
    }
  }

  // Centered label above the icon row. Dual-purpose:
  //   - When an icon is focused: shows that icon's name (existing behavior).
  //   - When no icon is focused BUT a Collections book is focused:
  //     HomeActivity has set focusedBookAuthorForLabel to that book's
  //     author; we use it as the label text. Same physical slot, so the
  //     user always sees ONE contextual label adjacent to the icon bar.
  //
  // One-shot: focusedBookAuthorForLabel is cleared at the end so a stale
  // value can't leak into a subsequent frame.
  std::string labelStr;
  if (selectedIndex >= 0 && selectedIndex < buttonCount && buttonLabel != nullptr) {
    labelStr = buttonLabel(selectedIndex);
  } else if (!focusedBookAuthorForLabel.empty()) {
    labelStr = focusedBookAuthorForLabel;
  }
  if (!labelStr.empty()) {
    const auto centered = renderer.truncatedText(SMALL_FONT_ID, labelStr.c_str(), screenW - 40);
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, centered.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, (screenW - labelWidth) / 2, labelY + 2, centered.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
  focusedBookAuthorForLabel.clear();
}
