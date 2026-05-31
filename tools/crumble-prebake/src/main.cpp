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
#include "fontIds.h"  // LEXENDDECA_14_FONT_ID (matches src/fontIds.h)

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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
               "  --check                Skip books whose existing book.bin is fresh against\n"
               "                         the input EPUB's mtime.\n"
               "  --verbose              Per-step timing on stderr.\n"
               "  -h, --help             Show this help.\n",
               argv0);
}

struct Options {
  std::string outputDir;
  std::string devicePathOverride;
  std::vector<std::string> epubs;
  bool check = false;
  bool verbose = false;
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
                    const std::string& cacheDirParent, GfxRenderer& renderer, int fontId,
                    uint16_t viewportWidth, uint16_t viewportHeight) {
  // Epub computes cachePath as cacheDir + "/epub_" + fnvHash(filepath),
  // where filepath is the HOST EPUB path -- but our Phase 1 cache lives
  // under "/.crosspoint/epub_<deviceHash>/" with the DEVICE path hash.
  // To make Epub::load find the book.bin we already wrote, we shadow-
  // copy book.bin to where Epub will look (cacheDirParent +
  // "/epub_<hostPathHash>/"), let load run, then sections land in that
  // shadow tree. After section gen, we'll copy sections/ back to the
  // real device-hash location so the SD-card-bound output has the
  // device-expected layout.
  //
  // This is a workaround. The cleaner answer is to extend Epub with an
  // explicit-cachePath ctor that decouples ZIP-read filepath from
  // cache-dir computation. Recorded for 2C.4 chip.
  const std::string shadowCacheDir = cacheDirParent + "/epub_" +
                                     std::to_string(ZipFile::fnvHash64(epubPath.c_str(), epubPath.size()));

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

  // Construct an Epub instance whose internal cachePath now resolves to
  // the shadow location (cacheDirParent + "/epub_<hostHash>"). load()
  // hits the shadow's book.bin and short-circuits the OPF parse.
  auto epub = std::make_shared<Epub>(epubPath, cacheDirParent);
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

  // Factory-default layout settings (mirror src/CrossPointSettings.h).
  // Reader activity passes these from SETTINGS.* at section-build time;
  // any divergence here will change the section file header fingerprint
  // and force the device to rebuild on first open. Once we have telemetry
  // on which settings users actually customize, we'll either ship the
  // most common combo as the default or expose explicit overrides.
  constexpr float kLineCompression = 1.0f;
  constexpr bool kExtraParagraphSpacing = false;
  constexpr bool kForceParagraphIndents = false;
  constexpr uint8_t kParagraphAlignment = 0;  // "default" (per CrossPointSettings)
  constexpr bool kHyphenationEnabled = true;
  constexpr bool kEmbeddedStyle = true;
  constexpr uint8_t kImageRendering = 0;  // "fit"
  constexpr bool kBionicReadingEnabled = false;
  constexpr bool kGuideReadingEnabled = false;

  int failures = 0;
  for (int spineIdx = 0; spineIdx < spineCount; ++spineIdx) {
    Section section(epub, spineIdx, renderer);
    bool imagesWereSuppressed = false;
    bool layoutAbortedForLowMemory = false;
    const bool ok = section.createSectionFile(
        fontId, kLineCompression, kExtraParagraphSpacing, kForceParagraphIndents, kParagraphAlignment, viewportWidth,
        viewportHeight, kHyphenationEnabled, kEmbeddedStyle, kImageRendering, kBionicReadingEnabled,
        kGuideReadingEnabled, /*popupFn=*/nullptr, &imagesWereSuppressed, &layoutAbortedForLowMemory);
    if (!ok) {
      LOG_ERR("PRE", "section %d FAILED", spineIdx);
      ++failures;
    } else {
      LOG_INF("PRE", "section %d wrote %u pages%s", spineIdx, static_cast<unsigned>(section.pageCount),
              imagesWereSuppressed ? " (images suppressed)" : "");
    }
  }

  // Sections were written into the shadow dir (epub_<hostHash>/sections/).
  // Copy them back to the real device-hash location so the SD-bound output
  // looks like what the device generates. We could rename instead of copy
  // since the shadow dir is throwaway, but copying keeps Epub::load happy
  // if the same binary gets invoked twice on the same EPUB (idempotent).
  std::error_code ec2;
  const std::string realSectionsDir = realCacheDir + "/sections";
  fs::create_directories(realSectionsDir, ec2);
  if (ec2) {
    LOG_ERR("PRE", "could not create real sections dir %s: %s", realSectionsDir.c_str(), ec2.message().c_str());
    return failures;
  }
  fs::copy(shadowCacheDir + "/sections", realSectionsDir,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec2);
  if (ec2) {
    LOG_ERR("PRE", "could not copy sections back to %s: %s", realSectionsDir.c_str(), ec2.message().c_str());
  } else {
    LOG_INF("PRE", "copied %d section files to real cache dir", spineCount);
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

  GfxRenderer renderer;
  renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14Family);
  // X4 viewport in landscape (the default device target). --device x3
  // would set 792x528 here; left as a static default for first byte-
  // match milestone.
  constexpr uint16_t kViewportWidth = 800;
  constexpr uint16_t kViewportHeight = 480;
  renderer.setViewport(kViewportWidth, kViewportHeight);

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
    const uint32_t t1 = millis();
    const int thumbFails = prebakeAllThumbs(epubPath, cacheDir, bookMetadata.coverItemHref);
    const uint32_t dtThumbs = millis() - t1;
    if (thumbFails == 0) {
      LOG_INF("CLI", "  thumbs OK (%u ms)", dtThumbs);
    } else {
      LOG_INF("CLI", "  thumbs PARTIAL: %d of 3 failed (%u ms)", thumbFails, dtThumbs);
    }

    // Phase 2C: section files. Builds sections/<spineIdx>.bin per spine
    // entry, mirroring the device's reader-activity behavior on first
    // book open. Doesn't bump `failures` -- a section that fails to
    // build is recoverable on-device (the reader self-rebuilds when the
    // user navigates to that chapter), so partial success still ships.
    const uint32_t t2 = millis();
    const int sectionFails = prebakeSections(epubPath, cacheDir, cacheDirParent, renderer, LEXENDDECA_14_FONT_ID,
                                              kViewportWidth, kViewportHeight);
    const uint32_t dtSections = millis() - t2;
    if (sectionFails == 0) {
      LOG_INF("CLI", "  sections OK (%u ms)", dtSections);
    } else if (sectionFails > 0) {
      LOG_INF("CLI", "  sections PARTIAL: %d failed (%u ms)", sectionFails, dtSections);
    } else {
      LOG_INF("CLI", "  sections SKIPPED (Epub::load failed, %u ms)", dtSections);
    }
  }

  return failures == 0 ? 0 : 1;
}
