#pragma once

#include <Arduino.h>
#include <cstdint>
#include <algorithm>

// Tracks a dirty bounding-box for incremental e-ink rendering. Activities
// call markDirty() each time they update a rect of the framebuffer during
// a frame; at end-of-frame they call getBoundingBox() to get the smallest
// rect that covers all updates, then pass that to
// HalDisplay::displayBufferRegion() so only the changed pixels are pushed
// to the panel.
//
// MVP semantics: keeps a single bounding box (not a list of disjoint
// rects). Simpler + smaller (16 bytes) and matches the common case where
// dirty regions are spatially clustered (a cover swap + matching text
// strip below it). For pathological cases where two small updates land at
// opposite corners, the bounding box grows to cover everything between
// them -- which is fine because displayBufferRegion's >=70%-of-screen
// fallback then promotes back to full refresh.
//
// Usage:
//   DirtyRegion dirty;
//   dirty.markDirty(coverX, coverY, coverW, coverH);
//   dirty.markDirty(titleX, titleY, titleW, titleH);
//   auto bbox = dirty.getBoundingBox();
//   if (!dirty.isEmpty()) {
//     display.displayBufferRegion(bbox.x, bbox.y, bbox.w, bbox.h);
//     dirty.clear();
//   }
class DirtyRegion {
 public:
  struct Rect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
  };

  bool isEmpty() const { return empty_; }

  // Add a rect to the dirty bounding box. Empty rects (w==0 or h==0) are
  // ignored. If this is the first markDirty() call (or after clear()), the
  // bbox becomes exactly this rect; subsequent calls grow it to cover.
  void markDirty(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (w == 0 || h == 0) return;
    const uint16_t x2 = x + w;  // exclusive right
    const uint16_t y2 = y + h;  // exclusive bottom
    if (empty_) {
      minX_ = x;
      minY_ = y;
      maxX_ = x2;
      maxY_ = y2;
      empty_ = false;
    } else {
      minX_ = std::min(minX_, x);
      minY_ = std::min(minY_, y);
      maxX_ = std::max(maxX_, x2);
      maxY_ = std::max(maxY_, y2);
    }
  }

  Rect getBoundingBox() const {
    if (empty_) return {0, 0, 0, 0};
    return {minX_, minY_, static_cast<uint16_t>(maxX_ - minX_),
            static_cast<uint16_t>(maxY_ - minY_)};
  }

  void clear() {
    minX_ = 0;
    minY_ = 0;
    maxX_ = 0;
    maxY_ = 0;
    empty_ = true;
  }

 private:
  uint16_t minX_ = 0;
  uint16_t minY_ = 0;
  uint16_t maxX_ = 0;  // exclusive
  uint16_t maxY_ = 0;  // exclusive
  bool empty_ = true;
};
