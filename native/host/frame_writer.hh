#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace skyline {

// Writes one frame into the Linux shared-memory file format documented in
// docs/contract.md section 4:
//
//   outer header (16 bytes): u64 data_size | i32 version(1) | 4 bytes padding
//   data region:             u32 frame_id | u32 frame_size | RGBA8 pixels
//
// The file lives at /dev/shm/skyline_<key>.dat, matching skyline-shared-memory.
class SharedFrameWriter {
 public:
  explicit SharedFrameWriter(std::string key);
  ~SharedFrameWriter();

  SharedFrameWriter(const SharedFrameWriter&) = delete;
  SharedFrameWriter& operator=(const SharedFrameWriter&) = delete;

  bool Valid() const { return fd_ >= 0; }
  const std::string& path() const { return path_; }

  // Grows the mapping on the first frame (official glue budgets four frames),
  // zeroes newly allocated bytes, then publishes frame_size followed by frame_id.
  bool WriteFrame(uint32_t frame_id, const void* pixels, size_t frame_size,
                  std::string* error);

 private:
  std::string key_;
  std::string path_;
  int fd_ = -1;
  uint8_t* mapping_ = nullptr;
  size_t data_size_ = 0;
  size_t file_size_ = 0;
};

}  // namespace skyline
