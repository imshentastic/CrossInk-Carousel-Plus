#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// v18.9.9.479: serialize() writes through a small RAM buffer (see
// <BufferedFileWriter.h>) instead of one SdFat call per field.
class BufferedFileWriter;

// CrumBLE 4.4 post-bisect: lightweight view types so callers can access
// TextBlock's compact storage without materializing per-word std::strings.
// WordView is a string-like reference into the TextBlock's owned data
// block; WordsView is an indexable + iterable collection of WordView.
// Lifetime: views are valid only while the source TextBlock is alive.
class WordView {
 public:
  WordView() : data_(""), size_(0) {}
  WordView(const char* data, uint16_t size) : data_(data), size_(size) {}
  const char* c_str() const { return data_; }
  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  char operator[](size_t i) const { return data_[i]; }
  // Implicit conversion to std::string_view so callers using string_view-aware
  // APIs (std::string::operator+=, std::string ctor, etc.) work unchanged.
  operator std::string_view() const { return std::string_view(data_, size_); }

 private:
  const char* data_;
  uint16_t size_;
};

class WordsView {
 public:
  WordsView() : contents_(nullptr), offsets_(nullptr), count_(0) {}
  WordsView(const char* contents, const uint32_t* offsets, uint16_t count)
      : contents_(contents), offsets_(offsets), count_(count) {}

  WordView operator[](size_t i) const {
    if (i >= count_) return WordView();
    const uint32_t start = offsets_[i];
    const uint32_t end = offsets_[i + 1];
    // Strings are stored null-terminated; size excludes the terminator.
    const uint16_t len = (end > start) ? static_cast<uint16_t>(end - start - 1) : 0;
    return WordView(contents_ + start, len);
  }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  WordView front() const { return (*this)[0]; }

  class iterator {
   public:
    iterator(const WordsView* v, size_t i) : v_(v), i_(i) {}
    WordView operator*() const { return (*v_)[i_]; }
    iterator& operator++() {
      ++i_;
      return *this;
    }
    bool operator==(const iterator& o) const { return i_ == o.i_; }
    bool operator!=(const iterator& o) const { return i_ != o.i_; }

   private:
    const WordsView* v_;
    size_t i_;
  };
  iterator begin() const { return iterator(this, 0); }
  iterator end() const { return iterator(this, count_); }

 private:
  const char* contents_;
  const uint32_t* offsets_;
  uint16_t count_;
};

// Represents a line of text on a page.
//
// CrumBLE 4.4 post-bisect: collapsed 7 std::vector members + N std::string
// content allocations into ONE owned data block. Lays out fixed-size arrays
// first, packs null-terminated word strings at the tail. Cuts per-Page-DOM
// allocation count from ~210+ (30 TextBlocks * 7 vectors + strings) to ~30
// (one block per TextBlock). On the ESP32-C3 RISC-V heap, the previous
// fragmented pattern was the main bad_alloc trigger under post-NimBLE
// pressure; one alloc per TextBlock is much less likely to fail mid-deserialize.
class TextBlock final : public Block {
 public:
  // Parser path: takes vectors (built up word-by-word during HTML parse),
  // copies their contents into a single compact data block.
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, std::vector<uint8_t> bionic_boundary,
                     std::vector<uint16_t> bionic_suffix_x, std::vector<uint16_t> guide_dot_x_offset,
                     std::vector<uint8_t> background_black, const BlockStyle& blockStyle = BlockStyle());

  ~TextBlock() override = default;

  void setBlockStyle(const BlockStyle& blockStyle) { blockStyle_ = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle_; }
  WordsView getWords() const { return WordsView(wordContents_, wordOffsets_, wordCount_); }
  std::span<const int16_t> getWordXpos() const {
    return wordXpos_ ? std::span<const int16_t>(wordXpos_, wordCount_) : std::span<const int16_t>();
  }
  std::span<const EpdFontFamily::Style> getWordStyles() const {
    return wordStyles_ ? std::span<const EpdFontFamily::Style>(wordStyles_, wordCount_)
                       : std::span<const EpdFontFamily::Style>();
  }
  bool isEmpty() override { return wordCount_ == 0; }
  size_t wordCount() const { return wordCount_; }
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y, bool foregroundBlack = true) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(BufferedFileWriter& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);

  // Diagnostic: size of the owned compact data block, or 0 if alloc failed.
  uint32_t getDataBlockSize() const { return dataBlockSize_; }

 private:
  // Internal constructor used by deserialize when it has already allocated
  // and populated the compact data block directly from disk.
  TextBlock(std::unique_ptr<uint8_t[]> block, uint32_t blockSize, uint16_t wordCount, const uint32_t* wordOffsets,
            const char* wordContents, const int16_t* wordXpos, const EpdFontFamily::Style* wordStyles,
            const uint8_t* wordBionicBoundary, const uint16_t* wordBionicSuffixX,
            const uint16_t* wordGuideDotXOffset, const uint8_t* wordBackgroundBlack, const BlockStyle& blockStyle)
      : dataBlock_(std::move(block)),
        dataBlockSize_(blockSize),
        wordCount_(wordCount),
        wordOffsets_(wordOffsets),
        wordContents_(wordContents),
        wordXpos_(wordXpos),
        wordStyles_(wordStyles),
        wordBionicBoundary_(wordBionicBoundary),
        wordBionicSuffixX_(wordBionicSuffixX),
        wordGuideDotXOffset_(wordGuideDotXOffset),
        wordBackgroundBlack_(wordBackgroundBlack),
        blockStyle_(blockStyle) {}

  // Single owned data block. Holds all per-word arrays + packed
  // null-terminated word string contents. Layout is computed at
  // construction time; offsets are baked into the pointer members below.
  std::unique_ptr<uint8_t[]> dataBlock_;
  uint32_t dataBlockSize_ = 0;

  // View pointers into dataBlock_. nullptr means the field is absent
  // (e.g. wordBionicBoundary_ when no word in this block has a bionic
  // split). All non-null pointers reference memory inside dataBlock_.
  uint16_t wordCount_ = 0;
  const uint32_t* wordOffsets_ = nullptr;            // [wordCount_ + 1]; sentinel at [wordCount_]
  const char* wordContents_ = nullptr;               // packed null-terminated strings
  const int16_t* wordXpos_ = nullptr;
  const EpdFontFamily::Style* wordStyles_ = nullptr;
  const uint8_t* wordBionicBoundary_ = nullptr;
  const uint16_t* wordBionicSuffixX_ = nullptr;
  const uint16_t* wordGuideDotXOffset_ = nullptr;
  const uint8_t* wordBackgroundBlack_ = nullptr;

  BlockStyle blockStyle_;

  // Compute the byte layout of the compact block. Returns total bytes
  // required; out-parameters give the offset of each array inside the block.
  static uint32_t computeLayout(uint16_t wordCount, uint32_t wordContentBytes, bool hasBionic, bool hasGuideDots,
                                uint32_t& outOffWordOffsets, uint32_t& outOffWordXpos, uint32_t& outOffWordStyles,
                                uint32_t& outOffWordBackgroundBlack, uint32_t& outOffWordBionicBoundary,
                                uint32_t& outOffWordBionicSuffixX, uint32_t& outOffWordGuideDotXOffset,
                                uint32_t& outOffWordContents);
};
