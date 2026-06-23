#include "DeviceProfiles.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <Logging.h>
#include <HalStorage.h>

namespace {
constexpr const char* CUSTOM_PROFILE_FILE = "/.crosspoint/ble_custom_profile.txt";
constexpr const char* DEVICE_PROFILE_FILE = "/.crosspoint/ble_device_profiles.txt";

DeviceProfiles::DeviceProfile customProfile = {"Custom BLE Remote", nullptr, 0x00, 0x00, false, 2, false};
bool customProfileLoaded = false;
std::map<std::string, DeviceProfiles::DeviceProfile> perDeviceProfiles;
bool perDeviceProfilesLoaded = false;

std::string normalizeAddress(const std::string& address) {
  std::string out = address;
  for (auto& ch : out) {
    if (ch >= 'A' && ch <= 'F') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

void savePerDeviceProfilesToStorage() {
  Storage.mkdir("/.crosspoint");

  std::string all;
  for (const auto& entry : perDeviceProfiles) {
    const auto& addr = entry.first;
    const auto& profile = entry.second;
    char line[80];
    snprintf(line, sizeof(line), "%s,%u,%u,%u\n", addr.c_str(), static_cast<unsigned>(profile.pageUpCode),
             static_cast<unsigned>(profile.pageDownCode), static_cast<unsigned>(profile.reportByteIndex));
    all += line;
  }

  Storage.writeFile(DEVICE_PROFILE_FILE, all.c_str());
}

void loadPerDeviceProfilesFromStorage() {
  if (perDeviceProfilesLoaded) {
    return;
  }
  perDeviceProfilesLoaded = true;
  perDeviceProfiles.clear();

  if (!Storage.exists(DEVICE_PROFILE_FILE)) {
    return;
  }

  String content = Storage.readFile(DEVICE_PROFILE_FILE);
  if (content.isEmpty()) {
    return;
  }

  int start = 0;
  while (start < content.length()) {
    int end = content.indexOf('\n', start);
    if (end < 0) {
      end = content.length();
    }
    String line = content.substring(start, end);
    line.trim();
    if (!line.isEmpty()) {
      char addrBuf[32] = {0};
      unsigned int up = 0;
      unsigned int down = 0;
      unsigned int reportIndex = 2;
      const int parsed = sscanf(line.c_str(), "%31[^,],%u,%u,%u", addrBuf, &up, &down, &reportIndex);
      if (parsed == 4 && up <= 0xFF && down <= 0xFF && up != 0 && down != 0 && reportIndex <= 0xFF) {
        DeviceProfiles::DeviceProfile profile = {"Custom BLE Remote", nullptr, static_cast<uint8_t>(up),
                                                 static_cast<uint8_t>(down), false,
                                                 static_cast<uint8_t>(reportIndex), false};
        perDeviceProfiles[normalizeAddress(addrBuf)] = profile;
      }
    }
    start = end + 1;
  }

  LOG_INF("DEV", "Loaded %u per-device BLE profiles", static_cast<unsigned>(perDeviceProfiles.size()));
}

void loadCustomProfileFromStorage() {
  if (customProfileLoaded) {
    return;
  }
  customProfileLoaded = true;

  if (!Storage.exists(CUSTOM_PROFILE_FILE)) {
    return;
  }

  String content = Storage.readFile(CUSTOM_PROFILE_FILE);
  if (content.isEmpty()) {
    return;
  }

  unsigned int up = 0;
  unsigned int down = 0;
  unsigned int reportIndex = 2;
  const int parsed = sscanf(content.c_str(), "%u,%u,%u", &up, &down, &reportIndex);
  if ((parsed == 2 || parsed == 3) && up <= 0xFF && down <= 0xFF && up != 0 && down != 0 && reportIndex <= 0xFF) {
    customProfile.pageUpCode = static_cast<uint8_t>(up);
    customProfile.pageDownCode = static_cast<uint8_t>(down);
    customProfile.reportByteIndex = static_cast<uint8_t>(reportIndex);
    LOG_INF("DEV", "Loaded custom BLE profile: up=0x%02X down=0x%02X idx=%u", customProfile.pageUpCode,
            customProfile.pageDownCode, static_cast<unsigned>(customProfile.reportByteIndex));
  }
}
}  // namespace

namespace DeviceProfiles {

static const DeviceProfile* findKnownProfileByName(const char* profileName) {
  if (!profileName) {
    return nullptr;
  }
  for (int i = 0; i < KNOWN_DEVICES_COUNT; i++) {
    if (strcmp(KNOWN_DEVICES[i].name, profileName) == 0) {
      return &KNOWN_DEVICES[i];
    }
  }
  return nullptr;
}

const DeviceProfile* findDeviceProfile(const char* macAddress, const char* deviceName) {
  // First, try to find by MAC address prefix (case-insensitive comparison)
  if (macAddress) {
    for (int i = 0; i < KNOWN_DEVICES_COUNT; i++) {
      if (KNOWN_DEVICES[i].macPrefix) {
        // Case-insensitive MAC prefix check
        const char* prefix = KNOWN_DEVICES[i].macPrefix;
        size_t prefixLen = strlen(prefix);
        bool matches = true;
        
        for (size_t j = 0; j < prefixLen && macAddress[j] != '\0'; j++) {
          char macChar = macAddress[j];
          char prefixChar = prefix[j];
          // Convert both to lowercase for comparison
          if (macChar >= 'A' && macChar <= 'F') macChar = macChar - 'A' + 'a';
          if (prefixChar >= 'A' && prefixChar <= 'F') prefixChar = prefixChar - 'A' + 'a';
          
          if (macChar != prefixChar) {
            matches = false;
            break;
          }
        }
        
        if (matches) {
          LOG_INF("DEV", "Matched device profile by MAC: %s -> %s", macAddress, KNOWN_DEVICES[i].name);
          return &KNOWN_DEVICES[i];
        }
      }
    }
    LOG_DBG("DEV", "No MAC match found for: %s", macAddress);
  }

  // Then try to find by device name (flexible matching)
  if (deviceName && strlen(deviceName) > 0) {
    // Exact name match wins over the fuzzy patterns below. Otherwise a
    // mode-specific name like "Free3-R" gets hijacked by the broad "Free3"
    // pattern and mapped to the wrong (Free3-M) report layout.
    for (int i = 0; i < KNOWN_DEVICES_COUNT; i++) {
      if (strcmp(deviceName, KNOWN_DEVICES[i].name) == 0) {
        LOG_INF("DEV", "Matched device profile by exact name: %s", KNOWN_DEVICES[i].name);
        return &KNOWN_DEVICES[i];
      }
    }

    for (int i = 0; i < KNOWN_DEVICES_COUNT; i++) {
      // Try case-insensitive substring match for common patterns
      // This allows "Game Brick", "GameBrick", "IINE Game Brick", etc.
      if (strstr(deviceName, "Game") || strstr(deviceName, "game") ||
          strstr(deviceName, "GAME")) {
        if (strstr(deviceName, "Brick") || strstr(deviceName, "brick") || 
            strstr(deviceName, "BRICK")) {
          LOG_INF("DEV", "Matched GameBrick by name pattern: %s -> IINE Game Brick", deviceName);
          if (const DeviceProfile* profile = findKnownProfileByName("IINE Game Brick")) {
            return profile;
          }
        }
      }

      // Match IINE_control naming used by some GameBrick remotes
      if (strstr(deviceName, "IINE") || strstr(deviceName, "iine") ||
          strstr(deviceName, "IINE_control") || strstr(deviceName, "iine_control")) {
        LOG_INF("DEV", "Matched GameBrick by IINE naming: %s", deviceName);
        if (const DeviceProfile* profile = findKnownProfileByName("IINE Game Brick")) {
          return profile;
        }
      }
      
      // Match MINI_KEYBOARD variants
      if (strstr(deviceName, "MINI") || strstr(deviceName, "mini") || 
          strstr(deviceName, "Mini")) {
        if (strstr(deviceName, "KEYBOARD") || strstr(deviceName, "keyboard") || 
            strstr(deviceName, "Keyboard")) {
          LOG_INF("DEV", "Matched MINI_KEYBOARD by name pattern: %s", deviceName);
          if (const DeviceProfile* profile = findKnownProfileByName("MINI_KEYBOARD")) {
            return profile;
          }
        }
      }

      // Match Free2 variants by name (Free2, Free2-M, Free 2, Free-2)
      if (strstr(deviceName, "Free2") || strstr(deviceName, "FREE2") || strstr(deviceName, "free2") ||
          strstr(deviceName, "Free 2") || strstr(deviceName, "FREE 2") || strstr(deviceName, "free 2") ||
          strstr(deviceName, "Free-2") || strstr(deviceName, "FREE-2") || strstr(deviceName, "free-2")) {
        LOG_INF("DEV", "Matched Free2 by name pattern: %s", deviceName);
        if (const DeviceProfile* profile = findKnownProfileByName("Free2-M")) {
          return profile;
        }
      }

      // Match Free3 variants by name (Free3, Free3-M, Free 3, Free-3)
      if (strstr(deviceName, "Free3") || strstr(deviceName, "FREE3") || strstr(deviceName, "free3") ||
          strstr(deviceName, "Free 3") || strstr(deviceName, "FREE 3") || strstr(deviceName, "free 3") ||
          strstr(deviceName, "Free-3") || strstr(deviceName, "FREE-3") || strstr(deviceName, "free-3")) {
        LOG_INF("DEV", "Matched Free3 by name pattern: %s", deviceName);
        if (const DeviceProfile* profile = findKnownProfileByName("Free3-M")) {
          return profile;
        }
      }
    }
    
    LOG_DBG("DEV", "No profile match for device name: %s", deviceName);
  }

  return nullptr;
}

bool isStandardConsumerPageCode(uint8_t code) {
  // Standard HID Consumer Page codes for page navigation
  return code == STANDARD_PAGE_UP || code == STANDARD_PAGE_DOWN;
}

bool isCommonPageTurnCode(uint8_t code) {
  switch (code) {
    // Consumer page navigation
    case STANDARD_PAGE_UP:
    case STANDARD_PAGE_DOWN:

    // Keyboard page/navigation keys
    case KEYBOARD_PAGE_UP:
    case KEYBOARD_PAGE_DOWN:
    case KEYBOARD_UP_ARROW:
    case KEYBOARD_DOWN_ARROW:
    case KEYBOARD_LEFT_ARROW:
    case KEYBOARD_RIGHT_ARROW:

    // Common clicker/media mappings often used by low-cost BLE remotes
    case KEYBOARD_SPACE:
    case KEYBOARD_ENTER:
    case KEYBOARD_VOLUME_UP:
    case KEYBOARD_VOLUME_DOWN:

    // Free 2 rolling key families
    case FREE2_FORWARD_A:
    case FREE2_FORWARD_B:
    case FREE2_FORWARD_C:
    case FREE2_FORWARD_D:
    case FREE2_BACK_A:
    case FREE2_BACK_B:
    case FREE2_BACK_C:
    case FREE2_BACK_D:

    // Existing GameBrick fallback codes
    case 0x07:
    case 0x09:
      return true;
    default:
      return false;
  }
}

bool mapCommonCodeToDirection(uint8_t code, bool& pageForward) {
  switch (code) {
    // Next page
    case STANDARD_PAGE_UP:
    case KEYBOARD_PAGE_DOWN:
    case KEYBOARD_DOWN_ARROW:
    case KEYBOARD_RIGHT_ARROW:
    case KEYBOARD_SPACE:
    case KEYBOARD_ENTER:
    case KEYBOARD_VOLUME_UP:
    case FREE2_FORWARD_A:
    case FREE2_FORWARD_B:
    case FREE2_FORWARD_C:
    case FREE2_FORWARD_D:
    case 0x07:
      pageForward = true;
      return true;

    // Previous page
    case STANDARD_PAGE_DOWN:
    case KEYBOARD_PAGE_UP:
    case KEYBOARD_UP_ARROW:
    case KEYBOARD_LEFT_ARROW:
    case KEYBOARD_VOLUME_DOWN:
    case FREE2_BACK_A:
    case FREE2_BACK_B:
    case FREE2_BACK_C:
    case FREE2_BACK_D:
    case 0x09:
      pageForward = false;
      return true;

    default:
      return false;
  }
}

const DeviceProfile* getCustomProfile() {
  loadCustomProfileFromStorage();
  if (customProfile.pageUpCode == 0x00 || customProfile.pageDownCode == 0x00) {
    return nullptr;
  }
  return &customProfile;
}

bool getCustomProfileForDevice(const std::string& macAddress, DeviceProfile& outProfile) {
  loadPerDeviceProfilesFromStorage();
  const std::string norm = normalizeAddress(macAddress);
  auto it = perDeviceProfiles.find(norm);
  if (it == perDeviceProfiles.end()) {
    return false;
  }
  outProfile = it->second;
  return true;
}

void setCustomProfile(uint8_t pageUpCode, uint8_t pageDownCode, uint8_t reportByteIndex) {
  customProfile.pageUpCode = pageUpCode;
  customProfile.pageDownCode = pageDownCode;
  customProfile.reportByteIndex = reportByteIndex;
  customProfileLoaded = true;

  Storage.mkdir("/.crosspoint");
  char buf[24];
  snprintf(buf, sizeof(buf), "%u,%u,%u", static_cast<unsigned>(pageUpCode), static_cast<unsigned>(pageDownCode),
           static_cast<unsigned>(reportByteIndex));
  Storage.writeFile(CUSTOM_PROFILE_FILE, buf);
  LOG_INF("DEV", "Custom profile set: up=0x%02X down=0x%02X idx=%u", pageUpCode, pageDownCode,
          static_cast<unsigned>(reportByteIndex));
}

void setCustomProfileForDevice(const std::string& macAddress, uint8_t pageUpCode, uint8_t pageDownCode,
                               uint8_t reportByteIndex) {
  loadPerDeviceProfilesFromStorage();
  DeviceProfile profile = {"Custom BLE Remote", nullptr, pageUpCode, pageDownCode, false, reportByteIndex, false};
  perDeviceProfiles[normalizeAddress(macAddress)] = profile;
  savePerDeviceProfilesToStorage();
  LOG_INF("DEV", "Saved per-device BLE profile for %s: up=0x%02X down=0x%02X idx=%u", macAddress.c_str(),
          pageUpCode, pageDownCode, static_cast<unsigned>(reportByteIndex));
}

void clearCustomProfileForDevice(const std::string& macAddress) {
  loadPerDeviceProfilesFromStorage();
  perDeviceProfiles.erase(normalizeAddress(macAddress));
  savePerDeviceProfilesToStorage();
  LOG_INF("DEV", "Cleared per-device BLE profile for %s", macAddress.c_str());
}

void clearCustomProfile() {
  customProfile.pageUpCode = 0x00;
  customProfile.pageDownCode = 0x00;
  customProfile.reportByteIndex = 2;
  customProfileLoaded = true;
  Storage.remove(CUSTOM_PROFILE_FILE);
  perDeviceProfiles.clear();
  perDeviceProfilesLoaded = true;
  Storage.remove(DEVICE_PROFILE_FILE);
  LOG_INF("DEV", "Custom profile cleared");
}

}  // namespace DeviceProfiles
