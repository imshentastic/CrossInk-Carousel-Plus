#pragma once

#include <cstdint>
#include <string>

/**
 * HID Device Profiles for Page Turner Devices
 * 
 * Supports multiple page turner devices with their specific keycodes.
 * Each device profile maps button keycodes to page navigation actions.
 */

namespace DeviceProfiles {

// Standard HID Consumer Page codes (preferred, auto-detected first)
constexpr uint8_t STANDARD_PAGE_UP = 0xE9;      // Consumer Page: Page Up
constexpr uint8_t STANDARD_PAGE_DOWN = 0xEA;    // Consumer Page: Page Down

// Common keyboard/navigation page-turn keycodes seen on BLE clickers/remotes
constexpr uint8_t KEYBOARD_PAGE_UP = 0x4B;
constexpr uint8_t KEYBOARD_PAGE_DOWN = 0x4E;
constexpr uint8_t KEYBOARD_UP_ARROW = 0x52;
constexpr uint8_t KEYBOARD_DOWN_ARROW = 0x51;
constexpr uint8_t KEYBOARD_LEFT_ARROW = 0x50;
constexpr uint8_t KEYBOARD_RIGHT_ARROW = 0x4F;
constexpr uint8_t KEYBOARD_SPACE = 0x2C;
constexpr uint8_t KEYBOARD_ENTER = 0x28;
constexpr uint8_t KEYBOARD_VOLUME_UP = 0x80;
constexpr uint8_t KEYBOARD_VOLUME_DOWN = 0x81;

// Free 2 observed rolling keycode families (while held)
constexpr uint8_t FREE2_FORWARD_A = 0x1C;
constexpr uint8_t FREE2_FORWARD_B = 0xC4;
constexpr uint8_t FREE2_FORWARD_C = 0x6C;
constexpr uint8_t FREE2_FORWARD_D = 0xBC;
constexpr uint8_t FREE2_BACK_A = 0xB4;
constexpr uint8_t FREE2_BACK_B = 0x0E;
constexpr uint8_t FREE2_BACK_C = 0x66;
constexpr uint8_t FREE2_BACK_D = 0x16;

struct DeviceProfile {
  const char* name;           // Device name for display
  const char* macPrefix;      // MAC address prefix to identify device (or nullptr)
  uint8_t pageUpCode;         // HID keycode for page up/previous
  uint8_t pageDownCode;       // HID keycode for page down/next
  bool isConsumerPage;        // If true, these are Consumer Page codes (0xE9, 0xEA range)
  uint8_t reportByteIndex;    // Which byte in HID report contains the keycode
  // When true, this profile uses device-specific report framing (e.g. bitmask, non-standard
  // byte positions) and MUST NOT be silently overridden by the user-learned custom profile.
  // Leave false for standard keyboard/consumer layouts that can safely be superseded.
  bool strictProfile;         // If true, custom profile cannot override this profile
};

// Known device profiles (database of popular page turners)
constexpr DeviceProfile KNOWN_DEVICES[] = {
  // IINE Game Brick variant seen with MAC prefix 60:4d:ec.
  // Uses a 5-byte report format with directional state encoded in non-standard bytes.
  {"IINE Game Brick V2", "60:4d:ec", 0x09, 0x07, false, 2, true},

    // IINE Game Brick - specific keycodes in byte[4] with bitmask press-state.
    // strictProfile=true: the custom user profile must NOT override this device.
    {"IINE Game Brick", nullptr, 0x09, 0x07, false, 4, true},

    // MINI_KEYBOARD - standard keyboard page codes in byte[2].
    // strictProfile=false: if the user has learned custom keys they take priority.
    {"MINI_KEYBOARD", nullptr, 0x4B, 0x4E, false, 2, false},

    // Kobo Elipsa 2E remote (common page turner) - Consumer Page
    {"Kobo Remote", nullptr, STANDARD_PAGE_UP, STANDARD_PAGE_DOWN, true, 2, false},

    // Generic Free2-style device pattern - standard HID Consumer Page
    {"Free2 Style", nullptr, STANDARD_PAGE_UP, STANDARD_PAGE_DOWN, true, 2, false},

    // Free2-M page turner (common keyboard-mode mapping)
    {"Free2-M", nullptr, 0x02, 0x01, false, 2, false},

    // Free3-M page turner (confirmed working keycodes from setup wizard).
    // CrumBLE 4.5.5: flipped strictProfile -> true. Field log: user with a
    // stale "Custom BLE Remote" learned profile (up=0x09 dn=0x08 idx=4) paired
    // with a fresh Free3-M; the non-strict flag let the stale custom profile
    // override this, so 0x02/0x01 at byte[2] never decoded and release frames
    // (keycode=0) read as still-pressed -> activeInjectedButton stuck after the
    // first press -> 2nd/3rd presses silently swallowed.
    {"Free3-M", nullptr, 0x02, 0x01, false, 2, true},

    // Free3-R (R + sound mode): code in byte[0], clean 0x00 releases.
    // LEFT=0x01 (back), SELECT/RIGHT=0x02 (forward).
    // CrumBLE 4.5.5: byte[0] (vs the standard byte[2]) is genuinely device-
    // specific framing; mark strict for the same reason as Free3-M above.
    {"Free3-R", nullptr, 0x01, 0x02, false, 0, true},
};

constexpr int KNOWN_DEVICES_COUNT = sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]);

/**
 * Find a device profile by MAC address or device name
 * Returns nullptr if not found in known devices
 */
const DeviceProfile* findDeviceProfile(const char* macAddress, const char* deviceName);

/**
 * Check if keycodes match standard HID Consumer Page codes
 * (0xE9 = Page Up, 0xEA = Page Down)
 */
bool isStandardConsumerPageCode(uint8_t code);

/**
 * Check if a keycode is a commonly used page-turn keycode across
 * generic BLE clickers/remotes in keyboard or consumer report modes.
 */
bool isCommonPageTurnCode(uint8_t code);

/**
 * Convert a keycode to page-turn direction for generic devices.
 * Returns true if mapped; false if the keycode should be ignored.
 * Sets pageForward=true for next-page, false for previous-page.
 */
bool mapCommonCodeToDirection(uint8_t code, bool& pageForward);

/**
 * Get a user-configured device profile from settings
 * Returns nullptr if no custom profile is set
 */
const DeviceProfile* getCustomProfile();

/**
 * Get a device-specific learned profile by MAC address.
 * Returns true and fills outProfile if a mapping exists.
 */
bool getCustomProfileForDevice(const std::string& macAddress, DeviceProfile& outProfile);

/**
 * Set a user-configured device profile in settings
 */
void setCustomProfile(uint8_t pageUpCode, uint8_t pageDownCode, uint8_t reportByteIndex);

/**
 * Set a device-specific learned profile in settings.
 */
void setCustomProfileForDevice(const std::string& macAddress, uint8_t pageUpCode, uint8_t pageDownCode,
                               uint8_t reportByteIndex);

/**
 * Remove a device-specific learned profile.
 */
void clearCustomProfileForDevice(const std::string& macAddress);

/**
 * Clear custom profile and revert to auto-detection
 */
void clearCustomProfile();

}  // namespace DeviceProfiles
