#pragma once

#include "level_bitmap.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace feed {

// Aggregate-only, best bid/ask book for one symbol. Same array-of-quantity-per-
// tick + occupied-tick bitmap scheme as lob-engine's BookSide, stripped of the
// intrusive per-order FIFO list: L1 only needs "best occupied tick" and "total
// quantity resting there", not order-by-order priority.
//
// Price ticks are indexed relative to `tick_base`: index = price - tick_base.
// A price outside [tick_base, tick_base + NLevels) is rejected (out of range),
// which the generator never produces.
template <std::size_t NLevels>
class L1Book {
public:
  struct Top {
    bool    has_bid = false;
    Price   bid = 0;
    Qty     bid_qty = 0;
    bool    has_ask = false;
    Price   ask = 0;
    Qty     ask_qty = 0;
  };

  explicit L1Book(Price tick_base = 0) noexcept : tick_base_(tick_base) {}

  enum class ApplyResult { Applied, OutOfRange, EmptyLevel };

  ApplyResult apply(const WireRecord& m) noexcept {
    switch (m.type) {
      case MsgType::AddOrder:
        return add(m.side, m.price, m.qty);
      case MsgType::OrderExecuted:
      case MsgType::OrderCanceled:
        return reduce(m.side, m.price, m.qty);
      case MsgType::OrderDeleted:
        // Delete carries no qty on the wire; remove whatever rests at the
        // level. Aggregate-only book cannot know the prior qty, so the level
        // is cleared outright.
        return remove_level(m.side, m.price);
    }
    return ApplyResult::Applied;
  }

  ApplyResult add(Side side, Price price, Qty qty) noexcept {
    const long idx = index_of(price);
    if (idx < 0) return ApplyResult::OutOfRange;
    auto& s = side_of(side);
    const std::size_t i = static_cast<std::size_t>(idx);
    if (s.qty[i] == 0 && qty > 0) s.bits.set(i);
    s.qty[i] += qty;
    return ApplyResult::Applied;
  }

  ApplyResult reduce(Side side, Price price, Qty qty) noexcept {
    const long idx = index_of(price);
    if (idx < 0) return ApplyResult::OutOfRange;
    auto& s = side_of(side);
    const std::size_t i = static_cast<std::size_t>(idx);
    if (s.qty[i] == 0) return ApplyResult::EmptyLevel;
    s.qty[i] = (qty >= s.qty[i]) ? 0 : (s.qty[i] - qty);
    if (s.qty[i] == 0) s.bits.clear(i);
    return ApplyResult::Applied;
  }

  ApplyResult remove_level(Side side, Price price) noexcept {
    const long idx = index_of(price);
    if (idx < 0) return ApplyResult::OutOfRange;
    auto& s = side_of(side);
    const std::size_t i = static_cast<std::size_t>(idx);
    if (s.qty[i] == 0) return ApplyResult::EmptyLevel;
    s.qty[i] = 0;
    s.bits.clear(i);
    return ApplyResult::Applied;
  }

  Qty qty_at(Side side, Price price) const noexcept {
    const long idx = index_of(price);
    if (idx < 0) return 0;
    return side_of(side).qty[static_cast<std::size_t>(idx)];
  }

  Top top() const noexcept {
    Top t;
    const auto& bid = bid_;
    const auto& ask = ask_;
    if (!bid.bits.empty()) {
      const std::size_t i = bid.bits.max();
      t.has_bid = true;
      t.bid     = price_of(i);
      t.bid_qty = bid.qty[i];
    }
    if (!ask.bits.empty()) {
      const std::size_t i = ask.bits.min();
      t.has_ask = true;
      t.ask     = price_of(i);
      t.ask_qty = ask.qty[i];
    }
    return t;
  }

  bool empty() const noexcept { return bid_.bits.empty() && ask_.bits.empty(); }

private:
  struct SideState {
    std::array<Qty, NLevels>           qty{};
    LevelBitmap<NLevels>               bits{};
  };

  static constexpr Price kNumLevels = static_cast<Price>(NLevels);

  long index_of(Price price) const noexcept {
    const Price rel = price - tick_base_;
    if (rel < 0 || rel >= kNumLevels) return -1;
    return static_cast<long>(rel);
  }

  Price price_of(std::size_t i) const noexcept {
    return tick_base_ + static_cast<Price>(i);
  }

  SideState& side_of(Side s) noexcept {
    return (s == Side::Bid) ? bid_ : ask_;
  }
  const SideState& side_of(Side s) const noexcept {
    return (s == Side::Bid) ? bid_ : ask_;
  }

  Price     tick_base_;
  SideState bid_;
  SideState ask_;
};

} // namespace feed
