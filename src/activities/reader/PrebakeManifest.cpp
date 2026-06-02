#include "PrebakeManifest.h"

#include <ArduinoJson.h>
#include <Logging.h>

// CrumBLE: read the JSON sidecar the prebake CLI writes alongside book.bin
// at <cachePath>/prebake-manifest.json. The file is self-contained (no
// dependency on any section file existing) so the manifest stays loadable
// even after the device's Section::clearCache + chapter rebuilds have
// overwritten the original section files. Without this sidecar, the
// switch-back prompt could only fire ONCE per book (before the first
// chapter rebuild ate sections/0.bin) -- with it, the prompt keeps firing
// until the user either accepts it or deletes the manifest.
bool tryLoadPrebakeManifest(const std::string& cachePath, PrebakeManifest& out) {
  const std::string mp = cachePath + "/prebake-manifest.json";
  FsFile f;
  if (!Storage.openFileForRead("PRM", mp, f)) {
    return false;  // no prebake on this book; not an error
  }
  const size_t sz = f.size();
  if (sz == 0 || sz > 1024) {
    f.close();
    LOG_DBG("PRM", "manifest size unreasonable (%u bytes), skipping", static_cast<unsigned>(sz));
    return false;
  }
  // Tiny file -- read into a stack buffer to avoid heap pressure on a path
  // that runs on every book open.
  uint8_t buf[1024] = {0};
  const size_t n = f.read(buf, sz);
  f.close();
  if (n != sz) {
    LOG_ERR("PRM", "short read on manifest: got %u of %u", static_cast<unsigned>(n), static_cast<unsigned>(sz));
    return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, buf, sz);
  if (err) {
    LOG_ERR("PRM", "manifest JSON parse failed: %s", err.c_str());
    return false;
  }
  out.fontId = doc["fontId"] | 0;
  out.lineCompression = doc["lineCompression"] | 1.0f;
  out.extraParagraphSpacing = (doc["extraParagraphSpacing"] | 0) != 0;
  out.forceParagraphIndents = (doc["forceParagraphIndents"] | 0) != 0;
  out.paragraphAlignment = static_cast<uint8_t>(doc["paragraphAlignment"] | 0);
  out.viewportWidth = static_cast<uint16_t>(doc["viewportWidth"] | 0);
  out.viewportHeight = static_cast<uint16_t>(doc["viewportHeight"] | 0);
  out.hyphenationEnabled = (doc["hyphenationEnabled"] | 0) != 0;
  out.embeddedStyle = (doc["embeddedStyle"] | 1) != 0;
  out.imageRendering = static_cast<uint8_t>(doc["imageRendering"] | 0);
  out.bionicReadingEnabled = (doc["bionicReadingEnabled"] | 0) != 0;
  out.guideReadingEnabled = (doc["guideReadingEnabled"] | 0) != 0;
  return true;
}
