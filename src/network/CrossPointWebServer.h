#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

class GfxRenderer;

// CrumBLE: returns true (consuming the flag) when sendBufferGzip flagged
// a heap-too-low serve as needing a silentRestart-to-FT cycle. The FT
// activity polls this from loop() and triggers the restart so the 200
// response physically reaches the phone before the device reboots.
bool consumeFtRestartRequest();

// CrumBLE: same flag, non-destructive read. The FT activity polls this
// INSIDE its handleClient() tight loop -- once a handler has set the
// flag, we must stop accepting more requests immediately or the next
// already-queued one drains the heap below the recovery threshold
// (typical sequence: SettingsPage triggers /api/settings, /api/wifi,
// /api/opds in parallel; the first one's guard fires, the next one's
// allocator craters because the response never gets a chance to send).
bool peekFtRestartRequest();

// CrumBLE 4.6 LAN-OTA: set by the WS INSTALL_FIRMWARE handler after a
// browser-uploaded firmware-pending.bin has been validated and is ready
// to flash. The FT activity polls this from loop() and switches to the
// install-progress activity, which calls firmware_flash::flashFromSdPath
// + ESP.restart(). One-shot consume; the second call returns false.
bool consumeFirmwareInstallRequest();

// CrumBLE 4.5.2: set by the WS upload DONE handler so the FT activity
// can trigger LibraryIndex::markStale + ensureWalked once the upload
// burst settles. Catches newly-uploaded books in the library index
// AND populates their author keys (from book.bin if the WASM prebake
// ran, else from the OPF peek). Without this, the new book stays
// unindexed until the user visits Home, and Sort by Author leaves it
// at the back. Consumed one-shot.
bool consumePendingLibraryRefreshRequest();

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    FsFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    UploadState() { buffer.resize(UPLOAD_BUFFER_SIZE); }
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

  // Renderer used by /api/reader-render-info to compute the reader's viewport and
  // emSize (so the optimizer can reproduce fitted image dimensions for .pxc
  // baking). Set by CrossPointWebServerActivity before begin().
  void setRenderer(GfxRenderer* r) { renderer_ = r; }

  // CrumBLE: pre-build the /api/settings JSON into a std::string cache while
  // the FT activity has plenty of free heap (post-BT-release, ~106 KB).
  // handleGetSettings then just serves the cached blob with one send() --
  // no getSettingsList() rebuild + no JsonDocument allocation pressure mid-
  // request. Called by CrossPointWebServerActivity before begin() and by
  // handlePostSettings to refresh after a settings change.
  void primeSettingsCache();

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  // CrumBLE: pre-built /api/settings JSON. Populated by primeSettingsCache()
  // and served verbatim by handleGetSettings. Cleared+rebuilt by handlePost
  // Settings so the next GET reflects the change.
  std::string cachedSettingsJson_;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  GfxRenderer* renderer_ = nullptr;
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // File scanning
  void scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleOptimizerJs() const;
  // CrumBLE Phase 5a: serve PROGMEM-embedded prebake WASM module.
  // Total ~870 KB gzipped; loaded on demand by the optimizer page when
  // the user opts in to chapter-prebake. Headers return 404 if the WASM
  // wasn't built (zero-length sentinel from embed_wasm.py) so the
  // firmware still works without crumble-prebake.{js,wasm} on disk.
  void handleCrumblePrebakeJs() const;
  void handleCrumblePrebakeWasm() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Reader render-info (for optimizer .pxc baking): reader viewport + emSize.
  void handleReaderRenderInfo() const;

  // POST /api/save-reader-settings  (Content-Type: application/json)
  // Body: subset of /api/reader-render-info's payload (any field that's
  // a writable SETTINGS member is accepted). Updates only the named fields
  // -- omitted fields stay at their current SETTINGS values. SETTINGS.
  // saveToFile() persists to flash on success. Used by the optimizer's
  // preflight modal so the user can correct any setting that's wrong
  // before locking it into the prebake's manifest.
  void handleSaveReaderSettings() const;

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleBuiltinFontList() const;
  void handleFontFile() const;
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();

  // Font upload state
  struct FontUploadState {
    FsFile file;
    std::string familyName;
    std::string filePath;
    bool valid = false;
    bool magicChecked = false;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    FontUploadState() { buffer.resize(BUFFER_SIZE); }
  } fontUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
};
