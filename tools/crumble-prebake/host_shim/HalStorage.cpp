#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Logging.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// HalFile::Impl  --  the std::fstream behind every HalFile we hand out.
// ---------------------------------------------------------------------------

struct HalFile::Impl {
  std::fstream stream;
  std::string path;
  bool readOnly = false;
  bool open = false;
  size_t fileSize = 0;

  bool openForRead(const std::string& p) {
    path = p;
    stream.open(p, std::ios::in | std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    fileSize = static_cast<size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);
    readOnly = true;
    open = true;
    return true;
  }
  bool openForWrite(const std::string& p) {
    path = p;
    // Match SdFat's wb+ semantics: create / truncate / read-write.
    stream.open(p, std::ios::out | std::ios::in | std::ios::trunc | std::ios::binary);
    if (!stream) {
      // No existing file: fall back to write-only create.
      stream.clear();
      stream.open(p, std::ios::out | std::ios::trunc | std::ios::binary);
      if (!stream) return false;
    }
    readOnly = false;
    open = true;
    fileSize = 0;
    return true;
  }
  void close() {
    if (open) {
      stream.flush();
      stream.close();
      open = false;
    }
  }
};

// ---------------------------------------------------------------------------
// HalFile -- rule of 5 + the actual stream-ops surface.
// ---------------------------------------------------------------------------

HalFile::HalFile() : impl_(std::make_unique<Impl>()) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) noexcept = default;
HalFile& HalFile::operator=(HalFile&&) noexcept = default;

void HalFile::flush() {
  if (impl_ && impl_->open) impl_->stream.flush();
}
size_t HalFile::size() {
  if (!impl_ || !impl_->open) return 0;
  if (impl_->readOnly) return impl_->fileSize;
  // Write mode: file size grows; query directly via tellp of end position.
  const auto cur = impl_->stream.tellp();
  impl_->stream.seekp(0, std::ios::end);
  const auto end = impl_->stream.tellp();
  impl_->stream.seekp(cur, std::ios::beg);
  return static_cast<size_t>(end);
}
bool HalFile::seek(size_t pos) {
  if (!impl_ || !impl_->open) return false;
  impl_->stream.clear();  // clear EOF if set
  if (impl_->readOnly) {
    impl_->stream.seekg(pos, std::ios::beg);
    return impl_->stream.good();
  }
  impl_->stream.seekg(pos, std::ios::beg);
  impl_->stream.seekp(pos, std::ios::beg);
  return impl_->stream.good();
}
bool HalFile::seek64(uint64_t pos) { return seek(static_cast<size_t>(pos)); }
bool HalFile::seekCur(int64_t offset) {
  if (!impl_ || !impl_->open) return false;
  impl_->stream.clear();
  impl_->stream.seekg(offset, std::ios::cur);
  if (!impl_->readOnly) impl_->stream.seekp(offset, std::ios::cur);
  return impl_->stream.good();
}
bool HalFile::seekSet(size_t offset) { return seek(offset); }
int HalFile::available() const {
  if (!impl_ || !impl_->open) return 0;
  const auto pos = const_cast<std::fstream&>(impl_->stream).tellg();
  if (pos < 0) return 0;
  if (impl_->readOnly) {
    const auto avail = static_cast<int64_t>(impl_->fileSize) - pos;
    return avail > 0 ? static_cast<int>(avail) : 0;
  }
  return 0;
}
size_t HalFile::position() const {
  if (!impl_ || !impl_->open) return 0;
  const auto pos = const_cast<std::fstream&>(impl_->stream).tellg();
  return pos < 0 ? 0 : static_cast<size_t>(pos);
}
int HalFile::read(void* buf, size_t count) {
  if (!impl_ || !impl_->open) return -1;
  impl_->stream.read(static_cast<char*>(buf), static_cast<std::streamsize>(count));
  const auto got = impl_->stream.gcount();
  if (got == 0 && !impl_->stream) return -1;
  return static_cast<int>(got);
}
int HalFile::read() {
  uint8_t b = 0;
  if (read(&b, 1) <= 0) return -1;
  return b;
}
size_t HalFile::write(const void* buf, size_t count) {
  if (!impl_ || !impl_->open || impl_->readOnly) return 0;
  impl_->stream.write(static_cast<const char*>(buf), static_cast<std::streamsize>(count));
  return impl_->stream.good() ? count : 0;
}
size_t HalFile::write(const uint8_t* buf, size_t count) { return write(static_cast<const void*>(buf), count); }
size_t HalFile::write(uint8_t b) { return write(&b, 1); }
bool HalFile::sync() {
  if (!impl_ || !impl_->open) return false;
  impl_->stream.flush();
  return impl_->stream.good();
}
bool HalFile::isOpen() const { return impl_ && impl_->open; }
bool HalFile::close() {
  if (impl_) {
    impl_->close();
    return true;
  }
  return false;
}
HalFile HalFile::openNextFile() { return HalFile{}; }

// ---------------------------------------------------------------------------
// HalStorage -- file-system operations against the host's std::filesystem.
// ---------------------------------------------------------------------------

namespace {
fs::path toPath(const char* p) { return fs::path{p ? p : ""}; }
}  // namespace

std::vector<std::string> HalStorage::listFiles(const char*, int) { return {}; }

std::string HalStorage::readFile(const char* path) {
  std::ifstream f(toPath(path), std::ios::binary);
  if (!f) return {};
  std::string out{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
  return out;
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  std::ifstream f(toPath(path), std::ios::binary);
  if (!f) return false;
  std::vector<char> buf(chunkSize);
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(chunkSize));
    const auto got = f.gcount();
    if (got <= 0) break;
    if (out.write(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(got)) != static_cast<size_t>(got)) {
      return false;
    }
  }
  return true;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  std::ifstream f(toPath(path), std::ios::binary);
  if (!f) {
    buffer[0] = '\0';
    return 0;
  }
  const size_t cap = bufferSize - 1;
  const size_t want = (maxBytes && maxBytes < cap) ? maxBytes : cap;
  f.read(buffer, static_cast<std::streamsize>(want));
  const auto got = f.gcount();
  buffer[got] = '\0';
  return static_cast<size_t>(got);
}

bool HalStorage::writeFile(const char* path, const std::string& content) {
  std::ofstream f(toPath(path), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  return f.good();
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  std::error_code ec;
  fs::create_directories(toPath(path), ec);
  return !ec;
}

HalFile HalStorage::open(const char* path, oflag_t oflag) {
  HalFile f;
  const bool want_write = (oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
  if (want_write) {
    f.impl_->openForWrite(path);
  } else {
    f.impl_->openForRead(path);
  }
  return f;
}

bool HalStorage::mkdir(const char* path, bool) {
  std::error_code ec;
  fs::create_directories(toPath(path), ec);
  return !ec;
}
bool HalStorage::exists(const char* path) {
  std::error_code ec;
  return fs::exists(toPath(path), ec);
}
bool HalStorage::remove(const char* path) {
  std::error_code ec;
  return fs::remove(toPath(path), ec);
}
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  std::error_code ec;
  fs::rename(toPath(oldPath), toPath(newPath), ec);
  return !ec;
}
bool HalStorage::rmdir(const char* path) {
  std::error_code ec;
  return fs::remove(toPath(path), ec) || fs::remove_all(toPath(path), ec) > 0;
}
bool HalStorage::removeDir(const char* path) {
  std::error_code ec;
  return fs::remove_all(toPath(path), ec) > 0 || !ec;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  file = HalFile{};
  if (!file.impl_->openForRead(path)) {
    LOG_DBG(moduleName ? moduleName : "FS", "File does not exist: %s", path);
    return false;
  }
  return true;
}
bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  file = HalFile{};
  // Match firmware behaviour: auto-create parent directories.
  std::error_code ec;
  fs::create_directories(fs::path{path}.parent_path(), ec);
  if (!file.impl_->openForWrite(path)) {
    LOG_ERR(moduleName ? moduleName : "FS", "Could not open file for write: %s", path);
    return false;
  }
  return true;
}
bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
