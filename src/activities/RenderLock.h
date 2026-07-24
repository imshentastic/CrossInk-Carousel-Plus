#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
  // CrumBLE 4.5.7: true when the calling task currently owns renderingMutex.
  // Callable from any context; safe on ISR? No -- xSemaphoreGetMutexHolder is
  // not ISR-safe. Use from task context (e.g. silent-restart snapshot path
  // that may fire from within a render).
  static bool heldByCurrentTask();
  // Best-effort take with deadline (FreeRTOS ticks). Returns true on success
  // (caller must forceUnlock() to release) or false on timeout. Used by the
  // silent-restart snapshot path to avoid hanging the device when the render
  // task is busy or a lock inversion would otherwise wait forever.
  static bool tryLockFor(TickType_t ticks);
  static void forceUnlock();
};
