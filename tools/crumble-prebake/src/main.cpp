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
#include <Epub/EmbeddedGlyphSubset.h>
#include <Epub/GlyphAtlas.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/parsers/ContainerParser.h>
#include <Epub/parsers/ContentOpfParser.h>
#include <Epub/parsers/TocNavParser.h>
#include <Epub/parsers/TocNcxParser.h>
#include <Utf8.h>

#include <fstream>
#include <unordered_set>

// Phase 2C font tables. Pure-data PROGMEM headers; including them on host
// pulls in the same uint8_t[] bitmaps + metadata tables the device links
// against. Default reading font is LexendDeca Medium (14px). Add the four
// styles a single family needs (regular, bold, italic, bold-italic) here
// when expanding to more font sizes / variants.
// CrumBLE 4.2: Lexend_14 dropped from WASM (~140 KB gzipped) to keep the
// embedded blob under the X3's web-serve heap-fluctuation tolerance.
// Slim firmware doesn't ship Lexend as a built-in either; users with a
// stale LEXENDDECA preference fall through to BITTER via
// getReaderFontId's effectiveFamily mapping, and Lexend-via-SD-cpfont
// (CrumBLE-LexendDeca-SDfont.zip) is the supported path going forward.
// #include <builtinFonts/lexenddeca_14_regular.h>
// #include <builtinFonts/lexenddeca_14_bold.h>
// #include <builtinFonts/lexenddeca_14_italic.h>
// #include <builtinFonts/lexenddeca_14_bolditalic.h>
#include <builtinFonts/bitter_12_regular.h>
#include <builtinFonts/bitter_12_bold.h>
#include <builtinFonts/bitter_12_italic.h>
#include <builtinFonts/bitter_12_bolditalic.h>
// CrumBLE 4.2: Bitter_14 is the slim build's default reader font after
// dropping Lexend Deca built-in. WASM has to register it so prewarm /
// layout calls with fontId=BITTER_14_FONT_ID don't return 0-height
// lines (the symptom that produced "all text stacked at top" on X3).
#include <builtinFonts/bitter_14_regular.h>
#include <builtinFonts/bitter_14_bold.h>
#include <builtinFonts/bitter_14_italic.h>
#include <builtinFonts/bitter_14_bolditalic.h>
#include "fontIds.h"  // LEXENDDECA_14_FONT_ID / BITTER_12_FONT_ID / BITTER_14_FONT_ID
#include <SdCardFont.h>
#include <SdCardFontManager.h>
#include <EpdFontFamily.h>

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
               "  --emit-section-glyph-subsets\n"
               "                         CrumBLE 4.3: after each section is baked, append an\n"
               "                         embedded glyph subset block (intervals + glyphs +\n"
               "                         bitmaps for the codepoints actually used in this\n"
               "                         section) and patch the section file's v39 trailer\n"
               "                         to point at it. Lets on-device load skip the\n"
               "                         SD-font miniData allocation for prebaked sections,\n"
               "                         which is the architectural fix for BT + SD-font\n"
               "                         heap fragmentation. No-op unless --sd-font-path\n"
               "                         is also supplied (built-in fonts don't benefit).\n"
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
  // CrumBLE 4.2: SD-card font input. When the user picks an SD .cpfont in
  // the optimizer preflight modal, JS writes the raw font bytes to MEMFS
  // and passes the path here, plus the family name (for fontId hashing)
  // and the point size of the chosen variant. main() loads the file via
  // SdCardFont::load, computes the fontId via SdCardFontManager's FNV
  // hash, registers it in the renderer, and uses it for the section
  // bake. The same fontId formula runs on-device, so the manifest's
  // baked fontId matches whatever the reader resolves at open time.
  std::string sdFontPath;
  std::string sdFontFamilyName;
  uint8_t sdFontPointSize = 0;
  // CrumBLE 4.3: emit per-section embedded glyph subset blocks (v39 section
  // file format). Opt-in because the work is only meaningful when an SD
  // font is also supplied AND every section is being baked; without those
  // the block is empty / mismatched. Plan: when set, the section loop
  // walks each just-written section's pages, accumulates the codepoints
  // actually used per style, prewarms SdCardFont with that exact set,
  // and serialises SdCardFont's prewarmed mini-data into a glyph subset
  // block appended to the section file. Section trailer's three
  // embeddedGlyphSubsetOffset / Size / CpfontHash uint32_t fields then
  // point at the block so on-device load can validate + install it.
  bool emitSectionGlyphSubsets = false;
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
    } else if (a == "--sd-font-path" && i + 1 < argc) {
      out.sdFontPath = argv[++i];
    } else if (a == "--sd-font-family" && i + 1 < argc) {
      out.sdFontFamilyName = argv[++i];
    } else if (a == "--sd-font-size" && i + 1 < argc) {
      out.sdFontPointSize = static_cast<uint8_t>(std::stoi(argv[++i]));
    } else if (a == "--emit-section-glyph-subsets") {
      out.emitSectionGlyphSubsets = true;
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
// CrumBLE 4.3: after a section file is written by Section::createSectionFile,
// walk its pages, collect the codepoints actually used per style, prewarm
// the SD font with that exact set, and serialise the prewarmed mini-data
// into a glyph subset block appended to the section file. Then patch the
// section's v39 trailer fields (embeddedGlyphSubsetOffset / Size /
// CpfontHash) to point at the block. Returns true on success; on failure
// the section file is left intact (trailer fields stay at 0/0/0, which
// on-device load treats as "no embedded subset" and falls back to the
// existing SdCardFont miss-handler path -- harmless degradation).
//
// Implementation lands incrementally:
//  - this commit: stub that logs the call shape; returns true so the rest
//    of the prebake pipeline continues unchanged.
//  - next commit(s): codepoint collection + prewarm + block serialisation
//    + trailer patching.
bool emitEmbeddedGlyphSubsetForSection(const std::string& sectionPath, Section& section, SdCardFont& font,
                                       int spineIdx);
// CrumBLE 4.4 task #35 step 2a forward decl: buildGlyphAtlasBlock is
// defined later in this file (near the embedded subset emitter) so the
// section-loop call site can reach it without re-ordering the file.
std::vector<uint8_t> buildGlyphAtlasBlock(const SdCardFont& font);

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
                    const SectionSettings& s, SdCardFont* sdFontForSubset,
                    bool emitGlyphSubsets) {
  // CrumBLE 4.3: when emitGlyphSubsets is true AND sdFontForSubset is
  // non-null, the post-createSectionFile step walks each section's pages,
  // collects the codepoints actually used per style, prewarms the
  // SD-card font with those codepoints, and serialises the prewarmed
  // mini-data into an embedded glyph subset block appended to the section
  // file. Section trailer's three v39 fields then point at the block.
  // The flag is silently ignored if no SD font is loaded (no point in
  // baking subsets for built-in fonts, which already live entirely in
  // .rodata on-device).
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
      // CrumBLE 4.3: emit the embedded glyph subset block for this section
      // when the CLI was invoked with --emit-section-glyph-subsets AND an
      // SD font is loaded. Silently skips otherwise (e.g. built-in-font
      // bakes -- they don't benefit from embedding; the .rodata on-device
      // already holds the full glyph set).
      if (emitGlyphSubsets && sdFontForSubset != nullptr) {
        const std::string sectionPath = shadowCacheDir + "/sections/" + std::to_string(spineIdx) + ".bin";
        const bool subsetOk = emitEmbeddedGlyphSubsetForSection(sectionPath, section, *sdFontForSubset, spineIdx);
        if (!subsetOk) {
          // Don't fail the whole prebake on a subset-emit failure -- the
          // section file itself is valid v39 with the trailer fields at
          // 0/0/0, which the on-device load path falls back to (existing
          // SdCardFont miss-handler path). Log + continue.
          LOG_ERR("PRE", "section %d: glyph subset emit FAILED (section file stays v39 with no subset)", spineIdx);
        }
        // CrumBLE 4.4 (v4.4 task #35 step 2b): emit the v40 glyph atlas
        // block too. The subset emit above leaves SdCardFont's mini-data
        // populated with exactly this section's working glyph set, which
        // buildGlyphAtlasBlock then iterates to produce a 1-bit packed
        // atlas. Best-effort like the subset emit; failures leave the v40
        // atlas trailer at 0/0/0 and the device falls back to the v39
        // subset path. Subset emit is the precondition (it primes the
        // mini-data); skipping that case keeps the atlas dimensions
        // matched against what the on-device renderer will actually look
        // up at draw time.
        if (subsetOk) {
          const std::vector<uint8_t> atlasBlock = buildGlyphAtlasBlock(*sdFontForSubset);
          if (!atlasBlock.empty()) {
            std::fstream af(sectionPath, std::ios::in | std::ios::out | std::ios::binary);
            if (!af.is_open()) {
              LOG_ERR("PRE", "section %d: cannot open %s for r+w during atlas emit", spineIdx, sectionPath.c_str());
            } else {
              af.seekp(0, std::ios::end);
              const uint32_t atlasStartOffset = static_cast<uint32_t>(af.tellp());
              af.write(reinterpret_cast<const char*>(atlasBlock.data()), static_cast<std::streamsize>(atlasBlock.size()));
              const uint32_t atlasSize = static_cast<uint32_t>(atlasBlock.size());
              // v40 trailer sits at HEADER_SIZE_V38 + 12 (after the v39
              // embedded-subset triple). Mirrors the seek arithmetic the
              // v39 emit path uses to patch its own trailer.
              constexpr uint32_t kV40TrailerOffset = 48u + 3u * sizeof(uint32_t);  // 60
              af.seekp(kV40TrailerOffset, std::ios::beg);
              const uint32_t atlasHash = sdFontForSubset->contentHash();
              af.write(reinterpret_cast<const char*>(&atlasStartOffset), sizeof(uint32_t));
              af.write(reinterpret_cast<const char*>(&atlasSize), sizeof(uint32_t));
              af.write(reinterpret_cast<const char*>(&atlasHash), sizeof(uint32_t));
              af.close();
              LOG_INF("PRE",
                      "  section %d: glyph atlas block written: offset=%u size=%u bytes "
                      "(cpfontHash=0x%08x)",
                      spineIdx, atlasStartOffset, atlasSize, atlasHash);
            }
          }
        }
      }
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

    // CrumBLE 4.2: write a separate version-marker file. The File Transfer
    // "Pre-cached" badge and the in-reader long-press menu's "Optimized"
    // label both gate on this marker rather than on the manifest's
    // existence. Older (pre-4.2) bakes have only the manifest -- they ran
    // before the SD-font measurement fast-path landed in host_shim
    // GfxRenderer, so their per-section layouts disagree with device runtime
    // for any SD-font book, and at least for built-in-font books they're
    // stale in subtle ways (different fontId hashing constants, missing
    // SD-font support). Marking only v2-aware bakes avoids the misleading
    // badge for users carrying stale prebakes from the old optimizer.
    //
    // Schema-versioned filename so a future v3 bake (or a v3-aware device
    // build) can simply add prebake-v3.marker without touching the existing
    // v2 detection path.
    const std::string markerPath = realCacheDir + "/prebake-v2.marker";
    std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
    if (marker) {
      // Empty file -- presence is the only signal we need. (~0 SD bytes.)
      marker.close();
      LOG_INF("PRE", "wrote prebake-v2.marker");
    } else {
      LOG_ERR("PRE", "could not write prebake-v2.marker at %s", markerPath.c_str());
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

// CrumBLE 4.4: per-section glyph atlas builder. Given a SdCardFont whose
// mini-data has already been prewarmed (by the v39 embedded-subset emit
// path right below), iterate the prewarmed per-style intervals + glyphs
// and pack each glyph's bitmap into a 1-bit packed output buffer. The
// returned byte array is a complete AtlasBlock as defined in
// lib/Epub/Epub/GlyphAtlas.h: BlockHeader + per-style StyleHeader +
// GlyphEntry[] + bitmap payload.
//
// Source bitmap format is per-style:
//   - 1-bit packed (8 px/byte, big-endian within byte): copy directly,
//     possibly with row-stride realignment if widths differ
//   - 2-bit packed (4 px/byte, 2 MSB = first pixel): threshold each pixel
//     -- any non-zero source value becomes a 1 in the output
//
// Output is always 1-bit packed, row-major, ceil(width/8) bytes per row.
// This matches the on-disk format the device-side renderer already uses
// for 1-bit glyph blits (no new decode path needed).
//
// Returns an empty vector iff the font has no prewarmed styles (caller
// treats that as "nothing to emit, not an error").
std::vector<uint8_t> buildGlyphAtlasBlock(const SdCardFont& font) {
  // Pre-scan: which styles have data, total glyph count.
  uint8_t styleMask = 0;
  uint16_t totalGlyphs = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    if (font.miniGlyphCount(s) > 0) {
      styleMask |= static_cast<uint8_t>(1u << s);
      totalGlyphs = static_cast<uint16_t>(totalGlyphs + font.miniGlyphCount(s));
    }
  }
  if (styleMask == 0) return {};

  // Pack each glyph to 1-bit. Build per-style GlyphEntry vectors and a
  // single shared output bitmap buffer; each entry's bitmapOffset points
  // into that buffer.
  std::vector<uint8_t> bitmapData;
  bitmapData.reserve(8 * 1024);
  std::vector<std::vector<glyphatlas::GlyphEntry>> entriesByStyle(4);

  for (uint8_t s = 0; s < 4; ++s) {
    if ((styleMask & (1u << s)) == 0) continue;
    const uint32_t glyphCount = font.miniGlyphCount(s);
    const EpdGlyph* glyphs = font.miniGlyphsPtr(s);
    const EpdUnicodeInterval* intervals = font.miniIntervalsPtr(s);
    const uint32_t intervalCount = font.miniIntervalCount(s);
    const uint8_t* srcBitmap = font.miniBitmapPtr(s);
    const bool is2Bit = font.miniIs2Bit(s);
    auto& entries = entriesByStyle[s];
    entries.reserve(glyphCount);

    // Enumerate codepoints via interval table. Each interval [first..last]
    // maps to contiguous glyph indices starting at interval.offset.
    for (uint32_t i = 0; i < intervalCount; ++i) {
      const EpdUnicodeInterval iv = intervals[i];
      for (uint32_t cp = iv.first; cp <= iv.last; ++cp) {
        const uint32_t glyphIdx = iv.offset + (cp - iv.first);
        if (glyphIdx >= glyphCount) continue;
        const EpdGlyph g = glyphs[glyphIdx];
        const uint16_t outRowBytes = glyphatlas::rowBytes(g.width, glyphatlas::BIT_DEPTH_1);
        const uint32_t outGlyphBytes = static_cast<uint32_t>(outRowBytes) * g.height;
        if (bitmapData.size() + outGlyphBytes > 0xFFFFu) {
          LOG_ERR("PRE", "atlas: bitmap payload would exceed 64 KB at cp U+%04X style %u; truncating", cp, s);
          break;  // bitmapBytes field is uint16_t
        }
        const uint16_t bitmapOffset = static_cast<uint16_t>(bitmapData.size());
        bitmapData.resize(bitmapData.size() + outGlyphBytes, 0);
        const uint8_t* srcGlyph = srcBitmap + g.dataOffset;

        // Per-pixel copy + threshold. Source format is per-glyph row-major
        // packed at the font's bit depth; output is row-major 1-bit big
        // endian within byte (matches the EpdFont blit path).
        for (uint32_t y = 0; y < g.height; ++y) {
          for (uint32_t x = 0; x < g.width; ++x) {
            uint8_t srcPixel;
            if (is2Bit) {
              const uint32_t srcBitOffset = (y * g.width + x) * 2u;
              const uint32_t srcByteIdx = srcBitOffset / 8u;
              const uint32_t srcBitInByte = srcBitOffset % 8u;
              srcPixel = (srcGlyph[srcByteIdx] >> (6u - srcBitInByte)) & 0x03u;
            } else {
              const uint32_t srcBitOffset = y * g.width + x;
              const uint32_t srcByteIdx = srcBitOffset / 8u;
              const uint32_t srcBitInByte = srcBitOffset % 8u;
              srcPixel = (srcGlyph[srcByteIdx] >> (7u - srcBitInByte)) & 0x01u;
            }
            // Threshold: any nonzero source pixel becomes lit in the
            // 1-bit output. For 2-bit fonts this loses anti-aliasing
            // (intentional trade for half the storage); for already-1-bit
            // sources it's identity.
            if (srcPixel != 0) {
              const uint32_t outByteIdx = y * outRowBytes + (x / 8u);
              const uint32_t outBitInByte = x % 8u;
              bitmapData[bitmapOffset + outByteIdx] |= static_cast<uint8_t>(1u << (7u - outBitInByte));
            }
          }
        }

        glyphatlas::GlyphEntry entry{};
        entry.codepoint = cp;
        entry.bitmapOffset = bitmapOffset;
        entry.width = g.width;
        entry.height = g.height;
        const int leftClamped = std::max(-128, std::min(127, static_cast<int>(g.left)));
        const int topClamped = std::max(-128, std::min(127, static_cast<int>(g.top)));
        entry.left = static_cast<int8_t>(leftClamped);
        entry.top = static_cast<int8_t>(topClamped);
        entry.advanceX = g.advanceX;
        entries.push_back(entry);
      }
    }
  }

  // Assemble the on-disk block: BlockHeader, then per-style StyleHeader
  // followed by that style's GlyphEntry[], then the shared bitmap payload.
  std::vector<uint8_t> output;
  const size_t headerBytes = sizeof(glyphatlas::BlockHeader);
  size_t styleSectionBytes = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    if ((styleMask & (1u << s)) == 0) continue;
    styleSectionBytes += sizeof(glyphatlas::StyleHeader) + entriesByStyle[s].size() * sizeof(glyphatlas::GlyphEntry);
  }
  output.reserve(headerBytes + styleSectionBytes + bitmapData.size());

  glyphatlas::BlockHeader bh{};
  bh.magic = glyphatlas::MAGIC;
  bh.version = glyphatlas::FORMAT_VERSION;
  bh.bitDepth = glyphatlas::BIT_DEPTH_1;
  bh.styleMask = styleMask;
  bh.reserved = 0;
  bh.totalGlyphs = totalGlyphs;
  bh.bitmapBytes = static_cast<uint16_t>(bitmapData.size());
  output.insert(output.end(), reinterpret_cast<const uint8_t*>(&bh),
                reinterpret_cast<const uint8_t*>(&bh) + sizeof(bh));

  for (uint8_t s = 0; s < 4; ++s) {
    if ((styleMask & (1u << s)) == 0) continue;
    glyphatlas::StyleHeader sh{};
    sh.styleId = s;
    sh.reserved = 0;
    sh.glyphCount = static_cast<uint16_t>(entriesByStyle[s].size());
    sh.ascender = static_cast<uint16_t>(font.miniAscender(s));
    sh.descender = static_cast<uint16_t>(font.miniDescender(s));
    sh.lineHeight = static_cast<uint16_t>(font.miniAdvanceY(s));
    sh.spaceWidth = 0;  // Not currently emitted; renderer can derive from
                        // the space glyph's advanceX entry as needed.
    output.insert(output.end(), reinterpret_cast<const uint8_t*>(&sh),
                  reinterpret_cast<const uint8_t*>(&sh) + sizeof(sh));
    output.insert(output.end(), reinterpret_cast<const uint8_t*>(entriesByStyle[s].data()),
                  reinterpret_cast<const uint8_t*>(entriesByStyle[s].data() + entriesByStyle[s].size()));
  }
  output.insert(output.end(), bitmapData.begin(), bitmapData.end());
  return output;
}

// CrumBLE 4.3: per-section glyph subset emit. Implementation in stages:
//
//   Stage 1 (this commit): walk the just-written section file's pages,
//     extract codepoints actually used in PageLine TextBlocks, bucket by
//     EpdFontFamily::Style (REGULAR / BOLD / ITALIC / BOLD_ITALIC). Log
//     summary; return true so the pipeline continues. Block serialisation
//     + trailer patching land in subsequent commits, each layered on top
//     of the collected codepoint sets.
//
//   Stage 2 (next): prewarm SdCardFont with the union of codepoints per
//     style (so SdCardFont's miniData / miniIntervals / miniGlyphs /
//     miniBitmap are populated with exactly the section's working set).
//
//   Stage 3 (next): serialise the prewarmed mini-data into an
//     EmbeddedGlyphSubset block, append it to the section file, patch
//     the v39 trailer fields (offset / size / cpfontHash).
//
// Each stage is gated on the previous stage's data so a partial commit
// can't produce a corrupt section file -- the trailer fields only get
// patched once the block bytes are fully written.
bool emitEmbeddedGlyphSubsetForSection(const std::string& sectionPath, Section& section, SdCardFont& font,
                                       int spineIdx) {
  // Bucket codepoints per resolved style. EpdFontFamily::Style values are
  // 0=REGULAR, 1=BOLD, 2=ITALIC, 3=BOLD_ITALIC; higher bits (UNDERLINE,
  // STRIKETHROUGH) are decoration-only and don't pick a different font,
  // so we mask them off before indexing.
  std::unordered_set<uint32_t> codepointsByStyle[4];
  const uint16_t pageCount = section.pageCount;
  uint32_t totalGlyphSlotsScanned = 0;
  // Stash + temporarily override currentPage so we can iterate without
  // disturbing the caller's expectation (createSectionFile leaves it at
  // 0; we restore it before returning anyway, but be explicit).
  const int savedCurrentPage = section.currentPage;
  for (uint16_t pageIdx = 0; pageIdx < pageCount; ++pageIdx) {
    section.currentPage = static_cast<int>(pageIdx);
    auto page = section.loadPageFromSectionFile();
    if (!page) {
      LOG_ERR("PRE", "  section %d: failed to load page %u for codepoint scan -- aborting subset emit", spineIdx,
              static_cast<unsigned>(pageIdx));
      section.currentPage = savedCurrentPage;
      return false;
    }
    // Walk page elements; only PageLine matters for text glyph coverage.
    // Other element types (PageImage, PageHorizontalRule, PageTableFragment)
    // don't render glyphs through the SD-font path so we don't need to
    // include their characters in the embedded subset.
    for (const auto& element : page->elements) {
      if (!element || element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      const auto& blockPtr = line->getBlock();
      if (!blockPtr) continue;
      const TextBlock& block = *blockPtr;
      const auto& words = block.getWords();
      const auto& styles = block.getWordStyles();
      const size_t wordCount = words.size();
      // styles is parallel to words but defensively bail if they ever
      // diverge -- a malformed section file would otherwise UB on the
      // styles[i] access.
      if (styles.size() < wordCount) {
        LOG_ERR("PRE", "  section %d page %u: TextBlock has %zu words but %zu styles -- skipping",
                spineIdx, static_cast<unsigned>(pageIdx), wordCount, styles.size());
        continue;
      }
      for (size_t i = 0; i < wordCount; ++i) {
        const uint8_t fontStyle = static_cast<uint8_t>(styles[i]) & 0x03;  // mask off decoration bits
        const std::string& word = words[i];
        const uint8_t* p = reinterpret_cast<const uint8_t*>(word.c_str());
        uint32_t cp = 0;
        while ((cp = utf8NextCodepoint(&p)) != 0) {
          codepointsByStyle[fontStyle].insert(cp);
          ++totalGlyphSlotsScanned;
        }
        // TODO(v4.3 follow-up): bionic reading bolds the first N bytes of
        // word[i]. Those codepoints would need to live in the BOLD bucket
        // of the embedded subset for the on-device renderer to pick them
        // up without falling back to the SD-font miss handler. Skipped
        // here because wordBionicBoundary is private on TextBlock; adding
        // a getter is a one-line change but lives in a follow-up so the
        // first-cut codepoint-collection stage stays narrow.
      }
    }
  }
  section.currentPage = savedCurrentPage;

  // CrumBLE 4.3: roll up codepoints from styles the SD font doesn't
  // actually have (typical: Readerly-family SD fonts ship REGULAR only)
  // into the REGULAR set. This makes the embedded subset cover EVERY
  // codepoint the section text uses, regardless of original styling.
  // Combined with EpdFontFamily::getGlyphData's REGULAR-style fallback,
  // italic/bold codepoints render as regular glyphs instead of '?'
  // (the alternative was the renderer rejecting them with REPLACEMENT_GLYPH
  // because the embedded subset had only regular-styled cp and the SD
  // font had no italic/bold style to fall through to). Trade-off: regular
  // subset grows by italic/bold cp count (~10-30% on text-heavy chapters)
  // = 0.5-2 KB extra per section.
  for (int s = 1; s < 4; ++s) {
    if (codepointsByStyle[s].empty()) continue;
    if (font.miniGlyphCount(s) > 0) continue;  // Style exists in font — keep separate
    for (uint32_t cp : codepointsByStyle[s]) {
      codepointsByStyle[0].insert(cp);
    }
    codepointsByStyle[s].clear();
  }

  // Sanity check: log per-style unique-codepoint counts so we can eyeball
  // whether a section actually used the BOLD/ITALIC styles before we go
  // through the trouble of serialising empty style buckets.
  uint32_t totalUnique = 0;
  for (int s = 0; s < 4; ++s) totalUnique += codepointsByStyle[s].size();
  LOG_INF("PRE",
          "  section %d (%u pages): scanned %u glyph slots, %u unique codepoints "
          "(R=%zu, B=%zu, I=%zu, BI=%zu) -- cpfontHash=0x%08x, styles=%u",
          spineIdx, static_cast<unsigned>(pageCount), totalGlyphSlotsScanned, totalUnique,
          codepointsByStyle[0].size(), codepointsByStyle[1].size(), codepointsByStyle[2].size(),
          codepointsByStyle[3].size(), font.contentHash(), static_cast<unsigned>(font.styleCount()));

  // Stage 2: prewarm SdCardFont per style with the union of codepoints
  // we just collected. SdCardFont::prewarm reads the .cpfont file and
  // populates the per-style miniData / miniIntervals / miniGlyphs /
  // miniBitmap with exactly the requested codepoints. After this loop
  // returns, the stage-3 serialiser pulls those buffers via the public
  // miniIntervalsPtr() / miniGlyphsPtr() / miniBitmapPtr() accessors.
  //
  // Encoding: prewarm takes UTF-8 text, so for each style we build a
  // string by concatenating the UTF-8 encoding of every codepoint in
  // its set. Order doesn't matter -- prewarm walks the string with
  // utf8NextCodepoint and dedupes internally.
  auto appendUtf8 = [](std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x110000u) {
      out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    // codepoints >= 0x110000 are out of range; ignore.
  };

  for (uint8_t styleIdx = 0; styleIdx < 4; ++styleIdx) {
    const auto& cpset = codepointsByStyle[styleIdx];
    if (cpset.empty()) continue;
    std::string utf8Text;
    utf8Text.reserve(cpset.size() * 4);  // upper bound (4 bytes per cp)
    for (uint32_t cp : cpset) appendUtf8(utf8Text, cp);
    // styleMask: 1 bit per EpdFontFamily::Style (REGULAR=bit0, BOLD=bit1,
    // ITALIC=bit2, BOLD_ITALIC=bit3). One call per style so each style's
    // miniData ends up with EXACTLY this section's working set for that
    // style (vs a broader-mask call which would over-allocate).
    const int missed = font.prewarm(utf8Text.c_str(), static_cast<uint8_t>(1u << styleIdx), /*metadataOnly=*/false);
    LOG_INF("PRE",
            "  section %d style %u prewarmed: %zu requested cp, %u miss(es) -- "
            "miniIntervalCount=%u miniGlyphCount=%u miniBitmapSize=%u",
            spineIdx, static_cast<unsigned>(styleIdx), cpset.size(), missed, font.miniIntervalCount(styleIdx),
            font.miniGlyphCount(styleIdx), font.miniBitmapSize(styleIdx));
  }

  // Stage 3: serialise the prewarmed mini-data into an EmbeddedGlyphSubset
  // block appended to the section file, then patch the v39 trailer fields
  // (embeddedGlyphSubsetOffset / Size / CpfontHash) so on-device load can
  // validate + install the block.
  //
  // Count non-empty styles -- the block's BlockHeader.styleCount field
  // describes how many StyleHeader+data records follow, so we skip styles
  // whose codepoint set was empty AND styles whose prewarm produced zero
  // glyphs (e.g. the .cpfont didn't carry that style; SdCardFont's style
  // fallback resolves it elsewhere).
  uint8_t emitStyleCount = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    if (codepointsByStyle[s].empty()) continue;
    if (font.miniGlyphCount(s) == 0) continue;
    emitStyleCount++;
  }
  if (emitStyleCount == 0) {
    LOG_INF("PRE", "  section %d: no styles with non-empty prewarmed working set; skipping block emit", spineIdx);
    return true;  // not an error, just nothing to embed
  }

  // Open the just-finalised section file for read+write WITHOUT truncating.
  // The host_shim's openFileForWrite() truncates (it mirrors SdFat's wb+
  // semantics), so we go around it with raw std::fstream -- the prebake
  // CLI is host-only code; portability across the device's FsFile isn't
  // required for this writer.
  std::fstream f(sectionPath, std::ios::in | std::ios::out | std::ios::binary);
  if (!f.is_open()) {
    LOG_ERR("PRE", "  section %d: cannot open %s for r+w during subset emit", spineIdx, sectionPath.c_str());
    return false;
  }
  // Block start offset = current EOF. Block is appended verbatim; the
  // section's existing page LUT + anchor / paragraph / li-LUT trailers
  // are untouched (the v39 trailer fields the section header points at
  // are AFTER the LUTs, so this doesn't reshuffle anything).
  f.seekp(0, std::ios::end);
  const uint32_t blockStartOffset = static_cast<uint32_t>(f.tellp());

  // BlockHeader (16 bytes, packed).
  embeddedGlyphSubset::BlockHeader hdr{};
  hdr.magic = embeddedGlyphSubset::BLOCK_MAGIC;
  hdr.version = embeddedGlyphSubset::BLOCK_VERSION;
  hdr.styleCount = emitStyleCount;
  hdr.reserved = 0;
  hdr.cpfontContentHash = font.contentHash();
  hdr.reserved2 = 0;
  static_assert(sizeof(hdr) == 16, "BlockHeader size drift");
  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

  // Per non-empty style: StyleHeader (24 bytes) + intervals + glyphs + bitmap.
  // EpdGlyph::dataOffset values inside the block are ALREADY relative to
  // the start of this style's miniBitmap (see SdCardFont::prewarm which
  // assigns dataOffset = miniBitmapOffset, a cumulative byte counter
  // within miniBitmap). No rebasing required.
  for (uint8_t s = 0; s < 4; ++s) {
    if (codepointsByStyle[s].empty()) continue;
    if (font.miniGlyphCount(s) == 0) continue;
    embeddedGlyphSubset::StyleHeader sh{};
    sh.styleId = s;
    sh.flags = font.miniIs2Bit(s) ? embeddedGlyphSubset::STYLE_FLAG_IS_2BIT : static_cast<uint8_t>(0);
    sh.reserved = 0;
    sh.intervalCount = font.miniIntervalCount(s);
    sh.glyphCount = font.miniGlyphCount(s);
    sh.bitmapDataSize = font.miniBitmapSize(s);
    sh.advanceY = font.miniAdvanceY(s);
    sh.ascender = font.miniAscender(s);
    sh.descender = font.miniDescender(s);
    sh.reserved2 = 0;
    // CrumBLE 4.3 v2: kerning + ligature counts.
    // EXPERIMENTAL: zero these so the subset stays small enough for the
    // post-NimBLE lazy reload on SD-font + BT (the kerning matrix + class
    // tables add ~6 KB per style and push the lazy reload over the
    // post-BT MaxAlloc budget). Layout pre-BT uses prewarm's kerning;
    // post-BT we accept some Outside range drift in exchange for glyphs
    // rendering at all.
    sh.kernLeftEntryCount = 0;
    sh.kernRightEntryCount = 0;
    sh.kernLeftClassCount = 0;
    sh.kernRightClassCount = 0;
    sh.ligaturePairCount = 0;
    static_assert(sizeof(sh) == 32, "StyleHeader v2 size drift");
    f.write(reinterpret_cast<const char*>(&sh), sizeof(sh));
    if (sh.intervalCount > 0) {
      f.write(reinterpret_cast<const char*>(font.miniIntervalsPtr(s)),
              static_cast<std::streamsize>(sh.intervalCount) * sizeof(EpdUnicodeInterval));
    }
    if (sh.glyphCount > 0) {
      f.write(reinterpret_cast<const char*>(font.miniGlyphsPtr(s)),
              static_cast<std::streamsize>(sh.glyphCount) * sizeof(EpdGlyph));
    }
    if (sh.bitmapDataSize > 0) {
      f.write(reinterpret_cast<const char*>(font.miniBitmapPtr(s)), sh.bitmapDataSize);
    }
    // v2 kerning + ligature blobs (in install order: kernLeft, kernRight,
    // kernMatrix, ligaturePairs).
    if (sh.kernLeftEntryCount > 0) {
      f.write(reinterpret_cast<const char*>(font.miniKernLeftClassesPtr(s)),
              static_cast<std::streamsize>(sh.kernLeftEntryCount) * sizeof(EpdKernClassEntry));
    }
    if (sh.kernRightEntryCount > 0) {
      f.write(reinterpret_cast<const char*>(font.miniKernRightClassesPtr(s)),
              static_cast<std::streamsize>(sh.kernRightEntryCount) * sizeof(EpdKernClassEntry));
    }
    const size_t matrixBytes = static_cast<size_t>(sh.kernLeftClassCount) * sh.kernRightClassCount;
    if (matrixBytes > 0) {
      f.write(reinterpret_cast<const char*>(font.miniKernMatrixPtr(s)), static_cast<std::streamsize>(matrixBytes));
    }
    if (sh.ligaturePairCount > 0) {
      f.write(reinterpret_cast<const char*>(font.miniLigaturePairsPtr(s)),
              static_cast<std::streamsize>(sh.ligaturePairCount) * sizeof(EpdLigaturePair));
    }
  }

  // Block end offset -> compute size for the trailer field.
  f.seekp(0, std::ios::end);
  const uint32_t blockEndOffset = static_cast<uint32_t>(f.tellp());
  const uint32_t blockSize = blockEndOffset - blockStartOffset;

  // Patch v39 trailer fields. They sit at HEADER_SIZE_V38 (48) in the
  // file, immediately after the v38 LUT offsets that the existing
  // page-load path consumes. Layout (matches Section.cpp's writer):
  //   [0..47]   v38 fields + LUT offsets
  //   [48..51]  embeddedGlyphSubsetOffset   (uint32_t)
  //   [52..55]  embeddedGlyphSubsetSize     (uint32_t)
  //   [56..59]  embeddedGlyphSubsetCpfontHash (uint32_t)
  constexpr uint32_t kV39TrailerOffset = 48u;
  f.seekp(kV39TrailerOffset, std::ios::beg);
  const uint32_t embedOffset = blockStartOffset;
  const uint32_t embedSize = blockSize;
  const uint32_t embedHash = font.contentHash();
  f.write(reinterpret_cast<const char*>(&embedOffset), sizeof(uint32_t));
  f.write(reinterpret_cast<const char*>(&embedSize), sizeof(uint32_t));
  f.write(reinterpret_cast<const char*>(&embedHash), sizeof(uint32_t));
  f.close();

  LOG_INF("PRE",
          "  section %d: embedded glyph subset block written: offset=%u size=%u bytes "
          "(styleCount=%u, cpfontHash=0x%08x)",
          spineIdx, embedOffset, embedSize, static_cast<unsigned>(emitStyleCount), embedHash);
  return true;
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
  // Lexend_14 EpdFont declarations elided (see top-of-file note).
  EpdFont bitter12Regular(&bitter_12_regular);
  EpdFont bitter12Bold(&bitter_12_bold);
  EpdFont bitter12Italic(&bitter_12_italic);
  EpdFont bitter12BoldItalic(&bitter_12_bolditalic);
  EpdFontFamily bitter12Family(&bitter12Regular, &bitter12Bold,
                               &bitter12Italic, &bitter12BoldItalic);
  EpdFont bitter14Regular(&bitter_14_regular);
  EpdFont bitter14Bold(&bitter_14_bold);
  EpdFont bitter14Italic(&bitter_14_italic);
  EpdFont bitter14BoldItalic(&bitter_14_bolditalic);
  EpdFontFamily bitter14Family(&bitter14Regular, &bitter14Bold,
                               &bitter14Italic, &bitter14BoldItalic);

  GfxRenderer renderer;
  // CrumBLE 4.2: Lexend_14 register call elided; see top-of-file rationale.
  renderer.insertFont(BITTER_12_FONT_ID, bitter12Family);
  renderer.insertFont(BITTER_14_FONT_ID, bitter14Family);

  // CrumBLE 4.2: load + register an SD-card .cpfont when the JS caller
  // supplies one. The font lives at opts.sdFontPath inside MEMFS (the JS
  // side writes the .cpfont bytes there before invoking main()).
  // computeFontId here mirrors the device's hash exactly, so the
  // manifest's fontId == reader's runtime fontId.
  std::unique_ptr<SdCardFont> sdFontKeepalive;
  if (!opts.sdFontPath.empty() && !opts.sdFontFamilyName.empty() && opts.sdFontPointSize > 0) {
    auto font = std::make_unique<SdCardFont>();
    if (!font->load(opts.sdFontPath.c_str())) {
      LOG_ERR("CLI", "SD font load failed: %s -- aborting", opts.sdFontPath.c_str());
      return 4;
    }
    const int sdFontId = SdCardFontManager::computeFontId(font->contentHash(),
                                                          opts.sdFontFamilyName.c_str(),
                                                          opts.sdFontPointSize);
    // insertFont puts the EpdFontFamily wrapper in the renderer's
    // fontMap so getData()/getKerning() lookups work. registerSdCardFont
    // separately puts the SdCardFont* in the sdCardFonts_ map so
    // ParsedText.cpp's isSdCardFont() check returns true and triggers
    // ensureSdCardFontReady() -> buildAdvanceTable() per-paragraph,
    // which populates the per-font persistent advance table that
    // getTextAdvanceX / getTextWidth / getSpaceWidth read from. Both
    // calls are necessary: without registerSdCardFont, ParsedText's
    // isSdCardFont() returns false, ensureSdCardFontReady is skipped,
    // hasAdvanceTable() stays false, and the renderer falls through to
    // EpdFontFamily::findGlyph -- which finds nothing on an SD font
    // (intervalCount=0 by design), so every glyph is "missing" and
    // sections lay out jumbled.
    EpdFontFamily sdFontFamily(font->getEpdFont(0), font->getEpdFont(1),
                               font->getEpdFont(2), font->getEpdFont(3));
    renderer.insertFont(sdFontId, sdFontFamily);
    renderer.registerSdCardFont(sdFontId, font.get());
    LOG_INF("CLI", "SD font registered: %s @ %u pt -> id=%d (contentHash=0x%08x)",
            opts.sdFontFamilyName.c_str(), opts.sdFontPointSize, sdFontId, font->contentHash());
    // CrumBLE 4.2 SD-font diagnostic: dump key glyph metrics so we can
    // diff against the device-side equivalent log (handleReaderRenderInfo's
    // SDFONT_DIAG) -- any divergence here means WASM and device parse the
    // same .cpfont bytes into different metrics, which would explain the
    // jumbled-layout-but-fingerprint-passes symptom.
    {
      EpdFont* ef = font->getEpdFont(0);  // regular
      if (ef && ef->data) {
        const auto* d = ef->data;
        LOG_INF("SDFONT_DIAG", "WASM regular: advanceY=%u ascender=%d descender=%d intervalCount=%u groupCount=%u",
                static_cast<unsigned>(d->advanceY), d->ascender, d->descender,
                static_cast<unsigned>(d->intervalCount), static_cast<unsigned>(d->groupCount));
        for (uint32_t cp : {static_cast<uint32_t>(0x41), static_cast<uint32_t>(0x61), static_cast<uint32_t>(0x20)}) {
          const EpdGlyph* g = ef->findGlyph(cp);
          if (g) {
            LOG_INF("SDFONT_DIAG", "WASM U+%04X: w=%u h=%u advX=%u left=%d top=%d dataLen=%u",
                    cp, g->width, g->height, g->advanceX, g->left, g->top,
                    static_cast<unsigned>(g->dataLength));
          } else {
            LOG_INF("SDFONT_DIAG", "WASM U+%04X: NOT FOUND", cp);
          }
        }
      }
    }
    sdFontKeepalive = std::move(font);  // keep alive for the lifetime of renderer
  }

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
      const int sectionFails = prebakeSections(epubPath, cacheDir, cacheDirParent, renderer, sectionSettings,
                                                sdFontKeepalive.get(), opts.emitSectionGlyphSubsets);
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
