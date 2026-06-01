#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

class GfxRenderer;

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

  // CrumBLE: /api/upload-prebake-cache uses the same streamed-upload pattern
  // as the /upload endpoint, but the destination is a fixed temp path and
  // the FILE_END hook extracts the zip's contents to SD instead of leaving
  // the upload as-is. Separate state from UploadState so a concurrent
  // file-manager upload doesn't trample the prebake cache extract.
  struct PrebakeCacheUploadState {
    FsFile file;
    String tmpPath = "/.crosspoint/_inflight_prebake_cache.zip";
    size_t size = 0;
    bool success = false;
    String error = "";
    int extractedCount = 0;
    size_t extractedBytes = 0;

    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    PrebakeCacheUploadState() { buffer.resize(UPLOAD_BUFFER_SIZE); }
  } prebakeCacheUpload;

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

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
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
  // CrumBLE prebake WASM module (~870 KB gzipped total). Optimizer.js loads
  // these on demand to run the off-device book.bin/section/thumb pipeline
  // client-side in the browser. Returns Content-Encoding: gzip; the handler
  // serves the raw PROGMEM bytes verbatim.
  void handleCrumblePrebakeJs() const;
  void handleCrumblePrebakeWasm() const;
  // POST /api/upload-prebake-cache: optimizer.js ships a zip whose entries
  // mirror the device's cache-dir layout (.crosspoint/epub_<hash>/book.bin,
  // sections/*.bin, thumb_*.bmp). Streamed upload writes the zip to a temp
  // SD file; the POST handler then walks the zip's central directory and
  // extracts each entry to its zip-encoded path under SD root, then deletes
  // the temp. Result: the device has a populated cache before the user
  // opens the book for the first time -> sub-second cold open.
  void handlePrebakeCacheUpload(PrebakeCacheUploadState& state);
  void handlePrebakeCacheUploadPost(PrebakeCacheUploadState& state) const;
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

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
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
