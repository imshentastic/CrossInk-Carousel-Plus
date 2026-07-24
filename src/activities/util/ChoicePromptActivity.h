#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"

// CrumBLE: 3-option (or N-option) confirmation dialog. Pattern mirrors
// ConfirmationActivity (heading + body + button hints) but the bottom of the
// dialog renders a vertical list of named options the user navigates with
// Up/Down and picks with Confirm; Back always cancels.
//
// Why a new activity instead of extending ConfirmationActivity: the picker
// has a per-option cursor + nav loop that doesn't apply to the simpler
// 2-button confirm case, and shoving both behaviors into one class made it
// harder to read. Tiny code footprint (~150 lines) makes the duplication
// worth the separation.
//
// Result: ChoicePromptResult { choice }. choice == -1 means Back/Cancel;
// choice >= 0 is the picked option index.
class ChoicePromptActivity final : public Activity {
 public:
  // v18.9.9.270: added `defaultSelectedIndex` so callers can pre-cursor
  // on a preferred option (e.g. the dict picker parking the cursor on
  // the last-picked dictionary so a "same choice" flow is one tap).
  // Clamped to [0, options.size()-1]; out-of-range values fall back to 0.
  ChoicePromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading, std::string body,
                       std::vector<std::string> options, bool ignoreInitialConfirmRelease = false,
                       int defaultSelectedIndex = 0);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  std::string heading_;
  std::string body_;
  std::vector<std::string> options_;

  const int margin_ = 20;
  const int spacing_ = 12;
  const int fontId_ = UI_10_FONT_ID;

  std::vector<std::string> headingLines_;
  std::vector<std::string> bodyLines_;
  int startY_ = 0;
  int lineHeight_ = 0;
  int selectedIndex_ = 0;
  bool ignoreConfirmRelease_ = false;
};
