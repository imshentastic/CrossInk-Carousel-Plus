#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}

  // Read-only accessor for logging (ActivityManager's onEnter timing).
  const std::string& getName() const { return name; }

  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  // CrumBLE: called by main.cpp::enterDeepSleep BEFORE the hardware
  // sleep enters. Activities can use this to commit in-flight state
  // (reading sessions, draft inputs, etc.) that would otherwise be
  // lost — the normal onExit() path doesn't fire when going to deep
  // sleep, because the activity isn't being torn down; the chip is
  // just being powered off. Default no-op.
  virtual void onBeforeDeepSleep() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual RequestUpdateResult requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual bool allowPowerAsConfirmInReaderMode() const { return false; }
  virtual bool canSnapshotForSleepOverlay() const { return false; }
  virtual std::string getCurrentBookPath() const { return {}; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);

  // Like onGoHome(), but paints a brief "Going home..." popup with
  // FAST_REFRESH before initiating the transition. Reader exits take
  // ~700 ms (BLE teardown + session commit + activity replace +
  // carousel render). Without the popup the user stares at the last
  // page for that whole interval and the device feels hung. The
  // popup persists on screen until HomeActivity's first render
  // replaces it. Safe to call from any activity; small overhead if
  // the actual transition would have been fast anyway.
  void exitToHomeWithPopup();
};
