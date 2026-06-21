#include "CrossPointWebServer.h"

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
#include "html/wasm/crumblePrebakeJs.generated.h"
#include "html/wasm/crumblePrebakeWasm.generated.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace {
// Folders/files to hide from the web interface file browser.
// Dot-prefixed items are hidden unless showHiddenFiles is enabled.
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


// CrumBLE: set by sendBufferGzip when the heap is too low to serve. The
// FT activity's loop() polls this via consumeFtRestartRequest() and
// triggers silentRestartToFileTransfer once the request handler has
// returned (so the response physically reaches the browser before the
// device reboots).
bool g_pendingFtRestart = false;

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
bool g_pendingLibraryRefresh = false;
}  // namespace anon

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
  g_pendingLibraryRefresh = false;
  return true;
}

namespace {
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

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
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
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
  server->on("/api/reader-render-info", HTTP_GET, [this] { handleReaderRenderInfo(); });
  server->on("/api/save-reader-settings", HTTP_POST, [this] { handleSaveReaderSettings(); });

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

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

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
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

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
  struct timeval tv;
  tv.tv_sec = 5;  // a stalled send fails after 5 s instead of hanging forever
  tv.tv_usec = 0;
  server->client().setSocketOption(SO_SNDTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
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
  // CrumBLE: 22 KB free / 12 KB maxAlloc was the working floor pre-cache.
  // The cache experiment was reverted; back to the original numbers so
  // page serves and API handlers have the headroom they expect.
  if (preFree >= 22u * 1024u && preMax >= 12u * 1024u) {
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
  LOG_ERR("WEB", "guard %s low-heap (free=%u maxAlloc=%u): scheduling silentRestart to FT", tag, preFree, preMax);
  server->sendHeader("Refresh", "8");
  // Some browsers will not honour Refresh on a JSON response. Send an
  // application/json body that the page-side fetch can detect (status 503
  // + an empty array fallback) AND meta-refresh-equivalent via the header
  // so a subsequent navigation still recovers.
  server->send(503, "application/json", "[]");
  g_pendingFtRestart = true;
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
  if (isHtmlSubstitutionSafe && preFree < 22u * 1024u) {
    // CrumBLE 4.4: same post-upload settle window as the api-files guard.
    // After a fresh upload, the page reload that fetches FilesPage.html
    // would otherwise immediately silent-restart and the browser's chapter
    // prebake step can't complete its post-upload queries.
    constexpr unsigned long POST_UPLOAD_SETTLE_MS = 15000;
    if (wsLastCompleteAt > 0 && (millis() - wsLastCompleteAt) < POST_UPLOAD_SETTLE_MS) {
      LOG_INF("WEB",
              "serve %s low-heap (free=%u) but within post-upload settle window; passing through",
              tag, preFree);
      // Fall through to the normal serve path (note: this may still fail
      // if heap is truly exhausted -- but that's better than restarting
      // mid-prebake when the browser still has work to do).
    } else {
      LOG_ERR("WEB", "serve %s low-heap: scheduling silentRestart to FT", tag);
      server->sendHeader("Refresh", "8");
      server->send(200, "text/html",
                   "<!doctype html><html><head><title>File Transfer</title></head><body></body></html>");
      // Send completes before we set the flag so the response actually
      // reaches the browser before the device reboots.
      g_pendingFtRestart = true;
      return;
    }
  }
  if (!isHtmlSubstitutionSafe && preFree < 6u * 1024u) {
    LOG_ERR("WEB", "serve %s low-heap (free=%u below 6 KB floor): sending 503", tag, preFree);
    server->send(503, "text/plain", "Server low memory; retry shortly");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, mime, data, len);
  LOG_INF("WEB", "serve %s done: post free=%d maxAlloc=%d", tag, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  sendBufferGzip(server, "text/html", data, len, "html");
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  sendBufferGzip(server.get(), "application/javascript", jszip_minJs,
                 jszip_minJsCompressedSize, "jszip.js");
}

void CrossPointWebServer::handleOptimizerJs() const {
  sendBufferGzip(server.get(), "application/javascript", optimizerJs,
                 optimizerJsCompressedSize, "optimizer.js");
}

void CrossPointWebServer::handleCrumblePrebakeJs() const {
  if (CrumblePrebakeJsCompressedSize == 0) {
    server->send(404, "text/plain", "Prebake WASM not built into this firmware");
    return;
  }
  sendBufferGzip(server.get(), "application/javascript", CrumblePrebakeJs,
                 CrumblePrebakeJsCompressedSize, "crumble-prebake.js");
}

void CrossPointWebServer::handleCrumblePrebakeWasm() const {
  if (CrumblePrebakeWasmCompressedSize == 0) {
    server->send(404, "text/plain", "Prebake WASM not built into this firmware");
    return;
  }
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
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
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
  bool seenFirst = false;
  bool truncated = false;
  size_t emittedRows = 0;

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[", 1);

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
  constexpr uint32_t kAbortIfMaxAllocBelow = 4 * 1024;
  scanFiles(currentPath.c_str(), [&](const FileInfo& info) {
    if (truncated) return;
    if (ESP.getMaxAllocHeap() < kAbortIfMaxAllocBelow) {
      truncated = true;
      return;
    }

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
      std::string fullPath = currentPath.c_str();
      if (fullPath.empty() || fullPath.back() != '/') fullPath += '/';
      fullPath += info.name.c_str();
      // CrumBLE 4.4: three-tier badge detection. Each marker is a 0-byte
      // sentinel written by the WASM CLI when the corresponding artifact
      // shipped. Fallback for old bakes: if prebake-chap.marker is missing
      // but sections-prebake/ directory exists, treat it as chap-cached
      // (the older CLI didn't write the chap marker, but the data is
      // there). Reusable stack buffer for the marker-path build avoids
      // four std::string concatenations per row -- those allocations were
      // fragmenting heap badly enough to trip the 4 KB MaxAlloc bailout
      // at row 15 on books-with-long-names listings.
      const std::string cacheDir = Epub::cachePathForFilePath(fullPath, "/.crosspoint");
      char markerBuf[256];
      auto checkUnder = [&](const char* leaf) -> bool {
        const int n = snprintf(markerBuf, sizeof(markerBuf), "%s/%s", cacheDir.c_str(), leaf);
        return n > 0 && static_cast<size_t>(n) < sizeof(markerBuf) && Storage.exists(markerBuf);
      };
      prebaked = checkUnder("prebake-v2.marker");
      prebakedChap = checkUnder("prebake-chap.marker") || checkUnder("sections-prebake");
      prebakedCpFont = checkUnder("prebake-cpfont.marker");
    }

    // CrumBLE 4.4: build a multi-line tooltip from the prebake manifest so
    // the FT page's badge mirrors the on-device viewer. Empty string when
    // the book isn't prebaked or heap is too tight to safely parse JSON
    // (helper bails internally). Escape \n and " for JSON embedding.
    char tooltipEscaped[512];
    tooltipEscaped[0] = '\0';
    if (prebaked) {
      const std::string cacheDir = Epub::cachePathForFilePath(
          (currentPath.length() == 0 || currentPath[currentPath.length() - 1] != '/')
              ? std::string(currentPath.c_str()) + "/" + info.name.c_str()
              : std::string(currentPath.c_str()) + info.name.c_str(),
          "/.crosspoint");
      const std::string tooltip = formatPrebakeTooltip(cacheDir);
      size_t oi = 0;
      for (char c : tooltip) {
        if (oi + 3 >= sizeof(tooltipEscaped)) break;
        if (c == '"' || c == '\\') {
          tooltipEscaped[oi++] = '\\';
          tooltipEscaped[oi++] = c;
        } else if (c == '\n') {
          tooltipEscaped[oi++] = '\\';
          tooltipEscaped[oi++] = 'n';
        } else {
          tooltipEscaped[oi++] = c;
        }
      }
      tooltipEscaped[oi] = '\0';
    }

    const int written = snprintf(
        rowBuf, sizeof(rowBuf),
        "{\"name\":\"%s\",\"size\":%lu,\"isDirectory\":%s,\"isEpub\":%s,\"prebaked\":%s,\"prebakedChap\":%s,\"prebakedCpFont\":%s,\"prebakeTooltip\":\"%s\"}",
        escapedName, static_cast<unsigned long>(info.size),
        info.isDirectory ? "true" : "false",
        info.isEpub ? "true" : "false",
        prebaked ? "true" : "false",
        prebakedChap ? "true" : "false",
        prebakedCpFont ? "true" : "false",
        tooltipEscaped);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(rowBuf)) return;

    if (seenFirst) server->sendContent(",", 1);
    else seenFirst = true;
    server->sendContent(rowBuf, static_cast<size_t>(written));
    ++emittedRows;
  });
  server->sendContent("]", 1);
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

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
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
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
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
  // CrumBLE: /api/settings is disabled in this build. Every implementation
  // we tried (chunked streaming, build-to-buffer, two-pass with explicit
  // Content-Length, cache-at-FT-entry) tripped some combination of the
  // ESP32-C3's lwIP send-timeout being unreliable, chunked-encoding
  // overhead, and the heap-pressure cliff once the cache or build
  // allocation was held. Returning 503 here makes the SettingsPage show
  // its error state instead of spinning forever; wifi/opds/file APIs
  // and the device-side Settings UI are unaffected.
  applyClientSendTimeout(server.get());
  LOG_INF("WEB", "api-settings: 503 (device-side Settings is the canonical path in 4.0)");
  server->send(503, "application/json",
               "{\"error\":\"web settings unavailable in 4.0; use device-side Settings\"}");
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
      // CrumBLE 4.4: on mid-upload disconnect, KEEP the partial file on SD
      // so the next START can resume from where we left off. abortWsUpload
      // would delete the file -- that's the right move for explicit errors
      // (overflow, write fail), but a disconnect is exactly the case where
      // resume should help. Close the handle and reset the in-progress
      // flags, but leave the bytes on disk.
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        LOG_INF("WS",
                "Client %u disconnected mid-upload at %u/%u bytes; preserving for resume",
                num, (unsigned)wsUploadReceived, (unsigned)wsUploadSize);
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;
        wsLastProgressSent = 0;
      }
      break;

    case WStype_CONNECTED: {
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
        // up at "n/m uploaded". A silent restart at START is cheap (no bytes
        // received yet, no file on disk yet) and the browser's auto-resume
        // logic re-issues START after reconnect. Also covers the between-
        // books case: each book's first message is a fresh START so the
        // pre-flight fires on every transition.
        constexpr uint32_t WS_CHUNK_SIZE = 4096;
        constexpr uint32_t kPreUploadMaxAllocFloor = WS_CHUNK_SIZE + 3u * 1024u;  // ~7 KB
        const uint32_t startFree = ESP.getFreeHeap();
        const uint32_t startMax = ESP.getMaxAllocHeap();
        if (startMax < kPreUploadMaxAllocFloor) {
          LOG_ERR("WS",
                  "START rejected: pre-upload MaxAlloc=%u below floor=%u (free=%u); "
                  "scheduling silentRestart to FT before accepting bytes",
                  startMax, kPreUploadMaxAllocFloor, startFree);
          wsServer->sendTXT(num, "ERROR:Heap too fragmented, device restarting; please retry");
          g_pendingFtRestart = true;
          break;
        }
        LOG_INF("WS", "START heap pre-flight ok: free=%u maxAlloc=%u (floor=%u)",
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
          if (resumeFrom > 0) {
            // Resume path: open without O_TRUNC, position at end.
            wsUploadFile = Storage.open(filePath.c_str(), O_RDWR | O_CREAT);
            if (!wsUploadFile) {
              wsServer->sendTXT(num, "ERROR:Failed to reopen file for resume");
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
              wsServer->sendTXT(num, "ERROR:Failed to create file");
              wsUploadInProgress = false;
              wsUploadClientNum = 255;
              return;
            }
          }
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
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
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
        if ((binFrameCount & 0x0F) == 0) {  // every 16 frames
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

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
        LOG_INF("WS", "DIAG progress %u/%u bytes (free=%u maxAlloc=%u)",
                (unsigned)wsUploadReceived, (unsigned)wsUploadSize,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

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
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
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
    FsFile f;
    if (!Storage.openFileForRead("WEB", file->path.c_str(), f)) {
      server->send(500, "text/plain", "Could not open .cpfont");
      return;
    }
    const size_t fileSize = f.size();
    server->setContentLength(fileSize);
    server->send(200, "application/octet-stream", "");
    constexpr size_t kBufSize = 1024;
    uint8_t buf[kBufSize];
    while (true) {
      const size_t n = f.read(buf, kBufSize);
      if (n == 0) break;
      server->client().write(buf, n);
    }
    f.close();
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
