#pragma once

// Subset of SdFat's FsApiConstants.h that firmware code references in
// signatures we must preserve on host. Only the symbols actually used
// are defined; the rest stay undefined to provoke a clean compile error
// if anything tries to drag in real device-specific flags.

#include <cstdint>

using oflag_t = int;

// File-open flag constants -- match SdFat's values so a future port that
// passes flags around works without re-translation.
constexpr oflag_t O_RDONLY = 0x00;
constexpr oflag_t O_WRONLY = 0x01;
constexpr oflag_t O_RDWR = 0x02;
constexpr oflag_t O_APPEND = 0x04;
constexpr oflag_t O_CREAT = 0x10;
constexpr oflag_t O_TRUNC = 0x40;
