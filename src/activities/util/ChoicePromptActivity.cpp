#include "ChoicePromptActivity.h"

#include <I18n.h>

#include <algorithm>

#include "../ActivityResult.h"
#include "HalDisplay.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

ChoicePromptActivity::ChoicePromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string heading,
                                           std::string body, std::vector<std::string> options,
                                           bool ignoreInitialConfirmRelease, int defaultSelectedIndex)
    : Activity("ChoicePrompt", renderer, mappedInput),
      heading_(std::move(heading)),
      body_(std::move(body)),
      options_(std::move(options)),
      ignoreConfirmRelease_(ignoreInitialConfirmRelease) {
  // v18.9.9.270: clamp the default cursor into range. If options_ was
  // empty (caller mistake) the activity is broken anyway; leave at 0.
  if (!options_.empty() && defaultSelectedIndex >= 0 &&
      defaultSelectedIndex < static_cast<int>(options_.size())) {
    selectedIndex_ = defaultSelectedIndex;
  }
}

void ChoicePromptActivity::onEnter() {
  Activity::onEnter();

  const auto& metrics = UITheme::getInstance().getMetrics();
  lineHeight_ = renderer.getLineHeight(fontId_);
  const int maxWidth = renderer.getScreenWidth() - (margin_ * 2);
  const int contentTop = margin_;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - margin_;
  const int contentHeight = contentBottom - contentTop;

  // Reserve vertical space for the options list at the bottom of the content
  // area (each option a row), then give heading + body whatever's left.
  const int optionsHeight = static_cast<int>(options_.size()) * lineHeight_ + spacing_;
  const int textBudget = std::max(lineHeight_, contentHeight - optionsHeight);
  const int textLineCap = std::max(1, textBudget / lineHeight_);

  if (!heading_.empty()) {
    const int headingLineCap = body_.empty() ? textLineCap : std::min(2, std::max(1, textLineCap - 1));
    headingLines_ = renderer.wrappedText(fontId_, heading_.c_str(), maxWidth, headingLineCap, EpdFontFamily::BOLD);
  }
  int totalTextLines = static_cast<int>(headingLines_.size());
  if (!body_.empty()) {
    int remaining = std::max(1, textLineCap - totalTextLines);
    // Honor hard '\n' in body (same trick as ConfirmationActivity).
    size_t start = 0;
    while (start <= body_.size() && remaining > 0) {
      const size_t nl = body_.find('\n', start);
      const std::string segment = body_.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
      if (segment.empty()) {
        bodyLines_.push_back("");
        --remaining;
      } else {
        auto wrapped = renderer.wrappedText(fontId_, segment.c_str(), maxWidth, remaining, EpdFontFamily::REGULAR);
        for (auto& l : wrapped) {
          if (remaining <= 0) break;
          bodyLines_.push_back(std::move(l));
          --remaining;
        }
      }
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
    totalTextLines += static_cast<int>(bodyLines_.size());
  }

  const int textHeight =
      static_cast<int>(headingLines_.size()) * lineHeight_ +
      (headingLines_.empty() || bodyLines_.empty() ? 0 : spacing_) +
      static_cast<int>(bodyLines_.size()) * lineHeight_;
  startY_ = contentTop + std::max(0, (textBudget - textHeight) / 2);

  selectedIndex_ = 0;
  requestUpdate(true);
}

void ChoicePromptActivity::loop() {
  using B = MappedInputManager::Button;

  // Swallow the long-press Confirm release that opened the prompt (caller can
  // request via ignoreInitialConfirmRelease=true) so the user doesn't
  // accidentally pick the default option as soon as the dialog appears.
  if (ignoreConfirmRelease_) {
    if (mappedInput.wasReleased(B::Confirm)) {
      ignoreConfirmRelease_ = false;
      return;
    }
    if (!mappedInput.isPressed(B::Confirm)) {
      ignoreConfirmRelease_ = false;
    }
  }

  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Left)) {
    if (!options_.empty()) {
      selectedIndex_ = (selectedIndex_ - 1 + static_cast<int>(options_.size())) % static_cast<int>(options_.size());
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::Right)) {
    if (!options_.empty()) {
      selectedIndex_ = (selectedIndex_ + 1) % static_cast<int>(options_.size());
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(B::Confirm)) {
    ChoicePromptResult res;
    res.choice = selectedIndex_;
    setResult(ActivityResult{res});
    finish();
    return;
  }

  if (mappedInput.wasReleased(B::Back)) {
    ChoicePromptResult res;
    res.choice = -1;  // Back = cancel sentinel
    ActivityResult ar;
    ar.isCancelled = true;
    ar.data = res;
    setResult(std::move(ar));
    finish();
    return;
  }
}

void ChoicePromptActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY_;
  for (const auto& line : headingLines_) {
    renderer.drawCenteredText(fontId_, currentY, line.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight_;
  }
  if (!headingLines_.empty() && !bodyLines_.empty()) {
    currentY += spacing_;
  }
  for (const auto& line : bodyLines_) {
    renderer.drawCenteredText(fontId_, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight_;
  }

  // Options list at the bottom: each option centered on its own line, the
  // selected one drawn in bold to make the cursor unmistakable on e-ink.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int optionsTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - margin_ -
                         static_cast<int>(options_.size()) * lineHeight_;
  for (size_t i = 0; i < options_.size(); ++i) {
    const std::string label =
        (static_cast<int>(i) == selectedIndex_) ? std::string("> ") + options_[i] + " <" : options_[i];
    const EpdFontFamily::Style style =
        (static_cast<int>(i) == selectedIndex_) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawCenteredText(fontId_, optionsTop + static_cast<int>(i) * lineHeight_, label.c_str(), true, style);
  }

  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_SELECT),
                                            I18N.get(StrId::STR_DIR_UP), I18N.get(StrId::STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}
