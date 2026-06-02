#pragma once

// FreeRTOS SemaphoreHandle_t stub. The prebake CLI is single-threaded;
// firmware sites that take/give these on host just no-op. Kept as
// opaque pointers so existing handle-comparison patterns (== nullptr,
// portMAX_DELAY waits) compile without warnings.

#include <cstdint>

using SemaphoreHandle_t = void*;
using TickType_t = uint32_t;

constexpr TickType_t portMAX_DELAY = static_cast<TickType_t>(-1);

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  // Non-null sentinel so `assert(handle != nullptr)` passes.
  static int sentinel = 0;
  return &sentinel;
}
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; /* pdTRUE */ }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
