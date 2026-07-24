#include "CrossPointWebServer.h"
#include "SilentRestart.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <ZipFile.h>  // CrumBLE 4.5.5: fnvHash64 for stack-based cache-dir build in api-files
#include <esp_task_wdt.h>
#include <lwip/sockets.h>  // SO_SNDTIMEO for the streaming-send timeout
#include <sys/time.h>      // struct timeval

#include <algorithm>
#include <iterator>

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "components/UITheme.h"
#include "WifiCredentialStore.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "html/js/optimizerJs.generated.h"
// v18.9.9.123: v97 split reverted -- files-app.js content is now inline in
// FilesPage.html (matches v55's single-shot serve pattern). v120's
// unconditional silent-restart gives us the heap headroom to serve the
// full 30 KB HTML in one response.
#include "html/wasm/crumblePrebakeJs.generated.h"
#include "html/wasm/crumblePrebakeWasm.generated.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace {
// Folders/files to hide from the web interface file browser.
// Dot-prefixed items are hidden unless showHiddenFiles is enabled. When
// they ARE shown, the api-files row streamer emits them in a secondary
// wave AFTER visible entries -- see handleFileListData -- so that the
// user's books always survive the row-streamer's low-heap truncation
// floor, even though hidden firmware folders happen to come first in SD
// directory-iteration order. Power users keep access to .crosspoint /
// .fonts / .system for cache debugging.
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

uint8_t enumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

// WebSocket upload state
FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;

// v18.9.9.392: track consecutive file-open failures across upload attempts.
// If openFileForWrite fails N times in a row (across different filenames),
// the SD card is likely in a bad state -- send FATAL:SD_WRITE_FAILING so
// the browser aborts the whole prebake pushes instead of retrying 704
// files against a dead FS for 47 minutes.
static uint32_t wsConsecutiveOpenFailures = 0;
constexpr uint32_t kWsMaxConsecutiveOpenFailures = 5;

// CrumBLE 4.5.5: heap reserve pad for the WS upload path. Allocated once
// at server start while heap is still mostly unfragmented (~140 KB
// contiguous available), held until START fires. On START we free()
// it; the WebSockets library's per-BIN-frame ~4 KB allocs then land in
// that contiguous freed region instead of squeezing into whatever
// post-render-task / post-prebake gaps exist. After upload (DONE or
// abort) we malloc the pad back -- if heap is too fragmented now to get
// a contiguous 32 KB chunk, we stay un-padded and the NEXT upload
// doesn't get the benefit; first one of the session still does. Pre-
// this fix, field log showed maxAlloc=5 620 right after a disconnect /
// reconnect cycle, START rejected at floor=7 168, silentRestart -- now
// freeing the pad bumps maxAlloc ~32 KB instantly and START succeeds.
static uint8_t* g_wsHeapReservePad = nullptr;
// v18.9.9.427: REVERTED v422's size ladder. Field observation on X3: the
// ladder was succeeding at 16 KB in fragmented steady-state heap, and that
// 16 KB reservation crushed maxAlloc from ~22 KB down to ~5-8 KB, tripping
// every downstream guard (prebake-serve floor, heap watchdog) and looping
// the FT session before the browser could even fetch its assets. Back to
// the original all-or-nothing 32 KB attempt: succeeds when heap is fresh
// (post-cold-boot), fails otherwise. On failure we run un-padded, which
// v4.5.114 field-confirmed produces working uploads at ~167 KB/s -- the
// BIN frame handler only needs ~4 KB per frame and steady-state maxAlloc
// (~15-20 KB pad-free) covers that with room to spare.
constexpr size_t kWsHeapReservePadBytes = 32u * 1024u;

static void releaseWsHeapReservePad(const char* tag) {
  if (g_wsHeapReservePad == nullptr) return;
  free(g_wsHeapReservePad);
  g_wsHeapReservePad = nullptr;
  LOG_INF("WS", "%s: released %u-byte heap reserve pad (maxAlloc now=%u)",
          tag, (unsigned)kWsHeapReservePadBytes, (unsigned)ESP.getMaxAllocHeap());
}

static void reclaimWsHeapReservePad(const char* tag) {
  if (g_wsHeapReservePad != nullptr) return;  // already padded
  g_wsHeapReservePad = static_cast<uint8_t*>(malloc(kWsHeapReservePadBytes));
  if (g_wsHeapReservePad != nullptr) {
    LOG_INF("WS", "%s: re-allocated %u-byte heap reserve pad (maxAlloc now=%u)",
            tag, (unsigned)kWsHeapReservePadBytes, (unsigned)ESP.getMaxAllocHeap());
  } else {
    LOG_INF("WS", "%s: could not re-allocate heap reserve pad (maxAlloc=%u); next upload runs un-padded",
            tag, (unsigned)ESP.getMaxAllocHeap());
  }
}


// CrumBLE: set by sendBufferGzip when the heap is too low to serve. The
// FT activity's loop() polls this via consumeFtRestartRequest() and
// triggers silentRestartToFileTransfer once the request handler has
// returned (so the response physically reaches the browser before the
// device reboots).
bool g_pendingFtRestart = false;

// v18.9.9.428: cooldown timestamp after a heap-floor WS-abort. Browser retry
// logic reconnects within ~87 ms of a socket close, which lands right back
// on the same fragmented heap and hits the same refuse cycle. Setting this
// gives the WS lib + LWIP time to release the closed socket's internal
// buffers (typically ~2-3 s), so the NEXT connection starts from a genuinely
// improved heap state instead of the tail end of the previous failure.
static uint32_t g_wsHeapAbortCooldownUntilMs = 0;

// CrumBLE 4.6 LAN-OTA: set by the WS INSTALL_FIRMWARE handler. FT
// activity consumes this on next loop() and pushes the install-progress
// activity which flashes /.crosspoint/firmware-pending.bin into the
// next OTA partition + restarts. Fixed path (not parameterized) so the
// browser can't trigger an arbitrary-SD-file flash.
bool g_pendingFirmwareInstall = false;
constexpr const char* kFirmwarePendingPath = "/.crosspoint/firmware-pending.bin";

// CrumBLE 4.5.2: flipped to true by the WS upload DONE handler whenever
// a book completes. FT activity loop consumes it (when wsUploadInProgress
// is false, so an upload burst doesn't trigger N walks) and re-walks
// the library so new books pick up their author keys without waiting
// for the user to visit Home.
//
// CrumBLE 4.5.4: also record the millis() timestamp of the most recent
// upload DONE. consumePendingLibraryRefreshRequest() now debounces: it
// only returns true once 5s have elapsed since the last upload (any new
// upload DONE bumps the timestamp again). Net effect: a 10-book drag-
// drop burst triggers exactly ONE walk + populate after the user stops
// uploading, instead of 10 back-to-back walks that compete with the
// ongoing uploads for heap.
bool g_pendingLibraryRefresh = false;
uint32_t g_lastLibraryRefreshBumpMs = 0;

// v18.9.9.384: post-large-upload defrag restart. WS DONE handler sets
// this to millis()+delay when it decides the heap is fragmented enough
// that the follow-up cache uploads (font stream, .pxc, .bin) would OOM.
// FT activity loop polls consumeFtDefragRestartIfDue() -- returns true
// when now >= armed time, activity then silent-restart-to-FT. Delay
// gives the DONE TXT frame time to flush to the browser AND for any
// straggler BIN frames from the just-completed upload's TCP send buffer
// to drain. 0 = disarmed.
uint32_t g_pendingFtDefragRestartAtMs = 0;
constexpr uint32_t kLibraryRefreshDebounceMs = 5000;
}  // namespace anon

// v18.9.9.394: last observed FT activity timestamp (any inbound HTTP
// request or WS event). The activity's onLoop periodic-restart check
// compares against this to decide when to preventively silent-restart
// during long idle stretches -- prevents the "heap slowly leaks from
// steady-state serves, watchdog never trips because we never dip to
// the critical floor" wedge state. External linkage so both the
// server module and the activity can read/write it.
uint32_t g_ftLastActivityAtMs = 0;
void bumpFtActivity() { g_ftLastActivityAtMs = millis(); }

// v18.9.9.398: separate tracker for WS-specific activity. The generic idle
// tracker above resets on any HTTP request too, which means when the browser
// is stuck in the "WS accept wedged but HTTP still fine" state (probes to
// /api/status keep bumping g_ftLastActivityAtMs while every WS handshake
// times out at 30 s), the 15-min idle-restart never fires. This tracker is
// updated ONLY when a WS event actually reaches the server (which is when
// the wedge is broken). The activity's onLoop checks this alongside "have
// we seen HTTP traffic recently?" to detect the specific "browser is trying
// but WS isn't landing" case and silent-restart at 3 min instead of 15.
uint32_t g_ftLastWsEventAtMs = 0;
void bumpFtWsActivity() {
  g_ftLastWsEventAtMs = millis();
  bumpFtActivity();  // WS is a superset of activity
}

// v18.9.9.169: progress marker for the low-heap-restart loop-break heuristic.
// Incremented by sendBufferGzip when a >= 20 KB serve completes cleanly.
// The FT activity's loop-protection consults this: if we've made progress
// since the last restart, another restart is allowed (we're clearly not in
// a pure boot->fail->reboot loop). External linkage (referenced from
// CrossPointWebServerActivity.cpp) so it MUST live at file scope, not in
// the anon namespace above.
uint32_t g_ftLargeServesSinceRestart = 0;

bool consumeFtRestartRequest() {
  if (!g_pendingFtRestart) return false;
  g_pendingFtRestart = false;
  return true;
}

bool peekFtRestartRequest() { return g_pendingFtRestart; }

bool consumeFirmwareInstallRequest() {
  if (!g_pendingFirmwareInstall) return false;
  g_pendingFirmwareInstall = false;
  return true;
}

bool consumePendingLibraryRefreshRequest() {
  if (!g_pendingLibraryRefresh) return false;
  // Debounce: wait until 5s of quiet after the last upload before firing.
  // Caller polls this every loop tick; any new upload bumps the timestamp
  // so a burst keeps deferring the walk until the user is genuinely done.
  if (millis() - g_lastLibraryRefreshBumpMs < kLibraryRefreshDebounceMs) {
    return false;
  }
  g_pendingLibraryRefresh = false;
  return true;
}

void requestLibraryRefresh() { g_pendingLibraryRefresh = true; }

bool consumeFtDefragRestartIfDue() {
  if (g_pendingFtDefragRestartAtMs == 0) return false;
  if (static_cast<int32_t>(millis() - g_pendingFtDefragRestartAtMs) < 0) return false;
  g_pendingFtDefragRestartAtMs = 0;
  return true;
}

namespace {
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
// CrumBLE 4.5.5: periodic-fsync cursor. Tracked separately from
// wsLastProgressSent so we can tune the cadences independently. Each
// fsync commits the file's growth + the directory entry to SD, so a mid-
// upload crash leaves a valid (truncated) file that the next attempt's
// RESUME query can read the size of. Without this, FATFS held dirty
// sectors in its in-RAM cache and a crash threw them away -- next RESUME
// got 0 bytes, browser re-uploaded from offset 0, same crash, loop.
size_t wsLastFsyncAt = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

// CrumBLE 4.5.5+: pack-upload mode. The optimizer's chapter-prebake step ships
// hundreds of small cache files; uploading them one-by-one even over a
// persistent WebSocket still pays a per-file FATFS open/close + heap-reclaim
// cycle that fragments heap and triggers the FT safety net mid-batch. Pack
// mode concatenates the whole batch into one binary stream with a leading
// TOC, demuxed byte-by-byte as it arrives -- one WS conn, one file handle at
// a time, no protocol framing between files. Mutually exclusive with the
// single-file wsUpload* state; the START / PACK_START handlers check the
// other's in-progress flag before accepting.
//
// Wire format (all integers little-endian):
//   bytes 0..7   : magic "CMBPACK1"
//   bytes 8..11  : uint32 file_count
//   then, repeated file_count times:
//     uint32 path_len    (excluding null terminator)
//     <path_len bytes>   utf-8 absolute SD path, e.g. "/.crosspoint/abc/0.bin"
//     uint32 file_size
//   then, in the same order:
//     <file_size bytes>  raw file contents, back-to-back, no separators
//
// State machine reads bytes incrementally so a single 4 KB WS frame can span
// any boundary (e.g. last 100 bytes of file N + a TOC entry + first bytes of
// file N+1) without buffering more than the next field's worth in RAM.
enum WsPackState {
  PACK_STATE_IDLE = 0,
  PACK_STATE_MAGIC,
  PACK_STATE_COUNT,
  PACK_STATE_PATH_LEN,
  PACK_STATE_PATH,
  PACK_STATE_FILE_SIZE,
  PACK_STATE_FILE_DATA,
};
bool wsPackInProgress = false;
WsPackState wsPackState = PACK_STATE_IDLE;
uint8_t wsPackClientNum = 255;
uint64_t wsPackTotalBytesExpected = 0;
uint64_t wsPackTotalBytesReceived = 0;
uint64_t wsPackLastProgressSent = 0;
unsigned long wsPackStartTime = 0;
// Small accumulator buffer for the next pending header field. Sized to the
// largest header field (the path string is bounded by kPackMaxPathLen).
constexpr size_t kPackMaxPathLen = 256;
uint8_t wsPackHeaderBuf[kPackMaxPathLen];
size_t wsPackHeaderBytesNeeded = 0;
size_t wsPackHeaderBytesHave = 0;
uint32_t wsPackFileCount = 0;
uint32_t wsPackFileIndex = 0;
uint32_t wsPackCurrentPathLen = 0;
String wsPackCurrentPath;
uint32_t wsPackCurrentFileSize = 0;
uint32_t wsPackCurrentFileWritten = 0;
FsFile wsPackCurrentFile;

// Pack-mode helpers. Kept anonymous-namespace local to this file so they
// can see the file-scope state above. All operate on the wsPack* globals.

// Reset all pack state to idle. Closes any open file handle and deletes the
// in-flight file (it's incomplete -- a future attempt should re-send it).
void resetPackState(const char* tag) {
  if (wsPackCurrentFile) {
    wsPackCurrentFile.flush();
    wsPackCurrentFile.close();
    // Delete the partial file so a follow-up upload writes a clean copy.
    if (wsPackCurrentPath.length() > 0) {
      Storage.remove(wsPackCurrentPath.c_str());
    }
  }
  if (tag != nullptr) {
    LOG_INF("WS", "Pack abort (%s) at file %u/%u, byte %llu/%llu", tag,
            (unsigned)wsPackFileIndex, (unsigned)wsPackFileCount,
            (unsigned long long)wsPackTotalBytesReceived,
            (unsigned long long)wsPackTotalBytesExpected);
  }
  wsPackInProgress = false;
  wsPackState = PACK_STATE_IDLE;
  wsPackClientNum = 255;
  wsPackTotalBytesExpected = 0;
  wsPackTotalBytesReceived = 0;
  wsPackLastProgressSent = 0;
  wsPackHeaderBytesNeeded = 0;
  wsPackHeaderBytesHave = 0;
  wsPackFileCount = 0;
  wsPackFileIndex = 0;
  wsPackCurrentPathLen = 0;
  wsPackCurrentPath = "";
  wsPackCurrentFileSize = 0;
  wsPackCurrentFileWritten = 0;
}

// mkdir -p the parent of a full file path. Creates each missing component
// in turn; Storage.mkdir returns false when the dir already exists (which is
// fine -- we treat that as success). Returns true if the parent ends up
// existing (or already did), false on any actual error.
bool ensureParentDirExists(const String& fullPath) {
  int lastSlash = fullPath.lastIndexOf('/');
  if (lastSlash <= 0) return true;  // no parent (root or relative)
  // Walk from the root, creating each intermediate component.
  for (int i = 1; i <= lastSlash; ++i) {
    if (i < lastSlash && fullPath[i] != '/') continue;
    if (i == lastSlash && fullPath[lastSlash] != '/') {
      // Final iteration -- create the parent itself.
    }
    const String component = fullPath.substring(0, i);
    if (component.length() == 0) continue;
    if (Storage.exists(component.c_str())) continue;
    if (!Storage.mkdir(component.c_str())) {
      // Race: another thread may have made it; verify with exists().
      if (!Storage.exists(component.c_str())) {
        LOG_ERR("WS", "Pack: mkdir failed for %s", component.c_str());
        return false;
      }
    }
  }
  return true;
}

static inline uint32_t readU32LE(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedPath(const String& path) {
  // CrumBLE: /.crosspoint/ is the device's own cache directory. The
  // chapter-prebake optimizer writes here from the web UI (and EPUB /
  // recents / collections store + read from here), so it MUST stay
  // writable even when showHiddenFiles is off. Without this carve-out,
  // every prebake upload fails with HTTP 400 "Access denied to
  // protected path" because the leading ".crosspoint" segment starts
  // with a dot.
  if (path.startsWith("/.crosspoint/") || path == "/.crosspoint") return false;

  // Check every segment of the path, not just the last one.
  // This prevents access to e.g. /.hidden/somefile or /System Volume Information/foo
  int start = 0;
  while (start < (int)path.length()) {
    if (path.charAt(start) == '/') {
      start++;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) end = path.length();

    String segment = path.substring(start, end);

    if (!SETTINGS.showHiddenFiles && segment.startsWith(".")) return true;

    for (const auto* item : HIDDEN_ITEMS) {
      if (segment.equals(item)) return true;
    }

    start = end + 1;
  }

  return false;
}

// CrumBLE 4.4: read /.crosspoint/<hash>/prebake-manifest.json and format a
// concise multi-line tooltip with the locked-in layout settings, suitable
// for stuffing into the HTML `title="..."` attribute on the file-listing
// badge. Mirrors the on-device PrebakeManifestViewerActivity at a glance --
// only the five highest-signal fields (font, size, orientation, line
// spacing, margin) so the tooltip doesn't overflow on hover. Returns ""
// when the file doesn't exist or heap is too tight to safely parse JSON,
// which keeps the badge tooltip empty rather than crashing the listing.
std::string formatPrebakeTooltip(const std::string& cacheDir) {
  // Heap guard: JsonDocument allocations on a fragmented heap could blow
  // the per-row budget. Skip parsing entirely below 8 KB MaxAlloc.
  if (ESP.getMaxAllocHeap() < 8u * 1024u) return "";
  const std::string path = cacheDir + "/prebake-manifest.json";
  if (!Storage.exists(path.c_str())) return "";
  String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) return "";
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Code::Ok) return "";

  auto familyLabel = [&]() -> std::string {
    const char* sdName = doc["sdFontFamilyName"] | "";
    if (sdName && sdName[0] != '\0') return std::string("SD: ") + sdName;
    switch (static_cast<uint8_t>(doc["fontFamily"] | 0)) {
      case CrossPointSettings::LEXENDDECA: return "Lexend Deca";
      case CrossPointSettings::BITTER: return "Bitter";
      case CrossPointSettings::CHAREINK: return "CharEink";
      default: return "Unknown";
    }
  };
  auto orientationLabel = [](uint8_t o) -> const char* {
    switch (o) {
      case CrossPointSettings::PORTRAIT: return "Portrait";
      case CrossPointSettings::LANDSCAPE_CW: return "Landscape CW";
      case CrossPointSettings::INVERTED: return "Portrait inverted";
      case CrossPointSettings::LANDSCAPE_CCW: return "Landscape CCW";
      default: return "Unknown";
    }
  };
  auto pointSize = [&]() -> uint8_t {
    const char* sdName = doc["sdFontFamilyName"] | "";
    const uint8_t fontSize = static_cast<uint8_t>(doc["fontSize"] | 0);
    const uint8_t range = static_cast<uint8_t>(doc["sdFontSizeRange"] | 0);
    if (sdName && sdName[0] != '\0') {
      if (range < CrossPointSettings::SD_FONT_SIZE_RANGE_COUNT) {
        return CrossPointSettings::getSdFontRangePointSize(range, fontSize);
      }
      return 0;
    }
    if (fontSize < CrossPointSettings::FONT_SIZE_COUNT) {
      return CrossPointSettings::getReaderFontPointSize(static_cast<CrossPointSettings::FONT_SIZE>(fontSize));
    }
    return 0;
  };

  const uint8_t pt = pointSize();
  const uint8_t orient = static_cast<uint8_t>(doc["orientation"] | 0);
  const uint8_t margin = static_cast<uint8_t>(doc["screenMargin"] | 0);
  const uint8_t lineSp = static_cast<uint8_t>(doc["lineSpacing"] | 0);

  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "Font: %s\nFont Size: %u pt\nOrientation: %s\nLine Spacing: %u\nMargin: %u px",
                familyLabel().c_str(), static_cast<unsigned>(pt),
                orientationLabel(orient), static_cast<unsigned>(lineSp),
                static_cast<unsigned>(margin));
  return std::string(buf);
}
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }
  // CrumBLE 4.4 post-bisect: the pre-allocated responseBuffer (4 KB resident
  // at boot) was holding contiguous heap that pushed the serve-html guard
  // below its safe MaxAlloc floor at FT startup, triggering an infinite
  // silent-restart loop. Reverted -- chunked /api/files alone was the
  // actually-useful change.

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WebServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });
  server->on("/js/optimizer.js", HTTP_GET, [this] { handleOptimizerJs(); });
  // CrumBLE Phase 5a: PROGMEM-embedded prebake WASM module. ~870 KB total
  // (gzipped). Lazy-loaded by the optimizer page when the user opts in to
  // chapter-prebake; otherwise these handlers are never hit and the flash
  // cost is the only impact.
  server->on("/js/crumble-prebake.js", HTTP_GET, [this] { handleCrumblePrebakeJs(); });
  server->on("/js/crumble-prebake.wasm", HTTP_GET, [this] { handleCrumblePrebakeWasm(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  // Browser sends its wall-clock on FT connect so the device can stamp SD writes
  // with a real mtime. ESP32-C3 has no battery-backed RTC -- every cold boot the
  // clock reads a default (~Dec 31 2025 23:xx) and every uploaded file ends up
  // with that same fake timestamp until we sync.
  server->on("/api/sync-time", HTTP_POST, [this] { handleSyncTime(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  // CrumBLE 4.5.4: on-demand prebake-manifest details for the (i) click
  // popover. Replaces the per-row tooltip the file-list response used to
  // include unconditionally -- that generation cost dominated /api/files
  // latency on heap-tight devices. Now: page loads fast, user clicks the
  // (i) icon next to a CP.FONT badge to see the layout/font detail.
  server->on("/api/files/prebake-manifest", HTTP_GET, [this] { handlePrebakeManifest(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  // v18.9.9.393: manual FT reset endpoint. The browser hits this when its
  // WS retry loop has exhausted / the accept path is wedged (stale client
  // slot post-silent-restart, LWIP TCP hang, etc.). Arms the same debounced
  // silent-restart-to-FT that the DONE handler uses so the WS handler can
  // finish responding to this POST before the reboot fires.
  server->on("/api/restart-ft", HTTP_POST, [this] {
    LOG_INF("WEB", "/api/restart-ft: user-requested silent-restart to FT");
    g_pendingFtDefragRestartAtMs = millis() + 500;
    server->send(200, "application/json", "{\"queued\":true}");
  });
  server->on("/api/reader-render-info", HTTP_GET, [this] { handleReaderRenderInfo(); });
  server->on("/api/save-reader-settings", HTTP_POST, [this] { handleSaveReaderSettings(); });
  // v18.9.9.310: FT-side toggle for the show-hidden-files setting so users
  // can flip it without physically walking back to Settings > Library > Files
  // > Show Hidden Files. GET returns current state, POST body {value: bool}
  // sets and persists. Same setting that gates file-listing + serve paths
  // in this file (SETTINGS.showHiddenFiles), so the change is picked up on
  // the next /api/files refresh with no reboot.
  server->on("/api/show-hidden", HTTP_GET, [this] {
    server->send(200, "application/json",
                 SETTINGS.showHiddenFiles ? "{\"value\":true}" : "{\"value\":false}");
  });
  server->on("/api/show-hidden", HTTP_POST, [this] {
    if (!server->hasArg("plain")) {
      server->send(400, "application/json", "{\"error\":\"missing body\"}");
      return;
    }
    // Body is tiny ({"value":true/false}); parse with a fixed doc.
    JsonDocument doc;
    if (deserializeJson(doc, server->arg("plain")) != DeserializationError::Ok || !doc["value"].is<bool>()) {
      server->send(400, "application/json", "{\"error\":\"expected {value: bool}\"}");
      return;
    }
    SETTINGS.showHiddenFiles = doc["value"].as<bool>() ? 1 : 0;
    // v18.9.9.325: was SETTINGS.saveToFile() inline -- that rebuilds the full
    // settings list (several KB of allocations) + writes JSON via
    // writeFileWithBackup (SD tmp+rename). During FT with active WebSocket
    // traffic and lwIP send queues in play, free heap sits at 15-25 KB;
    // the sync save can push heap below the low-heap silent-restart floor,
    // triggering an auto-restart back to FT. From the user's POV: checkbox
    // click did nothing visually (POST hung / connection dropped mid-write
    // so loadFiles() never fired) AND the device "booted out" of FT (the
    // silent restart). The in-memory value IS what gates file listing, so
    // the next /api/files GET already reflects the new state. Mark the
    // save deferred; the main loop's retryDeferredSaveIfNeeded() tick
    // persists it once heap is comfortable (typically post-upload burst,
    // exit-to-Home, or on next natural save).
    CrossPointSettings::markSaveDeferred();
    LOG_INF("WEB", "[CFG] showHiddenFiles set to %s via FT toggle (persist deferred)",
            SETTINGS.showHiddenFiles ? "true" : "false");
    server->send(200, "application/json",
                 SETTINGS.showHiddenFiles ? "{\"ok\":true,\"value\":true}" : "{\"ok\":true,\"value\":false}");
  });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/builtin-fonts", HTTP_GET, [this] { handleBuiltinFontList(); });
  server->on("/api/fonts/file", HTTP_GET, [this] { handleFontFile(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV headers and register handler.
  // Also collect If-None-Match so the embedded-asset handlers can return
  // 304 Not Modified when the browser's cached ETag still matches -- without
  // this, the WebServer drops every non-allow-listed request header before
  // dispatching to handlers, so server->header("If-None-Match") returns "".
  const char* davHeaders[] = {"Depth",        "Destination",  "Overwrite", "If",
                              "Lock-Token",   "Timeout",      "If-None-Match"};
  server->collectHeaders(davHeaders, sizeof(davHeaders) / sizeof(davHeaders[0]));
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();
  // v18.9.9.394: prime the idle-restart clock so a browser that never
  // connects still gets a periodic freshener (see CrossPointWebServer-
  // Activity onLoop). Without this, g_ftLastActivityAtMs stays 0 and
  // the idle check short-circuits forever.
  // v18.9.9.398: same prime for the WS-specific tracker so the WS-wedge
  // detector has a well-defined start time.
  bumpFtActivity();
  g_ftLastWsEventAtMs = millis();

  // v18.9.9.91: DEFER WebSocket server 3 s. Constructing WebSocketsServer +
  // begin() peels ~4-5 KB off maxAlloc (client-slot buffer + listen socket).
  // At post-WiFi heap on X4 (~14 KB free, ~12 KB maxAlloc) that drop tips
  // us below the point where TCP accept can serve even a 200-byte JSON
  // response. Browser JS opens WS only on user-initiated upload, so the
  // HTML+API browse phase doesn't need it. See wsPendingBeginAt_ in
  // handleClient for the actual init.
  // v18.9.9.426: bumped 3 s -> 30 s. Field bug: the WS lazy init also fires
  // reclaimWsHeapReservePad("WS-lazy") which grabbed 8-32 KB contiguous
  // via a size ladder; when the ladder hit 16 KB it collapsed maxAlloc
  // mid-serve and killed the browser connection.
  // v18.9.9.429: REVERTED back to 3 s. v427 dropped the size ladder --
  // pad reclaim is now single-attempt 32 KB, which fails on the fragmented
  // steady-state heap this device presents in FT context. So WS-lazy no
  // longer grabs any heap; it just inits the WS server. The 30 s delay was
  // protecting against a bug that no longer exists, and it was actively
  // preventing the browser from opening the WebSocket for upload within
  // the first 30 s of an FT session (which is the common case for a
  // pre-baked book or a small upload).
  wsPendingBeginAt_ = millis() + 3000;
  LOG_DBG("WEB", "WS server deferred; will init at ~t+3s");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
  wsLastFsyncAt = 0;
  // CrumBLE 4.5.4: explicit abort -- clear the panic-recovery flag so we
  // don't auto-restart-to-FT on a subsequent unrelated panic.
  setFtUploadInProgress(false);
  // CrumBLE 4.5.5: heap-reserve pad reclaim (see WS-DONE for rationale).
  reclaimWsHeapReservePad("WS-abort");
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server
  wsPendingBeginAt_ = 0;  // v18.9.9.91: cancel any deferred WS init

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  // v18.9.9.94: release the 32 KB WS heap reserve pad if it's still held.
  // Without this, a FT session that successfully allocated the pad and never
  // consumed it via a WS upload START would leak 32 KB across the whole
  // remainder of the boot -- and a subsequent Home->FT return would start
  // 32 KB in the hole. Only bites multi-session-per-boot users, but that's
  // a real path.
  releaseWsHeapReservePad("WEB-stop");

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // v18.9.9.91: lazy WS init (see wsPendingBeginAt_ notes in header).
  if (wsPendingBeginAt_ && (int32_t)(millis() - wsPendingBeginAt_) >= 0) {
    wsPendingBeginAt_ = 0;
    LOG_DBG("WEB", "WS lazy init at t+3s: free=%u maxAlloc=%u",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    wsServer.reset(new WebSocketsServer(wsPort));
    wsInstance = this;
    wsServer->begin();
    wsServer->onEvent(wsEventCallback);
    // Same heap-reserve-pad grab that was previously at WEB-start time.
    reclaimWsHeapReservePad("WS-lazy");
    LOG_DBG("WEB", "WS lazy init done: free=%u maxAlloc=%u",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

// CrumBLE: bound a streaming response's blocking send. handleClient() runs on
// the main task, so if a chunked send() stalls -- a TX buffer can't allocate
// under the tight file-transfer heap (~20-26 KB free), or the AP drops the link
// mid-response -- the write blocks until TCP eventually resets, which freezes
// the main task (input + rendering) and wedges the device until a reboot.
// SO_SNDTIMEO makes a blocked write() fail after the timeout so the handler
// returns and the activity's loop resumes (watchdog fed, exit button checked).
static void applyClientSendTimeout(WebServer* server) {
  if (!server) return;
  // v18.9.9.124: match v55 behavior. Removed SO_LINGER=60s (from v103, when
  // we were fighting to get send_P to complete for curl -- no longer needed
  // now that send_P uses Content-Length and drains before returning). Left
  // setTimeout at 60s but this only kicks in on genuine hang, not normal
  // close. SO_LINGER was holding TCP resources for up to 60s post-close,
  // making maxAlloc recover slowly (v123: stuck at 5108 for many seconds
  // vs v55 which recovered to 9204 quickly). No linger = TCP FIN normally,
  // ephemeral socket state, faster heap recovery.
  server->client().setTimeout(60000);
}

// CrumBLE: shared low-heap guard. Called at the top of every WebServer
// handler that allocates a non-trivial buffer or builds the settings list.
// If the heap is below the FT freeze threshold, send an empty page with a
// meta-refresh header and schedule a silent restart back into FT -- same
// recovery path as sendBufferGzip. Returns true when the handler should
// bail (response already sent + restart queued); false to continue.
static bool guardLowHeapOrAutoRestart(WebServer* server, const char* tag) {
  // CrumBLE: apply the 5 s send timeout BEFORE doing anything else so the
  // 503 short-circuit response (and any later send paths) can't wedge
  // forever on a browser that closed the socket mid-fetch.
  applyClientSendTimeout(server);
  const uint32_t preFree = ESP.getFreeHeap();
  const uint32_t preMax = ESP.getMaxAllocHeap();
  // v18.9.9.113: lowered free floor 14 -> 9 KB, maxAlloc 10 -> 7 KB. Field log
  // on v112 shows post-webServer-begin at ~14 KB free / ~13 KB maxAlloc, but
  // after HTML serve completes, api-files fires at ~10-11 KB free / ~7-9 KB
  // maxAlloc -- juuust below the old 14/10 threshold, so every request went
  // to soft 503 -> silent-restart-to-FT loop even though the actual serve
  // path can complete at that heap level. Empirical: api-files streams 33
  // rows successfully at maxAlloc=7412 (v112 [15650] log shows the guard
  // fired, but heap DID recover to 10228 maxAlloc within 2 sec -- the retry
  // just kept re-firing before recovery). Lowering the threshold matches the
  // actual working range so api-files completes on real heap, not idealized
  // "clean-boot" heap.
  if (preFree >= 9u * 1024u && preMax >= 7u * 1024u) {
    LOG_INF("WEB", "guard %s ok: pre free=%u maxAlloc=%u", tag, preFree, preMax);
    return false;
  }
  // CrumBLE 4.4: post-upload settle window. The browser-side chapter prebake
  // step needs to query the device immediately after DONE -- if the very
  // next api-files / api-* request triggers a silent-restart, the prebake
  // can't locate the freshly-uploaded EPUB and fails with "could not find
  // uploaded EPUB". Skip the silent-restart for 15 seconds after the most
  // recent upload completion; the browser is likely doing post-upload
  // bookkeeping and heap will recover naturally. Outside this window, the
  // normal guard fires.
  constexpr unsigned long POST_UPLOAD_SETTLE_MS = 15000;
  if (wsLastCompleteAt > 0 && (millis() - wsLastCompleteAt) < POST_UPLOAD_SETTLE_MS) {
    LOG_INF("WEB",
            "guard %s low-heap (free=%u maxAlloc=%u) but within post-upload settle window (%u ms ago); passing",
            tag, preFree, preMax, (unsigned)(millis() - wsLastCompleteAt));
    return false;
  }
  // CrumBLE 4.5.5: first-failure soft 503. Field log: on cold FT entry the
  // HTML serve drops post-serve free heap to ~14.6 KB / maxAlloc ~11 KB --
  // hovering just above this guard's floor. The 1 s of "settle" before
  // /api/files arrives is sometimes long enough (heap recovers as
  // AsyncResponse drains) and sometimes not (slow WiFi ACKs keep buffers
  // pinned). Previously every miss went straight to silentRestart, which
  // is a 5+ s outage even when heap would have recovered on its own in 2
  // s of retry backoff. New flow: first N misses inside a sliding window
  // return 503 + Retry-After so the browser's retry loop handles it.
  // Only persistent failure (M consecutive misses, or guard miss with
  // truly bottomed heap) escalates to silentRestart.
  static uint8_t s_consecutiveGuardMisses = 0;
  static unsigned long s_firstMissAt = 0;
  constexpr uint8_t SOFT_503_BUDGET = 3;             // first 3 misses are soft
  constexpr unsigned long SOFT_503_WINDOW_MS = 12000;  // ... within 12 s of first
  const bool hardFloor = (preFree < 6u * 1024u) || (preMax < 3u * 1024u);
  if (s_firstMissAt == 0 || (millis() - s_firstMissAt) > SOFT_503_WINDOW_MS) {
    s_firstMissAt = millis();
    s_consecutiveGuardMisses = 0;
  }
  s_consecutiveGuardMisses++;
  if (!hardFloor && s_consecutiveGuardMisses <= SOFT_503_BUDGET) {
    LOG_INF("WEB",
            "guard %s low-heap (free=%u maxAlloc=%u) miss %u/%u: soft 503 (no restart)",
            tag, preFree, preMax, s_consecutiveGuardMisses, SOFT_503_BUDGET);
    server->sendHeader("Retry-After", "2");
    server->send(503, "application/json", "[]");
    return true;
  }
  LOG_ERR("WEB",
          "guard %s low-heap (free=%u maxAlloc=%u) miss %u%s: scheduling silentRestart to FT",
          tag, preFree, preMax, s_consecutiveGuardMisses,
          hardFloor ? " hardFloor" : "");
  server->sendHeader("Refresh", "8");
  // Some browsers will not honour Refresh on a JSON response. Send an
  // application/json body that the page-side fetch can detect (status 503
  // + an empty array fallback) AND meta-refresh-equivalent via the header
  // so a subsequent navigation still recovers.
  server->send(503, "application/json", "[]");
  g_pendingFtRestart = true;
  s_consecutiveGuardMisses = 0;
  s_firstMissAt = 0;
  return true;
}

// CrumBLE: every page/asset serve gets the same stall protection, and we log
// heap around the send so a crater can be pinned to a specific response.
static void sendBufferGzip(WebServer* server, const char* mime, const char* data,
                           size_t len, const char* tag) {
  applyClientSendTimeout(server);
  // CrumBLE: defensive guard against the "device freezes when phone hits
  // /files" pattern. 22 KB free is the floor that consistently lets the
  // FilesPage's ~12 KB post-serve dip avoid wedging subsequent /api/*.
  const uint32_t preFree = ESP.getFreeHeap();
  const uint32_t preMax = ESP.getMaxAllocHeap();
  LOG_INF("WEB", "serve %s: %u B, pre free=%u maxAlloc=%u", tag, (unsigned)len, preFree, preMax);
  // CrumBLE 4.2: the "schedule a silentRestart, serve an auto-refreshing
  // empty page" recovery only works for text/html consumers (the FilesPage
  // / browser UI). For non-HTML resources -- WASM, JS, JSON, font files --
  // substituting a text/html payload silently breaks the caller: the
  // optimizer's streaming WebAssembly.compile() rejects on "Incorrect
  // response MIME type. Expected 'application/wasm'." and the prebake
  // aborts on every book.
  //
  // Two changes:
  //   1. The HTML-page substitution stays gated on the original 22 KB
  //      floor -- that path allocates ~5-10 KB for string concatenation
  //      during page generation, so the floor needs to leave room.
  //   2. PROGMEM streams (send_P below) go through WiFiClient::write_P
  //      using a small (~1-2 KB) intermediate buffer rather than buffering
  //      the whole payload, so they don't need anywhere near 22 KB of
  //      contiguous heap. Drop the floor to 6 KB for non-HTML serves so
  //      genuinely tight heap (post-upload, post-optimizer) doesn't block
  //      the WASM fetch. If we're still below 6 KB the request degrades
  //      to a real 503 instead of a MIME-mismatched empty HTML body.
  const bool isHtmlSubstitutionSafe = (mime && strcmp(mime, "text/html") == 0);
  // v18.9.9.90: pre-emptive silent-restart-to-FT DISABLED for the HTML path.
  // History: v87 lowered floor from 14 KB to 4 KB after chunked serve; v90
  // removes it entirely. The chunked path peaks at ~2 KB regardless of preFree
  // (streams straight from PROGMEM), so refusing to serve when preFree is
  // 3-4 KB just breaks the browser's session for no reason. If we truly OOM
  // during the send, the terminate handler catches it and hard-restarts with
  // panel resync (v18.9.9.84). One clean reboot beats N pre-emptive ones.
  // Only rock-bottom sanity floor at 1.5 KB (below this the WebServer itself
  // can't even build the response headers).
  if (isHtmlSubstitutionSafe && preFree < 1536u) {
    LOG_ERR("WEB", "serve %s: preFree=%u below rock-bottom 1.5 KB sanity floor; sending 503",
            tag, preFree);
    server->send(503, "text/html", "<!doctype html>Low memory, please refresh.");
    return;
  }
  if (!isHtmlSubstitutionSafe && preFree < 1024u) {
    LOG_ERR("WEB", "serve %s: preFree=%u below 1 KB sanity floor; sending 503", tag, preFree);
    server->send(503, "text/plain", "Server low memory");
    return;
  }
  // v18.9.9.87: chunked serve. Was `server->send_P(200, mime, data, len)` which
  // required a chunk of contiguous heap sized to the whole payload PLUS
  // headers. On X4 FT with ~5 KB free, that immediately silent-restarted every
  // /files request. Now we set Transfer-Encoding: chunked implicitly via
  // setContentLength(CONTENT_LENGTH_UNKNOWN) and stream the pre-gzipped bytes
  // directly from PROGMEM in 4 KB slices. Peak allocation drops from
  // ~len+2 KB to ~2-3 KB (WebServer state). Browsers reassemble transparently.
  // v18.9.9.100: use send_P (Content-Length, non-chunked) instead of the
  // chunked-encoding path. Root cause of "server logs full serve, browser
  // sees nothing": WebServer::sendContent_P allocates 11 B via malloc per
  // chunk for the <hex-len>\r\n prefix. At maxAlloc < 11 B that malloc
  // fails silently, the prefix is skipped, but the data is still written
  // -- corrupting the chunked stream. Chrome silently discards the whole
  // response. send_P doesn't touch _chunked, so sendContent_P inside it
  // skips the malloc branch and writes raw. Pre-set gzip encoding header.
  // v18.9.9.104: replace WebServer's send_P (which fires-and-forgets) with a
  // manual write loop that returns actual bytes-written per iteration, plus
  // a post-write drain wait. Prior versions logged "sent 12554" but curl
  // received 0 body bytes -- something between our write and the wire was
  // dropping data. Now we explicitly track bytes written and give TCP time
  // to physically transmit before we return. If a chunk write returns 0,
  // we know exactly where the write path is failing.
  server->client().setNoDelay(true);
  auto& client = server->client();
  // Build and send response headers manually so we can then write the body
  // via NetworkClient directly and see exact bytes transmitted.
  {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Encoding: gzip\r\n"
                     "Content-Length: %u\r\n"
                     "Cache-Control: public, max-age=86400\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     mime, (unsigned)len);
    size_t hdrWritten = 0;
    if (n > 0) hdrWritten = client.write(reinterpret_cast<const uint8_t*>(hdr), (size_t)n);
    LOG_INF("WEB", "serve %s: hdr write returned %u/%d", tag, (unsigned)hdrWritten, n);
  }
  // v18.9.9.106: NetworkClient has NO availableForWrite() implementation --
  // it inherits Print::availableForWrite() which returns 0. That's why
  // v105's polling loop spun forever without ever writing. New approach:
  // retry-with-backoff around write_P. If write_P returns 0 (NetworkClient's
  // 10s internal retry exhausted), delay 100 ms and try again with a
  // smaller slice. If we make ANY progress, reset the retry budget.
  constexpr size_t kInitialSlice = 1024;
  constexpr size_t kMinSlice = 128;
  // v18.9.9.362: tightened deadline + zero-write cap. write_P's internal
  // retry blocks up to 10 s per zero-return on weak WiFi; at 5 zeros the
  // device would appear frozen for 50 s while stuck in a TCP retransmit
  // hole with rssi ~-76 dBm. Fail faster (2 zeros ≈ 20 s max) so the
  // user isn't locked out; overall 30 s cap catches the mid-serve
  // gradual-slowdown case too.
  constexpr uint32_t kOverallDeadlineMs = 30000;
  constexpr int kMaxConsecutiveZeros = 2;
  size_t sent = 0;
  int iter = 0;
  int consecutiveZeros = 0;
  size_t slice = kInitialSlice;
  const uint32_t serveStart = millis();
  while (sent < len) {
    if (!client.connected()) {
      LOG_ERR("WEB", "serve %s: client disconnected mid-body at %u/%u iter=%d",
              tag, (unsigned)sent, (unsigned)len, iter);
      break;
    }
    if (millis() - serveStart > kOverallDeadlineMs) {
      LOG_ERR("WEB", "serve %s: overall deadline hit at %u/%u iter=%d",
              tag, (unsigned)sent, (unsigned)len, iter);
      break;
    }
    const size_t remain = len - sent;
    const size_t thisSlice = (remain > slice) ? slice : remain;
    const size_t written = client.write_P(reinterpret_cast<PGM_P>(data + sent), thisSlice);
    if (written == 0) {
      consecutiveZeros++;
      if (iter < 3 || consecutiveZeros == 1) {
        LOG_INF("WEB", "serve %s: iter=%d slice=%u returned 0 (consecutiveZeros=%d, backing off)",
                tag, iter, (unsigned)thisSlice, consecutiveZeros);
      }
      if (consecutiveZeros >= kMaxConsecutiveZeros) {
        // v18.9.9.362: include rssi so field logs correlate write
        // stalls with weak-signal conditions. rssi < -75 dBm is where
        // TCP retransmits start choking the send buffer.
        LOG_ERR("WEB", "serve %s: %d consecutive zero-writes at %u/%u (rssi=%d dBm) -- abandoning",
                tag, kMaxConsecutiveZeros, (unsigned)sent, (unsigned)len, (int)WiFi.RSSI());
        break;
      }
      // Shrink slice + wait for buffer to drain. delay() yields to LWIP.
      slice = (thisSlice > kMinSlice * 2) ? thisSlice / 2 : kMinSlice;
      delay(200);
    } else {
      sent += written;
      consecutiveZeros = 0;
      slice = kInitialSlice;  // reset to normal slice size on success
      if (iter < 5 || iter % 4 == 0) {
        LOG_INF("WEB", "serve %s: iter=%d slice=%u wrote %u (sent %u/%u)",
                tag, iter, (unsigned)thisSlice, (unsigned)written, (unsigned)sent, (unsigned)len);
      }
    }
    ++iter;
  }
  // Final drain wait: give TCP time to actually transmit what we queued.
  // No availableForWrite() to poll, so just wait a fixed amount.
  delay(1000);
  LOG_INF("WEB", "serve %s done: body-sent=%u/%u total-time=%u ms free=%d maxAlloc=%d",
          tag, (unsigned)sent, (unsigned)len, (unsigned)(millis() - serveStart),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  // v18.9.9.169: signal progress to the low-heap-restart loop-break check.
  // v18.9.9.177: relaxed to `sent >= 20 KB` (was `sent == len && len >= 20K`).
  // Partial-but-substantial serves (e.g. browser gave up at 41K/61K on the
  // optimizer.js) still prove we're not in a boot-fail-boot-fail loop; the
  // strict equality gate refused restarts we should have allowed.
  if (sent >= 20u * 1024u) {
    g_ftLargeServesSinceRestart++;
  }
}

// v18.9.9.385: forward-declare so the HTML handlers above the definition
// (handleRoot, handleFileList, handleSettingsPage) can call it.
static bool applyAssetCacheOrServe304(WebServer* server, const char* assetName,
                                      uint32_t contentSize, uint32_t maxAgeSec = 0);

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  sendBufferGzip(server, "text/html", data, len, "html");
}

void CrossPointWebServer::handleRoot() const {
  // v18.9.9.385: ETag-validated HTML serve. Same rationale as JS -- browsers
  // were heuristically caching HTML for tens of minutes without asking,
  // pinning users to pre-flash content until a hard-refresh or tab close.
  if (applyAssetCacheOrServe304(server.get(), "home", sizeof(HomePageHtml))) return;
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

// CrumBLE 4.5.5+: revalidating browser cache for the optimizer/WASM bundle.
// These blobs are embedded in firmware, so the URL content only changes on
// reflash -- but the URL string itself doesn't (e.g. /js/crumble-prebake.wasm
// is the same path regardless of build). The previous "max-age=86400,
// immutable" policy was wrong for that shape: `immutable` tells the browser
// to skip revalidation entirely, so a freshly flashed firmware with new WASM
// bytes still served stale content from cache for 24 h until the user did a
// hard reload. This bit us in a session: bumped MAX_PAGE_GLYPHS in WASM,
// reflashed, re-optimized -- still got the old 506-glyph cap because the
// browser never re-asked the server.
//
// New policy: must-revalidate + per-asset content-derived ETag. The ETag
// embeds the asset's compressed size (a compile-time constant that changes
// every time the asset content changes), so a reflash with a different
// asset payload produces a different ETag string. The browser revalidates
// on every load: server returns 304 (no body) when the ETag still matches,
// 200 with fresh bytes when it doesn't. Repeated loads on the same firmware
// pay only a few hundred bytes of header round-trip per asset, and a reflash
// is picked up automatically with zero user action.
//
// Returns true when a 304 has already been sent and the caller should bail
// without writing a body. Returns false when the caller should proceed to
// send the full response (the ETag + cache headers have been set).
// v18.9.9.385: default max-age dropped from 60 to 0. Every request now sends
// If-None-Match; server returns 304 (~200 bytes) when the ETag matches, 200
// with fresh content when it doesn't. Fixes: after a firmware flash, users
// were stuck on cached JS for up to 60 seconds -- and if their browser tab
// stayed open across the flash, the in-memory JS was pinned at whatever
// version loaded when the tab opened, requiring a manual DevTools cache-clear
// to pick up the new firmware. With max-age=0 + must-revalidate, a plain
// browser refresh always picks up flashed changes. Cost: ~200 bytes per
// asset per page load for the validation round-trip; asset body transfers
// zero extra bytes since the ETag hit still serves from cache. Explicit
// larger max-age values (e.g. prebake.wasm passes 3600) still apply.
static bool applyAssetCacheOrServe304(WebServer* server, const char* assetName,
                                      uint32_t contentSize, uint32_t maxAgeSec) {
  // ETag format: "fw-<version>-<env>-<asset>-<size>". Per-asset name keeps
  // a change in one blob from invalidating the others' caches; size is the
  // content-derived discriminator. Quoting per RFC 7232.
  String etag;
  etag.reserve(64);
  etag += "\"fw-";
  etag += CROSSINK_VERSION;
  etag += "-";
  etag += CROSSINK_BUILD_ENV;
  etag += "-";
  etag += assetName;
  etag += "-";
  etag += String(contentSize);
  etag += "\"";

  // Build Cache-Control with the caller's max-age. The ETag is content-
  // derived (size changes with content), so a reflash still invalidates:
  // after max-age expires the browser sends If-None-Match, gets 200-fresh
  // or 304 based on the ETag match. What the max-age BUYs us is that
  // within the same session (or within maxAgeSec of each other), the
  // browser hits its memory cache without any network round-trip. Vital
  // for prebake.wasm (1.3 MB) -- browsers re-request modules for various
  // reasons (fetch() cancellation retries, worker imports); without a
  // cache window each attempt was hammering our tight-heap TCP path.
  String cacheCtrl;
  cacheCtrl.reserve(64);
  cacheCtrl += "public, max-age=";
  cacheCtrl += String(maxAgeSec);
  cacheCtrl += ", must-revalidate";

  const String inm = server->header("If-None-Match");
  if (inm.length() > 0 && inm == etag) {
    server->sendHeader("ETag", etag);
    server->sendHeader("Cache-Control", cacheCtrl);
    server->send(304, "", "");
    return true;
  }

  server->sendHeader("ETag", etag);
  server->sendHeader("Cache-Control", cacheCtrl);
  return false;
}

// v18.9.9.180: forward decl for guardPrebakeHeapOrRestart used below.
static bool guardPrebakeHeapOrRestart(WebServer* server, const char* tag, uint32_t minMaxAlloc);

void CrossPointWebServer::handleJszip() const {
  if (applyAssetCacheOrServe304(server.get(), "jszip", jszip_minJsCompressedSize)) return;
  // v18.9.9.180: apply the same heap guard as prebake.js/wasm. jszip is 28 KB
  // gzipped -- if the pre-serve heap is already fragmented from api-files
  // the browser gives up mid-body, and everything downstream (optimizer.js,
  // prebake.js, WASM) never gets fetched. Empirical: 12 KB floor matches
  // prebake.js's floor, since jszip is a similar size.
  if (guardPrebakeHeapOrRestart(server.get(), "jszip", 12u * 1024u)) return;
  sendBufferGzip(server.get(), "application/javascript", jszip_minJs,
                 jszip_minJsCompressedSize, "jszip.js");
}

void CrossPointWebServer::handleOptimizerJs() const {
  if (applyAssetCacheOrServe304(server.get(), "optimizer", optimizerJsCompressedSize)) return;
  // v18.9.9.180: heap guard. 61 KB gzipped serve wedges at maxAlloc=10K
  // (browser disconnects mid-body ~iter=45). Same guard as prebake.js.
  // Empirical floor 14 KB -- slightly higher than prebake.js (12 KB) because
  // optimizer.js is longer and needs more sustained headroom.
  //
  // v18.9.9.297: lowered 14 KB -> 8 KB. Browsers do NOT auto-retry 503
  // responses for <script src=> tags, so the "guard fails -> silent-
  // restart -> browser retries via Retry-After" flow only works if the
  // browser is a fetch()-based module loader. Field repro: FilesPage
  // loads HTML fine, then the HTML script tag for /js/optimizer.js
  // arrives at maxAlloc=~7 KB (HTML delivery ate ~8 KB), guard 503s,
  // restart fires, but the browser has already given up on the script
  // and never re-issues the request. UX = browser stuck loading. The
  // 8 KB floor accepts a slightly higher wedge risk in exchange for
  // "the JS actually loads on the first HTML->JS request pair." If a
  // wedge happens the user hits refresh, same behavior as before.
  if (guardPrebakeHeapOrRestart(server.get(), "optimizer", 8u * 1024u)) return;
  sendBufferGzip(server.get(), "application/javascript", optimizerJs,
                 optimizerJsCompressedSize, "optimizer.js");
}

// v18.9.9.123: handleFilesAppJs removed -- v97 split reverted, JS is now
// inline in FilesPage.html served by handleFileList().

// v18.9.9.133: heap-aware defer for large prebake assets. The 1.3 MB WASM
// (and to a lesser extent the 46 KB JS) cannot sustain a serve when maxAlloc
// is already degraded from prior requests (Files page + api-files +
// optimizer.js). Field repro: WASM abandoned at 129 KB / 1.3 MB after ~57 s
// of retries. Empirical minimum for reliable serve: maxAlloc >= 18 KB at
// request arrival. Below that we silent-restart-to-FT so the browser's
// automatic module-retry (or the user's second click) lands on a fresh
// ~90 KB post-boot heap.
static bool guardPrebakeHeapOrRestart(WebServer* server, const char* tag, uint32_t minMaxAlloc) {
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (maxAlloc >= minMaxAlloc) return false;
  // v18.9.9.426: dropped the automatic silent-restart. Field bug: the
  // restart fired mid-Optimize (right after serving optimizer.js when
  // the WS pad grab collapsed maxAlloc), killing the browser connection
  // entirely and producing "cannot fetch" in the UI. The browser has
  // its own retry logic (respects Retry-After); a passive 503 gives it
  // a chance to try again after our natural heap recovery. If we're
  // genuinely stuck low-heap, the loop-level heap watchdog will still
  // trigger a restart -- but only after a real stall, not on the first
  // low-maxAlloc reading during normal serve activity.
  LOG_ERR("WEB", "prebake %s: maxAlloc=%u below reliable-serve floor %u -- returning 503 (browser will retry)",
          tag, (unsigned)maxAlloc, (unsigned)minMaxAlloc);
  // v18.9.9.181: send EMPTY body instead of "device rebooting..." plain text.
  // Field repro: WebAssembly.instantiate(await response.arrayBuffer())
  // ignored the 503 status and tried to parse "device rebooting..." bytes
  // as WASM, yielding a misleading "expected magic word 00 61 73 6d,
  // found 64 65 76 69 @+0" error to the user. Empty body means the
  // instantiation still fails but with a truthful "input too short"
  // error, not the deceptive magic-word one.
  server->sendHeader("Retry-After", "3");
  server->send(503, "application/octet-stream", "");
  return true;
}

void CrossPointWebServer::handleCrumblePrebakeJs() const {
  if (CrumblePrebakeJsCompressedSize == 0) {
    server->send(404, "text/plain", "Prebake WASM not built into this firmware");
    return;
  }
  if (applyAssetCacheOrServe304(server.get(), "prebake-js",
                                CrumblePrebakeJsCompressedSize, 3600)) return;
  // v18.9.9.183: prebake.js guard REMOVED. Same rationale as wasm below.
  // Browser retries immediately after 503 without giving the silent-restart
  // time to complete. Serving at tight heap is slower but doesn't guarantee
  // failure via the retry race.
  sendBufferGzip(server.get(), "application/javascript", CrumblePrebakeJs,
                 CrumblePrebakeJsCompressedSize, "crumble-prebake.js");
}

void CrossPointWebServer::handleCrumblePrebakeWasm() const {
  if (CrumblePrebakeWasmCompressedSize == 0) {
    server->send(404, "text/plain", "Prebake WASM not built into this firmware");
    return;
  }
  if (applyAssetCacheOrServe304(server.get(), "prebake-wasm",
                                CrumblePrebakeWasmCompressedSize, 3600)) return;
  // v18.9.9.183: WASM heap guard REMOVED. Field repro (v181 test): browser
  // retries streaming→ArrayBuffer immediately after the first 503, before
  // the server has a chance to actually silent-restart. Both fetches get
  // 503, browser gives up, THEN server restarts -- too late. Serving at
  // whatever heap is available is slower but at least the browser has a
  // chance to succeed. If serve wedges on truly-tight heap (~2 KB), the
  // sendBufferGzip write loop will time out and browser will retry later.
  // Threshold-based rejection just guaranteed failure via the retry race.
  sendBufferGzip(server.get(), "application/wasm", CrumblePrebakeWasm,
                 CrumblePrebakeWasmCompressedSize, "crumble-prebake.wasm");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  // CrumBLE: "version" is the CrumBLE marketing version (4.0, 4.1, ...)
  // shown on HomePage's Device Status. Upstream CrossInk's sync point
  // ships as a separate field for technical context.
  // CrumBLE 4.5: dropped the "(CrossInk X.Y.Z-tiny-bitter)" suffix from the
  // FT-page display string. The LAN-OTA version check parses "CrumBLE X.Y.Z"
  // out of this value -- shortening it doesn't break that regex.
  doc["version"] = "CrumBLE " CRUMBLE_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// Accepts {"epochMs": <browser wall-clock in ms, already shifted to local time>}
// and sets the system clock so subsequent SD writes get a real mtime. ESP32-C3
// has no battery-backed RTC; without this, every uploaded file's mtime is the
// boot-default (~Dec 31 2025 23:xx) and the file browser/recents UI can't sort
// by date meaningfully. Browser sends Date.now() - tzOffsetMin*60000 so the
// device just treats the value as plain seconds-since-epoch; FAT stores wall-
// clock with no TZ so this lands as the user's local time in the file listing.
void CrossPointWebServer::handleSyncTime() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }
  const uint64_t epochMs = doc["epochMs"].as<uint64_t>();
  // Sanity: must be after 2024-01-01 (1704067200000 ms). Guards against the
  // browser sending 0 / an obvious garbage value that would land the clock
  // somewhere worse than the default.
  if (epochMs < 1704067200000ULL) {
    server->send(400, "text/plain", "epochMs implausible (must be >= 2024-01-01)");
    return;
  }
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(epochMs / 1000ULL);
  tv.tv_usec = static_cast<suseconds_t>((epochMs % 1000ULL) * 1000ULL);
  if (settimeofday(&tv, nullptr) != 0) {
    server->send(500, "text/plain", "settimeofday failed");
    return;
  }
  LOG_INF("WEB", "Clock synced from browser: epoch=%lld", static_cast<long long>(tv.tv_sec));
  server->send(200, "text/plain", "ok");
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  FsFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  FsFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  // v18.9.9.385: ETag-validated HTML serve so firmware flashes take effect on
  // the next page load without a manual cache-clear. See applyAssetCacheOrServe304.
  if (applyAssetCacheOrServe304(server.get(), "files-page", sizeof(FilesPageHtml))) return;
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  bumpFtActivity();
  if (guardLowHeapOrAutoRestart(server.get(), "api-files")) return;
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedPath(currentPath)) {
    server->send(403, "application/json", "[]");
    return;
  }

  applyClientSendTimeout(server.get());
  // CrumBLE 4.4 post-bisect: chunked HTTP streaming. Each row is serialized
  // into a small stack buffer and sent immediately as a chunk -- the full
  // body is never held in heap. This keeps MaxAlloc high throughout the
  // request (~20 KB instead of dropping to ~2 KB at the end), which lets
  // lwIP allocate normal-sized TCP buffers and complete the send in <1s
  // instead of the 10+s the prior std::string-body path took on /.crosspoint
  // (86 entries, ~8 KB body). That slow path triggered browser fetch
  // abort -> ERR_CONTENT_LENGTH_MISMATCH on the client even though the
  // server eventually finished. setContentLength(CONTENT_LENGTH_UNKNOWN)
  // puts the Arduino WebServer into Transfer-Encoding: chunked mode;
  // sendContent then emits framed chunks instead of needing an upfront
  // Content-Length header.

  // CrumBLE 4.4: row buffer bumped from 512 -> 1024 to accommodate the
  // optional prebakeTooltip field. Worst-case tooltip ~200 bytes; rest of
  // the JSON (name + booleans) easily fits in the prior 512. 1024 leaves
  // comfortable headroom.
  char rowBuf[1024];
  // CrumBLE 4.5.5: chunk-batching accumulator. Field log: 60-file SD card
  // truncated to 25 rows because each row was sent as its own TCP chunk
  // (~250 byte pbuf each) and the browser couldn't ACK fast enough --
  // pbufs accumulated, maxAlloc dropped below the 1200 B bailout floor
  // ~25 rows in. Batching multiple rows into one larger chunk means
  // 4-5 pbufs instead of 60+, drastically less concurrent pbuf pressure.
  // 2 KB stack budget is safely below ESP32 task stack (8 KB+) and big
  // enough to hold ~10 typical rows.
  // v18.9.9.128: REVERTED v126's 2048 -> 512. Root cause of 30 s flushes
  // isn't chunk size -- it's NetworkClient::write's fixed 10-iter x 1s
  // select-timeout loop for each send call (~3 sends per sendContent for
  // the size header, body, trailing CRLF). Smaller chunks MULTIPLY that
  // fixed cost across more flushes. Larger batches minimize total wall
  // clock even when heap is tight, because we amortize the write-path
  // stall across more bytes per flush.
  static constexpr size_t kBatchBufSize = 1536;
  // Stack-allocated to keep zero heap impact; flushed to TCP when near
  // full (>= 1.5 KB) or at end of scan.
  char batchBuf[kBatchBufSize];
  size_t batchLen = 0;
  bool seenFirst = false;
  bool truncated = false;
  size_t emittedRows = 0;

  // Inline helper: copy rowBuf+sep into batchBuf, flush to TCP when near
  // full. The separator semantics (no comma before first row) live here.
  auto pushToBatch = [&](const char* data, size_t len) {
    if (batchLen + len + 1 >= kBatchBufSize) {
      server->sendContent(batchBuf, batchLen);
      batchLen = 0;
      // Yield so lwIP can push the batch and free its pbuf before we
      // pile more onto the queue.
      yield();
    }
    memcpy(batchBuf + batchLen, data, len);
    batchLen += len;
  };

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[", 1);
  // v18.9.9.128: hard wall-clock deadline so we abort cleanly instead of
  // hitting the browser's fetch timeout mid-flush. Chrome/Safari fetch
  // ~30 s tolerance -- picking 25 s leaves room for the final flush + CRLF
  // to complete after we bail. Truncated response is a valid (partial)
  // JSON array; the front-end shows partial file list rather than nothing.
  const uint32_t tServeStart = millis();
  constexpr uint32_t kServeDeadlineMs = 25000;

  // CrumBLE 4.4 post-bisect: build each row's JSON via snprintf into a stack
  // buffer instead of ArduinoJson. JsonDocument internally allocates a few
  // hundred bytes per row that compound into heap fragmentation across an
  // 80+ entry scan, dropping MaxAlloc under the streaming-safe floor mid-
  // request and triggering early truncation. snprintf-based building has
  // zero heap cost per row -- MaxAlloc only changes by the lwIP TCP buffer
  // allocations for the chunk send itself. Field test before this change
  // saw 18-67 rows depending on entry heap; after, all 87 stream cleanly.
  //
  // JSON escaping: only `"` and `\\` need escaping in JSON strings. We
  // escape them in the filename inline rather than via a helper.
  // CrumBLE 4.5.5: 2.5 KB -> 1.2 KB. Field log on 4.5.5 with 42 books and
  // a -54 dBm RSSI saw truncation at 37 rows (maxAlloc dropped from 17396
  // entry -> 7924 exit, 9.5K of accumulated fragmentation) and on a worse
  // attempt at 7 rows (16372 -> 3188). The drop is dominated by per-row
  // std::string allocs (cachePathForFilePath builds `dir + "/epub_" +
  // to_string(hash)` -- three heap allocs) plus Storage.exists()'s
  // transient SD scratch buffers. Even with those allocs eliminated below,
  // a 2.5 KB threshold cost ~5 rows on a worst-case scan -- the threshold
  // was protecting against a failure mode (lwIP pbuf exhaustion) that
  // hasn't recurred since the chunked-streaming refactor. 1.2 KB leaves
  // ~700 B of pbuf headroom over the smallest reasonable TCP send window
  // and gets the full 42 rows through every time in field test.
  // v18.9.9.121: lowered 1200 -> 600. Field log v120: 28/42 rows emitted
  // before maxAlloc hit 1012 (just below the 1200 floor). Typical row JSON
  // is ~250-400 bytes; a 600 B floor still leaves headroom for one more
  // row's alloc while capturing more of the tail. Combined with the
  // adaptive drain-wait below, this should stream all 42 rows on the
  // v55-quality post-restart heap.
  constexpr uint32_t kAbortIfMaxAllocBelow = 600;
  // CrumBLE 4.5.5: secondary-wave emission for hidden entries (showHidden
  // Files=true). SD directory iteration returns entries in creation order
  // -- .system / .fonts / .crosspoint were created at first boot, so they
  // sit at the FRONT of the iteration even though user-uploaded books are
  // what people actually want to see. If the row-streamer's low-heap
  // bailout trips early, those hidden internals were the only rows that
  // made it through, and books got truncated off. Defer hidden rows to a
  // second pass so books always emit first; hidden entries follow if
  // heap still permits. Buffer size is trivial -- a typical SD root has
  // <10 dot-prefixed entries.
  std::vector<FileInfo> deferredHidden;
  // Extract the row-emit body into a lambda so both the main pass and
  // the secondary hidden-wave can call it without duplication.
  auto emitRow = [&](const FileInfo& info) {
    if (truncated) return;
    if (ESP.getMaxAllocHeap() < kAbortIfMaxAllocBelow) {
      truncated = true;
      return;
    }
    if (millis() - tServeStart > kServeDeadlineMs) {
      LOG_INF("WEB", "api-files: deadline hit (%u ms) emittedRows=%u -- truncating",
              (unsigned)(millis() - tServeStart), (unsigned)emittedRows);
      truncated = true;
      return;
    }
    yield();

    // Escape name into local stack buffer. Truncate on overflow rather than
    // bail; a too-long name yields a still-valid JSON row with a partial name.
    char escapedName[256];
    size_t ni = 0;
    const char* np = info.name.c_str();
    while (*np && ni + 2 < sizeof(escapedName)) {
      if (*np == '"' || *np == '\\') escapedName[ni++] = '\\';
      escapedName[ni++] = *np++;
    }
    escapedName[ni] = 0;

    bool prebaked = false;
    bool prebakedChap = false;
    bool prebakedCpFont = false;
    if (info.isEpub && !info.isDirectory) {
      // CrumBLE 4.5.5: stack-only path build. The prior version did
      // `std::string fullPath = currentPath; fullPath += '/'; fullPath +=
      // info.name;` and then called Epub::cachePathForFilePath() which
      // does `cacheDir + "/epub_" + std::to_string(hash)` -- 3-5 heap
      // allocations per EPUB row, freed at end-of-scope but each leaving
      // ~50-250 B of fragmentation that accumulated across a 42-row scan
      // and dragged maxAlloc below the bailout floor mid-stream. Build
      // fullPath + cacheDir into stack chars and call fnvHash64 directly.
      char fullPathBuf[512];
      const char* curPathC = currentPath.c_str();
      const size_t curLen = currentPath.length();
      const bool needSlash = curLen == 0 || curPathC[curLen - 1] != '/';
      const int fpWritten = snprintf(fullPathBuf, sizeof(fullPathBuf),
                                     needSlash ? "%s/%s" : "%s%s",
                                     curPathC, info.name.c_str());
      if (fpWritten > 0 && static_cast<size_t>(fpWritten) < sizeof(fullPathBuf)) {
        const uint64_t pathHash = ZipFile::fnvHash64(fullPathBuf, static_cast<size_t>(fpWritten));
        // Same wire format Epub::cachePathForFilePath emits: "<cacheDir>/epub_<hash>"
        char markerBuf[300];
        // Build a prefix once, then append the marker leaf for each check.
        const int prefixLen = snprintf(markerBuf, sizeof(markerBuf),
                                       "/.crosspoint/epub_%llu/",
                                       static_cast<unsigned long long>(pathHash));
        if (prefixLen > 0 && static_cast<size_t>(prefixLen) < sizeof(markerBuf)) {
          auto appendAndCheck = [&](const char* leaf) -> bool {
            const int leafLen = snprintf(markerBuf + prefixLen,
                                         sizeof(markerBuf) - prefixLen, "%s", leaf);
            if (leafLen <= 0 ||
                static_cast<size_t>(prefixLen + leafLen) >= sizeof(markerBuf)) {
              return false;
            }
            return Storage.exists(markerBuf);
          };
          prebaked = appendAndCheck("prebake-v2.marker");
          prebakedChap = appendAndCheck("prebake-chap.marker") ||
                         appendAndCheck("sections-prebake");
          prebakedCpFont = appendAndCheck("prebake-cpfont.marker");
        }
      }
    }

    // CrumBLE 4.5.4: tooltip generation removed from the per-row response.
    // formatPrebakeTooltip opens manifest.json + parses via ArduinoJson per
    // book, costing 50-200ms per row on a heap-tight device.
    //
    // v18.9.9.375: skip false booleans. Emitting `"isDirectory":false,` etc.
    // on every row wastes ~80-90 chars per non-directory non-prebake row.
    // JS reads missing fields as undefined which is falsy, so the client
    // handles the abbreviated form transparently (all `file.isX` checks
    // already use truthy semantics). Cuts typical rows from ~170 chars to
    // ~85 chars -- roughly half the wire size, which halves the number of
    // TCP round-trips needed on weak WiFi where each ACK costs 1-2 seconds.
    int written;
    {
      // Build only the fields that are true. Always emit name + size.
      char extra[192];
      size_t ei = 0;
      auto append = [&](const char* s) {
        while (*s && ei + 1 < sizeof(extra)) extra[ei++] = *s++;
      };
      if (info.isDirectory) append(",\"isDirectory\":true");
      if (info.isEpub) append(",\"isEpub\":true");
      if (prebaked) append(",\"prebaked\":true");
      if (prebakedChap) append(",\"prebakedChap\":true");
      if (prebakedCpFont) append(",\"prebakedCpFont\":true");
      extra[ei] = 0;
      written = snprintf(
          rowBuf, sizeof(rowBuf),
          "{\"name\":\"%s\",\"size\":%lu%s}",
          escapedName, static_cast<unsigned long>(info.size), extra);
    }
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(rowBuf)) return;

    if (seenFirst) pushToBatch(",", 1);
    else seenFirst = true;
    pushToBatch(rowBuf, static_cast<size_t>(written));
    ++emittedRows;
  };

  // Main pass: emit visible entries inline, buffer hidden for the second
  // wave. info.name carries the original filename (.fonts, .system,
  // .crosspoint, etc.) so a leading-dot check classifies cleanly.
  scanFiles(currentPath.c_str(), [&](const FileInfo& info) {
    if (!info.name.isEmpty() && info.name[0] == '.') {
      // Hidden entry -- defer. scanFiles only emits these when
      // SETTINGS.showHiddenFiles is true, so reaching this branch means
      // the user opted in.
      deferredHidden.push_back(info);
      return;
    }
    emitRow(info);
  });
  // Secondary wave: emit deferred hidden entries (if any). Same maxAlloc
  // bailout applies -- under tight heap, hidden rows are the ones that get
  // truncated, never books.
  for (const auto& info : deferredHidden) {
    if (truncated) break;
    emitRow(info);
  }
  // Flush whatever's still in the batch, then close the JSON array and
  // terminate the chunked encoding. Both go through pushToBatch first so
  // we never emit a tiny standalone chunk for the trailing ']'.
  pushToBatch("]", 1);
  if (batchLen > 0) {
    server->sendContent(batchBuf, batchLen);
    batchLen = 0;
  }
  // Empty chunk terminates Transfer-Encoding: chunked.
  server->sendContent("", 0);

  if (truncated) {
    LOG_ERR("WEB", "api-files: truncated to %u rows for path=%s (heap-aware bailout)",
            (unsigned)emittedRows, currentPath.c_str());
  }
  LOG_INF("WEB", "api-files: done streaming %u rows path=%s (free=%u maxAlloc=%u)",
          (unsigned)emittedRows, currentPath.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
}

void CrossPointWebServer::handlePrebakeManifest() const {
  // CrumBLE 4.5.4: on-demand replacement for the per-row prebakeTooltip
  // field. Browser calls this when the user clicks the (i) icon next to
  // a CP.FONT badge in the file list. Returns the multi-line manifest
  // text as plain text (NOT JSON-escaped) -- browser shows it in a
  // popover with white-space: pre-wrap. Costs ~50-200ms (one SD read +
  // ArduinoJson parse) but only fires on user click, not per-row.
  if (guardLowHeapOrAutoRestart(server.get(), "api-prebake-manifest")) return;
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }
  const String pathArg = normalizeWebPath(server->arg("path"));
  if (isProtectedPath(pathArg)) {
    server->send(403, "text/plain", "");
    return;
  }
  const std::string cacheDir = Epub::cachePathForFilePath(pathArg.c_str(), "/.crosspoint");
  const std::string tooltip = formatPrebakeTooltip(cacheDir);
  if (tooltip.empty()) {
    server->send(404, "text/plain", "");
    return;
  }
  server->send(200, "text/plain", tooltip.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  applyClientSendTimeout(server.get());
  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      esp_task_wdt_reset();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
#ifndef SIMULATOR
  client.clear();
#endif
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    esp_task_wdt_reset();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // v18.9.9.85: lazy-allocate the 4 KB upload buffer on first upload tick.
  // See UploadState comment for rationale.
  state.ensureBufferAllocated();

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  esp_task_wdt_reset();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    esp_task_wdt_reset();

    state.fileName = StringUtils::sanitizeFilename(upload.filename.c_str()).c_str();
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    if (isProtectedPath(filePath)) {
      state.error = "Access denied to protected path";
      LOG_DBG("WEB", "[UPLOAD] FAILED: Access denied to protected path: %s", filePath.c_str());
      return;
    }

    // Check if file already exists - SD operations can be slow
    esp_task_wdt_reset();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      esp_task_wdt_reset();
      Storage.remove(filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    esp_task_wdt_reset();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearBookCache(filePath.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  // Validate folder name
  if (folderName.isEmpty() || folderName == "book") {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  if (isProtectedPath(folderPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Idempotent: already-exists is success, not an error. The browser's
  // optimize-then-upload flow issues mkdir for the book's cache dir +
  // sections-prebake subdir before pushing the 704 cache files; if the
  // device retained those dirs from a prior optimize run (the WS-DONE
  // clear-cache path now heap-skips, so re-uploads commonly do leave the
  // old cache dir in place), a 400 here halts the optimizer at 90%. POSIX
  // mkdir -p semantics: return 200 if the dir already exists.
  if (Storage.exists(folderPath.c_str())) {
    server->send(200, "text/plain", "Folder already exists (ok)");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }

  // Calculate new path to check if it's protected
  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (isProtectedPath(newPath)) {
    server->send(403, "text/plain", "Cannot rename to protected path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (isProtectedPath(destPath)) {
    server->send(403, "text/plain", "Cannot move into protected folder");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    // Security check: prevent deletion of protected items
    if (isProtectedPath(itemPath)) {
      failedItems += itemPath + " (protected path); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    FsFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      FsFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
      if (!success) {
        LOG_ERR("WEB", "rmdir failed for %s (heap free=%u maxAlloc=%u)",
                itemPath.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      // v18.9.9.401: retry Storage.remove twice with a small delay. On the
      // aging SD card we've been debugging, transient FAT-level hiccups
      // caused Storage.remove to return false on the first try even though
      // the file was fine (subsequent listing showed the file still there).
      // A short pause + retry gets through those intermittent failures
      // without any user-visible retry step.
      success = Storage.remove(itemPath.c_str());
      if (!success) {
        LOG_ERR("WEB", "Storage.remove(%s) failed on first try; retrying (heap free=%u maxAlloc=%u)",
                itemPath.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        delay(100);
        success = Storage.remove(itemPath.c_str());
        if (!success) {
          delay(250);
          success = Storage.remove(itemPath.c_str());
        }
        if (!success) {
          LOG_ERR("WEB", "Storage.remove(%s) failed after 3 tries -- SD card likely damaged",
                  itemPath.c_str());
        }
      }
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  // v18.9.9.385: ETag-validated HTML serve; see handleFileList.
  if (applyAssetCacheOrServe304(server.get(), "settings-page", sizeof(SettingsPageHtml))) return;
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::primeSettingsCache() {
  // CrumBLE: build /api/settings JSON when called from a healthy-heap
  // context (FT activity onEnter at ~106 KB). This is the ONLY place
  // we allocate the ~8 KB body + the ~10 KB getSettingsList vector +
  // the JsonDocument internal pool growth. handleGetSettings then just
  // sends the cached string -- one server->send, no mid-request alloc.
  applyClientSendTimeout(server.get());  // harmless no-op here; helps caller paths
  const uint32_t preFree = ESP.getFreeHeap();
  const uint32_t preMax = ESP.getMaxAllocHeap();

  sdFontSystem.refreshIfDirty();
  const auto settings = getSettingsList(&sdFontSystem.registry());

  std::string body;
  body.reserve(10 * 1024);
  body += '[';

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;
  bool seenFirst = false;
  int iterCount = 0;
  for (const auto& s : settings) {
    if ((iterCount++ & 0x0F) == 0) esp_task_wdt_reset();
    if (!s.key) continue;

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);
    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(enumDisplayIndexForRawValue(s, SETTINGS.*(s.valuePtr)));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) options.add(opt);
        } else {
          for (const auto& opt : s.enumValues) options.add(I18N.get(opt));
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;
    if (seenFirst) body += ',';
    else seenFirst = true;
    body.append(output, written);
  }
  body += ']';

  cachedSettingsJson_.swap(body);
  LOG_INF("WEB", "primeSettingsCache: built %u B (pre free=%u maxAlloc=%u; post free=%u maxAlloc=%u)",
          (unsigned)cachedSettingsJson_.size(), preFree, preMax, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void CrossPointWebServer::handleGetSettings() const {
  // v18.9.9.326: re-enabled after being 503-stubbed since 4.0. Prior attempts
  // (chunked streaming, build-to-buffer, two-pass Content-Length, cache-at-
  // FT-entry) failed in the CrumBLE-era FT which is heavier than upstream
  // (WebSocket uploads, prebake, WASM optimizer, etc.). This port matches
  // CrossInk / CrossPoint's working pattern verbatim: build the settings
  // list once, stream one row at a time through a 512-byte stack buffer
  // via server->sendContent. Peak dynamic memory = the settings vector
  // (~5-10 KB) + one row's JsonDocument (~1 KB). Fits inside the 15-25 KB
  // FT free-heap envelope with headroom, and never holds the full JSON
  // payload in RAM.
  applyClientSendTimeout(server.get());
  const auto& settings = getSettingsList(&sdFontSystem.registry());

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;  // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(enumDisplayIndexForRawValue(s, SETTINGS.*(s.valuePtr)));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}


void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = enumRawValueForDisplayIndex(s, static_cast<uint8_t>(val));
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  // CrumBLE: rebuild the /api/settings cache so the next GET reflects
  // the change. Done here while the POST handler context is still live
  // -- the heap is at whatever level the POST request found it at, but
  // building a cache on that same heap budget is no worse than the old
  // path that built the JSON on every GET.
  primeSettingsCache();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// ---- Reader render-info (for optimizer .pxc baking) ----

void CrossPointWebServer::handleReaderRenderInfo() const {
  // CrumBLE 4.5.5: route through the low-heap guard. The handler builds a
  // ~1-2 KB JsonDocument; field log showed the WASM prefetch shredding
  // heap to maxAlloc=1.6 KB by the time the upload pipeline called
  // /api/reader-render-info, and the JsonDocument alloc silently failed
  // -> empty 200 body -> browser saw ERR_EMPTY_RESPONSE and aborted the
  // entire upload. Routing through the guard means heap pressure now
  // surfaces as a 503+Retry-After (handled by the browser's retry loop)
  // instead of a corrupt response that the upload can't recover from.
  if (guardLowHeapOrAutoRestart(server.get(), "api-reader-render-info")) return;
  if (!renderer_) {
    server->send(503, "application/json", "{\"error\":\"renderer unavailable\"}");
    return;
  }
  GfxRenderer& r = *renderer_;

  // Compute the reader's viewport exactly as EpubReaderActivity::render() does, by
  // briefly switching the renderer to the reader's orientation -- a pure setter
  // (no buffer realloc, no panel push), restored before returning -- and reusing
  // the same instance methods. This guarantees the optimizer's fitted-image
  // dimensions match the device's, so baked .pxc files pass the on-device
  // dimension check. The handler runs synchronously on the main task, so nothing
  // else renders while the orientation is temporarily changed.
  const GfxRenderer::Orientation savedOrientation = r.getOrientation();

  GfxRenderer::Orientation readerOrientation = GfxRenderer::Orientation::Portrait;
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      readerOrientation = GfxRenderer::Orientation::Portrait;
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      readerOrientation = GfxRenderer::Orientation::LandscapeClockwise;
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      readerOrientation = GfxRenderer::Orientation::PortraitInverted;
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      readerOrientation = GfxRenderer::Orientation::LandscapeCounterClockwise;
      break;
    default:
      readerOrientation = GfxRenderer::Orientation::Portrait;
      break;
  }
  r.setOrientation(readerOrientation);

  int marginTop, marginRight, marginBottom, marginLeft;
  r.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;
  // Match render()'s non-auto-page-turn branch (the bake targets normal reading).
  marginBottom += std::max<int>(SETTINGS.screenMargin, statusBarHeight + STATUS_BAR_TEXT_PADDING);

  // CrumBLE 4.2: ensure the SD-card font (if any) is loaded BEFORE asking
  // SETTINGS for the fontId. Without this, sdFontIdResolver returns 0
  // because the manager has no loaded font, getReaderFontId silently falls
  // back to the built-in BITTER, the bake stores the BITTER fontId in the
  // section file -- and then at reader-open time the device's
  // ensureLoaded() finally runs, returns the SD font's real hash, and
  // every section fails the fingerprint check ("indexing between pages").
  const_cast<SdCardFontSystem&>(sdFontSystem).ensureLoaded(r);
  const int fontId = SETTINGS.getReaderFontId();
  // CrumBLE 4.2 SD-font diagnostic: dump key glyph metrics for the
  // currently-loaded SD font so they can be diffed against the WASM-side
  // log of the same family (see tools/crumble-prebake/src/main.cpp,
  // SDFONT_DIAG). The two logs should print identical numbers; any
  // divergence is the source of jumbled-layout-but-fingerprint-passes.
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    auto it = r.getFontMap().find(fontId);
    if (it != r.getFontMap().end()) {
      const EpdFontData* d = it->second.getData(EpdFontFamily::REGULAR);
      if (d) {
        LOG_INF("SDFONT_DIAG", "DEV regular: advanceY=%u ascender=%d descender=%d intervalCount=%u groupCount=%u",
                static_cast<unsigned>(d->advanceY), d->ascender, d->descender,
                static_cast<unsigned>(d->intervalCount), static_cast<unsigned>(d->groupCount));
        for (uint32_t cp : {static_cast<uint32_t>(0x41), static_cast<uint32_t>(0x61), static_cast<uint32_t>(0x20)}) {
          const EpdGlyph* g = it->second.getGlyph(cp, EpdFontFamily::REGULAR);
          if (g) {
            LOG_INF("SDFONT_DIAG", "DEV U+%04X: w=%u h=%u advX=%u left=%d top=%d dataLen=%u",
                    cp, g->width, g->height, g->advanceX, g->left, g->top,
                    static_cast<unsigned>(g->dataLength));
          } else {
            LOG_INF("SDFONT_DIAG", "DEV U+%04X: NOT FOUND", cp);
          }
        }
      }
    }
  }
  const int screenW = r.getScreenWidth();
  const int screenH = r.getScreenHeight();
  const int viewportWidth = screenW - marginLeft - marginRight;
  const int viewportHeight = screenH - marginTop - marginBottom;
  const float emSize = r.getFontAscenderSize(fontId);

  r.setOrientation(savedOrientation);  // restore; no panel push happened in between

  JsonDocument doc;
  // fitVersion 2: adds the 8 section-header layout fields (line compression,
  // paragraph spacing/indents/alignment, hyphenation, embedded style, bionic
  // reading, guide reading) consumed by the crumble-prebake CLI when
  // generating sections/*.bin. The CLI hard-errors on fitVersion < 2 because
  // those values are baked into the section file fingerprint -- skipping
  // them would mean every prebake'd section gets rejected on first device
  // open. v1 fields are unchanged so existing .pxc-only flows still work.
  doc["fitVersion"] = 2;
  // CrumBLE 4.2 fix: was hardcoded "X4" -- the WASM optimizer + crumble-
  // prebake CLI both branch on this string for device-specific layout
  // metrics, so reporting "X4" on an actual X3 caused every prebake'd
  // section to be baked with X4 positions. Symptom on X3: all text on
  // every page rendered at y=0 (stacked at top), images positioned
  // correctly. Mirrors the same detection used by /api/* elsewhere in
  // this file (line ~573).
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
  doc["orientation"] = static_cast<int>(SETTINGS.orientation);
  doc["screenMargin"] = static_cast<int>(SETTINGS.screenMargin);
  doc["imageRendering"] = static_cast<int>(SETTINGS.imageRendering);
  doc["fontId"] = fontId;
  // CrumBLE: raw font selection fields. Manifest carries these so the device
  // can show human-readable font names in the .pxc mismatch prompt without
  // having to reverse-engineer fontId -> (family, size). For SD fonts, the
  // family name string is the user-visible label and sdFontSizeRange is the
  // S/M/L choice; for built-in fonts, fontFamily is the enum index and
  // fontSize is the point size.
  doc["fontFamily"] = static_cast<int>(SETTINGS.fontFamily);
  doc["fontSize"] = static_cast<int>(SETTINGS.fontSize);
  doc["sdFontSizeRange"] = static_cast<int>(SETTINGS.sdFontSizeRange);
  doc["sdFontFamilyName"] = SETTINGS.sdFontFamilyName;
  // CrumBLE 4.5.4: the actual currently-loaded SD font point size. Optimizer
  // uses this to pin the prebake to the device's exact size instead of
  // guessing from fontSize index against the family.sizes array (which
  // produced a font fingerprint mismatch the user had to resolve manually
  // when fontSize index didn't align with the device's resolved pt). 0
  // when no SD font is active; optimizer falls back to its legacy guess.
  doc["sdFontPickedPointSize"] = static_cast<int>(sdFontSystem.currentPrimaryPointSize());
  doc["screenWidth"] = screenW;
  doc["screenHeight"] = screenH;
  doc["viewportWidth"] = viewportWidth;
  doc["viewportHeight"] = viewportHeight;
  doc["emSize"] = emSize;
  // fitVersion 2 additions: the eight layout settings baked into the
  // section file header by Section::writeSectionFileHeader. The crumble-
  // prebake CLI passes these into Section::createSectionFile so the
  // section header it writes matches what the device will fingerprint
  // against on first open. Any drift here invalidates the prebake'd
  // section cache and the device falls back to a fresh build.
  doc["lineCompression"] = SETTINGS.getReaderLineCompression();
  // Raw lineSpacing enum (NORMAL / TIGHT / WIDE) alongside the derived
  // lineCompression float. The prebake manifest carries lineSpacing so
  // the device-side switch-back prompt can reverse-apply (lineCompression
  // is a one-way derivation we can't cleanly invert without the enum).
  doc["lineSpacing"] = static_cast<int>(SETTINGS.lineSpacing);
  doc["extraParagraphSpacing"] = static_cast<int>(SETTINGS.extraParagraphSpacing);
  doc["forceParagraphIndents"] = static_cast<int>(SETTINGS.forceParagraphIndents);
  doc["paragraphAlignment"] = static_cast<int>(SETTINGS.paragraphAlignment);
  doc["hyphenationEnabled"] = static_cast<int>(SETTINGS.hyphenationEnabled);
  doc["embeddedStyle"] = static_cast<int>(SETTINGS.embeddedStyle);
  doc["bionicReadingEnabled"] = static_cast<int>(SETTINGS.bionicReadingEnabled);
  doc["guideReadingEnabled"] = static_cast<int>(SETTINGS.guideReadingEnabled);
  // CrumBLE 4.2: report the font sizes this firmware actually supports.
  // Slim builds OMIT Teensy/Itty-Bitty/Extra-Large/Huge from env:tiny,
  // so the optimizer's font-size dropdown was offering selections that
  // the device can't render (it would silently fall back to a different
  // size, breaking the prebake fingerprint). Each entry's "value" is the
  // SETTINGS.fontSize integer the device would store for that size --
  // matches the API contract of /api/save-reader-settings -- and
  // "pointSize" lets the JS render a readable "(NNpt)" label without
  // needing to duplicate the enum-to-pt mapping client-side.
  JsonArray availableSizes = doc["availableFontSizes"].to<JsonArray>();
  for (uint8_t i = 0; i < static_cast<uint8_t>(CrossPointSettings::FONT_SIZE_COUNT); i++) {
    const auto size = static_cast<CrossPointSettings::FONT_SIZE>(i);
    const uint8_t stored = CrossPointSettings::getStoredReaderFontSize(size);
    if (stored == 0xFF) continue;  // sentinel: not available in this build
    JsonObject entry = availableSizes.add<JsonObject>();
    entry["value"] = static_cast<int>(stored);
    entry["pointSize"] = static_cast<int>(CrossPointSettings::getReaderFontPointSize(size));
  }
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);

  // CrumBLE: the SD font was loaded above (ensureLoaded) just to read its
  // metadata for the prebake manifest. The optimizer modal calls this
  // endpoint multiple times during the upload flow, and each load leaves the
  // font resident -- fragmenting MaxAlloc by ~5 KB per call against the
  // already-tight FT heap. WebServerActivity::onEnter explicitly releases
  // the loaded font (line ~104) to give FT the most contiguous heap it can,
  // so leaving it loaded here defeats that. Release it now; the next
  // ReaderActivity::onEnter re-loads on demand. No-op if user is on built-in.
  const_cast<SdCardFontSystem&>(sdFontSystem).releaseLoadedFont(r);
}

void CrossPointWebServer::handleSaveReaderSettings() const {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("JSON parse failed: ") + err.c_str());
    return;
  }

  // CrumBLE 4.5.5: dryRun=true returns the would-be derived render-info
  // without persisting any change. Used by the optimizer preflight modal
  // so the user can pick a font / size for THIS bake without silently
  // mutating their device's reader settings. We snapshot every field
  // this handler can touch, apply, compute, then restore -- the renderer
  // is a singleton so we can't fork it; restore must be paired with
  // every successful apply. The set of snapshotted fields mirrors the
  // applyU8 calls below 1:1; if you add a new applyU8 here, add the
  // matching snap/restore lines too.
  const bool dryRun = doc["dryRun"].is<bool>() ? doc["dryRun"].as<bool>() : false;
  struct Snapshot {
    uint8_t orientation;
    uint8_t screenMargin;
    uint8_t imageRendering;
    uint8_t fontFamily;
    uint8_t fontSize;
    uint8_t sdFontSizeRange;
    uint8_t lineSpacing;
    uint8_t paragraphAlignment;
    uint8_t extraParagraphSpacing;
    uint8_t forceParagraphIndents;
    uint8_t hyphenationEnabled;
    uint8_t embeddedStyle;
    uint8_t bionicReadingEnabled;
    uint8_t guideReadingEnabled;
    uint8_t glyphAtlasEnabled;
    char sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName)];
  } snap;
  if (dryRun) {
    snap.orientation = SETTINGS.orientation;
    snap.screenMargin = SETTINGS.screenMargin;
    snap.imageRendering = SETTINGS.imageRendering;
    snap.fontFamily = SETTINGS.fontFamily;
    snap.fontSize = SETTINGS.fontSize;
    snap.sdFontSizeRange = SETTINGS.sdFontSizeRange;
    snap.lineSpacing = SETTINGS.lineSpacing;
    snap.paragraphAlignment = SETTINGS.paragraphAlignment;
    snap.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
    snap.forceParagraphIndents = SETTINGS.forceParagraphIndents;
    snap.hyphenationEnabled = SETTINGS.hyphenationEnabled;
    snap.embeddedStyle = SETTINGS.embeddedStyle;
    snap.bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
    snap.guideReadingEnabled = SETTINGS.guideReadingEnabled;
    snap.glyphAtlasEnabled = SETTINGS.glyphAtlasEnabled;
    memcpy(snap.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(snap.sdFontFamilyName));
  }

  // Helper to clamp + apply an integer field. Skips when the key is absent
  // so partial updates work -- the preflight modal only PUTs back fields
  // the user actually edited.
  auto applyU8 = [&doc](const char* key, uint8_t& field, uint8_t maxValue) {
    if (!doc[key].is<int>()) return;
    const int v = doc[key].as<int>();
    if (v < 0 || v > maxValue) return;
    field = static_cast<uint8_t>(v);
  };
  applyU8("orientation", SETTINGS.orientation, 3);
  applyU8("screenMargin", SETTINGS.screenMargin, 50);
  applyU8("imageRendering", SETTINGS.imageRendering, CrossPointSettings::IMAGE_RENDERING_COUNT - 1);
  applyU8("fontFamily", SETTINGS.fontFamily, CrossPointSettings::FONT_FAMILY_COUNT - 1);
  applyU8("fontSize", SETTINGS.fontSize, CrossPointSettings::FONT_SIZE_COUNT - 1);
  applyU8("sdFontSizeRange", SETTINGS.sdFontSizeRange, CrossPointSettings::SD_FONT_SIZE_RANGE_COUNT - 1);
  applyU8("lineSpacing", SETTINGS.lineSpacing, CrossPointSettings::LINE_COMPRESSION_COUNT - 1);
  applyU8("paragraphAlignment", SETTINGS.paragraphAlignment, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1);
  applyU8("extraParagraphSpacing", SETTINGS.extraParagraphSpacing, 1);
  applyU8("forceParagraphIndents", SETTINGS.forceParagraphIndents, 1);
  applyU8("hyphenationEnabled", SETTINGS.hyphenationEnabled, 1);
  applyU8("embeddedStyle", SETTINGS.embeddedStyle, 1);
  applyU8("bionicReadingEnabled", SETTINGS.bionicReadingEnabled, 1);
  applyU8("guideReadingEnabled", SETTINGS.guideReadingEnabled, 1);
  applyU8("glyphAtlasEnabled", SETTINGS.glyphAtlasEnabled, 1);
  if (doc["sdFontFamilyName"].is<const char*>()) {
    const char* name = doc["sdFontFamilyName"].as<const char*>();
    strncpy(SETTINGS.sdFontFamilyName, name ? name : "", sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }

  if (dryRun) {
    // Compute derived values from the dry-run-applied SETTINGS and serialise
    // a render-info payload (same shape as /api/reader-render-info, so the
    // optimizer modal can resolve(dryRunResponse) and feed it straight into
    // the bake without merging). We skip ensureLoaded(): loading a brand-new
    // SD font for every modal click would burn ~5 KB / FT-heap allocation
    // every time the user opens the picker. The optimizer fetches the
    // matching .cpfont via /api/fonts/file separately and the CLI computes
    // the SD fontId from the loaded font's contentHash, so the response's
    // fontId for SD fonts may be stale -- safe because the bake doesn't
    // consume it. For built-in fonts, getReaderFontId() returns the correct
    // value (it doesn't touch the SD path).
    if (!renderer_) {
      // Restore before bailing so a failed dry-run doesn't strand state.
      SETTINGS.orientation = snap.orientation;
      SETTINGS.screenMargin = snap.screenMargin;
      SETTINGS.imageRendering = snap.imageRendering;
      SETTINGS.fontFamily = snap.fontFamily;
      SETTINGS.fontSize = snap.fontSize;
      SETTINGS.sdFontSizeRange = snap.sdFontSizeRange;
      SETTINGS.lineSpacing = snap.lineSpacing;
      SETTINGS.paragraphAlignment = snap.paragraphAlignment;
      SETTINGS.extraParagraphSpacing = snap.extraParagraphSpacing;
      SETTINGS.forceParagraphIndents = snap.forceParagraphIndents;
      SETTINGS.hyphenationEnabled = snap.hyphenationEnabled;
      SETTINGS.embeddedStyle = snap.embeddedStyle;
      SETTINGS.bionicReadingEnabled = snap.bionicReadingEnabled;
      SETTINGS.guideReadingEnabled = snap.guideReadingEnabled;
      SETTINGS.glyphAtlasEnabled = snap.glyphAtlasEnabled;
      memcpy(SETTINGS.sdFontFamilyName, snap.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName));
      server->send(503, "application/json", "{\"error\":\"renderer unavailable\"}");
      return;
    }
    GfxRenderer& r = *renderer_;
    const GfxRenderer::Orientation savedOrientation = r.getOrientation();
    GfxRenderer::Orientation readerOrientation = GfxRenderer::Orientation::Portrait;
    switch (SETTINGS.orientation) {
      case CrossPointSettings::ORIENTATION::PORTRAIT:
        readerOrientation = GfxRenderer::Orientation::Portrait;
        break;
      case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
        readerOrientation = GfxRenderer::Orientation::LandscapeClockwise;
        break;
      case CrossPointSettings::ORIENTATION::INVERTED:
        readerOrientation = GfxRenderer::Orientation::PortraitInverted;
        break;
      case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
        readerOrientation = GfxRenderer::Orientation::LandscapeCounterClockwise;
        break;
      default:
        readerOrientation = GfxRenderer::Orientation::Portrait;
        break;
    }
    r.setOrientation(readerOrientation);
    int marginTop, marginRight, marginBottom, marginLeft;
    r.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
    marginTop += SETTINGS.screenMargin;
    marginLeft += SETTINGS.screenMargin;
    marginRight += SETTINGS.screenMargin;
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;
    marginBottom += std::max<int>(SETTINGS.screenMargin, statusBarHeight + STATUS_BAR_TEXT_PADDING);
    const int fontId = SETTINGS.getReaderFontId();
    const int screenW = r.getScreenWidth();
    const int screenH = r.getScreenHeight();
    const int viewportWidth = screenW - marginLeft - marginRight;
    const int viewportHeight = screenH - marginTop - marginBottom;
    const float emSize = r.getFontAscenderSize(fontId);
    r.setOrientation(savedOrientation);

    JsonDocument resp;
    resp["fitVersion"] = 2;
    resp["device"] = gpio.deviceIsX3() ? "X3" : "X4";
    resp["orientation"] = static_cast<int>(SETTINGS.orientation);
    resp["screenMargin"] = static_cast<int>(SETTINGS.screenMargin);
    resp["imageRendering"] = static_cast<int>(SETTINGS.imageRendering);
    resp["fontId"] = fontId;
    resp["fontFamily"] = static_cast<int>(SETTINGS.fontFamily);
    resp["fontSize"] = static_cast<int>(SETTINGS.fontSize);
    resp["sdFontSizeRange"] = static_cast<int>(SETTINGS.sdFontSizeRange);
    resp["sdFontFamilyName"] = SETTINGS.sdFontFamilyName;
    // Stale for SD font changes (we skipped ensureLoaded); modal overrides
    // from family.sizes when it picks an SD font. 0 for built-in.
    resp["sdFontPickedPointSize"] = static_cast<int>(sdFontSystem.currentPrimaryPointSize());
    resp["screenWidth"] = screenW;
    resp["screenHeight"] = screenH;
    resp["viewportWidth"] = viewportWidth;
    resp["viewportHeight"] = viewportHeight;
    resp["emSize"] = emSize;
    resp["lineCompression"] = SETTINGS.getReaderLineCompression();
    resp["lineSpacing"] = static_cast<int>(SETTINGS.lineSpacing);
    resp["extraParagraphSpacing"] = static_cast<int>(SETTINGS.extraParagraphSpacing);
    resp["forceParagraphIndents"] = static_cast<int>(SETTINGS.forceParagraphIndents);
    resp["paragraphAlignment"] = static_cast<int>(SETTINGS.paragraphAlignment);
    resp["hyphenationEnabled"] = static_cast<int>(SETTINGS.hyphenationEnabled);
    resp["embeddedStyle"] = static_cast<int>(SETTINGS.embeddedStyle);
    resp["bionicReadingEnabled"] = static_cast<int>(SETTINGS.bionicReadingEnabled);
    resp["guideReadingEnabled"] = static_cast<int>(SETTINGS.guideReadingEnabled);
    resp["dryRun"] = true;
    JsonArray availableSizes = resp["availableFontSizes"].to<JsonArray>();
    for (uint8_t i = 0; i < static_cast<uint8_t>(CrossPointSettings::FONT_SIZE_COUNT); i++) {
      const auto size = static_cast<CrossPointSettings::FONT_SIZE>(i);
      const uint8_t stored = CrossPointSettings::getStoredReaderFontSize(size);
      if (stored == 0xFF) continue;
      JsonObject entry = availableSizes.add<JsonObject>();
      entry["value"] = static_cast<int>(stored);
      entry["pointSize"] = static_cast<int>(CrossPointSettings::getReaderFontPointSize(size));
    }

    // Restore SETTINGS before sending the response. saveToFile() is NOT
    // called -- this is the whole point of dryRun.
    SETTINGS.orientation = snap.orientation;
    SETTINGS.screenMargin = snap.screenMargin;
    SETTINGS.imageRendering = snap.imageRendering;
    SETTINGS.fontFamily = snap.fontFamily;
    SETTINGS.fontSize = snap.fontSize;
    SETTINGS.sdFontSizeRange = snap.sdFontSizeRange;
    SETTINGS.lineSpacing = snap.lineSpacing;
    SETTINGS.paragraphAlignment = snap.paragraphAlignment;
    SETTINGS.extraParagraphSpacing = snap.extraParagraphSpacing;
    SETTINGS.forceParagraphIndents = snap.forceParagraphIndents;
    SETTINGS.hyphenationEnabled = snap.hyphenationEnabled;
    SETTINGS.embeddedStyle = snap.embeddedStyle;
    SETTINGS.bionicReadingEnabled = snap.bionicReadingEnabled;
    SETTINGS.guideReadingEnabled = snap.guideReadingEnabled;
    SETTINGS.glyphAtlasEnabled = snap.glyphAtlasEnabled;
    memcpy(SETTINGS.sdFontFamilyName, snap.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName));

    String json;
    serializeJson(resp, json);
    server->send(200, "application/json", json);
    return;
  }

  SETTINGS.saveToFile();
  LOG_INF("WEB", "[CFG] reader settings updated via /api/save-reader-settings");
  server->send(200, "application/json", "{\"ok\":true}");
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  if (guardLowHeapOrAutoRestart(server.get(), "api-opds")) return;
  const auto& servers = OPDS_STORE.getServers();

  // CrumBLE: build-then-send-once (see handleGetSettings for rationale).
  String body;
  body.reserve(1024);
  body += '[';

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) body += ',';
    body += output;
  }

  body += ']';
  server->send(200, "application/json", body);
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    // Preserve existing password if not explicitly provided
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  if (guardLowHeapOrAutoRestart(server.get(), "api-wifi")) return;
  const auto& credentials = WIFI_STORE.getCredentials();
  const std::string& lastConnectedSsid = WIFI_STORE.getLastConnectedSsid();

  // CrumBLE: build-then-send-once (see handleGetSettings for rationale).
  String body;
  body.reserve(1024);
  body += '[';

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !credentials[i].password.empty();
    doc["isLastConnected"] = credentials[i].ssid == lastConnectedSsid;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) body += ',';
    body += output;
  }

  body += ']';
  server->send(200, "application/json", body);
  LOG_DBG("WEB", "Served Wi-Fi credentials API (%zu network(s))", credentials.size());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    const auto& credentials = WIFI_STORE.getCredentials();
    if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credentials[static_cast<size_t>(idx)].ssid;
    if (!hasPasswordField) {
      password = credentials[static_cast<size_t>(idx)].password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  const std::string ssid = credentials[static_cast<size_t>(idx)].ssid;
  if (!WIFI_STORE.removeCredential(ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  // v18.9.9.394: any WS event = user is actively transacting; reset the
  // idle-restart clock so the periodic freshener doesn't fire mid-upload.
  // v18.9.9.398: also bump the WS-specific tracker so the FT-side WS-wedge
  // detector knows we're healthy.
  bumpFtWsActivity();
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // CrumBLE 4.5.5+: pack-mode disconnect. Unlike single-file uploads
      // (which preserve the partial file for RESUME), pack uploads have no
      // resume semantics today -- the browser re-sends the whole pack on
      // retry. We delete the in-progress file so a follow-up attempt
      // writes clean data instead of layering over the truncated previous
      // attempt.
      if (num == wsPackClientNum && wsPackInProgress) {
        LOG_INF("WS", "Client %u disconnected mid-pack at file %u/%u",
                num, (unsigned)wsPackFileIndex, (unsigned)wsPackFileCount);
        setFtUploadInProgress(false);
        resetPackState("disconnect");
        reclaimWsHeapReservePad("WS-PACK-disc");
        break;
      }
      // CrumBLE 4.4: on mid-upload disconnect, KEEP the partial file on SD
      // so the next START can resume from where we left off. abortWsUpload
      // would delete the file -- that's the right move for explicit errors
      // (overflow, write fail), but a disconnect is exactly the case where
      // resume should help. Close the handle and reset the in-progress
      // flags, but leave the bytes on disk.
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        // CrumBLE 4.5.4 post-disconnect heap reclaim. Field log over many
        // upload sessions showed pre-flight rejects piling up across
        // disconnect cycles (free=23K, maxAlloc=4K -> floor=7168 reject
        // -> silentRestart). The proximate cause is fragmentation that
        // builds up because the WS recv path holds transient buffers
        // (lwIP pbufs, SD sector cache, the open FsFile object) that
        // don't all release synchronously when the socket closes. We
        // snapshot heap before + after, force a flush+close of the
        // partial file (FATFS sector cache drops on close), and yield
        // long enough for lwIP's tcp_tmr to run its cleanup pass. Twin
        // delays bracket the close so we can see what each step buys.
        const uint32_t freeBefore = ESP.getFreeHeap();
        const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
        LOG_INF("WS",
                "Client %u disconnected mid-upload at %u/%u bytes; preserving for resume "
                "[heap pre-reclaim: free=%u maxAlloc=%u]",
                num, (unsigned)wsUploadReceived, (unsigned)wsUploadSize,
                (unsigned)freeBefore, (unsigned)maxAllocBefore);
        // Force a sync before close so FATFS commits any dirty sectors
        // to SD rather than carrying them in cache until the next write.
        // FsFile::close() syncs internally on most FAT backends but the
        // explicit flush makes the contract obvious and survives any
        // future backend swap.
        wsUploadFile.flush();
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;
        wsLastProgressSent = 0;
        wsLastFsyncAt = 0;
        // Yield twice with a tick gap so lwIP's tcp_tmr (runs every
        // 250 ms but cooperatively, on the calling task's quantum) gets
        // at least one full pass to walk its tcp_active_pcbs list and
        // release the closed connection's pbufs back to the pool. A
        // single delay(0) is often not enough on a busy heap.
        delay(20);
        delay(20);
        const uint32_t freeAfter = ESP.getFreeHeap();
        const uint32_t maxAllocAfter = ESP.getMaxAllocHeap();
        const int32_t freeDelta = (int32_t)freeAfter - (int32_t)freeBefore;
        const int32_t maxAllocDelta = (int32_t)maxAllocAfter - (int32_t)maxAllocBefore;
        LOG_INF("WS",
                "Heap reclaim done: free=%u (%+d) maxAlloc=%u (%+d)",
                (unsigned)freeAfter, (int)freeDelta,
                (unsigned)maxAllocAfter, (int)maxAllocDelta);
        // CrumBLE 4.5.5: try to re-grab the heap reserve pad after the
        // lwIP cleanup yield -- subsequent START (typically the browser's
        // resume request a few hundred ms later) then sees the pad
        // available and releases it again before its floor check.
        reclaimWsHeapReservePad("WS-disc");
      }
      break;

    case WStype_CONNECTED: {
      // v18.9.9.428: enforce post-heap-abort cooldown. If a previous upload
      // aborted due to the BIN heap floor, hold new connections off for
      // ~4 s so the WS lib + LWIP release closed-socket buffers and heap
      // has a chance to defrag before we accept another START.
      const uint32_t nowMs = millis();
      if (nowMs < g_wsHeapAbortCooldownUntilMs) {
        const uint32_t remainingMs = g_wsHeapAbortCooldownUntilMs - nowMs;
        LOG_INF("WS", "Client %u refused: %u ms remain in heap-recovery cooldown (free=%d maxAlloc=%d)",
                num, (unsigned)remainingMs, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        wsServer->sendTXT(num, "ERROR:Device recovering heap; retry in a few seconds");
        wsServer->disconnect(num);
        break;
      }
      LOG_INF("WS", "Client %u connected: free=%d maxAlloc=%d", num,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      LOG_INF("WS", "DIAG TEXT from client %u (len=%u): %s", num, (unsigned)length,
              msg.length() > 80 ? "<truncated>" : msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // CrumBLE 4.4: pre-upload heap pre-flight. The WS chunked-receive path
        // needs a contiguous ~4 KB MaxAlloc per BIN frame plus headroom for
        // SD writes. If we accept an upload on a fragmented heap (the prior
        // 2 books left it stitched together), the connection drops every few
        // frames and we crawl forward via resume cycles -- eventually giving
        // up at "n/m uploaded".
        //
        // CrumBLE 4.5.5: release the WS heap reserve pad RIGHT BEFORE the
        // floor check, not after. The pad's whole purpose is bumping
        // maxAlloc into floor territory on a post-disconnect / post-prebake
        // fragmented heap. Pre-this fix, START sequence was: check floor
        // -> reject if fragmented -> silentRestart -> waste 5 s rebooting.
        // Now: release pad (maxAlloc jumps ~32 KB) -> check floor (almost
        // always passes) -> accept. Pad-frees are idempotent; if it was
        // already nullptr (e.g. previous DONE couldn't re-allocate) we
        // just fall through and the floor check decides on its own.
        releaseWsHeapReservePad("WS-START");
        constexpr uint32_t WS_CHUNK_SIZE = 4096;
        constexpr uint32_t kPreUploadMaxAllocFloor = WS_CHUNK_SIZE + 3u * 1024u;  // ~7 KB
        const uint32_t startFree = ESP.getFreeHeap();
        const uint32_t startMax = ESP.getMaxAllocHeap();
        if (startMax < kPreUploadMaxAllocFloor) {
          LOG_ERR("WS",
                  "START rejected: pre-upload MaxAlloc=%u below floor=%u (free=%u); "
                  "sending close so browser reconnects + retries",
                  startMax, kPreUploadMaxAllocFloor, startFree);
          wsServer->sendTXT(num, "ERROR:Heap too fragmented, please retry");
          // CrumBLE 4.5.5: close the connection so the browser's onclose
          // handler fires + its reconnect logic re-issues START after the
          // backoff. Previously we sent ERROR + scheduled silentRestart,
          // but the close frame here is BOTH cheaper (no reboot) and
          // necessary -- without the close, browser saw ERROR and waited
          // forever for further messages while the device, having
          // rejected, ignored further BIN frames. Silent multi-minute
          // stalls were exactly this case.
          wsServer->disconnect(num);
          // Try to re-grab the pad now so the next attempt has it again.
          reclaimWsHeapReservePad("WS-reject");
          break;
        }
        LOG_INF("WS", "START heap pre-flight ok: free=%u maxAlloc=%u (floor=%u, pad released)",
                startFree, startMax, kPreUploadMaxAllocFloor);

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = StringUtils::sanitizeFilename(msg.substring(6, firstColon).c_str()).c_str();
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsLastFsyncAt = 0;
          wsUploadStartTime = millis();

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          if (isProtectedPath(filePath)) {
            wsServer->sendTXT(num, "ERROR:Access denied to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          LOG_INF("WS", "DIAG Starting upload: %s (%d bytes) to %s (free=%u maxAlloc=%u)",
                  wsUploadFileName.c_str(), wsUploadSize, filePath.c_str(),
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

          // CrumBLE 4.4: resume support. If a partial file already exists at
          // the destination with size < the declared total, treat it as a
          // continuation of a previous failed upload: open in non-truncating
          // mode (O_RDWR | O_CREAT, no O_TRUNC), seek to the end, send
          // RESUME:<bytes> instead of READY, and seed wsUploadReceived so
          // the BIN handler picks up from there. Match is just by path +
          // declared size; the browser is expected to slice the file from
          // <bytes> on receiving RESUME. If the file is the same size as
          // expected or larger, treat it as already-complete / dirty and
          // restart fresh.
          esp_task_wdt_reset();
          uint64_t resumeFrom = 0;
          if (Storage.exists(filePath.c_str())) {
            FsFile probe = Storage.open(filePath.c_str(), O_RDONLY);
            if (probe) {
              const uint64_t existingSize = probe.fileSize64();
              probe.close();
              if (existingSize > 0 && existingSize < wsUploadSize) {
                resumeFrom = existingSize;
              } else {
                // Same-or-larger: stale / complete leftover; truncate fresh.
                Storage.remove(filePath.c_str());
              }
            } else {
              Storage.remove(filePath.c_str());
            }
          }

          esp_task_wdt_reset();
          // v18.9.9.392: ensure parent dirs exist BEFORE opening the file for
          // write. Field bug: browser-side prebake push tried 704 files at
          // /.crosspoint/epub_<hash>/sections-prebake/*.bin; every openFileFor-
          // Write failed with "Failed to open file for writing" because
          // sections-prebake/ never got created. Loop retried for 47 minutes.
          if (!ensureParentDirExists(filePath)) {
            LOG_ERR("WS", "ensureParentDirExists FAILED for %s", filePath.c_str());
            wsConsecutiveOpenFailures++;
            if (wsConsecutiveOpenFailures >= kWsMaxConsecutiveOpenFailures) {
              wsServer->sendTXT(num, "FATAL:SD_WRITE_FAILING");
              LOG_ERR("WS", "FATAL: %u consecutive open failures -- SD card likely bad",
                      static_cast<unsigned>(wsConsecutiveOpenFailures));
            } else {
              wsServer->sendTXT(num, "ERROR:Failed to create parent dir");
            }
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          if (resumeFrom > 0) {
            // Resume path: open without O_TRUNC, position at end.
            wsUploadFile = Storage.open(filePath.c_str(), O_RDWR | O_CREAT);
            if (!wsUploadFile) {
              wsConsecutiveOpenFailures++;
              if (wsConsecutiveOpenFailures >= kWsMaxConsecutiveOpenFailures) {
                wsServer->sendTXT(num, "FATAL:SD_WRITE_FAILING");
                LOG_ERR("WS", "FATAL: %u consecutive open failures -- SD card likely bad",
                        static_cast<unsigned>(wsConsecutiveOpenFailures));
              } else {
                wsServer->sendTXT(num, "ERROR:Failed to reopen file for resume");
              }
              wsUploadInProgress = false;
              wsUploadClientNum = 255;
              return;
            }
            wsUploadFile.seekSet(static_cast<size_t>(resumeFrom));
            wsUploadReceived = static_cast<size_t>(resumeFrom);
            wsLastProgressSent = wsUploadReceived;
          } else {
            // Fresh upload: existing helper handles O_TRUNC + create.
            if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
              wsConsecutiveOpenFailures++;
              if (wsConsecutiveOpenFailures >= kWsMaxConsecutiveOpenFailures) {
                wsServer->sendTXT(num, "FATAL:SD_WRITE_FAILING");
                LOG_ERR("WS", "FATAL: %u consecutive open failures -- SD card likely bad",
                        static_cast<unsigned>(wsConsecutiveOpenFailures));
              } else {
                wsServer->sendTXT(num, "ERROR:Failed to create file");
              }
              wsUploadInProgress = false;
              wsUploadClientNum = 255;
              return;
            }
          }
          // Success -- reset the fatal counter.
          wsConsecutiveOpenFailures = 0;
          esp_task_wdt_reset();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCache(filePath.c_str());
            // CrumBLE 4.4: include sanitized device path -- see main DONE branch.
            String zdoneMsg = String("DONE:") + filePath;
            wsServer->sendTXT(num, zdoneMsg.c_str());
            wsLastProgressSent = 0;
            wsLastFsyncAt = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          // CrumBLE 4.5.4: arm panic-recovery flag. If the device hard-
          // crashes during the rest of this upload, setup() detects this
          // on the next boot and silent-restart-to-FT so the browser's
          // WS retry + RESUME picks up at the saved byte offset.
          setFtUploadInProgress(true);
          if (resumeFrom > 0) {
            char resumeMsg[48];
            snprintf(resumeMsg, sizeof(resumeMsg), "RESUME:%lu", static_cast<unsigned long>(resumeFrom));
            wsServer->sendTXT(num, resumeMsg);
            LOG_INF("WS", "DIAG RESUME sent to client %u from offset %lu (free=%u maxAlloc=%u)",
                    num, static_cast<unsigned long>(resumeFrom),
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          } else {
            wsServer->sendTXT(num, "READY");
            LOG_INF("WS", "DIAG READY sent to client %u (free=%u maxAlloc=%u)",
                    num, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          }
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      } else if (msg.startsWith("PACK_START:")) {
        // CrumBLE 4.5.5+: pack-upload mode. Format: "PACK_START:<totalBytes>"
        // where totalBytes is the COMPLETE size of the incoming binary stream
        // (TOC header + concatenated file payloads). Used by the optimizer
        // chapter-prebake step to ship hundreds of small cache files as one
        // contiguous WS upload, removing the per-file open/close + protocol
        // framing overhead that fragments heap mid-batch even on a persistent
        // WS.
        if (wsUploadInProgress || wsPackInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }
        // Pre-flight check, same shape as single-file START. The pack writes
        // to one file at a time, so 4 KB per BIN frame + ~3 KB headroom is
        // the same minimum it needs.
        releaseWsHeapReservePad("WS-PACK-START");
        constexpr uint32_t kPreUploadMaxAllocFloor = 7u * 1024u;
        const uint32_t packStartFree = ESP.getFreeHeap();
        const uint32_t packStartMax = ESP.getMaxAllocHeap();
        if (packStartMax < kPreUploadMaxAllocFloor) {
          LOG_ERR("WS", "PACK_START rejected: MaxAlloc=%u below floor=%u (free=%u)",
                  packStartMax, kPreUploadMaxAllocFloor, packStartFree);
          wsServer->sendTXT(num, "ERROR:Heap too fragmented, please retry");
          wsServer->disconnect(num);
          reclaimWsHeapReservePad("WS-PACK-reject");
          break;
        }
        const String sizeToken = msg.substring(11);
        bool sizeValid = sizeToken.length() > 0;
        for (uint16_t i = 0; i < sizeToken.length() && sizeValid; ++i) {
          if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
        }
        if (!sizeValid) {
          wsServer->sendTXT(num, "ERROR:Invalid PACK_START format");
          break;
        }
        const uint64_t totalBytes = strtoull(sizeToken.c_str(), nullptr, 10);
        // Reasonable upper bound on a single pack. The whole point of pack
        // mode is "hundreds of small cache files"; if the caller wants to
        // push >32 MB of data it should split into multiple packs.
        constexpr uint64_t kPackMaxTotalBytes = 32ULL * 1024ULL * 1024ULL;
        if (totalBytes == 0 || totalBytes > kPackMaxTotalBytes) {
          wsServer->sendTXT(num, "ERROR:Pack size out of range");
          break;
        }
        // Reset to clean state (idempotent) and enter MAGIC mode.
        resetPackState(nullptr);
        wsPackInProgress = true;
        wsPackState = PACK_STATE_MAGIC;
        wsPackClientNum = num;
        wsPackTotalBytesExpected = totalBytes;
        wsPackTotalBytesReceived = 0;
        wsPackLastProgressSent = 0;
        wsPackStartTime = millis();
        wsPackHeaderBytesNeeded = 8;  // magic is 8 bytes
        wsPackHeaderBytesHave = 0;
        // Arm the panic-recovery flag the same way single-file uploads do;
        // a crash during a pack lets the next boot resume back into FT.
        setFtUploadInProgress(true);
        wsServer->sendTXT(num, "PACK_READY");
        LOG_INF("WS", "PACK_START accepted: totalBytes=%llu free=%u maxAlloc=%u",
                (unsigned long long)totalBytes, packStartFree, packStartMax);
      } else if (msg == "INSTALL_FIRMWARE") {
        // CrumBLE 4.6 LAN-OTA: frontend uploaded firmware-pending.bin via the
        // existing book-upload pipeline; now asks us to flash it. We only
        // queue the request here -- the actual flashFromSdPath is blocking
        // (~60s) and would block the WS event loop, so the FT activity's
        // loop() picks up the consumeFirmwareInstallRequest() and pushes
        // the install-progress activity which does the actual flash + restart.
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "INSTALL_ERROR:Upload still in progress");
          break;
        }
        if (!Storage.exists(kFirmwarePendingPath)) {
          wsServer->sendTXT(num, "INSTALL_ERROR:firmware-pending.bin not found");
          break;
        }
        FsFile probe = Storage.open(kFirmwarePendingPath, O_RDONLY);
        if (!probe) {
          wsServer->sendTXT(num, "INSTALL_ERROR:Cannot open firmware-pending.bin");
          break;
        }
        const uint64_t pendingSize = probe.fileSize64();
        probe.close();
        if (pendingSize < 65536) {  // any real firmware is much bigger; reject obvious garbage early
          wsServer->sendTXT(num, "INSTALL_ERROR:firmware-pending.bin too small");
          break;
        }
        LOG_INF("WS", "INSTALL_FIRMWARE queued: %s (%llu bytes)", kFirmwarePendingPath,
                static_cast<unsigned long long>(pendingSize));
        g_pendingFirmwareInstall = true;
        wsServer->sendTXT(num, "INSTALL_QUEUED");
      }
      break;
    }

    case WStype_BIN: {
      // CrumBLE 4.5.5+: pack-mode dispatch. When pack upload is active for
      // this client, route the frame through the byte-streaming TOC parser.
      // The state machine consumes the frame end-to-end -- a single 4 KB
      // frame can span any combination of header fields and file data.
      if (wsPackInProgress && num == wsPackClientNum) {
        size_t consumed = 0;
        bool fail = false;
        const char* failReason = nullptr;
        while (consumed < length && !fail) {
          switch (wsPackState) {
            case PACK_STATE_MAGIC: {
              const size_t want = 8 - wsPackHeaderBytesHave;
              const size_t take = std::min(want, length - consumed);
              memcpy(wsPackHeaderBuf + wsPackHeaderBytesHave, payload + consumed, take);
              wsPackHeaderBytesHave += take;
              consumed += take;
              wsPackTotalBytesReceived += take;
              if (wsPackHeaderBytesHave == 8) {
                if (memcmp(wsPackHeaderBuf, "CMBPACK1", 8) != 0) {
                  fail = true; failReason = "bad magic"; break;
                }
                wsPackState = PACK_STATE_COUNT;
                wsPackHeaderBytesHave = 0;
                wsPackHeaderBytesNeeded = 4;
              }
              break;
            }
            case PACK_STATE_COUNT: {
              const size_t want = 4 - wsPackHeaderBytesHave;
              const size_t take = std::min(want, length - consumed);
              memcpy(wsPackHeaderBuf + wsPackHeaderBytesHave, payload + consumed, take);
              wsPackHeaderBytesHave += take;
              consumed += take;
              wsPackTotalBytesReceived += take;
              if (wsPackHeaderBytesHave == 4) {
                wsPackFileCount = readU32LE(wsPackHeaderBuf);
                wsPackFileIndex = 0;
                if (wsPackFileCount == 0) {
                  // Empty pack -- terminate cleanly.
                  wsServer->sendTXT(num, "PACK_DONE");
                  setFtUploadInProgress(false);
                  resetPackState(nullptr);
                  reclaimWsHeapReservePad("WS-PACK-empty");
                  return;
                }
                if (wsPackFileCount > 5000) {
                  fail = true; failReason = "file count too high"; break;
                }
                wsPackState = PACK_STATE_PATH_LEN;
                wsPackHeaderBytesHave = 0;
                wsPackHeaderBytesNeeded = 4;
              }
              break;
            }
            case PACK_STATE_PATH_LEN: {
              const size_t want = 4 - wsPackHeaderBytesHave;
              const size_t take = std::min(want, length - consumed);
              memcpy(wsPackHeaderBuf + wsPackHeaderBytesHave, payload + consumed, take);
              wsPackHeaderBytesHave += take;
              consumed += take;
              wsPackTotalBytesReceived += take;
              if (wsPackHeaderBytesHave == 4) {
                wsPackCurrentPathLen = readU32LE(wsPackHeaderBuf);
                if (wsPackCurrentPathLen == 0 || wsPackCurrentPathLen >= kPackMaxPathLen) {
                  fail = true; failReason = "path length out of range"; break;
                }
                wsPackState = PACK_STATE_PATH;
                wsPackHeaderBytesHave = 0;
                wsPackHeaderBytesNeeded = wsPackCurrentPathLen;
              }
              break;
            }
            case PACK_STATE_PATH: {
              const size_t want = wsPackCurrentPathLen - wsPackHeaderBytesHave;
              const size_t take = std::min(want, length - consumed);
              memcpy(wsPackHeaderBuf + wsPackHeaderBytesHave, payload + consumed, take);
              wsPackHeaderBytesHave += take;
              consumed += take;
              wsPackTotalBytesReceived += take;
              if (wsPackHeaderBytesHave == wsPackCurrentPathLen) {
                wsPackHeaderBuf[wsPackCurrentPathLen] = '\0';
                // Hard-reject paths that escape the FT-allowed root. Anything
                // that doesn't start with '/' is rejected; "..", relative
                // segments, and double-slashes get normalized away by
                // FsHelpers::normalisePath below.
                if (wsPackHeaderBuf[0] != '/') {
                  fail = true; failReason = "path must be absolute"; break;
                }
                const std::string normalized =
                    FsHelpers::normalisePath(reinterpret_cast<const char*>(wsPackHeaderBuf));
                wsPackCurrentPath = normalized.c_str();
                wsPackState = PACK_STATE_FILE_SIZE;
                wsPackHeaderBytesHave = 0;
                wsPackHeaderBytesNeeded = 4;
              }
              break;
            }
            case PACK_STATE_FILE_SIZE: {
              const size_t want = 4 - wsPackHeaderBytesHave;
              const size_t take = std::min(want, length - consumed);
              memcpy(wsPackHeaderBuf + wsPackHeaderBytesHave, payload + consumed, take);
              wsPackHeaderBytesHave += take;
              consumed += take;
              wsPackTotalBytesReceived += take;
              if (wsPackHeaderBytesHave == 4) {
                wsPackCurrentFileSize = readU32LE(wsPackHeaderBuf);
                wsPackCurrentFileWritten = 0;
                if (wsPackFileIndex + 1 == wsPackFileCount) {
                  // Last file -- after this, expect no more TOC entries.
                } else if (wsPackFileIndex >= wsPackFileCount) {
                  fail = true; failReason = "TOC overrun"; break;
                }
                wsPackState = PACK_STATE_FILE_DATA;
                wsPackHeaderBytesHave = 0;
                if (!ensureParentDirExists(wsPackCurrentPath)) {
                  fail = true; failReason = "parent dir create failed"; break;
                }
                // Zero-byte file -- open + close immediately, then advance.
                if (wsPackCurrentFileSize == 0) {
                  if (!Storage.openFileForWrite("WS-PACK", wsPackCurrentPath, wsPackCurrentFile)) {
                    fail = true; failReason = "file open failed (0-byte)"; break;
                  }
                  wsPackCurrentFile.close();
                  wsPackFileIndex++;
                  if (wsPackFileIndex >= wsPackFileCount) {
                    wsServer->sendTXT(num, "PACK_DONE");
                    setFtUploadInProgress(false);
                    LOG_INF("WS", "PACK_DONE: %u files in %lu ms (%llu bytes total)",
                            (unsigned)wsPackFileCount,
                            millis() - wsPackStartTime,
                            (unsigned long long)wsPackTotalBytesReceived);
                    resetPackState(nullptr);
                    reclaimWsHeapReservePad("WS-PACK-DONE");
                    return;
                  }
                  // More files ahead -- back to TOC parsing.
                  wsPackState = PACK_STATE_PATH_LEN;
                  wsPackHeaderBytesNeeded = 4;
                  break;
                }
                if (!Storage.openFileForWrite("WS-PACK", wsPackCurrentPath, wsPackCurrentFile)) {
                  fail = true; failReason = "file open failed"; break;
                }
              }
              break;
            }
            case PACK_STATE_FILE_DATA: {
              const size_t remaining = wsPackCurrentFileSize - wsPackCurrentFileWritten;
              const size_t take = std::min(remaining, length - consumed);
              esp_task_wdt_reset();
              const size_t written = wsPackCurrentFile.write(payload + consumed, take);
              esp_task_wdt_reset();
              if (written != take) {
                fail = true; failReason = "SD write short"; break;
              }
              wsPackCurrentFileWritten += take;
              consumed += take;
              wsPackTotalBytesReceived += take;
              if (wsPackCurrentFileWritten == wsPackCurrentFileSize) {
                wsPackCurrentFile.flush();
                wsPackCurrentFile.close();
                wsPackFileIndex++;
                if (wsPackFileIndex >= wsPackFileCount) {
                  // All files done. Expect no more bytes; whatever's left in
                  // this frame is a protocol violation but we treat it as a
                  // terminating "done" since we have what we need.
                  wsServer->sendTXT(num, "PACK_DONE");
                  setFtUploadInProgress(false);
                  LOG_INF("WS", "PACK_DONE: %u files in %lu ms (%llu bytes total)",
                          (unsigned)wsPackFileCount,
                          millis() - wsPackStartTime,
                          (unsigned long long)wsPackTotalBytesReceived);
                  resetPackState(nullptr);
                  reclaimWsHeapReservePad("WS-PACK-DONE");
                  return;
                }
                wsPackState = PACK_STATE_PATH_LEN;
                wsPackHeaderBytesHave = 0;
                wsPackHeaderBytesNeeded = 4;
              }
              break;
            }
            case PACK_STATE_IDLE:
            default:
              fail = true; failReason = "pack state invalid"; break;
          }
        }
        if (fail) {
          LOG_ERR("WS", "Pack error: %s (file %u/%u at byte %llu/%llu)",
                  failReason ? failReason : "unknown",
                  (unsigned)wsPackFileIndex, (unsigned)wsPackFileCount,
                  (unsigned long long)wsPackTotalBytesReceived,
                  (unsigned long long)wsPackTotalBytesExpected);
          String errMsg = String("PACK_ERROR:") + (failReason ? failReason : "stream error");
          wsServer->sendTXT(num, errMsg.c_str());
          setFtUploadInProgress(false);
          resetPackState("error");
          wsServer->disconnect(num);
          reclaimWsHeapReservePad("WS-PACK-error");
        } else {
          // Optional periodic progress, cheap to send and helps a stalled
          // browser detect a working device. Same 256 KB cadence as single-
          // file uploads.
          if (wsPackTotalBytesReceived - wsPackLastProgressSent >= 262144) {
            String progress = "PACK_PROGRESS:" + String((uint32_t)wsPackTotalBytesReceived) +
                              ":" + String((uint32_t)wsPackTotalBytesExpected);
            wsServer->sendTXT(num, progress);
            wsPackLastProgressSent = wsPackTotalBytesReceived;
          }
        }
        return;
      }

      // DIAG: first BIN frame is the most informative for instant-stall debugging
      // -- if we never see this log, the browser isn't sending or the WS lib is
      // rejecting frames before our handler runs. Also dump every 16th frame
      // (~64 KB cadence, matches progress) so we can see processing speed.
      if (wsUploadReceived == 0) {
        LOG_INF("WS", "DIAG first BIN frame: len=%u inProgress=%d file=%d clientNum=%u expectedClient=%u free=%u maxAlloc=%u",
                (unsigned)length, wsUploadInProgress ? 1 : 0,
                wsUploadFile ? 1 : 0, num, wsUploadClientNum,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      } else {
        static uint16_t binFrameCount = 0;
        binFrameCount++;
        // v18.9.9.421: log every 128 frames (was 16). Serial output at 115200
        // baud costs ~1-2 ms per line -- at every-16 that was ~3-5 s of raw
        // log I/O on a 10 MB upload, subtracting from real throughput. Every
        // 128 frames still yields ~20 diag lines per 10 MB upload (one every
        // 512 KB), enough to eyeball stall points without stealing wire time.
        if ((binFrameCount & 0x7F) == 0) {  // every 128 frames
          LOG_INF("WS", "DIAG BIN frame %u: len=%u received=%u/%u free=%u maxAlloc=%u",
                  (unsigned)binFrameCount, (unsigned)length,
                  (unsigned)wsUploadReceived, (unsigned)wsUploadSize,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        }
      }

      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        LOG_ERR("WS", "DIAG BIN rejected: inProgress=%d file=%d clientNum=%u expected=%u",
                wsUploadInProgress ? 1 : 0, wsUploadFile ? 1 : 0, num, wsUploadClientNum);
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // v18.9.9.402: hard-floor heap check per frame. Field crash: an upload
      // that ground down to free=7640 maxAlloc=4084 mid-frame triggered
      // heap_caps_free's "target pointer outside heap areas" assert deep
      // inside the SD write path -- unrecoverable hard reboot. Panic-recovery
      // then hit a stale-fontId Load-access-fault, killing the resume state
      // entirely. Well before we get that low, close the socket cleanly with
      // an error. Browser resume protocol picks up the partial file (fsync'd
      // every 256 KB) on next START, and the WS-wedge / retry-streak paths
      // (v395/v398) drive a silent-restart if needed to defrag heap.
      // v18.9.9.421: bumped 6 KB -> 20 KB. Field crash on X3 fresh boot:
      // heap_caps_free target-outside-heap assert fired at free=19928
      // maxAlloc=17396 -- 3x above the old floor. The corruption path
      // (LWIP/WS lib allocator metadata overrun during high-frequency
      // pbuf turnover) manifests at much higher maxAlloc than the v402
      // measurement suggested. 20 KB is above the last observed pre-crash
      // maxAlloc (17.4 KB) with a safety margin. Trade-off: uploads pause
      // ~3-5 min earlier on fragmentation-heavy libraries, but the
      // browser resumes cleanly across the socket close instead of the
      // device hard-rebooting into panic recovery.
      // v18.9.9.422: cut 20 KB -> 17 KB. v421 was too aggressive: steady-
      // state maxAlloc immediately after a silent-restart-to-FT lands at
      // ~20 KB (WS server init consumes 5-10 KB of the fresh boot heap),
      // which is *right at* the v421 floor. Every upload attempt was
      // refused within 12 bytes of the threshold, and with the reserve
      // pad system also failing (v422 fixes that separately with a size
      // ladder), the browser was stuck in an infinite reject/retry loop.
      // 17 KB reflects the observed pre-crash maxAlloc (17.4 KB) as the
      // approximate corruption boundary -- floor just below means we
      // catch attempts that would enter the crash zone without blocking
      // clean fresh-boot starts.
      // v18.9.9.423: reverted 17 KB -> 7 KB. Field data from the pre-CJK-
      // re-add build (v4.5.114) uploading the same 10.9 MB file showed
      // uploads advancing 3-4 MB per attempt at floor=7168, then hitting
      // the SAME heap_caps_free crash class at ~free=4668 maxAlloc=884.
      // The 17/20 KB floors weren't preventing that crash; they were
      // preventing uploads from starting at all. Restore the working
      // floor so uploads happen + browser auto-resume grinds through,
      // and let the HeapCorruptionDetector wrap (v423) capture the crash
      // callstack for a real root-cause fix instead of gating around it.
      constexpr uint32_t kBinFrameHardFloorMaxAlloc = 7u * 1024u;
      const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
      if (maxAllocNow < kBinFrameHardFloorMaxAlloc) {
        LOG_ERR("WS",
                "BIN frame refused: maxAlloc=%u below hard floor %u (free=%u); closing socket for defrag",
                maxAllocNow, static_cast<unsigned>(kBinFrameHardFloorMaxAlloc), ESP.getFreeHeap());
        abortWsUpload("WS-heapfloor");
        // v18.9.9.428: arm cooldown so browser retry doesn't land right back
        // on the same fragmented heap. See g_wsHeapAbortCooldownUntilMs decl.
        g_wsHeapAbortCooldownUntilMs = millis() + 4000;
        wsServer->sendTXT(num, "ERROR:Device out of heap; retrying will resume");
        wsServer->disconnect(num);
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      esp_task_wdt_reset();
      size_t written = wsUploadFile.write(payload, length);
      esp_task_wdt_reset();

      if (written != length) {
        LOG_ERR("WS", "DIAG direct write failed: tried=%u wrote=%u received=%u/%u",
                (unsigned)length, (unsigned)written,
                (unsigned)(wsUploadReceived + written), (unsigned)wsUploadSize);
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // CrumBLE 4.5.5: periodic fsync. Commits FATFS dirty sectors + the
      // file's directory entry every 256 KB so a crash mid-upload leaves
      // a valid truncated file on SD. Without this, a 10 MB upload that
      // crashed at 5 MB would lose ALL 5 MB (FATFS held dirty sectors in
      // RAM cache; reboot threw them away) -- next attempt's RESUME query
      // got file-size 0 -> browser re-uploaded from offset 0 -> same crash
      // -> infinite restart loop. With fsync every 256 KB, the worst case
      // is losing the last <256 KB and the next attempt resumes from
      // ~the right place. Cost: each flush is a few ms (FATFS just writes
      // the dirty sectors that were going to flush eventually anyway).
      constexpr size_t kFsyncIntervalBytes = 256 * 1024;
      if (wsUploadReceived - wsLastFsyncAt >= kFsyncIntervalBytes) {
        wsUploadFile.flush();
        wsLastFsyncAt = wsUploadReceived;
      }

      // CrumBLE 4.5.4: PROGRESS interval bumped 64 KB -> 256 KB, plus
      // heap-aware skip. Field log: during sustained upload free heap
      // collapsed from 11 KB to 2.4 KB between two adjacent PROGRESS
      // sends, then the WS lib couldn't allocate the next incoming frame
      // and dropped the connection at 93%. Root cause: each PROGRESS
      // sendTXT enters lwIP's TCP send queue; if the browser is even
      // slightly slow to ACK, those queued bytes pile up and eat heap.
      // 4x fewer PROGRESS messages reduces the queue growth proportionally.
      // The heap-aware skip suppresses the send entirely when free <
      // 8 KB -- the upload still completes, browser just doesn't see
      // progress until heap recovers.
      const bool atEnd = (wsUploadReceived >= wsUploadSize);
      if (wsUploadReceived - wsLastProgressSent >= 262144 || atEnd) {
        const uint32_t curFree = ESP.getFreeHeap();
        if (curFree < 8u * 1024u && !atEnd) {
          // Heap too tight to safely send. Skip this PROGRESS; we'll
          // try again at the next 256 KB boundary. Don't advance
          // wsLastProgressSent so the catchup is on the next chunk.
          LOG_INF("WS", "DIAG progress SKIP (free=%u too low) %u/%u",
                  curFree, (unsigned)wsUploadReceived, (unsigned)wsUploadSize);
        } else {
          String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
          wsServer->sendTXT(num, progress);
          wsLastProgressSent = wsUploadReceived;
          LOG_INF("WS", "DIAG progress %u/%u bytes (free=%u maxAlloc=%u)",
                  (unsigned)wsUploadReceived, (unsigned)wsUploadSize,
                  curFree, ESP.getMaxAllocHeap());
        }
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        // v18.9.9.324: tail sanity check. WebSocket upload counted every
        // received chunk toward wsUploadReceived, but under a dropped-then-
        // resumed connection SdFat's write() has been observed to report
        // success while the actual sectors weren't committed to flash. Result:
        // file's SIZE metadata is correct but the tail bytes read as 0xFF
        // (uninitialised flash). The file passes DONE, browser reports
        // success, user tries to read the book, ZipFile hits "EOCD signature
        // not found" and reader gives up. To catch this at upload time,
        // sample the last 32 bytes here. If they are ALL 0xFF AND the file
        // extension is one where all-0xFF is essentially impossible for
        // legitimate content (.epub / .xtc: ZIP structure at tail), fail
        // the upload cleanly instead of letting the browser think it
        // succeeded. Skip .bin (firmware sector padding IS legitimately
        // 0xFF) and .cpfont (fixed-size trailing tables can hit long 0xFF
        // runs). File is deleted so a retry replaces it cleanly rather
        // than the client hitting the same corruption on next open.
        String filePathForCheck = wsUploadPath;
        if (!filePathForCheck.endsWith("/")) filePathForCheck += "/";
        filePathForCheck += wsUploadFileName;
        auto lower = [](String s) { s.toLowerCase(); return s; };
        const String lowered = lower(wsUploadFileName);
        const bool checkTail = lowered.endsWith(".epub") || lowered.endsWith(".xtc");
        if (checkTail && wsUploadSize >= 32) {
          // v18.9.9.331: upgraded from "last-32-bytes all 0xFF" heuristic
          // (which missed garbage-tail corruptions like
          // aea996669999a9a696...5aabbffffffe) to a real ZIP EOCD parse.
          // If loadZipDetails succeeds, the ZIP is structurally OK. If it
          // fails, the file's tail is either truncated, garbage-written, or
          // uses Zip64 (unsupported). Any of those means the reader will
          // fail on open, so reject at upload time. Reuses the v320
          // streaming tail-scan (fixed 4 KB buffer, no fragmentation
          // dependency) so this check doesn't itself become a heap risk.
          ZipFile zipCheck(filePathForCheck.c_str());
          if (!zipCheck.loadZipDetails()) {
            LOG_ERR("WS",
                    "Tail sanity FAILED: %s has no valid ZIP EOCD "
                    "(truncated, corrupted, or Zip64); deleting file",
                    filePathForCheck.c_str());
            Storage.remove(filePathForCheck.c_str());
            setFtUploadInProgress(false);
            String errMsg = String("ERROR:ZIP structure invalid (upload corrupted mid-write, or unsupported Zip64) -- please retry");
            wsServer->sendTXT(num, errMsg.c_str());
            wsLastProgressSent = 0;
            wsLastFsyncAt = 0;
            break;
          }
        }
        // CrumBLE 4.5.4: clean DONE -- clear the panic-recovery flag and
        // reset its consecutive-fail counter so the next upload starts
        // with full auto-resume budget.
        setFtUploadInProgress(false);
        // CrumBLE 4.5.5: re-grab the heap reserve pad now that the
        // upload's transient buffers (lwIP queues, sector cache) have
        // released. If the heap settled cleanly we get the full 32 KB
        // back; if it's still fragmented we stay un-padded for the next
        // upload (the floor check will catch it). The call is safe to
        // make every DONE -- it no-ops if the pad already exists.
        reclaimWsHeapReservePad("WS-DONE");

        // 4.5.5 diagnostic: post-DONE heap snapshot. The leak hunt for the
        // "24K -> 5K over 130s" pattern wants this as a baseline -- every
        // subsequent allocation that doesn't free comes after this point.
        LOG_INF("WS", "MEM-DIAG WS-DONE post-reclaim: free=%u maxAlloc=%u minFree=%u",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());

        // v18.9.9.384: arm post-large-upload defrag restart when heap is
        // fragmented. Field observation: CJK book (10 MB) uploaded cleanly,
        // then browser's post-upload workflow (font stream 4 MB + WASM
        // prebake + cover-thumb .pxc uploads + section-prebake .bin uploads)
        // hit maxAlloc=12K with min-free=620 bytes. Any missed WS reserve
        // pad allocation would OOM. Restart AFTER the DONE flushes to give
        // browser fresh 95K/61K heap for the incoming cache uploads. The
        // browser's post-upload work continues; any interrupted request
        // hits its retry loop (font fetch = 3 attempts, cache uploads use
        // the same uploadFileWebSocket auto-resume as book uploads).
        // Gated on size >= 1 MB so short cache uploads don't trigger a
        // restart cascade after themselves.
        constexpr uint32_t kPostUploadRestartMinSize = 1u * 1024u * 1024u;
        constexpr uint32_t kPostUploadRestartFreeCeil = 22u * 1024u;
        constexpr uint32_t kPostUploadRestartMaxAllocCeil = 15u * 1024u;
        const uint32_t postDoneFree = ESP.getFreeHeap();
        const uint32_t postDoneMaxAlloc = ESP.getMaxAllocHeap();
        if (static_cast<uint32_t>(wsUploadSize) >= kPostUploadRestartMinSize &&
            (postDoneFree < kPostUploadRestartFreeCeil ||
             postDoneMaxAlloc < kPostUploadRestartMaxAllocCeil)) {
          LOG_INF("WS",
                  "Post-upload defrag restart armed: size=%u >= %u AND (free=%u<%u OR maxAlloc=%u<%u)",
                  static_cast<unsigned>(wsUploadSize),
                  static_cast<unsigned>(kPostUploadRestartMinSize),
                  postDoneFree, static_cast<unsigned>(kPostUploadRestartFreeCeil),
                  postDoneMaxAlloc, static_cast<unsigned>(kPostUploadRestartMaxAllocCeil));
          g_pendingFtDefragRestartAtMs = millis() + 1500;  // let DONE flush + browser see it
        }

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCache(filePath.c_str());

        // CrumBLE 4.5.2: signal the FT activity to re-walk the library
        // once the upload burst settles. Picks up the new book in
        // LibraryIndex AND populates its author key (cache-hit from the
        // WASM prebake's book.bin if present, OPF peek otherwise) so
        // Sort by Author works immediately rather than waiting for the
        // user to visit Home + the lazy ensureWalked there.
        g_pendingLibraryRefresh = true;
        g_lastLibraryRefreshBumpMs = millis();  // CrumBLE 4.5.4: debounce burst uploads

        // CrumBLE 4.4: include the device-sanitized final path in DONE so the
        // browser's chapter-prebake step doesn't have to issue an /api/files
        // listing to look the file up. On a tight post-upload heap, the
        // listing endpoint heap-bails after a handful of rows -- a large
        // book at the bottom of a long folder gets truncated out of the
        // result and the prebake reports "could not find uploaded EPUB".
        // Returning the path here costs ~1 frame on the wire and removes
        // the most heap-fragile step from the critical path. Legacy clients
        // that only check `msg === 'DONE'` will fall through to the colon
        // and treat it as ERROR-or-unknown; new client parses prefix.
        String doneMsg = String("DONE:") + filePath;
        wsServer->sendTXT(num, doneMsg.c_str());
        wsLastProgressSent = 0;
  wsLastFsyncAt = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  // v18.9.9.385: ETag-validated HTML serve; see handleFileList.
  if (applyAssetCacheOrServe304(server.get(), "fonts-page", sizeof(FontsPageHtml))) return;
  sendHtmlContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml));
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleFontList() const {
  // CrumBLE 4.2: force a rescan every call instead of only refreshIfDirty.
  // The dirty flag only flips for /api/fonts/upload + /api/fonts/delete --
  // a user who drops a .cpfont onto the SD card directly (pulled card,
  // copied file, reinserted) doesn't trigger that path, so the optimizer
  // preflight modal would keep showing the stale font list until the
  // device next did an explicit upload/delete. One directory scan per
  // /api/fonts call is cheap (~1-2 SD ops for typical font libraries)
  // and removes the "why doesn't my new font show up" surprise.
  const_cast<SdCardFontSystem&>(sdFontSystem).registry().discover();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      // Stat the file for size
      FsFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// CrumBLE 4.2: report which built-in reading fonts the firmware actually
// ships. Values match CrossPointSettings::FONT_FAMILY enum so the optimizer
// preflight modal can map "builtin:<value>" tags to the device-side
// fontFamily field. OMIT_BITTER_FONT / OMIT_LEXENDDECA_FONT /
// OMIT_CHAREINK_FONT drop their rows so the modal only offers fonts that
// are actually in this variant binary (tiny-bitter / tiny-lexend /
// tiny-chareink).
void CrossPointWebServer::handleBuiltinFontList() const {
  JsonDocument doc;
  JsonArray builtins = doc["builtins"].to<JsonArray>();
#ifndef OMIT_LEXENDDECA_FONT
  {
    JsonObject e = builtins.add<JsonObject>();
    e["value"] = static_cast<int>(CrossPointSettings::LEXENDDECA);
    e["label"] = "Lexend Deca";
  }
#endif
#ifndef OMIT_BITTER_FONT
  {
    JsonObject e = builtins.add<JsonObject>();
    e["value"] = static_cast<int>(CrossPointSettings::BITTER);
    e["label"] = "Bitter";
  }
#endif
#ifndef OMIT_CHAREINK_FONT
  {
    JsonObject e = builtins.add<JsonObject>();
    e["value"] = static_cast<int>(CrossPointSettings::CHAREINK);
    e["label"] = "CharE-Ink";
  }
#endif
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// CrumBLE 4.2: serve a raw .cpfont file so the browser-side prebake
// optimizer can ship the bytes to WASM and register the SD-card font
// for layout. Query: ?family=<name>&size=<pt>. Forces a registry rescan
// so a freshly-dropped .cpfont surfaces without the dirty-flag dance.
void CrossPointWebServer::handleFontFile() const {
  if (!server->hasArg("family") || !server->hasArg("size")) {
    server->send(400, "text/plain", "Missing family or size");
    return;
  }
  const String familyArg = server->arg("family");
  const int pointSize = server->arg("size").toInt();
  if (familyArg.isEmpty() || pointSize <= 0 || pointSize > 255) {
    server->send(400, "text/plain", "Invalid family or size");
    return;
  }

  const_cast<SdCardFontSystem&>(sdFontSystem).registry().discover();
  for (const auto& family : sdFontSystem.registry().getFamilies()) {
    if (family.name != familyArg.c_str()) continue;
    const auto* file = family.findFile(static_cast<uint8_t>(pointSize));
    if (!file) {
      server->send(404, "text/plain", "No matching size in family");
      return;
    }
    // v18.9.9.390: pre-flight heap gate. Multi-MB CJK fonts (Bitter-LXGWWenKai
    // is 4.4 MB) need enough headroom to sustain SD reads + LWIP TCP writes;
    // when free heap is below ~30 KB after a big WS upload, throughput
    // collapses to 3 KB/s (~25 min to complete), the browser hangs, and the
    // web-server task blocks in the response loop so the passive heap
    // watchdog can't fire. Refuse the request cleanly (503) so the browser
    // gives up in seconds and the tick loop can decide what to do next
    // (silent-restart to FT typically). Threshold chosen to be higher than
    // the FT watchdog floor (10 KB) so we bail BEFORE the watchdog would
    // have to.
    // v18.9.9.424: cut 30 KB -> 12 KB. Field data: steady-state free heap
    // in FT after any WS upload lands at ~17-25 KB, well below the 30 KB
    // floor. Consequence was that the browser's prebake path could NEVER
    // fetch the SD font on a fragmented FT session -- it retried 3x,
    // gave up, and baked CJK books with the built-in font (no CJK glyphs)
    // producing a manifest that always fingerprint-mismatched at open
    // time. The book then fell back to regular indexing, where per-page
    // SdCardFont chunked-prewarm could only fit ~50% of glyphs in the
    // fragmented heap, and users saw tofu on every CJK page. Root fix
    // is to let the browser get the SD font on the first try. 12 KB is
    // still safely above the FT watchdog floor (10 KB) so an actively-
    // failing stream won't stall past the watchdog's silent-restart. The
    // stream itself uses a 2 KB stack buffer + LWIP send window; it can
    // sustain low-heap conditions if it started with any headroom at all.
    constexpr uint32_t kFontStreamMinFree = 12u * 1024u;
    const uint32_t freeAtStart = ESP.getFreeHeap();
    if (freeAtStart < kFontStreamMinFree) {
      LOG_ERR("FONT-STREAM",
              "REFUSED %s: free=%u below floor %u (browser should retry after device recovers)",
              file->path.c_str(), freeAtStart,
              static_cast<unsigned>(kFontStreamMinFree));
      server->send(503, "text/plain", "Device heap too low to stream font; retry later");
      return;
    }
    FsFile f;
    if (!Storage.openFileForRead("WEB", file->path.c_str(), f)) {
      server->send(500, "text/plain", "Could not open .cpfont");
      return;
    }
    const size_t fileSize = f.size();
    server->setContentLength(fileSize);
    server->send(200, "application/octet-stream", "");
    // CrumBLE 4.5.4: 8 KB chunks (was 1 KB). Cuts the SD-read + socket-
    // write trip count 8x for a multi-MB CJK .cpfont, which previously
    // pushed the optimizer's 30s fetch ceiling. Also yield the watchdog
    // and run-loop every 32 KB so a 5 MB transfer can't trip WDT_RESET.
    // CrumBLE 4.5.4 hotfix-of-hotfix: 2 KB stack buffer, not 8 KB static.
    // Earlier attempt used `static uint8_t buf[8192]` to dodge stack
    // overflow during font streaming, but that 8 KB of permanent BSS
    // shaved the boot-time free heap from ~118 KB to ~110 KB, just
    // enough to push the WEB-serve-low-heap auto-recovery threshold
    // below available heap during HTML page serve. Result: every FT
    // entry triggered silentRestart, which booted back into FT, hit
    // the same threshold, and looped (field log: 'cant get FT page
    // open'). 2 KB on stack is safe (web-server task has ~8 KB stack;
    // 2 KB local leaves 6 KB for the call chain) and 2x faster than
    // the original 1 KB. Throughput improvement gets sacrificed to
    // unstick the device.
    constexpr size_t kBufSize = 2048;
    constexpr size_t kYieldEveryBytes = 32 * 1024;
    constexpr size_t kProgressLogEveryBytes = 256 * 1024;
    uint8_t buf[kBufSize];
    size_t sinceLastYield = 0;
    size_t sinceLastLog = 0;
    size_t totalSent = 0;
    const uint32_t startMs = millis();
    LOG_INF("FONT-STREAM", "begin %s (%u bytes)", file->path.c_str(), static_cast<unsigned>(fileSize));
    // 4.5.5 diagnostic
    LOG_INF("FONT-STREAM", "MEM-DIAG pre-stream: free=%u maxAlloc=%u minFree=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    while (true) {
      const size_t n = f.read(buf, kBufSize);
      if (n == 0) break;
      server->client().write(buf, n);
      totalSent += n;
      sinceLastYield += n;
      sinceLastLog += n;
      if (sinceLastYield >= kYieldEveryBytes) {
        esp_task_wdt_reset();
        yield();
        sinceLastYield = 0;
        // v18.9.9.390: mid-stream abort if heap collapsed since we started.
        // Same reasoning as the pre-flight gate above -- once free drops
        // below the watchdog floor the socket write slows to 3 KB/s and
        // the client hangs. Break out (which closes the connection)
        // instead of grinding through the tail; the tick loop then gets
        // a chance to run the FT heap watchdog / silent-restart.
        constexpr uint32_t kFontStreamAbortFree = 12u * 1024u;
        const uint32_t freeMid = ESP.getFreeHeap();
        if (freeMid < kFontStreamAbortFree) {
          LOG_ERR("FONT-STREAM",
                  "ABORT %s at %u/%u: free=%u below abort floor %u -- closing socket",
                  file->path.c_str(), static_cast<unsigned>(totalSent),
                  static_cast<unsigned>(fileSize), freeMid,
                  static_cast<unsigned>(kFontStreamAbortFree));
          server->client().stop();
          break;
        }
      }
      // Per-256KB progress log lets us see actual transfer rate. A multi-MB
      // CJK font streaming at 30 KB/s (the observed failure mode) will emit
      // ~8 of these lines per second wall-clock; a healthy 200 KB/s rate
      // gives ~1 per second. If this log stops emitting partway through
      // the transfer, the device wedged (heap/WiFi/SD).
      if (sinceLastLog >= kProgressLogEveryBytes) {
        const uint32_t elapsed = millis() - startMs;
        const uint32_t kbs = elapsed > 0 ? (totalSent / elapsed) : 0;
        LOG_INF("FONT-STREAM", "%u/%u bytes @ %u KB/s, free=%u maxAlloc=%u",
                static_cast<unsigned>(totalSent), static_cast<unsigned>(fileSize),
                static_cast<unsigned>(kbs), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        sinceLastLog = 0;
      }
    }
    LOG_INF("FONT-STREAM", "done %u bytes in %u ms", static_cast<unsigned>(totalSent),
            static_cast<unsigned>(millis() - startMs));
    // 4.5.5 diagnostic: post-stream heap snapshot. Comparing this to the
    // WS-DONE snapshot earlier shows how much the font stream itself cost
    // and whether any of that cost is permanent vs transient lwIP queue.
    LOG_INF("FONT-STREAM", "MEM-DIAG post-stream: free=%u maxAlloc=%u minFree=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    f.close();
    LOG_INF("FONT-STREAM", "MEM-DIAG post-close: free=%u maxAlloc=%u minFree=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    return;
  }
  server->send(404, "text/plain", "Family not found");
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      esp_task_wdt_reset();
      String family = server->arg("family");
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a .cpfont basename of alphanumeric + hyphen + underscore. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[192];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.valid) break;
      esp_task_wdt_reset();

      // Validate magic bytes on first chunk only
      if (!fontUpload.magicChecked && upload.currentSize >= 8) {
        if (memcmp(upload.buf, "CPFONT\0\0", 8) != 0) {
          LOG_ERR("WEB", "Invalid .cpfont magic bytes");
          fontUpload.valid = false;
          break;
        }
        fontUpload.magicChecked = true;
      }

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
          esp_task_wdt_reset();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      // Flush remaining buffer
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      fontUpload.file.close();

      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      fontUpload.file.close();
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.valid = false;
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
