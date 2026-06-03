#include "CoverThumbStatus.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

namespace {

// Mirrors the Epub/Xtc on-disk layout. The cache dir for any book the
// firmware has ever seen is /.crosspoint/<format>_<hash>; the marker
// is a sibling of stats.bin / thumb_*.bmp inside that dir.
constexpr char kCacheDir[] = "/.crosspoint";
// CrumBLE: suffix is _v3 -- bumped from _v2 so markers written by
// pre-per-size builds get silently ignored (the global-per-book v2
// markers permanently poisoned books whose covers had only failed at a
// single transient-OOM size). The {width}x{height} portion is filled
// in per call site so the marker for Collections@130x190 doesn't block
// regeneration at Carousel@296x468. Old _v2 .marker files leak as
// zero-byte files on SD -- acceptable; isMarkedFailed only checks the
// new size-scoped path.
constexpr char kMarkerPrefix[] = "/thumb_failed_v3_";
constexpr char kMarkerSuffix[] = ".marker";

std::string bookCacheDir(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    return Epub::cachePathForFilePath(bookPath, kCacheDir);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    // Mirrors Xtc(filepath, cacheDir) constructor's path derivation. Kept
    // inline rather than adding Xtc::cachePathForFilePath() to avoid
    // touching the Xtc class for a single call site.
    return std::string(kCacheDir) + "/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
  }
  // TXT / Markdown have no cover thumbnail concept; the markers system
  // never applies to them. Return empty so exists() short-circuits to
  // false in callers.
  return "";
}

std::string markerPathForBook(const std::string& bookPath, int width, int height) {
  if (width <= 0 || height <= 0) return "";
  const std::string dir = bookCacheDir(bookPath);
  if (dir.empty()) return "";
  return dir + kMarkerPrefix + std::to_string(width) + "x" + std::to_string(height) + kMarkerSuffix;
}

}  // namespace

namespace CoverThumbStatus {

bool isMarkedFailed(const std::string& bookPath, int width, int height) {
  const std::string marker = markerPathForBook(bookPath, width, height);
  if (marker.empty()) return false;
  return Storage.exists(marker.c_str());
}

void markFailed(const std::string& bookPath, int width, int height) {
  const std::string marker = markerPathForBook(bookPath, width, height);
  if (marker.empty()) return;
  // Make sure the cache dir exists -- a failure that triggers BEFORE any
  // successful cache write would otherwise leave the marker un-creatable.
  const std::string parent = bookCacheDir(bookPath);
  if (!parent.empty()) Storage.mkdir(parent.c_str());
  if (!Storage.writeFile(marker.c_str(), String(""))) {
    LOG_ERR("CTS", "Failed to write thumb-failed marker for %s (%dx%d)", bookPath.c_str(), width, height);
    return;
  }
  LOG_INF("CTS", "Marked thumb generation failed for %s (%dx%d)", bookPath.c_str(), width, height);
}

void clearFailed(const std::string& bookPath, int width, int height) {
  const std::string marker = markerPathForBook(bookPath, width, height);
  if (marker.empty()) return;
  if (!Storage.exists(marker.c_str())) return;
  Storage.remove(marker.c_str());
}

}  // namespace CoverThumbStatus
