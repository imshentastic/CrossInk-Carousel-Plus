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

  // CrumBLE reversion fields. The 12 fingerprint values above lock the
  // section header but include derived quantities (fontId is hash of
  // family+size; viewport is screen minus margins; lineCompression is
  // SETTINGS::getReaderLineCompression(lineSpacing)). The on-device "Use
  // prepared layout?" prompt applies these RAW values on confirm so the
  // post-revert fingerprint check actually matches the prebake'd cache.
  uint8_t orientation = 0;
  uint8_t screenMargin = 0;
  uint8_t fontFamily = 0;
  uint8_t fontSize = 0;
  uint8_t sdFontSizeRange = 0;
  char sdFontFamilyName[64] = "";
  uint8_t lineSpacing = 0;
};

// Try to load the fingerprint from the prebake CLI's JSON sidecar at
// <cachePath>/prebake-manifest.json. Returns true on success. Reads a small
// (~250 byte) JSON file and parses with ArduinoJson -- cheap to call on
// every book open.
//
// Important: the manifest is decoupled from the section files. After the
// device's Section::clearCache + chapter rebuild has overwritten the
// prebake'd section files (when current settings don't match), the manifest
// JSON is still on disk and tryLoadPrebakeManifest still succeeds. That's
// what lets the switch-back prompt keep firing on subsequent book opens --
// until the user accepts the prompt OR manually deletes the manifest.
bool tryLoadPrebakeManifest(const std::string& cachePath, PrebakeManifest& out);
