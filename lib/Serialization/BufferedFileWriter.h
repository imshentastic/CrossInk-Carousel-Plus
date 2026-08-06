#pragma once
#include <HalStorage.h>
#include <Serialization.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// v18.9.9.479: write-through buffer over an FsFile.
//
// Why this exists: page serialization used to issue one FsFile::write() per
// field (see serialization::tryWritePod). A ~250-word page emits roughly 2000
// of those, and SdFat's per-call overhead -- not the byte count -- dominated,
// costing ~500 ms per page (about half of all chapter-indexing time).
// Coalescing the fields into a small RAM buffer turns that into a handful of
// SD writes per page. The bytes handed to the file are byte-for-byte the same
// bytes, in the same order, so the on-disk section format is unchanged.
//
// Ownership: the buffer is CALLER-OWNED (heap-allocated by the caller and
// reused across pages). This class never allocates. Pass buffer == nullptr /
// capacity == 0 to get a pass-through writer that behaves exactly like the
// old unbuffered path -- that's the OOM fallback, not an error.
//
// Position accounting: `position()` returns the LOGICAL write position
// (underlying file position + bytes sitting in the buffer). Callers that need
// the real file position -- to record a page offset, seek, close, or query
// size -- must flush() first, or go through seek() which flushes for them.
// Getting this wrong silently corrupts page seeking, so treat any direct use
// of the underlying FsFile while a writer is live as a bug.
//
// Failure handling: any short/failed write latches a sticky failure. Once
// latched, write() and flush() return failure without touching the file, so a
// truncated page can never be reported as a success.
class BufferedFileWriter {
 public:
  BufferedFileWriter(FsFile& file, uint8_t* buffer, const size_t capacity)
      : file_(file),
        buf_(capacity > 0 ? buffer : nullptr),
        cap_(buffer != nullptr ? capacity : 0),
        logicalPos_(static_cast<uint32_t>(file.position())) {}

  // Backstop only. Callers must call flush() explicitly and check its result;
  // a destructor cannot report failure.
  ~BufferedFileWriter() { (void)flush(); }

  BufferedFileWriter(const BufferedFileWriter&) = delete;
  BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;
  BufferedFileWriter(BufferedFileWriter&&) = delete;
  BufferedFileWriter& operator=(BufferedFileWriter&&) = delete;

  // Returns bytes accepted. Anything other than `len` means failure (and
  // latches failed_).
  size_t write(const uint8_t* data, const size_t len) {
    if (failed_) return 0;
    if (len == 0) return 0;
    // Writes that can't fit in the buffer bypass it entirely: flush what's
    // pending (to keep byte order) and hand the payload straight to the file
    // rather than copying it through in slices.
    if (buf_ == nullptr || len >= cap_) {
      if (!flush()) return 0;
      const size_t written = file_.write(data, len);
      logicalPos_ += static_cast<uint32_t>(written);
      if (written != len) failed_ = true;
      return written;
    }
    if (used_ + len > cap_ && !flush()) return 0;
    std::memcpy(buf_ + used_, data, len);
    used_ += len;
    logicalPos_ += static_cast<uint32_t>(len);
    return len;
  }

  // Pushes any buffered bytes to the file. Safe to call repeatedly.
  bool flush() {
    if (failed_) return false;
    if (used_ == 0) return true;
    const size_t pending = used_;
    used_ = 0;  // cleared first so a failed flush can't re-emit the bytes
    if (file_.write(buf_, pending) != pending) {
      failed_ = true;
      return false;
    }
    return true;
  }

  // Logical write position: what file.position() would report after a flush.
  uint32_t position() const { return logicalPos_; }

  // Flushes, then moves the underlying file cursor. Used by the table
  // fragment size-prefix patch in Page::serialize.
  bool seek(const uint32_t pos) {
    if (!flush()) return false;
    if (!file_.seek(pos)) {
      failed_ = true;
      return false;
    }
    logicalPos_ = pos;
    return true;
  }

  bool ok() const { return !failed_; }

 private:
  FsFile& file_;
  uint8_t* buf_;
  size_t cap_;
  size_t used_ = 0;
  uint32_t logicalPos_;
  bool failed_ = false;
};

namespace serialization {
// Buffered mirrors of the FsFile overloads in Serialization.h. Same bytes,
// same order -- only the syscall granularity differs.
template <typename T>
static bool tryWritePod(BufferedFileWriter& out, const T& value) {
  return out.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

inline bool tryWriteString(BufferedFileWriter& out, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(out, len) &&
         (len == 0 || out.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}
}  // namespace serialization
