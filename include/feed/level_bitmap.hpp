#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace feed {

// Occupied-tick bitset, shared in spirit with lob-engine's BookSide: O(1)
// best-tick lookup via ctz/clz over 64-bit words. Used to find the highest
// occupied bid tick and the lowest occupied ask tick without scanning the
// quantity array.
template <std::size_t NLevels>
class LevelBitmap {
public:
  static constexpr std::size_t kWords = (NLevels + 63) / 64;

  void set(std::size_t i) noexcept {
    words_[i >> 6] |= (uint64_t{1} << (i & 63));
  }

  void clear(std::size_t i) noexcept {
    words_[i >> 6] &= ~(uint64_t{1} << (i & 63));
  }

  bool test(std::size_t i) const noexcept {
    return ((words_[i >> 6] >> (i & 63)) & uint64_t{1}) != 0;
  }

  bool empty() const noexcept {
    for (std::size_t w = 0; w < kWords; ++w) {
      if (words_[w] != 0) return false;
    }
    return true;
  }

  // Lowest occupied tick. Precondition: !empty().
  std::size_t min() const noexcept {
    for (std::size_t w = 0; w < kWords; ++w) {
      if (words_[w] != 0) {
        return (w << 6) + static_cast<std::size_t>(std::countr_zero(words_[w]));
      }
    }
    return 0; // unreachable when !empty()
  }

  // Highest occupied tick. Precondition: !empty().
  std::size_t max() const noexcept {
    for (std::size_t w = kWords; w-- > 0;) {
      if (words_[w] != 0) {
        return (w << 6) + 63u -
               static_cast<std::size_t>(std::countl_zero(words_[w]));
      }
    }
    return 0; // unreachable when !empty()
  }

  void clear_all() noexcept { words_.fill(0); }

private:
  std::array<uint64_t, kWords> words_{};
};

} // namespace feed
