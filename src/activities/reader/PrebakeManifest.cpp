#include "PrebakeManifest.h"

#include <Logging.h>
#include <cstring>

// Layout MUST match Section::writeSectionFileHeader exactly. See
// lib/Epub/Epub/Section.cpp around line 74. We don't link against
// Section.cpp here (would pull in the entire Epub stack) -- so this is a
// hardcoded parallel read with a magic-byte check that catches any future
// layout drift.
namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FFu;
constexpr size_t HEADER_READ_LEN = 25;  // magic(4) + ver(1) + 12 settings
}  // namespace

bool tryLoadPrebakeManifest(const std::string& cachePath, PrebakeManifest& out) {
  const std::string secPath = cachePath + "/sections/0.bin";
  FsFile f;
  if (!Storage.openFileForRead("PRM", secPath, f)) {
    return false;  // no cache, not an error
  }
  if (f.size() < HEADER_READ_LEN) {
    f.close();
    return false;
  }
  uint8_t buf[HEADER_READ_LEN] = {0};
  const size_t n = f.read(buf, HEADER_READ_LEN);
  f.close();
  if (n < HEADER_READ_LEN) return false;

  uint32_t magic = 0;
  std::memcpy(&magic, &buf[0], 4);
  if (magic != SECTION_CACHE_MAGIC) {
    LOG_DBG("PRM", "section 0 magic mismatch (0x%08x), not a prebake'd cache", magic);
    return false;
  }
  // buf[4] = version. We don't enforce a specific version here -- if the
  // device's Section.cpp can't deserialize this version it'll handle the
  // rebuild itself. We just want the 12 settings for the prompt.
  std::memcpy(&out.fontId, &buf[5], 4);
  std::memcpy(&out.lineCompression, &buf[9], 4);
  out.extraParagraphSpacing = buf[13] != 0;
  out.forceParagraphIndents = buf[14] != 0;
  out.paragraphAlignment = buf[15];
  std::memcpy(&out.viewportWidth, &buf[16], 2);
  std::memcpy(&out.viewportHeight, &buf[18], 2);
  out.hyphenationEnabled = buf[20] != 0;
  out.embeddedStyle = buf[21] != 0;
  out.imageRendering = buf[22];
  out.bionicReadingEnabled = buf[23] != 0;
  out.guideReadingEnabled = buf[24] != 0;
  return true;
}
