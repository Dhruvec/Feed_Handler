#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>

namespace feed {

struct GapEvent {
  SymbolId symbol;
  Seq      expected;
  Seq      got;
};

// One expected-next-sequence counter per symbol. On a discontinuity the gap is
// reported and the counter resynchronises to the received sequence so a single
// missing packet does not cascade into permanent desync.
template <std::size_t NumSymbols>
class SequenceChecker {
public:
  // Returns true when `seq` is the expected next sequence for `symbol` (no
  // gap). Returns false and fills `gap` on a discontinuity.
  bool check(SymbolId symbol, Seq seq, GapEvent& gap) noexcept {
    if (!initialized_[symbol]) {
      initialized_[symbol] = true;
      expected_[symbol] = seq + 1;
      return true;
    }
    if (seq == expected_[symbol]) {
      expected_[symbol] = seq + 1;
      return true;
    }

    gap.symbol   = symbol;
    gap.expected = expected_[symbol];
    gap.got      = seq;
    expected_[symbol] = seq + 1; // resync: do not cascade
    ++gap_count_;
    return false;
  }

  Seq expected(SymbolId symbol) const noexcept { return expected_[symbol]; }
  bool initialized(SymbolId symbol) const noexcept { return initialized_[symbol]; }
  std::size_t gap_count() const noexcept { return gap_count_; }

private:
  std::array<Seq, NumSymbols>  expected_{};
  std::array<bool, NumSymbols> initialized_{};
  std::size_t                  gap_count_ = 0;
};

} // namespace feed
