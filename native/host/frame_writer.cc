#include "frame_writer.hh"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>

#include "embedder/embedder.hh"
#include "skyline/frame_protocol.h"

namespace skyline {
namespace {

bool IsValidKey(const std::string& key) {
  if (key.empty() || key.size() > 200) return false;
  for (unsigned char c : key) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

void PutU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

void PutU64(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

std::string ErrnoMessage(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

SharedFrameWriter::SharedFrameWriter(std::string key) : key_(std::move(key)) {
  if (!IsValidKey(key_)) return;
  path_ = std::string(SKYLINE_SHM_DIR "/" SKYLINE_SHM_NAME_PREFIX) + key_ +
          SKYLINE_SHM_NAME_SUFFIX;
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

SharedFrameWriter::~SharedFrameWriter() {
  if (mapping_ != nullptr && file_size_ > 0) {
    ::msync(mapping_, file_size_, MS_SYNC);
    ::munmap(mapping_, file_size_);
    mapping_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool SharedFrameWriter::WriteFrame(uint32_t frame_id, const void* pixels,
                                   size_t frame_size, std::string* error) {
  if (fd_ < 0) {
    if (error) *error = "shared memory file is not open (invalid key?)";
    return false;
  }
  if (pixels == nullptr || frame_size == 0) {
    if (error) *error = "empty frame";
    return false;
  }
  if (frame_size > (std::numeric_limits<size_t>::max() -
                    SKYLINE_FRAME_HEADER_BYTES) /
                       SKYLINE_FRAME_BUDGET) {
    if (error) *error = "frame too large";
    return false;
  }

  // Official DevTools budgets four RGBA frames plus the 8-byte frame header.
  // The data region and outer header sizes come from contract 4.3.
  const size_t required_data =
      SKYLINE_FRAME_HEADER_BYTES + frame_size * SKYLINE_FRAME_BUDGET;
  const size_t required_file = SKYLINE_SHM_HEADER_BYTES + required_data;
  if (mapping_ == nullptr || required_file > file_size_) {
    struct statvfs vfs {};
    if (::statvfs("/dev/shm", &vfs) == 0) {
      const uint64_t available =
          static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
      if (available < required_file) {
        if (error) {
          *error = "not enough space in /dev/shm (need " +
                   std::to_string(required_file) + ", have " +
                   std::to_string(available) + ")";
        }
        return false;
      }
    }
    if (mapping_ != nullptr) {
      ::munmap(mapping_, file_size_);
      mapping_ = nullptr;
    }
    if (::ftruncate(fd_, static_cast<off_t>(required_file)) != 0) {
      if (error) *error = ErrnoMessage("ftruncate");
      return false;
    }
    void* mapping = ::mmap(nullptr, required_file, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd_, 0);
    if (mapping == MAP_FAILED) {
      if (error) *error = ErrnoMessage("mmap");
      return false;
    }
    std::memset(mapping, 0, required_file);
    mapping_ = static_cast<uint8_t*>(mapping);
    file_size_ = required_file;
    data_size_ = required_data;
    PutU64(mapping_, static_cast<uint64_t>(data_size_));
    PutU32(mapping_ + 8, SKYLINE_SHM_VERSION);
    // bytes 12..15 stay zero (alignment padding).
  }

  uint8_t* data = mapping_ + SKYLINE_SHM_HEADER_BYTES;
  std::memcpy(data + SKYLINE_FRAME_HEADER_BYTES, pixels, frame_size);
  embedder::WriteFrameHeader(data, frame_id, static_cast<uint32_t>(frame_size));
  if (::msync(mapping_, file_size_, MS_SYNC) != 0) {
    if (error) *error = ErrnoMessage("msync");
    return false;
  }
  return true;
}

}  // namespace skyline
