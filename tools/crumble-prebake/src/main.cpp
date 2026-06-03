// crumble-prebake CLI -- off-device EPUB cache prebake.
//
// Phase 1 scope: emit book.bin per input EPUB. Phases 2-4 (sections,
// css_rules.cache, cover thumbs) follow on the same scaffolding.

#include <Arduino.h>
#include <Epub.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>

#include <Epub/BookMetadataCache.h>
#include <Epub/Section.h>
#include <Epub/parsers/ContainerParser.h>
#include <Epub/parsers/ContentOpfParser.h>
#include <Epub/parsers/TocNavParser.h>
#include <Epub/parsers/TocNcxParser.h>

// Phase 2C font tables. Pure-data PROGMEM headers; including them on host
// pulls in the same uint8_t[] bitmaps + metadata tables the device links
// against. Default reading font is LexendDeca Medium (14px). Add the four
// styles a single family needs (regular, bold, italic, bold-italic) here
// when expanding to more font sizes / variants.
#include <builtinFonts/lexenddeca_14_regular.h>
#include <builtinFonts/lexenddeca_14_bold.h>
#include <builtinFonts/lexenddeca_14_italic.h>
#include <builtinFonts/lexenddeca_14_bolditalic.h>
#include <builtinFonts/bitter_12_regular.h>
#include <builtinFonts/bitter_12_bold.h>
#include <builtinFonts/bitter_12_italic.h>
#include <builtinFonts/bitter_12_bolditalic.h>
#include "fontIds.h"  // LEXENDDECA_14_FONT_ID / BITTER_12_FONT_ID

#include <ArduinoJson.h>
#ifndef CRUMBLE_PREBAKE_WASM
#include <curl/curl.h>
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Section-header layout settings. Mirror the 12 fields baked into
// Section::writeSectionFileHeader. All values come from the device's
// /api/reader-render-info endpoint (fitVersion=2). --device-url is
// mandatory for section generation -- we used to allow factory-default
// fallback but in practice every real user has customized at least one
// of the 12 settings (font, viewport, line compression, etc.), so any
// factory-defaults-generated section would get rejected by the device's
// load-time fingerprint check anyway and trigger an on-board rebuild,
// defeating the whole point of prebaking. Requiring --device-url makes
// the CLI honest about its assumptions.
struct SectionSettings {
  // All defaults are placeholder values overridden by fetchDeviceSettings.
  // They only matter if section gen is somehow attempted without a
  // successful settings fetch, in which case we would (correctly) error
  // out before reaching the section build step.
  int fontId = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  bool forceParagraphIndents = false;
  uint8_t paragraphAlignment = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool bionicReadingEnabled = false;
  bool guideReadingEnabled = false;
  // Device target ("X4" or "X3") detected from the render-info response.
  // Used by future per-device thumb-set logic; currently only logged.
  std::string device;
  // CrumBLE reversion fields. The 12 fingerprint values above lock the
  // section header, but they include derived quantities (fontId, viewport,
  // lineCompression) that the on-device "Use prepared layout?" prompt can't
  // reverse-apply without knowing the RAW SETTINGS that produced them.
  // These mirror /api/reader-render-info v2's font-family/size + screen-
  // margin/orientation + lineSpacing so the manifest stores enough state
  // for the device to fully restore the prebake's reader settings on a
  // single user confirm.
  uint8_t orientation = 0;
  uint8_t screenMargin = 0;
  uint8_t fontFamily = 0;
  uint8_t fontSize = 0;
  uint8_t sdFontSizeRange = 0;
  std::string sdFontFamilyName;
  uint8_t lineSpacing = 0;
};

#ifndef CRUMBLE_PREBAKE_WASM
// libcurl write callback: appends incoming bytes to a std::string.
size_t curlWriteToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* str = static_cast<std::string*>(userdata);
  str->append(static_cast<char*>(ptr), size * nmemb);
  return size * nmemb;
}

// Fetch /api/reader-render-info from the device + populate sectionSettings.
// Returns true on success. Uses libcurl with a short timeout because the
// device's web server can hang under tight heap (the user has hit this
// chip-tracked bug; we don't want prebake to wedge waiting for a stalled
// /api/settings response). On failure, sectionSettings stays at the
// caller-provided defaults.
//
// WASM mode bypasses this entirely: JS calls fetch() against the device
// directly and passes the parsed JSON into the WASM core (see step 28.3).
bool fetchDeviceSettings(const std::string& deviceUrl, SectionSettings& out) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    LOG_ERR("CFG", "curl_easy_init failed");
    return false;
  }
  const std::string url = deviceUrl + "/api/reader-render-info";
  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);          // 10 s hard cap
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);    // 5 s connect cap
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  const CURLcode rc = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    LOG_ERR("CFG", "GET %s failed: %s", url.c_str(), curl_easy_strerror(rc));
    return false;
  }
  if (httpCode != 200) {
    LOG_ERR("CFG", "GET %s returned HTTP %ld", url.c_str(), httpCode);
    return false;
  }

  // ArduinoJson handles host parsing just like firmware. We only read
  // fields we care about; extras the server adds (orientation, screenMargin,
  // sdFontFamilyName, emSize, etc.) get ignored.
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR("CFG", "render-info JSON parse failed: %s", err.c_str());
    return false;
  }
  const int fitVersion = doc["fitVersion"] | 1;
  if (fitVersion < 2) {
    LOG_ERR("CFG",
            "device reports fitVersion=%d; need >=2 for prebake settings sync. "
            "Update firmware to a build that includes the v2 render-info fields.",
            fitVersion);
    return false;
  }

  // Device target (X4 / X3 / future SKUs) auto-detected from the endpoint.
  if (doc["device"].is<const char*>()) out.device = doc["device"].as<const char*>();
  if (doc["fontId"].is<int>()) out.fontId = doc["fontId"].as<int>();
  if (doc["viewportWidth"].is<int>()) out.viewportWidth = doc["viewportWidth"].as<int>();
  if (doc["viewportHeight"].is<int>()) out.viewportHeight = doc["viewportHeight"].as<int>();
  if (doc["lineCompression"].is<float>()) out.lineCompression = doc["lineCompression"].as<float>();
  if (doc["extraParagraphSpacing"].is<int>())
    out.extraParagraphSpacing = doc["extraParagraphSpacing"].as<int>() != 0;
  if (doc["forceParagraphIndents"].is<int>())
    out.forceParagraphIndents = doc["forceParagraphIndents"].as<int>() != 0;
  if (doc["paragraphAlignment"].is<int>()) out.paragraphAlignment = doc["paragraphAlignment"].as<int>();
  if (doc["hyphenationEnabled"].is<int>()) out.hyphenationEnabled = doc["hyphenationEnabled"].as<int>() != 0;
  if (doc["embeddedStyle"].is<int>()) out.embeddedStyle = doc["embeddedStyle"].as<int>() != 0;
  if (doc["imageRendering"].is<int>()) out.imageRendering = doc["imageRendering"].as<int>();
  if (doc["bionicReadingEnabled"].is<int>())
    out.bionicReadingEnabled = doc["bionicReadingEnabled"].as<int>() != 0;
  if (doc["guideReadingEnabled"].is<int>())
    out.guideReadingEnabled = doc["guideReadingEnabled"].as<int>() != 0;

  // Reversion fields. These are the RAW SETTINGS values used to produce
  // the prebake; the device's switch-back prompt copies them back into
  // SETTINGS on confirm. The 12 fingerprint fields above include DERIVED
  // values (fontId is hash of family+size, lineCompression is derived
  // from lineSpacing) that we can't cleanly invert, so we carry the raw
  // values alongside. Without these, the manifest reverts to zeros and
  // the switch-back prompt either misfires or restores factory defaults.
  if (doc["orientation"].is<int>()) out.orientation = doc["orientation"].as<int>();
  if (doc["screenMargin"].is<int>()) out.screenMargin = doc["screenMargin"].as<int>();
  if (doc["fontFamily"].is<int>()) out.fontFamily = doc["fontFamily"].as<int>();
  if (doc["fontSize"].is<int>()) out.fontSize = doc["fontSize"].as<int>();
  if (doc["sdFontSizeRange"].is<int>()) out.sdFontSizeRange = doc["sdFontSizeRange"].as<int>();
  if (doc["sdFontFamilyName"].is<const char*>())
    out.sdFontFamilyName = doc["sdFontFamilyName"].as<const char*>();
  if (doc["lineSpacing"].is<int>()) out.lineSpacing = doc["lineSpacing"].as<int>();

  LOG_INF("CFG",
          "device=%s settings: fontId=%d viewport=%ux%u lineCompression=%.3f extraPS=%d fpI=%d pA=%u "
          "hyph=%d embed=%d imgR=%u bionic=%d guide=%d",
          out.device.empty() ? "(unknown)" : out.device.c_str(), out.fontId, out.viewportWidth, out.viewportHeight,
          static_cast<double>(out.lineCompression), out.extraParagraphSpacing, out.forceParagraphIndents,
          out.paragraphAlignment, out.hyphenationEnabled, out.embeddedStyle, out.imageRendering,
          out.bionicReadingEnabled, out.guideReadingEnabled);
  return true;
}
#else
// WASM mode: render-info is delivered via --settings-file (loaded below
// by loadSettingsFromFile). fetchDeviceSettings stays around as a stub
// to keep CLI-shared parseArgs validation simple but is never called.
bool fetchDeviceSettings(const std::string& /*deviceUrl*/, SectionSettings& /*out*/) {
  return false;
}
#endif  // CRUMBLE_PREBAKE_WASM

// Load render-info JSON from a local file and populate sectionSettings.
// Same schema as /api/reader-render-info (fitVersion >= 2). Returns true
// on success. Used by the --settings-file CLI flag and -- by virtue of
// MEMFS -- by the WASM build's optimizer.js integration: JS writes the
// fetched JSON into MEMFS at a known path, then passes that path through
// argv to main(). No HTTP / no libcurl, so this path is identical on the
// CLI build and the WASM build, which keeps test coverage shared.
bool loadSettingsFromFile(const std::string& path, SectionSettings& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    LOG_ERR("CFG", "could not open settings file: %s", path.c_str());
    return false;
  }
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  if (body.empty()) {
    LOG_ERR("CFG", "settings file %s is empty", path.c_str());
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR("CFG", "settings file JSON parse failed: %s", err.c_str());
    return false;
  }
  const int fitVersion = doc["fitVersion"] | 1;
  if (fitVersion < 2) {
    LOG_ERR("CFG",
            "settings file reports fitVersion=%d; need >=2 for prebake. Regenerate from a "
            "firmware build that includes the v2 render-info fields.",
            fitVersion);
    return false;
  }

  if (doc["device"].is<const char*>()) out.device = doc["device"].as<const char*>();
  if (doc["fontId"].is<int>()) out.fontId = doc["fontId"].as<int>();
  if (doc["viewportWidth"].is<int>()) out.viewportWidth = doc["viewportWidth"].as<int>();
  if (doc["viewportHeight"].is<int>()) out.viewportHeight = doc["viewportHeight"].as<int>();
  if (doc["lineCompression"].is<float>()) out.lineCompression = doc["lineCompression"].as<float>();
  if (doc["extraParagraphSpacing"].is<int>())
    out.extraParagraphSpacing = doc["extraParagraphSpacing"].as<int>() != 0;
  if (doc["forceParagraphIndents"].is<int>())
    out.forceParagraphIndents = doc["forceParagraphIndents"].as<int>() != 0;
  if (doc["paragraphAlignment"].is<int>()) out.paragraphAlignment = doc["paragraphAlignment"].as<int>();
  if (doc["hyphenationEnabled"].is<int>()) out.hyphenationEnabled = doc["hyphenationEnabled"].as<int>() != 0;
  if (doc["embeddedStyle"].is<int>()) out.embeddedStyle = doc["embeddedStyle"].as<int>() != 0;
  if (doc["imageRendering"].is<int>()) out.imageRendering = doc["imageRendering"].as<int>();
  if (doc["bionicReadingEnabled"].is<int>())
    out.bionicReadingEnabled = doc["bionicReadingEnabled"].as<int>() != 0;
  if (doc["guideReadingEnabled"].is<int>())
    out.guideReadingEnabled = doc["guideReadingEnabled"].as<int>() != 0;
  // CrumBLE reversion fields. Missing values default to 0 / "" which the
  // manifest writer will still emit (so the device-side prompt knows whether
  // a given field is reverse-applicable).
  if (doc["orientation"].is<int>()) out.orientation = doc["orientation"].as<int>();
  if (doc["screenMargin"].is<int>()) out.screenMargin = doc["screenMargin"].as<int>();
  if (doc["fontFamily"].is<int>()) out.fontFamily = doc["fontFamily"].as<int>();
  if (doc["fontSize"].is<int>()) out.fontSize = doc["fontSize"].as<int>();
  if (doc["sdFontSizeRange"].is<int>()) out.sdFontSizeRange = doc["sdFontSizeRange"].as<int>();
  if (doc["sdFontFamilyName"].is<const char*>()) out.sdFontFamilyName = doc["sdFontFamilyName"].as<const char*>();
  if (doc["lineSpacing"].is<int>()) out.lineSpacing = doc["lineSpacing"].as<int>();

  LOG_INF("CFG",
          "device=%s settings (from file): fontId=%d viewport=%ux%u lineCompression=%.3f "
          "extraPS=%d fpI=%d pA=%u hyph=%d embed=%d imgR=%u bionic=%d guide=%d",
          out.device.empty() ? "(unknown)" : out.device.c_str(), out.fontId, out.viewportWidth, out.viewportHeight,
          static_cast<double>(out.lineCompression), out.extraParagraphSpacing, out.forceParagraphIndents,
          out.paragraphAlignment, out.hyphenationEnabled, out.embeddedStyle, out.imageRendering,
          out.bionicReadingEnabled, out.guideReadingEnabled);
  return true;
}

void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [options] <epub> [<epub> ...]\n"
               "\n"
               "Options:\n"
               "  --output-dir <dir>     Write cache state into <dir>/.crosspoint/epub_<hash>/\n"
               "                         instead of next to each input EPUB.\n"
               "  --sd-mount <path>      Alias for --output-dir; self-documents the SD-card\n"
               "                         workflow.\n"
               "  --device-path <path>   Override the SD-relative path the cache-dir hash is\n"
               "                         computed from. Required when the EPUB lives anywhere\n"
               "                         other than the SD root on-device (e.g. /Books/X.epub).\n"
               "                         Only valid with a single input EPUB. Defaults to\n"
               "                         \"/\" + filename, matching the drop-at-root workflow.\n"
               "  --device-url <url>     REQUIRED. Pull live reader settings from the device's\n"
               "                         File Transfer web server. e.g.\n"
               "                            --device-url http://192.168.5.158\n"
               "                         Reads /api/reader-render-info (fitVersion >= 2) which\n"
               "                         returns the user's font, viewport, layout settings, and\n"
               "                         device target (X4 / X3). Every real device has at least\n"
               "                         one customized setting, so a factory-defaults build\n"
               "                         would fail the device's load-time fingerprint check\n"
               "                         and trigger an on-board rebuild -- defeating the point\n"
               "                         of prebaking. Use --skip-sections if you only need\n"
               "                         book.bin + thumbs and don't have a device available.\n"
               "  --skip-sections        Generate book.bin + thumbs only, no sections/. Removes\n"
               "                         the --device-url requirement. Useful for batch-\n"
               "                         processing on systems without device network access;\n"
               "                         the resulting cache still saves ~3s of cold-open time\n"
               "                         from the OPF + thumb-gen skip.\n"
               "  --skip-thumbs          Skip cover-thumb generation. The device's reader\n"
               "                         generates the same thumbs on first cover render, so\n"
               "                         they're optional for the prebake flow. Use this when\n"
               "                         the cover is a progressive JPEG (the bundled JPEGDEC\n"
               "                         decoder crashes on those) or for batches where you\n"
               "                         don't want a single bad cover to halt the run.\n"
               "  --settings-file <path> Load render-info from a JSON file on disk instead\n"
               "                         of fetching it from a live device. The file must\n"
               "                         contain the same JSON schema as /api/reader-render-info\n"
               "                         (fitVersion >= 2). Use this for batch processing,\n"
               "                         CI, or any flow where the device URL isn't reachable\n"
               "                         but the user's render-info is already known. Mutually\n"
               "                         exclusive with --device-url; one of the two is required\n"
               "                         when generating sections. This is also the entry point\n"
               "                         the WASM build uses: optimizer.js writes the settings\n"
               "                         JSON into MEMFS and points at it via this flag.\n"
               "  --check                Skip books whose existing book.bin is fresh against\n"
               "                         the input EPUB's mtime.\n"
               "  --verbose              Per-step timing on stderr.\n"
               "  -h, --help             Show this help.\n",
               argv0);
}

struct Options {
  std::string outputDir;
  std::string devicePathOverride;
  std::string deviceUrl;     // base URL like "http://192.168.5.158"; query
                             // /api/reader-render-info for live settings.
                             // Required unless skipSections or settingsFile is set.
  std::string settingsFile;  // local JSON file with the same schema as
                             // /api/reader-render-info. Alternative to
                             // --device-url for batch / CI / WASM flows
                             // where the device URL isn't reachable.
  std::vector<std::string> epubs;
  bool check = false;
  bool verbose = false;
  bool skipSections = false;  // skip section gen; book.bin + thumbs only.
                              // Removes the settings-source requirement.
  bool skipThumbs = false;    // skip cover-thumb pipeline. The device's
                              // reader generates the same thumbs on first
                              // cover render anyway, so they're optional
                              // for the prebake flow. Useful when the cover
                              // is a progressive JPEG (JPEGDEC crashes on
                              // those) or when batching books and you don't
                              // want a single bad cover to halt the run.
};

bool parseArgs(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else if ((a == "--output-dir" || a == "--sd-mount") && i + 1 < argc) {
      out.outputDir = argv[++i];
    } else if (a == "--device-path" && i + 1 < argc) {
      out.devicePathOverride = argv[++i];
    } else if (a == "--device-url" && i + 1 < argc) {
      out.deviceUrl = argv[++i];
      // Strip a trailing slash so callers can pass either "http://x" or
      // "http://x/" without the joined URL ending up as "/api/.." vs
      // "//api/..".
      if (!out.deviceUrl.empty() && out.deviceUrl.back() == '/') out.deviceUrl.pop_back();
    } else if (a == "--settings-file" && i + 1 < argc) {
      out.settingsFile = argv[++i];
    } else if (a == "--skip-sections") {
      out.skipSections = true;
    } else if (a == "--skip-thumbs") {
      out.skipThumbs = true;
    } else if (a == "--check") {
      out.check = true;
    } else if (a == "--verbose") {
      out.verbose = true;
    } else if (a.rfind("--", 0) == 0) {
      std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
      return false;
    } else {
      out.epubs.push_back(a);
    }
  }
  if (out.epubs.empty()) {
    std::fprintf(stderr, "Error: at least one EPUB path required.\n");
    return false;
  }
  if (!out.devicePathOverride.empty() && out.epubs.size() != 1) {
    std::fprintf(stderr,
                 "Error: --device-path is only valid with a single input EPUB "
                 "(got %zu).\n", out.epubs.size());
    return false;
  }
  if (!out.devicePathOverride.empty() && out.devicePathOverride.front() != '/') {
    std::fprintf(stderr,
                 "Error: --device-path must be absolute (start with '/'). Got: %s\n",
                 out.devicePathOverride.c_str());
    return false;
  }
  if (!out.deviceUrl.empty() && !out.settingsFile.empty()) {
    std::fprintf(stderr,
                 "Error: --device-url and --settings-file are mutually exclusive. Pick one\n"
                 "       source for the render-info -- live device fetch OR local JSON file.\n");
    return false;
  }
  if (out.deviceUrl.empty() && out.settingsFile.empty() && !out.skipSections) {
    std::fprintf(stderr,
                 "Error: one of --device-url, --settings-file, or --skip-sections is required.\n"
                 "       --device-url <url>     fetch live render-info from the device\n"
                 "       --settings-file <p>    load render-info JSON from disk (batch / WASM)\n"
                 "       --skip-sections        emit book.bin + thumbs only, no sections/\n");
    return false;
  }
  return true;
}

// Compute the per-book cache directory the device would use for this
// EPUB. Mirrors Epub::cachePathForFilePath: cacheRoot + "/epub_" +
// fnvHash64(filepath). The hash is over the path string AS THE DEVICE
// SEES IT -- so the caller has to pass the SD-card-relative path here,
// not the local filesystem path.
std::string deviceCacheDir(const std::string& cacheRoot, const std::string& devicePath) {
  return cacheRoot + "/epub_" + std::to_string(ZipFile::fnvHash64(devicePath.c_str(), devicePath.size()));
}

// Drive the same parser chain Epub::load uses, but populate a
// BookMetadataCache directly and call buildBookBin to emit the output.
// Returns true on success. `epubPath` is the path TO THE INPUT FILE on
// the host filesystem; `cacheDir` is where the per-book artifacts land.
// On success, `outMetadata` is populated with the parsed metadata so
// downstream phases (2A cover thumbs, 2C section files) can pick up
// coverItemHref / spine info without re-parsing the OPF.
bool prebakeBookBin(const std::string& epubPath, const std::string& cacheDir,
                    BookMetadataCache::BookMetadata* outMetadata = nullptr) {
  if (!fs::exists(epubPath)) {
    LOG_ERR("PRE", "input EPUB does not exist: %s", epubPath.c_str());
    return false;
  }
  std::error_code ec;
  fs::create_directories(cacheDir, ec);
  if (ec) {
    LOG_ERR("PRE", "could not create cache dir %s: %s", cacheDir.c_str(), ec.message().c_str());
    return false;
  }

  // Mirror Epub::load's slow-path orchestration. BookMetadataCache lives
  // inside cacheDir; the parsers stream their input from the EPUB ZIP
  // and write their progressive output via the cache's writer API.
  BookMetadataCache cache(cacheDir);
  if (!cache.beginWrite()) {
    LOG_ERR("PRE", "BookMetadataCache::beginWrite failed for %s", epubPath.c_str());
    return false;
  }

  // ---- Stage 1: container.xml -> content.opf path ----
  std::string contentOpfFilePath;
  {
    const char* containerPath = "META-INF/container.xml";
    size_t containerSize = 0;
    ZipFile zip(epubPath);
    if (!zip.getInflatedFileSize(containerPath, &containerSize)) {
      LOG_ERR("PRE", "could not size META-INF/container.xml in %s", epubPath.c_str());
      return false;
    }
    ContainerParser containerParser(containerSize);
    if (!containerParser.setup()) {
      LOG_ERR("PRE", "ContainerParser setup failed");
      return false;
    }
    if (!zip.readFileToStream(containerPath, containerParser, 512)) {
      LOG_ERR("PRE", "could not read container.xml");
      return false;
    }
    if (containerParser.fullPath.empty()) {
      LOG_ERR("PRE", "container.xml had no rootfile");
      return false;
    }
    contentOpfFilePath = std::move(containerParser.fullPath);
  }

  const std::string contentBasePath =
      contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  // ---- Stage 2: content.opf -> spine + cover + metadata + (NCX path | NAV path) ----
  BookMetadataCache::BookMetadata bookMetadata;
  std::string tocNcxItem, tocNavItem;
  {
    if (!cache.beginContentOpfPass()) {
      LOG_ERR("PRE", "beginContentOpfPass failed");
      return false;
    }
    LOG_INF("PRE", "opf trace: opening zip");
    ZipFile zip(epubPath);
    LOG_INF("PRE", "opf trace: sizing content.opf at %s", contentOpfFilePath.c_str());
    size_t contentOpfSize = 0;
    if (!zip.getInflatedFileSize(contentOpfFilePath.c_str(), &contentOpfSize)) {
      LOG_ERR("PRE", "could not size content.opf");
      return false;
    }
    LOG_INF("PRE", "opf trace: size=%zu base=%s cache=%s", contentOpfSize, contentBasePath.c_str(), cacheDir.c_str());
    ContentOpfParser opfParser(cacheDir, contentBasePath, contentOpfSize, &cache);
    LOG_INF("PRE", "opf trace: parser constructed");
    if (!opfParser.setup()) {
      LOG_ERR("PRE", "ContentOpfParser setup failed");
      return false;
    }
    LOG_INF("PRE", "opf trace: setup ok; streaming");
    if (!zip.readFileToStream(contentOpfFilePath.c_str(), opfParser, 1024)) {
      LOG_ERR("PRE", "could not read content.opf");
      return false;
    }
    LOG_INF("PRE", "opf trace: streaming done");
    bookMetadata.title = opfParser.title;
    bookMetadata.author = opfParser.author;
    bookMetadata.language = opfParser.language;
    bookMetadata.coverItemHref = opfParser.coverItemHref;
    // The "start reading" location pulled from <guide reference type="text">
    // (or "start" as fallback). Persisted in book.bin so QuickResume and the
    // first-render path don't have to re-parse content.opf to find where the
    // body content begins. See Epub.cpp:351 for the same assignment on-device.
    bookMetadata.textReferenceHref = opfParser.textReferenceHref;
    // tocNcxPath / tocNavPath are absolute (ZIP-root-relative); the OPF
    // parser has already resolved them against contentBasePath.
    if (!opfParser.tocNcxPath.empty()) tocNcxItem = opfParser.tocNcxPath;
    if (!opfParser.tocNavPath.empty()) tocNavItem = opfParser.tocNavPath;
    if (!cache.endContentOpfPass()) {
      LOG_ERR("PRE", "endContentOpfPass failed");
      return false;
    }
  }

  // ---- Stage 3: NCX (preferred) or NAV (fallback) -> TOC entries ----
  {
    if (!cache.beginTocPass()) {
      LOG_ERR("PRE", "beginTocPass failed");
      return false;
    }
    if (!tocNcxItem.empty()) {
      ZipFile zip(epubPath);
      size_t ncxSize = 0;
      if (zip.getInflatedFileSize(tocNcxItem.c_str(), &ncxSize)) {
        TocNcxParser ncxParser(contentBasePath, ncxSize, &cache);
        if (ncxParser.setup() && zip.readFileToStream(tocNcxItem.c_str(), ncxParser, 1024)) {
          LOG_DBG("PRE", "parsed NCX TOC from %s", tocNcxItem.c_str());
        } else {
          LOG_ERR("PRE", "NCX parse failed for %s", tocNcxItem.c_str());
        }
      }
    } else if (!tocNavItem.empty()) {
      ZipFile zip(epubPath);
      size_t navSize = 0;
      if (zip.getInflatedFileSize(tocNavItem.c_str(), &navSize)) {
        TocNavParser navParser(contentBasePath, navSize, &cache);
        if (navParser.setup() && zip.readFileToStream(tocNavItem.c_str(), navParser, 1024)) {
          LOG_DBG("PRE", "parsed NAV TOC from %s", tocNavItem.c_str());
        } else {
          LOG_ERR("PRE", "NAV parse failed for %s", tocNavItem.c_str());
        }
      }
    }
    if (!cache.endTocPass()) {
      LOG_ERR("PRE", "endTocPass failed");
      return false;
    }
  }

  if (!cache.endWrite()) {
    LOG_ERR("PRE", "endWrite failed");
    return false;
  }
  if (!cache.buildBookBin(epubPath, bookMetadata)) {
    LOG_ERR("PRE", "buildBookBin failed");
    return false;
  }
  cache.cleanupTmpFiles();
  if (outMetadata) *outMetadata = bookMetadata;
  return true;
}

// Phase 2A: emit a single thumb_<W>x<H>.bmp for the EPUB's cover. Mirrors
// Epub::convertCoverToThumbBmp from lib/Epub/Epub.cpp: extract the cover
// image to a temp file inside cacheDir, hand it to the appropriate
// converter (JPEG or PNG), let the converter write the dithered 1-bit BMP
// to thumbPath, then clean up the temp file. adaptiveContain=false here
// because the LyraCarousel canonical thumbs use crop-to-fill, not
// letterbox; _fit.bmp variants would pass true.
//
// Returns false if the cover is empty, unsupported, or any step in the
// extract/decode pipeline fails. The output file at thumbPath is removed
// on failure so subsequent runs see the same "no thumb yet" state.
bool prebakeCoverThumb(const std::string& epubPath, const std::string& cacheDir, const std::string& coverItemHref,
                      int width, int height) {
  if (coverItemHref.empty()) return false;

  // Pick converter + temp-file extension from the cover's filename
  // (mirrors the device's hasJpgExtension/hasPngExtension dispatch).
  const bool isJpg = FsHelpers::hasJpgExtension(coverItemHref);
  const bool isPng = !isJpg && FsHelpers::hasPngExtension(coverItemHref);
  if (!isJpg && !isPng) {
    LOG_ERR("PRE", "unsupported cover format for thumb: %s", coverItemHref.c_str());
    return false;
  }

  const std::string thumbPath =
      cacheDir + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  const std::string coverTmpPath = cacheDir + (isJpg ? "/.cover.jpg" : "/.cover.png");

  // Extract the cover image from the EPUB ZIP into the temp file.
  // ZipFile::readFileToStream writes via the Print interface, which our
  // HalFile shim implements -- the same path Phase 1's XML parser uses.
  {
    ZipFile zip(epubPath);
    FsFile coverTmp;
    if (!Storage.openFileForWrite("PRE", coverTmpPath, coverTmp)) {
      LOG_ERR("PRE", "could not open temp cover for write: %s", coverTmpPath.c_str());
      return false;
    }
    if (!zip.readFileToStream(coverItemHref.c_str(), coverTmp, 1024)) {
      LOG_ERR("PRE", "could not read cover from EPUB: %s", coverItemHref.c_str());
      coverTmp.close();
      Storage.remove(coverTmpPath.c_str());
      return false;
    }
    coverTmp.close();
  }

  // Reopen the temp cover for reading and open the destination BMP.
  FsFile coverIn;
  if (!Storage.openFileForRead("PRE", coverTmpPath, coverIn)) {
    Storage.remove(coverTmpPath.c_str());
    return false;
  }
  FsFile thumbOut;
  if (!Storage.openFileForWrite("PRE", thumbPath, thumbOut)) {
    coverIn.close();
    Storage.remove(coverTmpPath.c_str());
    return false;
  }

  // Drive the same converter the device uses. 1-bit output is what the
  // LyraCarousel home theme renders -- not the multi-bit variant. False
  // for adaptiveContain because the carousel uses crop-to-fill, NOT
  // letterbox/contain (_fit.bmp variants do, but those aren't in the
  // 2A MVP canonical set).
  bool ok = false;
  if (isJpg) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverIn, thumbOut, width, height, false);
  } else {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverIn, thumbOut, width, height, false);
  }

  coverIn.close();
  thumbOut.close();
  Storage.remove(coverTmpPath.c_str());

  if (!ok) {
    LOG_ERR("PRE", "thumb gen failed: %s", thumbPath.c_str());
    Storage.remove(thumbPath.c_str());
  } else {
    LOG_INF("PRE", "wrote thumb %dx%d -> %s", width, height, thumbPath.c_str());
  }
  return ok;
}

// Phase 2C: emit sections/<spineIdx>.bin for every spine entry in the
// EPUB, byte-targeting the device's section file format. Loads an Epub
// instance from the same on-disk cache Phase 1 just wrote, then loops
// the spine and calls Section::createSectionFile per chapter.
//
// fontId, viewport, and the eight layout/setting flags are the same
// inputs the device's reader activity passes from SETTINGS. To stay
// byte-identical, these MUST match the active device default for the
// target user. For first acceptance we pick the X4 (800x480) factory
// defaults: LexendDeca-14 + hyphenation on + embedded style on +
// imageRendering 0 ("fit") + bionic/guide off + line compression 1.0f
// + paragraph alignment 0 ("default") + no extra spacing/indents.
//
// Returns the number of spine entries that failed to emit (0 = clean).
int prebakeSections(const std::string& epubPath, const std::string& realCacheDir,
                    const std::string& /*cacheDirParent*/, GfxRenderer& renderer,
                    const SectionSettings& s) {
  // Epub computes cachePath as cacheDir + "/epub_" + fnvHash(filepath).
  // The HOST has output at "<outputDir>/.crosspoint/epub_<deviceHash>", but
  // the DEVICE will see the same files at "/.crosspoint/epub_<deviceHash>"
  // (SD root is "/" on device). Section files embed image paths -- those
  // paths must be the DEVICE-VISIBLE path, not the host's outputDir-rooted
  // path, or the device's image lookups all fail and chapter renders go
  // blank.
  //
  // So we do path substitute against the DEVICE-RELATIVE prefix, not
  // realCacheDir. Length-preserving substitute keeps file sizes + LUT
  // offsets intact -- pick shadowParent length so
  // "<shadowParent>/epub_<hostHash>" == "/.crosspoint/epub_<deviceHash>".
  //
  // The deviceCacheDir on SD is literally "/.crosspoint/epub_<deviceHash>" --
  // we derive it from realCacheDir by stripping the host outputDir prefix.
  // realCacheDir always ends in "/.crosspoint/epub_<deviceHash>" because
  // main() builds it that way.
  const std::string deviceCacheDirOnSd = [&]() {
    const std::string suffix = "/.crosspoint/";
    const size_t pos = realCacheDir.find(suffix);
    if (pos == std::string::npos) return realCacheDir;  // unexpected; fall back
    return realCacheDir.substr(pos);  // includes the leading "/.crosspoint/..."
  }();

  const std::string hostHashStr =
      std::to_string(ZipFile::fnvHash64(epubPath.c_str(), epubPath.size()));
  const size_t hostSuffixLen = 6 + hostHashStr.size();  // "/epub_" + hash
  const size_t targetTotalLen = deviceCacheDirOnSd.size();
  std::string shadowParent;
  bool pathSubstituteEnabled = false;
  if (targetTotalLen > hostSuffixLen + 4) {  // need at least "/tmp/" parent prefix
    const size_t parentLen = targetTotalLen - hostSuffixLen;
    // Compose "/tmp/" + (parentLen-5) padding chars.
    shadowParent = "/tmp/" + std::string(parentLen - 5, 'p');
    if (shadowParent.size() == parentLen) {
      pathSubstituteEnabled = true;
    }
  }
  if (!pathSubstituteEnabled) {
    LOG_INF("PRE",
            "hash length mismatch prevents path substitute; sections will embed shadow paths and "
            "the device will runtime-decode chapter images instead of using bundled .pxc");
    shadowParent = "/tmp/inkprebake_fallback";
  }
  const std::string shadowCacheDir = shadowParent + "/epub_" + hostHashStr;

  std::error_code ec;
  fs::create_directories(shadowCacheDir, ec);
  if (ec) {
    LOG_ERR("PRE", "could not create shadow cache dir %s: %s", shadowCacheDir.c_str(), ec.message().c_str());
    return -1;
  }
  fs::copy_file(realCacheDir + "/book.bin", shadowCacheDir + "/book.bin",
                fs::copy_options::overwrite_existing, ec);
  if (ec) {
    LOG_ERR("PRE", "could not shadow-copy book.bin from %s: %s", realCacheDir.c_str(), ec.message().c_str());
    return -1;
  }

  if (pathSubstituteEnabled) {
    LOG_INF("PRE", "path substitute enabled: shadow=%s device=%s (len=%zu)",
            shadowCacheDir.c_str(), deviceCacheDirOnSd.c_str(), shadowCacheDir.size());
  }

  // Construct an Epub instance whose internal cachePath resolves to the
  // shadow location.
  auto epub = std::make_shared<Epub>(epubPath, shadowParent);
  if (!epub->load(/*buildIfMissing=*/false, /*skipLoadingCss=*/false)) {
    LOG_ERR("PRE", "Epub::load failed for %s (shadow=%s)", epubPath.c_str(), shadowCacheDir.c_str());
    return -1;
  }
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    LOG_INF("PRE", "no spine entries; skipping section gen");
    return 0;
  }
  LOG_INF("PRE", "section gen: %d spine entries to build", spineCount);

  int failures = 0;
  for (int spineIdx = 0; spineIdx < spineCount; ++spineIdx) {
    Section section(epub, spineIdx, renderer);
    bool imagesWereSuppressed = false;
    bool layoutAbortedForLowMemory = false;
    // EXPERIMENTAL (2C.8 workaround): force embeddedStyle=false to ChapterHtmlSlimParser
    // so it produces ZERO-margin BlockStyle (matching device's rebuild behavior). The
    // device's renderer evidently applies CSS margins at draw time using its own CssParser
    // lookup, not from BlockStyle, so shipping BlockStyle margins double-applies them and
    // breaks pagination. We patch the header byte AFTER the build to report the
    // user's real embeddedStyle setting so device's settings-fingerprint check passes.
    const bool ok = section.createSectionFile(
        s.fontId, s.lineCompression, s.extraParagraphSpacing, s.forceParagraphIndents, s.paragraphAlignment,
        s.viewportWidth, s.viewportHeight, s.hyphenationEnabled, /*embeddedStyle=*/false, s.imageRendering,
        s.bionicReadingEnabled, s.guideReadingEnabled, /*popupFn=*/nullptr, &imagesWereSuppressed,
        &layoutAbortedForLowMemory);
    if (!ok) {
      LOG_ERR("PRE", "section %d FAILED", spineIdx);
      ++failures;
    } else {
      LOG_INF("PRE", "section %d wrote %u pages%s", spineIdx, static_cast<unsigned>(section.pageCount),
              imagesWereSuppressed ? " (images suppressed)" : "");
    }
  }

  // Copy section files back to real cache dir. If path substitution is
  // enabled, rewrite each section file's embedded shadow path -> real
  // device path on the way over. Length-preserving (we matched
  // shadowCacheDir's length to realCacheDir's), so file sizes and offset
  // tables stay intact.
  std::error_code ec2;
  // CrumBLE: write prebake sections to a SIDE path (sections-prebake/) so the
  // device's Section::clearCache (which eats sections/<n>.bin on fingerprint
  // mismatch when the user changes a reader setting) can never destroy the
  // prebake artifact. Section::loadSectionFile tries sections/ first, then
  // falls back to sections-prebake/ when fingerprints match -- enabling
  // "revert your settings, the prebake cache is still there to pick back up
  // where you left off" without forcing a re-run of the optimizer.
  const std::string realSectionsDir = realCacheDir + "/sections-prebake";
  fs::create_directories(realSectionsDir, ec2);
  if (ec2) {
    LOG_ERR("PRE", "could not create real sections dir %s: %s", realSectionsDir.c_str(), ec2.message().c_str());
    return failures;
  }

  int copiedSections = 0;
  for (int spineIdx = 0; spineIdx < spineCount; ++spineIdx) {
    const std::string srcPath = shadowCacheDir + "/sections/" + std::to_string(spineIdx) + ".bin";
    const std::string dstPath = realSectionsDir + "/" + std::to_string(spineIdx) + ".bin";
    if (!fs::exists(srcPath)) continue;
    // Read full file, do prefix substitute, write to real location.
    std::ifstream in(srcPath, std::ios::binary);
    if (!in) continue;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (pathSubstituteEnabled) {
      // In-place substring replace -- shadow and device prefixes are the same
      // length, so we can walk the buffer with std::string::find. The target
      // is the DEVICE-VISIBLE path (e.g. "/.crosspoint/epub_<deviceHash>"),
      // not the host's outputDir-rooted realCacheDir -- the device sees its
      // SD root as "/" and won't find files under our host outputDir prefix.
      size_t pos = 0;
      while ((pos = content.find(shadowCacheDir, pos)) != std::string::npos) {
        std::memcpy(content.data() + pos, deviceCacheDirOnSd.data(), deviceCacheDirOnSd.size());
        pos += deviceCacheDirOnSd.size();
      }
    }
    // 2C.8 workaround: patch the embeddedStyle byte (offset 21 in header) to
    // match the user's real setting. We built with embeddedStyle=false so the
    // parser produced zero-margin BlockStyle, but the header must report the
    // user's real value so device's section-load fingerprint check passes.
    if (content.size() > 21) {
      content[21] = static_cast<char>(s.embeddedStyle ? 1 : 0);
    }
    std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
    if (!out) continue;
    out.write(content.data(), content.size());
    out.close();
    ++copiedSections;
  }
  LOG_INF("PRE", "copied %d section files to real cache dir%s", copiedSections,
          pathSubstituteEnabled ? " (with path substitute)" : "");

  // Also carry over the image artifacts that ChapterHtmlSlimParser extracted
  // into the shadow dir during section build: img_<spine>_<n>.jpg (or .png)
  // plus the optimizer-bundled .pxc files that the parser carried over from
  // the EPUB zip. Plus css_rules.cache if present. Without these the device
  // falls back to runtime image decode -- ~5-7 s per full-size image on
  // first display, which is exactly what the optimizer's .pxc baking exists
  // to avoid.
  int copiedImages = 0;
  int copiedPxc = 0;
  for (const auto& entry : fs::directory_iterator(shadowCacheDir, ec2)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("img_", 0) == 0 || name == "css_rules.cache") {
      fs::copy_file(entry.path(), realCacheDir + "/" + name,
                    fs::copy_options::overwrite_existing, ec2);
      if (!ec2) {
        if (name.size() > 4 && name.substr(name.size() - 4) == ".pxc") ++copiedPxc;
        else ++copiedImages;
      }
      ec2.clear();
    }
  }
  LOG_INF("PRE", "copied %d image files + %d pxc caches + css to real cache dir", copiedImages, copiedPxc);

  // CrumBLE: write the 12-field fingerprint as a tiny JSON sidecar
  // (prebake-manifest.json) so the device's switch-back prompt can detect
  // the prebake's existence AFTER the section files have been overwritten
  // by chapter rebuilds under mismatched settings. Without this, the moment
  // the user changes a setting and reads through the book, Section.cpp's
  // clearCache() deletes each section file and the prebake's fingerprint
  // becomes unrecoverable -- they'd have to re-run the optimizer to get
  // the prompt back, even after reverting their settings.
  //
  // The manifest is self-contained: doesn't depend on any section file
  // existing, and survives chapter rebuilds because Section.cpp only
  // touches per-section files. ~250 bytes of SD per book.
  {
    JsonDocument mdoc;
    mdoc["v"] = 1;  // schema version; bump if fields are added
    mdoc["fontId"] = s.fontId;
    mdoc["lineCompression"] = s.lineCompression;
    mdoc["extraParagraphSpacing"] = s.extraParagraphSpacing ? 1 : 0;
    mdoc["forceParagraphIndents"] = s.forceParagraphIndents ? 1 : 0;
    mdoc["paragraphAlignment"] = static_cast<int>(s.paragraphAlignment);
    mdoc["viewportWidth"] = static_cast<int>(s.viewportWidth);
    mdoc["viewportHeight"] = static_cast<int>(s.viewportHeight);
    mdoc["hyphenationEnabled"] = s.hyphenationEnabled ? 1 : 0;
    mdoc["embeddedStyle"] = s.embeddedStyle ? 1 : 0;
    mdoc["imageRendering"] = static_cast<int>(s.imageRendering);
    mdoc["bionicReadingEnabled"] = s.bionicReadingEnabled ? 1 : 0;
    mdoc["guideReadingEnabled"] = s.guideReadingEnabled ? 1 : 0;
    // CrumBLE reversion fields. The device-side "Use prepared layout?"
    // prompt applies these RAW SETTINGS values on confirm so the fingerprint
    // check then matches the prebake'd cache.
    mdoc["orientation"] = static_cast<int>(s.orientation);
    mdoc["screenMargin"] = static_cast<int>(s.screenMargin);
    mdoc["fontFamily"] = static_cast<int>(s.fontFamily);
    mdoc["fontSize"] = static_cast<int>(s.fontSize);
    mdoc["sdFontSizeRange"] = static_cast<int>(s.sdFontSizeRange);
    mdoc["sdFontFamilyName"] = s.sdFontFamilyName;
    mdoc["lineSpacing"] = static_cast<int>(s.lineSpacing);
    std::string mjson;
    serializeJson(mdoc, mjson);
    const std::string manifestPath = realCacheDir + "/prebake-manifest.json";
    std::ofstream mfile(manifestPath, std::ios::binary | std::ios::trunc);
    if (mfile) {
      mfile.write(mjson.data(), static_cast<std::streamsize>(mjson.size()));
      mfile.close();
      LOG_INF("PRE", "wrote prebake-manifest.json (%zu bytes)", mjson.size());
    } else {
      LOG_ERR("PRE", "could not write prebake-manifest.json at %s", manifestPath.c_str());
    }
  }

  return failures;
}

// Convenience: emit the full Phase 2A MVP thumb set (296x468 + 200x390).
// One log line per size; we don't bail the whole run if one size fails so
// the user sees both attempts (a corrupt-cover EPUB might fail at the
// larger size but succeed at the smaller, or vice versa, and we want the
// transcript to show that).
int prebakeAllThumbs(const std::string& epubPath, const std::string& cacheDir,
                     const std::string& coverItemHref) {
  if (coverItemHref.empty()) {
    LOG_INF("PRE", "no cover image href; skipping thumb gen");
    return 0;
  }
  // Canonical thumb sizes -- revised 2A.3-revealed set, see DESIGN.md for
  // the screen each one renders on. 222x370 and 192x320 cover the common
  // home screens (Base/non-Carousel theme + LyraFlow sleep screen); 100x150
  // covers Bookshelf grid cells. LyraCarousel-specific sizes (296x468 and
  // 200x390) are NOT included here -- they're only generated on cover-miss
  // self-heal and only when LyraCarousel is the active theme. Add a per-
  // theme override flag once we have telemetry on which themes users run.
  constexpr int kThumbSizes[][2] = {
      {222, 370},  // Base/non-Carousel home cover
      {192, 320},  // LyraFlow sleep-screen center cover
      {100, 150},  // Bookshelf grid cell / recents list
  };
  int failures = 0;
  for (const auto& [w, h] : kThumbSizes) {
    if (!prebakeCoverThumb(epubPath, cacheDir, coverItemHref, w, h)) ++failures;
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parseArgs(argc, argv, opts)) {
    usage(argv[0]);
    return 2;
  }

  // Phase 2C: construct the font + renderer up-front so the per-EPUB
  // loop can pass them in. Single shared GfxRenderer holds the font
  // map; expanding to multiple sizes/families means more EpdFontFamily
  // instances + insertFont calls here. fontIds.h defines the integer
  // hash constants the device + host both bake into section headers.
  EpdFont lexenddeca14Regular(&lexenddeca_14_regular);
  EpdFont lexenddeca14Bold(&lexenddeca_14_bold);
  EpdFont lexenddeca14Italic(&lexenddeca_14_italic);
  EpdFont lexenddeca14BoldItalic(&lexenddeca_14_bolditalic);
  EpdFontFamily lexenddeca14Family(&lexenddeca14Regular, &lexenddeca14Bold,
                                   &lexenddeca14Italic, &lexenddeca14BoldItalic);
  EpdFont bitter12Regular(&bitter_12_regular);
  EpdFont bitter12Bold(&bitter_12_bold);
  EpdFont bitter12Italic(&bitter_12_italic);
  EpdFont bitter12BoldItalic(&bitter_12_bolditalic);
  EpdFontFamily bitter12Family(&bitter12Regular, &bitter12Bold,
                               &bitter12Italic, &bitter12BoldItalic);

  GfxRenderer renderer;
  renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14Family);
  renderer.insertFont(BITTER_12_FONT_ID, bitter12Family);

  // Resolve section settings. Section gen needs the same 12-field header
  // payload the device computes; we can get it from a live device fetch
  // (--device-url) or a local JSON file (--settings-file). Arg parsing has
  // already enforced exactly one of those is set when sections are on.
  // Without sections (book.bin + thumbs only), the settings struct is
  // constructed but unused.
  SectionSettings sectionSettings;
  if (!opts.skipSections) {
    bool settingsOk = false;
    if (!opts.settingsFile.empty()) {
      settingsOk = loadSettingsFromFile(opts.settingsFile, sectionSettings);
      if (!settingsOk) {
        LOG_ERR("CLI", "Could not load settings from %s -- aborting.", opts.settingsFile.c_str());
        return 3;
      }
    } else {
      settingsOk = fetchDeviceSettings(opts.deviceUrl, sectionSettings);
      if (!settingsOk) {
        LOG_ERR("CLI", "Could not fetch settings from %s -- aborting.", opts.deviceUrl.c_str());
        return 3;
      }
    }
    // Sanity-check the device target. Today only X4 and X3 ship; a new
    // SKU should warn but proceed using the reported viewport so we
    // don't fail closed on hardware we haven't validated against.
    if (sectionSettings.device != "X4" && sectionSettings.device != "X3") {
      LOG_INF("CLI",
              "Unrecognized device target '%s' from render-info. Continuing with the reported "
              "viewport (%dx%d), but verify behaviour against the actual device.",
              sectionSettings.device.c_str(), sectionSettings.viewportWidth, sectionSettings.viewportHeight);
    } else {
      LOG_INF("CLI", "device target detected: %s", sectionSettings.device.c_str());
    }
    renderer.setViewport(sectionSettings.viewportWidth, sectionSettings.viewportHeight);
  } else {
    LOG_INF("CLI", "--skip-sections set; section generation disabled. Producing book.bin + thumbs only.");
  }

  int failures = 0;
  for (const auto& epubPath : opts.epubs) {
    // Cache-dir layout mirrors the device's /.crosspoint/epub_<hash>/. The
    // hash is computed over the SD-card-absolute path the device sees -- NOT
    // the host filesystem path. Both branches below agree on this derivation;
    // they only differ in where the .crosspoint/ tree itself lands on disk
    // (next to the input EPUB, or under --output-dir / --sd-mount).
    //
    // Default device path: "/" + filename, matching the common "drop EPUBs at
    // SD root" workflow. Override with --device-path when the EPUB lives in a
    // subdirectory on-device (e.g. /Books/X.epub) -- otherwise the cache dir
    // we emit won't match the dir the device generates and a byte-comparison
    // (or a warm-cache drop-in) will silently land in the wrong place.
    const std::string devicePath = opts.devicePathOverride.empty()
                                       ? "/" + fs::path(epubPath).filename().string()
                                       : opts.devicePathOverride;
    const std::string cacheDirSuffix = "/.crosspoint/" +
                                       fs::path(deviceCacheDir("", devicePath)).filename().string();
    const std::string cacheDirParent =
        opts.outputDir.empty() ? fs::path(epubPath).parent_path().string() : opts.outputDir;
    const std::string cacheDir = cacheDirParent + cacheDirSuffix;

    LOG_INF("CLI", "prebake %s -> %s", epubPath.c_str(), cacheDir.c_str());
    const uint32_t t0 = millis();
    BookMetadataCache::BookMetadata bookMetadata;
    const bool ok = prebakeBookBin(epubPath, cacheDir, &bookMetadata);
    const uint32_t dtBookBin = millis() - t0;
    if (!ok) {
      LOG_ERR("CLI", "  book.bin FAILED (%u ms)", dtBookBin);
      ++failures;
      continue;
    }
    LOG_INF("CLI", "  book.bin OK (%u ms) at %s/book.bin", dtBookBin, cacheDir.c_str());

    // Phase 2A: cover thumbs. Continues even on partial failure -- a book
    // with no cover image is fine, we just skip thumbs for it. The exit
    // code only counts book.bin failures since thumbs are recoverable
    // on-device (the reader will generate them itself on first cover
    // render).
    if (opts.skipThumbs) {
      LOG_INF("CLI", "  thumbs SKIPPED (--skip-thumbs); device will generate on first cover render");
    } else {
      const uint32_t t1 = millis();
      const int thumbFails = prebakeAllThumbs(epubPath, cacheDir, bookMetadata.coverItemHref);
      const uint32_t dtThumbs = millis() - t1;
      if (thumbFails == 0) {
        LOG_INF("CLI", "  thumbs OK (%u ms)", dtThumbs);
      } else {
        LOG_INF("CLI", "  thumbs PARTIAL: %d of 3 failed (%u ms)", thumbFails, dtThumbs);
      }
    }

    // Phase 2C: section files. Builds sections/<spineIdx>.bin per spine
    // entry, mirroring the device's reader-activity behavior on first
    // book open. Doesn't bump `failures` -- a section that fails to
    // build is recoverable on-device (the reader self-rebuilds when the
    // user navigates to that chapter), so partial success still ships.
    if (opts.skipSections) {
      LOG_INF("CLI", "  sections SKIPPED (--skip-sections)");
    } else {
      const uint32_t t2 = millis();
      const int sectionFails = prebakeSections(epubPath, cacheDir, cacheDirParent, renderer, sectionSettings);
      const uint32_t dtSections = millis() - t2;
      if (sectionFails == 0) {
        LOG_INF("CLI", "  sections OK (%u ms)", dtSections);
      } else if (sectionFails > 0) {
        LOG_INF("CLI", "  sections PARTIAL: %d failed (%u ms)", sectionFails, dtSections);
      } else {
        LOG_INF("CLI", "  sections SKIPPED (Epub::load failed, %u ms)", dtSections);
      }
    }
  }

  return failures == 0 ? 0 : 1;
}
