#pragma once

#include <string>

#include "../Activity.h"
#include "PrebakeManifest.h"

// CrumBLE 4.2: read-only inspector for a book's prebake-manifest.json.
// Surfaced from the long-press menu when the user navigates UP onto the
// "Optimized" header label and presses Confirm. Shows every reader
// setting baked into the .json so the user can verify what their saved
// layout actually captured (helpful when troubleshooting a "use prepared
// layout but indexing every chapter" symptom -- they can eyeball the
// fontId / size combo and compare to current SETTINGS without restoring).
//
// Navigation: Back exits, nothing else does anything. Intentionally
// non-interactive -- this is an info screen, not a settings editor. To
// actually APPLY any of these values the user goes back to the long-
// press menu and picks the "Restore prepared layout" prompt via the
// normal open-book flow.
class PrebakeManifestViewerActivity final : public Activity {
 public:
  PrebakeManifestViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                                PrebakeManifest manifest);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string bookTitle_;
  PrebakeManifest manifest_;
};
