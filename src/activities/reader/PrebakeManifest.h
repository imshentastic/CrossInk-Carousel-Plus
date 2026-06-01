#pragma once

#include <HalStorage.h>
#include <cstdint>
#include <string>

// CrumBLE: parsed fingerprint of a prebake'd section file's header.
//
// Mirrors the 12 fields baked into the section header (lib/Epub/Epub/Section.cpp
// writeSectionFileHeader). We read them once on book open from sections/0.bin
// so the reader can detect when current SETTINGS would invalidate the cache --
// every cache miss after a settings change drops the prebake speedup AND forces
// the device to rebuild every chapter from HTML, which on tight heap is also a
// crash risk.
//
// Compared against current SETTINGS at book-open time. On any mismatch the
// reader prompts "your settings changed; restore the prepared layout?" similar
// to the existing .pxc manifest mismatch dialog for Bluetooth users.
struct PrebakeManifest {
  // 12 fingerprint fields, in the same order Section.cpp writes them.
  int32_t fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  bool forceParagraphIndents = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool bionicReadingEnabled = false;
  bool guideReadingEnabled = false;
};

// Try to load the fingerprint from the cache's section 0. Returns true on
// success. Reads only the first ~25 bytes of the file -- one SD seek + small
// read, no full deserialize -- so this is cheap to call on every book open
// even when the user has hundreds of cached books.
//
// cachePath is the per-book cache dir (i.e. /.crosspoint/epub_<hash>); the
// function reads <cachePath>/sections/0.bin's header.
bool tryLoadPrebakeManifest(const std::string& cachePath, PrebakeManifest& out);
