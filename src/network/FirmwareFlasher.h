#pragma once

#include <cstddef>
#include <cstdint>

// Flash a firmware image from an SD-card path into the next OTA app
// partition, then switch otadata so the X3/X4 stock bootloader picks it up
// on next boot. Mirrors the web flasher: raw esp_partition_erase_range +
// esp_partition_write + ota_boot::switchTo (no Arduino Update class, no
// esp_image_verify — those reject our patched image on X4 silicon).
//
// Both the SD update activity and the OTA path land here. OTA first
// downloads the firmware to an SD-card cache file, then calls this.

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 64 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// `alreadyValidated` lets callers that have just run `validateImageFile()`
// themselves (e.g. SdFirmwareUpdateActivity, which validates before showing
// the user the confirmation prompt) skip the redundant second pass. Defaults
// to false so callers without prior validation (any future entry point) keep
// the defense-in-depth check.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated = false);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Run this before flashing a candidate firmware so a
// truncated/corrupted .bin never reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check (e.g. when validating ahead of partition
// lookup). Streams the file in CHUNK-sized reads; the file is rewound on
// success so the caller can immediately reread it for flashing.
Result validateImageFile(const char* sdPath, size_t partitionSize);

const char* resultName(Result r);

// CrumBLE 4.6 LAN-OTA re-anchor pass.
//
// Problem this solves: ESP-IDF's OTA APIs flash the "other" partition each
// install (ota_0 -> ota_1 -> ota_0 ...). So after a LAN-OTA, the device
// boots from ota_1. Users who later USB-flash via the CrossPoint web
// flasher (which always writes ota_0 only and doesn't touch otadata) need
// a double-flash to recover -- a non-obvious regression we'd be causing.
//
// Fix: on the first boot of a LAN-OTA-installed firmware, detect we're
// running from a non-ota_0 partition AND the source bin is still sitting
// at /.crosspoint/firmware-pending.bin on SD (the LAN-OTA install path
// leaves it there for this pass). Re-flash the same bin into ota_0
// (esp_ota_get_next_update_partition returns ota_0 while we're on ota_1),
// switch otadata to point at ota_0, then reboot. Net result: every
// LAN-OTA-installed device ends up anchored on ota_0, exactly where USB
// flashes write, so subsequent USB flashes work in a single pass.
//
// Behaviour matrix:
//   - bin missing             -> NOT_NEEDED (normal boot)
//   - bin present, on ota_0   -> NOT_NEEDED (cleanup deletes bin)
//   - bin present, on ota_1   -> attempts relocation; RELOCATED on success
//     (caller MUST ESP.restart() so new ota_0 takes over)
//   - relocation write fails  -> FAILED (bin preserved for next-boot retry)
enum class RelocateResult { NOT_NEEDED, RELOCATED, FAILED };
RelocateResult maybeRelocateLanOtaToOta0(ProgressCb onProgress, void* ctx);

// True iff a relocation pass will run on the next maybeRelocateLanOtaToOta0
// call (bin present AND we're booted from non-ota_0). Lets the boot path
// render a "Finalizing update..." screen ONLY when work will actually
// happen, instead of flashing it for normal boots.
bool relocateLanOtaPending();

}  // namespace firmware_flash
