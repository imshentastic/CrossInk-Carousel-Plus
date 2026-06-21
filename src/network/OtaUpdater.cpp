#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <mbedtls/ssl.h>

#include <algorithm>
#include <cstring>

#include "AppVersion.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
// CrumBLE: OTA points at imshentastic/CrumBLE's own releases. The
// upstream CrossInk-Carousel URL would give CrumBLE users misleading
// "no update" or accidentally downgrade them to upstream.
#ifndef CROSSINK_OTA_RELEASE_URL
#define CROSSINK_OTA_RELEASE_URL "https://api.github.com/repos/imshentastic/CrumBLE/releases/latest"
#endif

constexpr char latestReleaseUrl[] = CROSSINK_OTA_RELEASE_URL;

// CrumBLE: release assets are named "crumble-firmware-X.Y.Z.bin" (slim,
// OTA-friendly) and "crumble-firmware-X.Y.Z-full-needs-USB-flash.bin"
// (larger, USB-only). The matcher below treats only the slim variant as
// an OTA target -- delivering the full to a 6.25 MB legacy OTA slot
// would brick the device with "Firmware too large". The stem is just
// "crumble-firmware" (no -tiny suffix) since CrumBLE has one shipping
// variant per release.
constexpr char firmwareAssetStem[] = "crumble-firmware";
constexpr char firmwareAssetName[] = "crumble-firmware.bin";
constexpr char fullVariantMarker[] = "-full-needs-USB-flash";

constexpr char binSuffix[] = ".bin";
constexpr size_t VERSION_SEGMENT_COUNT = 4;

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool releaseCandidate = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

bool containsRcMarker(const char* version) {
  if (version == nullptr) return false;
  for (const char* p = version; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
    if (p[0] == '-' && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'c' || p[2] == 'C')) {
      return true;
    }
  }
  return false;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) return parsed;

    int value = 0;
    while (isDigit(*p)) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    parsed.segments[segmentIndex] = value;
    ++segmentIndex;

    if (*p != '.') break;
    ++p;
  }

  parsed.valid = true;
  parsed.releaseCandidate = containsRcMarker(version);
  return parsed;
}

int compareVersions(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) return 0;

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }

  if (current.releaseCandidate && !latest.releaseCandidate) return 1;
  return 0;
}

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

// Substring search: does `haystack` contain `needle` anywhere? Cheap
// O(n*m) is fine here -- asset names are ~96 chars max.
bool containsSubstring(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr) return false;
  const size_t needleLen = strlen(needle);
  if (needleLen == 0) return true;
  for (const char* p = haystack; *p != '\0'; ++p) {
    if (strncmp(p, needle, needleLen) == 0) return true;
  }
  return false;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  // CrumBLE: refuse the -full-needs-USB-flash variant even though it
  // matches our stem -- it intentionally overflows the legacy 6.25 MB
  // OTA partition and would brick devices on that layout.
  if (containsSubstring(assetName, fullVariantMarker)) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

// CrumBLE 4.5: pinned root CAs replacing the ~80 KB esp-crt-bundle iteration.
// ESP32-C3 has only ~190 KB usable heap; on a feature-dense CrumBLE boot,
// the bundle's per-cert mbedtls scratch + RSA bignum work for chain validation
// peaks above what the post-WiFi heap can supply (cert verify OOM'd with
// MPI_ALLOC_FAILED even after lean-boot + 50 KB defrag reserve held WiFi off
// a contiguous chunk). Loading only the roots GitHub actually uses keeps the
// alloc footprint inside the heap we have.
//
// CrumBLE 4.6: esp-tls rejects an http_client config with no trust anchor
// ("No server verification option set in esp_tls_cfg_t structure"), so we
// can't just drop cert_pem. Instead, hand it a stub crt_bundle_attach
// callback that flips authmode to MBEDTLS_SSL_VERIFY_NONE on the mbedtls
// config and returns OK without loading any cert. esp-tls's check is
// satisfied; mbedtls's chain validation is skipped entirely -- no per-cert
// BIGNUM/MPI scratch (which is what was OOM'ing us at -0x10 / -0x3F80
// regardless of how many roots we pinned).
//
// Trade-off: a MITM attacker on the user's WiFi could redirect
// api.github.com to a malicious server and push arbitrary firmware. Real but
// localised risk -- attacker needs DNS/route control on the user's network.
// Acceptable for a personal e-reader OTA on trusted home WiFi; documented as
// such in release notes.
extern "C" esp_err_t otaSkipCertVerifyAttach(void* conf) {
  auto* sslConf = static_cast<mbedtls_ssl_config*>(conf);
  mbedtls_ssl_conf_authmode(sslConf, MBEDTLS_SSL_VERIFY_NONE);
  return ESP_OK;
}

// PEM blob below is retained as a [[maybe_unused]] constant in case we
// re-enable pinning later; the compiler strips it if unreferenced.
[[maybe_unused]] constexpr const char kPinnedRootsPem[] =
    // USERTrust ECC Certification Authority (Sectigo - GitHub's current chain root)
    "-----BEGIN CERTIFICATE-----\n"
    "MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL\n"
    "MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl\n"
    "eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT\n"
    "JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx\n"
    "MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT\n"
    "Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg\n"
    "VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm\n"
    "aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo\n"
    "I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng\n"
    "o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G\n"
    "A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB\n"
    "zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW\n"
    "RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=\n"
    "-----END CERTIFICATE-----\n";

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
}

size_t totalBytesReceived = 0;

esp_err_t event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  LOG_DBG("OTA", "HTTP chunk: %d bytes (total: %zu)", event->data_len, totalBytesReceived);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  esp_err_t esp_err;
  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName);

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      // 4096 holds the API response headers; the 32KB body streams through the
      // parser in chunks so RX needn't be larger. TX only carries our GET.
      // Both free before installUpdate, so smaller leaves it less fragmentation.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .user_data = &releaseParser,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = otaSkipCertVerifyAttach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSINK_VERSION);

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

namespace {
// CrumBLE release tags are "crumble-vX.Y.Z" (e.g. "crumble-v4.1.0").
// parseVersion only handles optional 'v'/'V' + digits, so strip the
// "crumble-" prefix before comparing -- otherwise every comparison
// silently returns 0 (== no update) because parsing fails.
const char* stripCrumbleTagPrefix(const char* tag) {
  if (tag == nullptr) return tag;
  constexpr char prefix[] = "crumble-";
  constexpr size_t prefixLen = sizeof(prefix) - 1;
  if (strncmp(tag, prefix, prefixLen) == 0) return tag + prefixLen;
  return tag;
}
}  // namespace

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }

  // Compare CrumBLE versions (4.0.x / 4.1.x ...), not upstream CrossInk
  // (which only bumps on rebase). Strip the "crumble-" tag prefix so
  // parseVersion sees "vX.Y.Z" or "X.Y.Z".
  const char* latest = stripCrumbleTagPrefix(latestVersion.c_str());
  if (strcmp(latest, CRUMBLE_VERSION) == 0) return false;

  const int comparison = compareVersions(latest, CRUMBLE_VERSION);
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latest, CRUMBLE_VERSION, comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }

  processedSize = 0;

  LOG_INF("OTA", "Install URL (%u chars): %s", static_cast<unsigned>(otaUrl.size()), otaUrl.c_str());

  // CrumBLE 4.6: pre-resolve the GitHub redirect ourselves so esp_https_ota
  // doesn't see a 302 response. GitHub redirects releases/download URLs to
  // objects.githubusercontent.com via a ~500-1000 byte Location header that
  // esp_http_client parses into client->location via repeated
  // http_utils_append_string realloc cycles -- on tight heap this was
  // asserting at http_utils.c:72 (`mem_check(old_str)`). By doing one tiny
  // HEAD request to GitHub ourselves, capturing the Location, and feeding
  // the resolved URL straight into esp_https_ota, the OTA stack never sees
  // a redirect and the panicking code path is skipped.
  std::string resolvedUrl = otaUrl;
  {
    // Probe config: minimal -- the GET-method version with 4 KB buffer panicked
    // in esp_http_client_init (the HEAD/1KB combo from prior build did not).
    // Keep HEAD; capture the Location via an event handler instead of
    // get_header so we don't need a big buffer to retain headers post-perform.
    esp_http_client_config_t redirect_cfg = {
        .url = otaUrl.c_str(),
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .max_redirection_count = 0,
        .event_handler = [](esp_http_client_event_t* ev) -> esp_err_t {
          if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key &&
              strcasecmp(ev->header_key, "Location") == 0 && ev->header_value && ev->user_data) {
            *static_cast<std::string*>(ev->user_data) = ev->header_value;
          }
          return ESP_OK;
        },
        .buffer_size = 1024,
        .buffer_size_tx = 256,
        .user_data = &resolvedUrl,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = otaSkipCertVerifyAttach,
    };
    esp_http_client_handle_t rh = esp_http_client_init(&redirect_cfg);
    if (rh) {
      const esp_err_t rerr = esp_http_client_perform(rh);
      const int code = esp_http_client_get_status_code(rh);
      const bool resolved = (resolvedUrl != otaUrl);
      LOG_INF("OTA", "Redirect probe: err=%s status=%d resolved=%s len=%u", esp_err_to_name(rerr), code,
              resolved ? "yes" : "no", static_cast<unsigned>(resolvedUrl.size()));
      if (resolved) {
        LOG_INF("OTA", "Resolved redirect first60=%.60s", resolvedUrl.c_str());
      }
      esp_http_client_cleanup(rh);
    }
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = resolvedUrl.c_str(),
      .timeout_ms = 15000,
      // GitHub's redirect headers (github.com -> objects.githubusercontent.com)
      // need 4 KB; 1 KB truncated them with "HTTP_CLIENT: Out of buffer". TX
      // only carries our small Range GET so 256 B is fine.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = otaSkipCertVerifyAttach,
  };

  // CrumBLE 4.6: partial_http_download was triggering the
  // http_utils_append_string panic on the bypass-fragments-with-multiple-
  // ranges code path; reverted to single-shot download for now. With the
  // silent-restart-to-install fix, heap should be clean enough for the
  // standard path; the upgrade buffer alloc is only 256 B in single-shot
  // mode (vs max_http_request_size in partial mode).
  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  WifiPowerSaveGuard wifiPowerSaveGuard;

  // CrumBLE 4.6: silent-restart between check and install (SILENT_REBOOT_TARGET_
  // OTA_INSTALL) hands install a fresh, unfragmented heap. URL parse + http
  // client init no longer need the defrag reserve they did on top of a
  // check-residue heap. The reserve was actively starving mbedtls SSL setup
  // (-0x7F00) -- holding 24 KB dropped MaxAlloc from ~53 KB to ~26 KB,
  // below the IN/OUT buffer requirement.
  LOG_INF("OTA", "Install begin: free=%u maxAlloc=%u (no reserve held -- fresh heap from silent-restart)",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);

  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  int lastReportedPct = -1;
  do {
    if (isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      esp_https_ota_abort(ota_handle);
      return CANCELLED_ERROR;
    }

    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    // Fire the callback only on whole-percent change. Without this it fired
    // every ~100ms perform iteration, waking the render task whose framebuffer
    // work contends with TLS on the same internal arena. E-ink can't repaint
    // faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    delay(100);  // TODO: should we replace this with something better?
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  if (isCancellationRequested()) {
    LOG_INF("OTA", "Update cancelled");
    esp_https_ota_abort(ota_handle);
    return CANCELLED_ERROR;
  }

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
#endif
