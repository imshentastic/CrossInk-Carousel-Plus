#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
// v18.9.9.42 (task #27): CrumBLE writes/reads to its own fork-scoped
// state file, matching what CrossPointSettings does with
// crumble-settings.json. Legacy /.crosspoint/state.json is now
// read-only migration source -- if we don't have a crumble-state.json
// yet but the legacy file exists (a fresh CrumBLE install on a device
// that ran CrossPoint / CrossInk / vCodex before), copy the state
// forward on the first successful load and save to the new path. The
// legacy file is left INTACT so a user who flashes back to CrossPoint
// keeps their prior state on that fork.
constexpr char STATE_FILE_JSON[] = "/.crosspoint/crumble-state.json";
constexpr char LEGACY_STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

bool CrossPointState::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
}

bool CrossPointState::loadFromFile() {
  // v18.9.9.42 (task #27): CrumBLE-scoped file first.
  // v18.9.9.311: use readFileWithFallback so a corrupt/missing primary
  // auto-recovers from crumble-state.json.bak (written by
  // writeFileWithBackup on every successful save). Silent recovery is
  // safe here because APP_STATE is transient session state; losing it
  // means losing openEpubPath / last sleep image cursor, not user data.
  if (Storage.exists(STATE_FILE_JSON) || Storage.exists((String(STATE_FILE_JSON) + ".bak").c_str())) {
    bool fromBackup = false;
    String json = Storage.readFileWithFallback(STATE_FILE_JSON, fromBackup);
    if (!json.isEmpty()) {
      if (fromBackup) {
        LOG_INF("CPS", "crumble-state.json corrupt or missing; recovered from .bak");
      }
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
  }

  // No crumble-state.json yet -- try the legacy /.crosspoint/state.json
  // written by CrossPoint / CrossInk / vCodex. Copy forward on first
  // successful load. Legacy file is left intact so a firmware swap
  // preserves the sibling fork's state.
  if (Storage.exists(LEGACY_STATE_FILE_JSON)) {
    String json = Storage.readFile(LEGACY_STATE_FILE_JSON);
    if (!json.isEmpty()) {
      const bool result = JsonSettingsIO::loadState(*this, json.c_str());
      if (result) {
        LOG_INF("CPS", "First-boot migration: copied state from legacy %s into %s",
                LEGACY_STATE_FILE_JSON, STATE_FILE_JSON);
        if (!saveToFile()) {
          LOG_ERR("CPS", "Migration save to crumble-state.json failed");
        }
      }
      return result;
    }
  }

  // Fall back to binary migration (very old CrossPoint installs).
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to crumble-state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during binary migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  if (version >= 5) {
    serialization::readPod(inputFile, pendingBookmarkSpine);
    serialization::readPod(inputFile, pendingBookmarkProgress);
  } else {
    pendingBookmarkSpine = UINT16_MAX;
    pendingBookmarkProgress = -1.0f;
  }

  return true;
}
