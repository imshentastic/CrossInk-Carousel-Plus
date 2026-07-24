#pragma once

#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {

struct TabItem {
  const char* label = nullptr;
  BitmapRef icon{};
  AssetRef iconAsset{};
  int16_t value = 0;
  bool selected = false;
  bool enabled = true;
};

inline TabItem tabItem(const int value, const bool selected = false, const bool enabled = true,
                       const char* label = nullptr) {
  TabItem item;
  item.label = label;
  item.value = clampI16(value, -32768);
  item.selected = selected;
  item.enabled = enabled;
  return item;
}

using TabIconPainter = bool (*)(DrawTarget& target, Rect rect, const TabItem& tab, uint8_t index, void* userData);

struct TabBarProps {
  const TabItem* tabs = nullptr;
  uint8_t count = 0;
  ActionId action = NO_ACTION;
  uint16_t inputMask = InputDefault | InputPrev | InputNext;
  TextStyle text{};
  // selected state drives the pill: set selected.background + radius.
  StyleSet tabStyles{};
  // Inset of each tab pill within its equal-width slot.
  Insets tabInset{4, 4, 8, 4};
  int16_t gap = 0;
  int16_t iconSize = 0;
  int16_t minTouchSize = 44;
  TabIconPainter iconPainter = nullptr;
  void* iconPainterUserData = nullptr;
  // 1px divider along the bottom edge (RoundedRaff-style settings tabs).
  bool divider = false;
  Paint dividerPaint = Paint::solid(Color::Black);
};

template <size_t MaxInteractions>
void tabBar(Frame<MaxInteractions>& frame, Rect rect, const TabBarProps& props) {
  if (!props.tabs || props.count == 0) return;
  StyleSet styles = props.tabStyles;
  if (styles.unset()) {
    styles.selected.background = Paint::solid(Color::Black);
    styles.selected.foreground = Paint::solid(Color::White);
  }

  const int16_t dividerH = props.divider ? 1 : 0;
  const int16_t gap = props.gap > 0 ? props.gap : 0;
  const int16_t slotW = static_cast<int16_t>((rect.width - gap * (props.count - 1)) / props.count);
  for (uint8_t i = 0; i < props.count; ++i) {
    const TabItem& tab = props.tabs[i];
    const int16_t slotX = static_cast<int16_t>(rect.x + i * (slotW + gap));
    Rect slot{slotX, rect.y,
              static_cast<int16_t>(i == props.count - 1 ? rect.right() - slotX : slotW),
              static_cast<int16_t>(rect.height - dividerH)};
    Rect pill = slot.inset(props.tabInset);
    if (pill.empty()) pill = slot;
    State state = tab.selected ? StateSelected : StateNormal;
    if (!tab.enabled) state |= StateDisabled;
    if (tab.enabled && props.action != NO_ACTION) {
      frame.hit(ensureMinTouchRect(slot, props.minTouchSize, frame.screen()), props.action, tab.value,
                props.inputMask, state);
    }
    state = frame.stateFor(props.action, tab.value, state);
    const BoxStyle& style = styles.resolve(state);
    frame.target().fill(pill, style.background, style.radius, style.corners);
    if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
      frame.target().stroke(pill, style.border, style.borderWidth, style.radius, style.corners);
    }
    BitmapRef icon = tab.icon ? tab.icon : resolveBitmap(frame.assets(), tab.iconAsset);
    const bool hasIcon = icon || props.iconPainter != nullptr;
    const bool hasLabel = tab.label != nullptr && tab.label[0] != '\0';
    if (hasIcon) {
      const int16_t iconSize =
          props.iconSize > 0 ? props.iconSize : static_cast<int16_t>(icon ? (icon.height > 0 ? icon.height : icon.width) : 16);
      Rect iconRect{static_cast<int16_t>(pill.x + (pill.width - iconSize) / 2),
                    static_cast<int16_t>(pill.y + (pill.height - iconSize) / 2), iconSize, iconSize};
      bool drawn = false;
      if (props.iconPainter) drawn = props.iconPainter(frame.target(), iconRect, tab, i, props.iconPainterUserData);
      if (!drawn && icon) frame.target().bitmap(iconRect, icon, BitmapMode::Center, style.foreground);
    }
    if (hasLabel) {
      TextStyle label = textStyleWithForeground(props.text, style.foreground);
      label.align = TextAlign::Center;
      frame.target().text(pill, tab.label, label);
    }
  }
  if (props.divider) {
    frame.target().fill(Rect{rect.x, static_cast<int16_t>(rect.bottom() - 1), rect.width, 1}, props.dividerPaint);
  }
}

}  // namespace ui
}  // namespace freeink
