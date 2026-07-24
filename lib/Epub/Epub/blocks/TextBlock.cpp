#include "TextBlock.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap() for deserialize pre-flight
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint16_t MAX_WORDS_PER_TEXT_BLOCK = 512;
constexpr uint32_t MAX_SERIALIZED_WORD_BYTES = 4096;
constexpr uint32_t SERIALIZED_TEXT_BLOCK_TAIL_BYTES =
    sizeof(EpdFontFamily::Style) + sizeof(bool) + sizeof(int16_t) * 7 + sizeof(bool);
constexpr uint32_t SERIALIZED_MIN_WORD_METADATA_BYTES =
    sizeof(uint32_t) + sizeof(int16_t) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t);
constexpr uint32_t SERIALIZED_POST_WORD_MIN_METADATA_BYTES =
    sizeof(int16_t) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t);

uint16_t measureBackgroundWidth(const GfxRenderer& renderer, const int fontId, std::string_view word,
                                const EpdFontFamily::Style style) {
  if (word.size() == 1 && word[0] == ' ') {
    return renderer.getSpaceWidth(fontId, style);
  }
  return static_cast<uint16_t>(std::max(0, renderer.getTextAdvanceX(fontId, std::string(word).c_str(), style)));
}

bool isWhitespaceOnlyBackgroundToken(std::string_view word) {
  if (word.empty()) {
    return false;
  }
  for (size_t i = 0; i < word.size();) {
    const auto c = static_cast<uint8_t>(word[i]);
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      ++i;
      continue;
    }
    if (c == 0xC2 && i + 1 < word.size() && static_cast<uint8_t>(word[i + 1]) == 0xA0) {
      i += 2;
      continue;
    }
    if (c == 0xE2 && i + 2 < word.size() && static_cast<uint8_t>(word[i + 2]) == 0xAF &&
        static_cast<uint8_t>(word[i + 1]) == 0x80) {
      i += 3;
      continue;
    }
    return false;
  }
  return true;
}

bool readBoundedString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (!serialization::tryReadPod(file, len)) {
    LOG_ERR("TXB", "Deserialization failed: could not read word length");
    return false;
  }
  if (len > MAX_SERIALIZED_WORD_BYTES) {
    LOG_ERR("TXB", "Deserialization failed: word length %lu exceeds maximum", static_cast<unsigned long>(len));
    return false;
  }
  const int remaining = file.available();
  if (remaining < 0 || static_cast<uint32_t>(remaining) < len) {
    LOG_ERR("TXB", "Deserialization failed: truncated word payload (%lu bytes requested, %d available)",
            static_cast<unsigned long>(len), remaining);
    return false;
  }
  if (len == 0) {
    s.clear();
    return true;
  }
  s.resize(len);
  if (file.read(&s[0], len) != static_cast<int>(len)) {
    LOG_ERR("TXB", "Deserialization failed: could not read %lu-byte word payload",
            static_cast<unsigned long>(len));
    return false;
  }
  return true;
}

}  // namespace

uint32_t TextBlock::computeLayout(uint16_t wordCount, uint32_t wordContentBytes, bool hasBionic, bool hasGuideDots,
                                  uint32_t& outOffWordOffsets, uint32_t& outOffWordXpos, uint32_t& outOffWordStyles,
                                  uint32_t& outOffWordBackgroundBlack, uint32_t& outOffWordBionicBoundary,
                                  uint32_t& outOffWordBionicSuffixX, uint32_t& outOffWordGuideDotXOffset,
                                  uint32_t& outOffWordContents) {
  // Layout (highest alignment first to avoid padding):
  //   wordOffsets[wordCount+1]    uint32_t, 4-byte aligned
  //   wordXpos[wordCount]         int16_t, 2-byte
  //   wordBionicSuffixX[wc]       uint16_t, 2-byte    (if hasBionic)
  //   wordGuideDotXOffset[wc]     uint16_t, 2-byte    (if hasGuideDots)
  //   wordStyles[wordCount]       uint8_t,  1-byte
  //   wordBackgroundBlack[wc]     uint8_t,  1-byte
  //   wordBionicBoundary[wc]      uint8_t,  1-byte    (if hasBionic)
  //   wordContents                char[],   1-byte (packed null-terminated)
  uint32_t offset = 0;
  outOffWordOffsets = offset;
  offset += (static_cast<uint32_t>(wordCount) + 1) * sizeof(uint32_t);
  outOffWordXpos = offset;
  offset += static_cast<uint32_t>(wordCount) * sizeof(int16_t);
  if (hasBionic) {
    outOffWordBionicSuffixX = offset;
    offset += static_cast<uint32_t>(wordCount) * sizeof(uint16_t);
  } else {
    outOffWordBionicSuffixX = 0;
  }
  if (hasGuideDots) {
    outOffWordGuideDotXOffset = offset;
    offset += static_cast<uint32_t>(wordCount) * sizeof(uint16_t);
  } else {
    outOffWordGuideDotXOffset = 0;
  }
  outOffWordStyles = offset;
  offset += static_cast<uint32_t>(wordCount) * sizeof(EpdFontFamily::Style);
  outOffWordBackgroundBlack = offset;
  offset += static_cast<uint32_t>(wordCount) * sizeof(uint8_t);
  if (hasBionic) {
    outOffWordBionicBoundary = offset;
    offset += static_cast<uint32_t>(wordCount) * sizeof(uint8_t);
  } else {
    outOffWordBionicBoundary = 0;
  }
  outOffWordContents = offset;
  offset += wordContentBytes;
  return offset;
}

TextBlock::TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, std::vector<uint8_t> bionic_boundary,
                     std::vector<uint16_t> bionic_suffix_x, std::vector<uint16_t> guide_dot_x_offset,
                     std::vector<uint8_t> background_black, const BlockStyle& blockStyle) {
  blockStyle_ = blockStyle;
  const uint16_t wc = static_cast<uint16_t>(words.size());
  const bool hasBionic = !bionic_boundary.empty();
  const bool hasGuideDots = !guide_dot_x_offset.empty();

  // Size-mismatch check. If invalid, construct empty block (wordCount_ stays 0).
  if (word_xpos.size() != wc || word_styles.size() != wc || background_black.size() != wc ||
      (hasBionic && (bionic_boundary.size() != wc || bionic_suffix_x.size() != wc)) ||
      (!hasBionic && !bionic_suffix_x.empty()) || (hasGuideDots && guide_dot_x_offset.size() != wc)) {
    LOG_ERR("TXB", "Construction skipped: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, dotX=%u, bg=%u)",
            wc, static_cast<uint32_t>(word_xpos.size()), static_cast<uint32_t>(word_styles.size()),
            static_cast<uint32_t>(bionic_boundary.size()), static_cast<uint32_t>(bionic_suffix_x.size()),
            static_cast<uint32_t>(guide_dot_x_offset.size()), static_cast<uint32_t>(background_black.size()));
    return;
  }

  uint32_t wordContentBytes = 0;
  for (const auto& w : words) {
    wordContentBytes += static_cast<uint32_t>(w.size()) + 1;  // +1 for null terminator
  }

  uint32_t offWordOffsets, offWordXpos, offWordStyles, offWordBackgroundBlack;
  uint32_t offWordBionicBoundary, offWordBionicSuffixX, offWordGuideDotXOffset, offWordContents;
  const uint32_t totalBytes =
      computeLayout(wc, wordContentBytes, hasBionic, hasGuideDots, offWordOffsets, offWordXpos, offWordStyles,
                    offWordBackgroundBlack, offWordBionicBoundary, offWordBionicSuffixX, offWordGuideDotXOffset,
                    offWordContents);

  auto block = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[totalBytes]);
  if (!block) {
    LOG_ERR("TXB", "Construction OOM: failed to alloc %u-byte data block (wc=%u, contentBytes=%u)", totalBytes, wc,
            wordContentBytes);
    return;
  }

  uint8_t* base = block.get();
  auto* wordOffsetsPtr = reinterpret_cast<uint32_t*>(base + offWordOffsets);
  auto* wordXposPtr = reinterpret_cast<int16_t*>(base + offWordXpos);
  auto* wordStylesPtr = reinterpret_cast<EpdFontFamily::Style*>(base + offWordStyles);
  auto* wordBackgroundBlackPtr = reinterpret_cast<uint8_t*>(base + offWordBackgroundBlack);
  uint8_t* wordBionicBoundaryPtr = hasBionic ? reinterpret_cast<uint8_t*>(base + offWordBionicBoundary) : nullptr;
  uint16_t* wordBionicSuffixXPtr = hasBionic ? reinterpret_cast<uint16_t*>(base + offWordBionicSuffixX) : nullptr;
  uint16_t* wordGuideDotXOffsetPtr =
      hasGuideDots ? reinterpret_cast<uint16_t*>(base + offWordGuideDotXOffset) : nullptr;
  char* wordContentsPtr = reinterpret_cast<char*>(base + offWordContents);

  uint32_t curOffset = 0;
  for (uint16_t i = 0; i < wc; ++i) {
    wordOffsetsPtr[i] = curOffset;
    const auto& w = words[i];
    if (!w.empty()) {
      std::memcpy(wordContentsPtr + curOffset, w.data(), w.size());
    }
    curOffset += static_cast<uint32_t>(w.size());
    wordContentsPtr[curOffset++] = '\0';
  }
  wordOffsetsPtr[wc] = curOffset;

  for (uint16_t i = 0; i < wc; ++i) wordXposPtr[i] = word_xpos[i];
  for (uint16_t i = 0; i < wc; ++i) wordStylesPtr[i] = word_styles[i];
  for (uint16_t i = 0; i < wc; ++i) wordBackgroundBlackPtr[i] = background_black[i];
  if (hasBionic) {
    for (uint16_t i = 0; i < wc; ++i) wordBionicBoundaryPtr[i] = bionic_boundary[i];
    for (uint16_t i = 0; i < wc; ++i) wordBionicSuffixXPtr[i] = bionic_suffix_x[i];
  }
  if (hasGuideDots) {
    for (uint16_t i = 0; i < wc; ++i) wordGuideDotXOffsetPtr[i] = guide_dot_x_offset[i];
  }

  dataBlock_ = std::move(block);
  dataBlockSize_ = totalBytes;
  wordCount_ = wc;
  wordOffsets_ = wordOffsetsPtr;
  wordContents_ = wordContentsPtr;
  wordXpos_ = wordXposPtr;
  wordStyles_ = wordStylesPtr;
  wordBackgroundBlack_ = wordBackgroundBlackPtr;
  wordBionicBoundary_ = wordBionicBoundaryPtr;
  wordBionicSuffixX_ = wordBionicSuffixXPtr;
  wordGuideDotXOffset_ = wordGuideDotXOffsetPtr;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const bool foregroundBlack) const {
  if (wordCount_ == 0 || !dataBlock_) return;
  const bool hasBionic = wordBionicBoundary_ != nullptr;
  const bool hasGuideDots = wordGuideDotXOffset_ != nullptr;
  const WordsView words = getWords();

  for (uint16_t i = 0; i < wordCount_; ++i) {
    const int wordX = wordXpos_[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles_[i];
    const uint8_t boundary = hasBionic ? wordBionicBoundary_[i] : 0;
    const WordView word = words[i];

    if (wordBackgroundBlack_[i] != 0 && isWhitespaceOnlyBackgroundToken(word)) {
      const uint16_t backgroundWidth = measureBackgroundWidth(renderer, fontId, word, currentStyle);
      if (backgroundWidth > 0) {
        renderer.fillRect(wordX, y, backgroundWidth, renderer.getFontAscenderSize(fontId), foregroundBlack);
      }
    }

    if (boundary > 0) {
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      char boldBuf[40];
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), word.size(), sizeof(boldBuf) - 1});
      std::memcpy(boldBuf, word.c_str(), boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, y, boldBuf, foregroundBlack, boldStyle);
      const int suffixX = wordX + wordBionicSuffixX_[i];
      renderer.drawText(fontId, suffixX, y, word.c_str() + boldLen, foregroundBlack, currentStyle);
    } else {
      renderer.drawText(fontId, wordX, y, word.c_str(), foregroundBlack, currentStyle);
    }

    if (hasGuideDots && wordGuideDotXOffset_[i] > 0) {
      renderer.drawText(fontId, wordX + wordGuideDotXOffset_[i], y, "\xc2\xb7", foregroundBlack, EpdFontFamily::REGULAR);
    }

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const int fullWordWidth = renderer.getTextWidth(fontId, word.c_str(), currentStyle);
      const int underlineY = y + renderer.getFontAscenderSize(fontId) + 2;
      int startX = wordX;
      int underlineWidth = fullWordWidth;
      if (word.size() >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visiblePtr = word.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }
      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, 3, foregroundBlack);
    }

    if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
      const int fullWordWidth = renderer.getTextWidth(fontId, word.c_str(), currentStyle);
      const int strikeY = y + renderer.getFontAscenderSize(fontId) / 2 + 6;
      int startX = wordX;
      int strikeWidth = fullWordWidth;
      if (word.size() >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visiblePtr = word.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        strikeWidth = visibleWidth;
      }
      renderer.drawLine(startX, strikeY, startX + strikeWidth, strikeY, 3, foregroundBlack);
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  const bool hasBionic = wordBionicBoundary_ != nullptr;
  const bool hasGuideDots = wordGuideDotXOffset_ != nullptr;

  if (!serialization::tryWritePod(file, wordCount_)) {
    LOG_ERR("TXB", "Serialization failed: could not write word count");
    return false;
  }
  const WordsView words = getWords();
  for (uint16_t i = 0; i < wordCount_; ++i) {
    const WordView w = words[i];
    if (!serialization::tryWritePod(file, static_cast<uint32_t>(w.size()))) return false;
    if (w.size() > 0 && file.write(reinterpret_cast<const uint8_t*>(w.data()), w.size()) != static_cast<int>(w.size())) {
      LOG_ERR("TXB", "Serialization failed: could not write word payload");
      return false;
    }
  }
  for (uint16_t i = 0; i < wordCount_; ++i) {
    if (!serialization::tryWritePod(file, wordXpos_[i])) return false;
  }
  for (uint16_t i = 0; i < wordCount_; ++i) {
    if (!serialization::tryWritePod(file, wordStyles_[i])) return false;
  }
  if (!serialization::tryWritePod(file, static_cast<uint8_t>(hasBionic ? 1 : 0))) return false;
  if (hasBionic) {
    for (uint16_t i = 0; i < wordCount_; ++i) {
      if (!serialization::tryWritePod(file, wordBionicBoundary_[i])) return false;
    }
    for (uint16_t i = 0; i < wordCount_; ++i) {
      if (!serialization::tryWritePod(file, wordBionicSuffixX_[i])) return false;
    }
  }
  if (!serialization::tryWritePod(file, static_cast<uint8_t>(hasGuideDots ? 1 : 0))) return false;
  if (hasGuideDots) {
    for (uint16_t i = 0; i < wordCount_; ++i) {
      if (!serialization::tryWritePod(file, wordGuideDotXOffset_[i])) return false;
    }
  }
  for (uint16_t i = 0; i < wordCount_; ++i) {
    if (!serialization::tryWritePod(file, wordBackgroundBlack_[i])) return false;
  }

  return serialization::tryWritePod(file, blockStyle_.alignment) &&
         serialization::tryWritePod(file, blockStyle_.textAlignDefined) &&
         serialization::tryWritePod(file, blockStyle_.marginTop) &&
         serialization::tryWritePod(file, blockStyle_.marginBottom) &&
         serialization::tryWritePod(file, blockStyle_.marginLeft) &&
         serialization::tryWritePod(file, blockStyle_.marginRight) &&
         serialization::tryWritePod(file, blockStyle_.paddingTop) &&
         serialization::tryWritePod(file, blockStyle_.paddingBottom) &&
         serialization::tryWritePod(file, blockStyle_.paddingLeft) &&
         serialization::tryWritePod(file, blockStyle_.paddingRight) &&
         serialization::tryWritePod(file, blockStyle_.textIndent) &&
         serialization::tryWritePod(file, blockStyle_.textIndentDefined);
}

// CrumBLE 4.4 post-bisect: deserialize allocates ONE compact data block
// and streams the on-disk fields directly into it. Two-pass:
//   Pass 1: scan word length prefixes (skipping content bytes) to compute
//           total content bytes + locate hasBionic / hasGuideDots flags.
//   Pass 2: seek back, allocate the compact block sized exactly to the
//           computed layout, stream fields into the block.
// No temporary std::vectors. Peak allocation per TextBlock collapses from
// 7+ small vector allocs + N per-word std::string allocs to ONE alloc.
std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  if (!serialization::tryReadPod(file, wc)) {
    LOG_ERR("TXB", "Deserialization failed: could not read word count");
    return nullptr;
  }
  if (wc > MAX_WORDS_PER_TEXT_BLOCK) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  const uint32_t minimumRemainingBytes = static_cast<uint32_t>(wc) * SERIALIZED_MIN_WORD_METADATA_BYTES +
                                         sizeof(uint8_t) + sizeof(uint8_t) + SERIALIZED_TEXT_BLOCK_TAIL_BYTES;
  const int remainingBeforeWords = file.available();
  if (remainingBeforeWords < 0 || static_cast<uint32_t>(remainingBeforeWords) < minimumRemainingBytes) {
    LOG_ERR("TXB", "Deserialization failed: truncated block metadata (%u words need at least %lu bytes, %d available)",
            wc, static_cast<unsigned long>(minimumRemainingBytes), remainingBeforeWords);
    return nullptr;
  }

  // ---- Pass 1: scan word lengths + locate flags without consuming bytes ----
  const uint32_t wordSectionStart = file.position();
  uint32_t totalContentBytes = 0;
  for (uint16_t i = 0; i < wc; ++i) {
    uint32_t len = 0;
    if (!serialization::tryReadPod(file, len)) {
      LOG_ERR("TXB", "Deserialization failed: could not read word %u length prefix", i);
      return nullptr;
    }
    if (len > MAX_SERIALIZED_WORD_BYTES) {
      LOG_ERR("TXB", "Deserialization failed: word %u length %lu exceeds maximum", i, static_cast<unsigned long>(len));
      return nullptr;
    }
    totalContentBytes += len + 1;  // +1 for null terminator we add in-block
    // Skip the content bytes without reading them.
    if (len > 0 && !file.seek(file.position() + len)) {
      LOG_ERR("TXB", "Deserialization failed: could not seek past word %u content", i);
      return nullptr;
    }
  }
  // Skip wordXpos + wordStyles
  if (!file.seek(file.position() + static_cast<uint32_t>(wc) * sizeof(int16_t) +
                 static_cast<uint32_t>(wc) * sizeof(EpdFontFamily::Style))) {
    return nullptr;
  }
  uint8_t hasBionicByte = 0;
  if (!serialization::tryReadPod(file, hasBionicByte) || hasBionicByte > 1) {
    LOG_ERR("TXB", "Deserialization failed: invalid bionic metadata flag");
    return nullptr;
  }
  const bool hasBionic = (hasBionicByte == 1);
  if (hasBionic) {
    if (!file.seek(file.position() + static_cast<uint32_t>(wc) * sizeof(uint8_t) +
                   static_cast<uint32_t>(wc) * sizeof(uint16_t))) {
      return nullptr;
    }
  }
  uint8_t hasGuideDotsByte = 0;
  if (!serialization::tryReadPod(file, hasGuideDotsByte) || hasGuideDotsByte > 1) {
    LOG_ERR("TXB", "Deserialization failed: invalid guide-dot metadata flag");
    return nullptr;
  }
  const bool hasGuideDots = (hasGuideDotsByte == 1);

  // ---- Pass 2: compute layout, allocate single block, stream into it ----
  uint32_t offWordOffsets, offWordXpos, offWordStyles, offWordBackgroundBlack;
  uint32_t offWordBionicBoundary, offWordBionicSuffixX, offWordGuideDotXOffset, offWordContents;
  const uint32_t totalBytes =
      computeLayout(wc, totalContentBytes, hasBionic, hasGuideDots, offWordOffsets, offWordXpos, offWordStyles,
                    offWordBackgroundBlack, offWordBionicBoundary, offWordBionicSuffixX, offWordGuideDotXOffset,
                    offWordContents);

  // Pre-flight: skip the alloc entirely if MaxAlloc can't cover the block.
  // 128 byte margin for allocator metadata overhead.
  //
  // CrumBLE 4.5.7 v18.2 tried trimming to 24 and REVERTED: the 128 margin
  // is not conservative, it's a safety valve. When BT connect leaves the
  // heap at ~10 KB free with heavy fragmentation, refusing the first
  // text-block deserialize triggers the graceful BT-cycle recovery (the
  // page then loads at 68 KB free with BT off). Allowing that first small
  // alloc through instead lets rendering advance until a DOWNSTREAM alloc
  // fails with bad_alloc -> std::terminate mid-render, leaving the panel
  // half-painted before the panic reset. Refusing early wins.
  if (ESP.getMaxAllocHeap() < totalBytes + 128) {
    LOG_ERR("TXB", "Refusing dataBlock alloc(%u): maxAlloc=%u < needed=%u (wc=%u)", totalBytes,
            ESP.getMaxAllocHeap(), totalBytes + 128, wc);
    return nullptr;
  }
  auto block = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[totalBytes]);
  if (!block) {
    LOG_ERR("TXB", "dataBlock alloc(%u) returned nullptr", totalBytes);
    return nullptr;
  }
  uint8_t* base = block.get();
  auto* wordOffsetsPtr = reinterpret_cast<uint32_t*>(base + offWordOffsets);
  auto* wordXposPtr = reinterpret_cast<int16_t*>(base + offWordXpos);
  auto* wordStylesPtr = reinterpret_cast<EpdFontFamily::Style*>(base + offWordStyles);
  auto* wordBackgroundBlackPtr = reinterpret_cast<uint8_t*>(base + offWordBackgroundBlack);
  uint8_t* wordBionicBoundaryPtr = hasBionic ? reinterpret_cast<uint8_t*>(base + offWordBionicBoundary) : nullptr;
  uint16_t* wordBionicSuffixXPtr = hasBionic ? reinterpret_cast<uint16_t*>(base + offWordBionicSuffixX) : nullptr;
  uint16_t* wordGuideDotXOffsetPtr =
      hasGuideDots ? reinterpret_cast<uint16_t*>(base + offWordGuideDotXOffset) : nullptr;
  char* wordContentsPtr = reinterpret_cast<char*>(base + offWordContents);

  // Seek back to the word section and stream-read into block.
  if (!file.seek(wordSectionStart)) {
    LOG_ERR("TXB", "Deserialization failed: could not seek back to word section");
    return nullptr;
  }
  uint32_t curOffset = 0;
  for (uint16_t i = 0; i < wc; ++i) {
    uint32_t len = 0;
    if (!serialization::tryReadPod(file, len)) return nullptr;
    wordOffsetsPtr[i] = curOffset;
    if (len > 0) {
      if (file.read(reinterpret_cast<uint8_t*>(wordContentsPtr) + curOffset, len) != static_cast<int>(len)) {
        LOG_ERR("TXB", "Deserialization failed: could not stream-read word %u content", i);
        return nullptr;
      }
      curOffset += len;
    }
    wordContentsPtr[curOffset++] = '\0';
  }
  wordOffsetsPtr[wc] = curOffset;

  for (uint16_t i = 0; i < wc; ++i) {
    if (!serialization::tryReadPod(file, wordXposPtr[i])) return nullptr;
  }
  for (uint16_t i = 0; i < wc; ++i) {
    if (!serialization::tryReadPod(file, wordStylesPtr[i])) return nullptr;
  }
  uint8_t skipFlag = 0;
  if (!serialization::tryReadPod(file, skipFlag)) return nullptr;  // hasBionic, already known
  if (hasBionic) {
    for (uint16_t i = 0; i < wc; ++i) {
      if (!serialization::tryReadPod(file, wordBionicBoundaryPtr[i])) return nullptr;
    }
    for (uint16_t i = 0; i < wc; ++i) {
      if (!serialization::tryReadPod(file, wordBionicSuffixXPtr[i])) return nullptr;
    }
  }
  if (!serialization::tryReadPod(file, skipFlag)) return nullptr;  // hasGuideDots, already known
  if (hasGuideDots) {
    for (uint16_t i = 0; i < wc; ++i) {
      if (!serialization::tryReadPod(file, wordGuideDotXOffsetPtr[i])) return nullptr;
    }
  }
  for (uint16_t i = 0; i < wc; ++i) {
    if (!serialization::tryReadPod(file, wordBackgroundBlackPtr[i])) return nullptr;
  }

  BlockStyle blockStyle;
  if (!serialization::tryReadPod(file, blockStyle.alignment) ||
      !serialization::tryReadPod(file, blockStyle.textAlignDefined) ||
      !serialization::tryReadPod(file, blockStyle.marginTop) ||
      !serialization::tryReadPod(file, blockStyle.marginBottom) ||
      !serialization::tryReadPod(file, blockStyle.marginLeft) ||
      !serialization::tryReadPod(file, blockStyle.marginRight) ||
      !serialization::tryReadPod(file, blockStyle.paddingTop) ||
      !serialization::tryReadPod(file, blockStyle.paddingBottom) ||
      !serialization::tryReadPod(file, blockStyle.paddingLeft) ||
      !serialization::tryReadPod(file, blockStyle.paddingRight) ||
      !serialization::tryReadPod(file, blockStyle.textIndent) ||
      !serialization::tryReadPod(file, blockStyle.textIndentDefined)) {
    LOG_ERR("TXB", "Deserialization failed: truncated block style metadata");
    return nullptr;
  }

  auto* textBlock = new (std::nothrow) TextBlock(std::move(block), totalBytes, wc, wordOffsetsPtr, wordContentsPtr,
                                                  wordXposPtr, wordStylesPtr, wordBionicBoundaryPtr,
                                                  wordBionicSuffixXPtr, wordGuideDotXOffsetPtr,
                                                  wordBackgroundBlackPtr, blockStyle);
  if (!textBlock) {
    LOG_ERR("TXB", "Deserialization failed: could not allocate TextBlock wrapper");
    return nullptr;
  }
  return std::unique_ptr<TextBlock>(textBlock);
}
