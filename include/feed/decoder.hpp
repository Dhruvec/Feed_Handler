#pragma once

#include "types.hpp"

#include <cstddef>

namespace feed {

// Fixed-width record -> typed message. Because every record is exactly
// kWireRecordSize bytes and the struct is packed (alignment 1), each record is
// a reinterpret_cast over the replay buffer: no per-field parsing, no
// intermediate representation, no allocation on the hot path.
class Decoder {
public:
  Decoder(const std::byte* data, std::size_t bytes) noexcept
      : data_(data), bytes_(bytes) {}

  // Returns the next record, or nullptr once the buffer is exhausted. A
  // trailing partial record (bytes not a whole multiple of the record size) is
  // treated as truncation and ignored.
  const WireRecord* next() noexcept {
    if (offset_ + kWireRecordSize > bytes_) return nullptr;
    const std::byte* p = data_ + offset_;
    offset_ += kWireRecordSize;
    return reinterpret_cast<const WireRecord*>(p);
  }

  std::size_t offset() const noexcept { return offset_; }
  std::size_t remaining() const noexcept { return bytes_ - offset_; }
  std::size_t total_records() const noexcept { return bytes_ / kWireRecordSize; }

private:
  const std::byte* data_;
  std::size_t      bytes_;
  std::size_t      offset_ = 0;
};

} // namespace feed
