#pragma once

#include <atomic>
#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    CANCELLED_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  // CrumBLE 4.6: needed by OtaUpdateActivity to persist install state across
  // a silent-restart between check and install.
  const std::string& getOtaUrl() const { return otaUrl; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr,
                                std::atomic<bool>* cancelRequested = nullptr);

  // CrumBLE 4.6: restore install-ready state from a prior checkForUpdate that
  // ran on the boot before a silent-restart. The activity calls this in
  // onEnter when consumePendingOtaInstall returns true.
  void preloadInstallState(const char* url, size_t size, const char* version) {
    otaUrl = url ? url : "";
    otaSize = size;
    totalSize = size;
    latestVersion = version ? version : "";
    updateAvailable = true;
  }
};
