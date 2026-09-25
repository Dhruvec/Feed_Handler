#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace feed {

struct OrderEntry {
  OrderId id        = 0;
  Side    side      = Side::Bid;
  Price   price     = 0;
  Qty     remaining = 0;
};

// Open-addressing index order_id -> resting order details, with tombstone
// deletion and load-factor-triggered growth. Execute/Cancel/Delete carry only
// an order id, not a price, so an aggregate L1 book cannot decrement the
// correct level without this lookup. Growth is amortised: no per-event
// allocation on the steady-state path.
class OrderIndex {
public:
  explicit OrderIndex(std::size_t capacity_pow2 = 1u << 16)
      : slots_(capacity_pow2) {
    mask_ = capacity_pow2 - 1;
  }

  void clear() noexcept {
    for (auto& s : slots_) s.state = kEmpty;
    size_ = 0;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

  // Insert or overwrite. Always succeeds (grows as needed).
  bool insert(const OrderEntry& e) noexcept {
    if ((size_ + 1) * 10 >= slots_.size() * 7) grow();
    insert_no_grow(e);
    return true;
  }

  OrderEntry* find(OrderId id) noexcept {
    std::size_t i = hash(id) & mask_;
    for (;;) {
      Slot& s = slots_[i];
      if (s.state == kEmpty) return nullptr;
      if (s.state == kUsed && s.entry.id == id) return &s.entry;
      i = (i + 1) & mask_;
    }
  }

  bool erase(OrderId id) noexcept {
    std::size_t i = hash(id) & mask_;
    for (;;) {
      Slot& s = slots_[i];
      if (s.state == kEmpty) return false;
      if (s.state == kUsed && s.entry.id == id) {
        s.state = kTomb;
        --size_;
        return true;
      }
      i = (i + 1) & mask_;
    }
  }

private:
  enum : std::uint8_t { kEmpty = 0, kUsed = 1, kTomb = 2 };
  static constexpr std::size_t kUnset = static_cast<std::size_t>(-1);

  struct Slot {
    OrderEntry   entry{};
    std::uint8_t state = kEmpty;
  };

  static std::size_t hash(OrderId id) noexcept {
    uint64_t z = id + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<std::size_t>(z ^ (z >> 31));
  }

  void insert_no_grow(const OrderEntry& e) noexcept {
    std::size_t i = hash(e.id) & mask_;
    std::size_t first_tomb = kUnset;
    for (;;) {
      Slot& s = slots_[i];
      if (s.state == kUsed) {
        if (s.entry.id == e.id) { s.entry = e; return; }
      } else if (s.state == kTomb) {
        if (first_tomb == kUnset) first_tomb = i;
      } else { // kEmpty
        const std::size_t dst = (first_tomb == kUnset) ? i : first_tomb;
        Slot& d = slots_[dst];
        d.entry = e;
        d.state = kUsed;
        ++size_;
        return;
      }
      i = (i + 1) & mask_;
    }
  }

  void grow() noexcept {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(old.size() * 2, Slot{});
    mask_ = slots_.size() - 1;
    size_ = 0;
    for (Slot& s : old) {
      if (s.state == kUsed) insert_no_grow(s.entry);
    }
  }

  std::size_t       mask_ = 0;
  std::vector<Slot> slots_;
  std::size_t       size_ = 0;
};

} // namespace feed
