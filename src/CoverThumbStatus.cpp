#include "CoverThumbStatus.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <vector>

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

int sweepAllMarkers() {
  auto root = Storage.open(kCacheDir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    LOG_INF("CTS", "Sweep: no /.crosspoint dir (yet); skipping");
    return 0;
  }

  int removed = 0;
  char nameBuf[128];
  // Iterate per-book cache subdirs (epub_* / xtc_*). Each subdir may contain
  // one or more thumb_failed_v3_<W>x<H>.marker files -- one per size that
  // failed (Carousel @ 296x468, Collections @ 130x190, etc.).
  for (auto sub = root.openNextFile(); sub; sub = root.openNextFile()) {
    sub.getName(nameBuf, sizeof(nameBuf));
    if (!sub.isDirectory()) {
      sub.close();
      continue;
    }
    const std::string subPath = std::string(kCacheDir) + "/" + nameBuf;
    sub.close();

    auto bookDir = Storage.open(subPath.c_str());
    if (!bookDir || !bookDir.isDirectory()) {
      if (bookDir) bookDir.close();
      continue;
    }
    char fileNameBuf[128];
    std::vector<std::string> toRemove;
    for (auto f = bookDir.openNextFile(); f; f = bookDir.openNextFile()) {
      f.getName(fileNameBuf, sizeof(fileNameBuf));
      // Match thumb_failed_v3_*.marker. String::starts_with isn't available;
      // do a manual prefix + suffix check. The full prefix is "thumb_failed_v3_"
      // (16 chars including underscore) and suffix is ".marker" (7 chars).
      const std::string filename = fileNameBuf;
      const std::string prefix = "thumb_failed_v3_";
      const std::string suffix = ".marker";
      const bool matches = filename.size() > prefix.size() + suffix.size() &&
                           filename.compare(0, prefix.size(), prefix) == 0 &&
                           filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
      f.close();
      if (matches) {
        toRemove.push_back(subPath + "/" + filename);
      }
    }
    bookDir.close();

    for (const auto& path : toRemove) {
      if (Storage.remove(path.c_str())) {
        ++removed;
      } else {
        LOG_ERR("CTS", "Sweep: failed to remove %s", path.c_str());
      }
    }
  }
  root.close();
  LOG_INF("CTS", "Sweep: removed %d thumb-failed marker(s)", removed);
  return removed;
}

int regenerateAllCovers() {
  auto root = Storage.open(kCacheDir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    LOG_INF("CTS", "Regenerate: no /.crosspoint dir; nothing to do");
    return 0;
  }

  int removedThumbs = 0;
  char nameBuf[128];
  for (auto sub = root.openNextFile(); sub; sub = root.openNextFile()) {
    sub.getName(nameBuf, sizeof(nameBuf));
    if (!sub.isDirectory()) {
      sub.close();
      continue;
    }
    const std::string subPath = std::string(kCacheDir) + "/" + nameBuf;
    sub.close();

    auto bookDir = Storage.open(subPath.c_str());
    if (!bookDir || !bookDir.isDirectory()) {
      if (bookDir) bookDir.close();
      continue;
    }
    char fileNameBuf[128];
    std::vector<std::string> toRemove;
    for (auto f = bookDir.openNextFile(); f; f = bookDir.openNextFile()) {
      f.getName(fileNameBuf, sizeof(fileNameBuf));
      const std::string filename = fileNameBuf;
      // Match thumb_<W>x<H>.bmp -- all cached thumbnail sizes for this book.
      // Conservative: require prefix "thumb_" and suffix ".bmp" and NOT match
      // the "thumb_failed_*" prefix (those are markers, handled below).
      const std::string thumbPrefix = "thumb_";
      const std::string failedPrefix = "thumb_failed_";
      const std::string bmpSuffix = ".bmp";
      const bool isThumb = filename.size() > thumbPrefix.size() + bmpSuffix.size() &&
                           filename.compare(0, thumbPrefix.size(), thumbPrefix) == 0 &&
                           filename.compare(0, failedPrefix.size(), failedPrefix) != 0 &&
                           filename.compare(filename.size() - bmpSuffix.size(), bmpSuffix.size(), bmpSuffix) == 0;
      f.close();
      if (isThumb) toRemove.push_back(subPath + "/" + filename);
    }
    bookDir.close();

    for (const auto& path : toRemove) {
      if (Storage.remove(path.c_str())) {
        ++removedThumbs;
      } else {
        LOG_ERR("CTS", "Regenerate: failed to remove %s", path.c_str());
      }
    }
  }
  root.close();

  // Also clear failure markers so books with markers re-attempt with the new
  // settings instead of being skipped.
  const int markers = sweepAllMarkers();
  LOG_INF("CTS", "Regenerate: removed %d thumb(s) + %d marker(s)", removedThumbs, markers);
  return removedThumbs;
}

int regenerateThumbsForBook(const std::string& cacheDir) {
  auto dir = Storage.open(cacheDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  char fileNameBuf[128];
  std::vector<std::string> toRemove;
  const std::string thumbPrefix = "thumb_";
  const std::string failedPrefix = "thumb_failed_";
  const std::string bmpSuffix = ".bmp";
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(fileNameBuf, sizeof(fileNameBuf));
    const std::string filename = fileNameBuf;
    const bool matchesPrefix = filename.compare(0, thumbPrefix.size(), thumbPrefix) == 0;
    const bool isFailedMarker = filename.compare(0, failedPrefix.size(), failedPrefix) == 0;
    const bool isBmpThumb = matchesPrefix && !isFailedMarker &&
                            filename.size() > bmpSuffix.size() &&
                            filename.compare(filename.size() - bmpSuffix.size(), bmpSuffix.size(), bmpSuffix) == 0;
    f.close();
    // Sweep both cached bmps AND the failure markers for this book so the next
    // render is unconditionally re-attempted.
    if (isBmpThumb || isFailedMarker) toRemove.push_back(cacheDir + "/" + filename);
  }
  dir.close();
  int removed = 0;
  for (const auto& path : toRemove) {
    if (Storage.remove(path.c_str())) ++removed;
  }
  LOG_INF("CTS", "Regenerate for %s: removed %d entr(y/ies)", cacheDir.c_str(), removed);
  return removed;
}

}  // namespace CoverThumbStatus
