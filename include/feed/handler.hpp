#pragma once

#include "decoder.hpp"
#include "l1_book.hpp"
#include "order_index.hpp"
#include "sequence_checker.hpp"
#include "stats.hpp"
#include "types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace feed {

// Addressable feeds are small in practice: a handful to a few hundred symbols,
// each with a modest tick range. L1 state per symbol is 2 sides * levels * 8
// bytes; with 2048 levels that is 32 KiB/symbol, or 8 MiB for 256 symbols.
inline constexpr std::size_t kNumSymbols      = kMaxSymbols;
inline constexpr std::size_t kLevelsPerSymbol = 2048;

struct L1Update {
  Seq      seq;
  Timestamp ts;         // generation timestamp carried on the wire
  SymbolId symbol;
  Side     side;
  Price    price;
  Qty      qty;         // aggregate qty at that price after the event
  uint64_t latency_ns;  // generation -> book update, stamped at emit time
};

// Orchestrates decode -> sequence check -> order index -> L1 book update ->
// stats. The hot path holds no locks; the only allocation is amortised index
// growth, never per message. Emission is a callback so main can print and tests
// can capture.
class FeedHandler {
public:
  using EmitFn = std::function<void(const L1Update&)>;

  explicit FeedHandler(Price tick_base = 0) : tick_base_(tick_base) {
    books_.reserve(kNumSymbols);
    for (std::size_t i = 0; i < kNumSymbols; ++i) {
      books_.emplace_back(tick_base);
    }
  }

  void reset() {
    for (auto& b : books_) b = L1Book<kLevelsPerSymbol>(tick_base_);
    seq_ = SequenceChecker<kNumSymbols>{};
    lat_ = LatencyStats{};
    orders_.clear();
    applied_ = 0;
    gaps_reported_ = 0;
    out_of_range_ = 0;
    unresolved_ = 0;
  }

  void replay(const std::byte* data, std::size_t bytes, const EmitFn& emit) {
    Decoder dec(data, bytes);
    while (const WireRecord* m = dec.next()) {
      process(*m, emit);
    }
  }

  // Process a single decoded message. Public so tests can drive the handler
  // record-by-record and snapshot state.
  void process(const WireRecord& m, const EmitFn& emit) {
    GapEvent gap{};
    if (!seq_.check(m.symbol_id, m.seq, gap)) {
      ++gaps_reported_;
      if (gap_log_) gap_log_(gap);
    }

    L1Book<kLevelsPerSymbol>& book = books_[m.symbol_id];
    Side  side  = m.side;
    Price price = m.price;
    Qty   resulting_qty = 0;
    bool  applied = true;

    switch (m.type) {
      case MsgType::AddOrder: {
        book.add(m.side, m.price, m.qty);
        orders_.insert(OrderEntry{m.order_id, m.side, m.price, m.qty});
        resulting_qty = book.qty_at(m.side, m.price);
        break;
      }
      case MsgType::OrderExecuted:
      case MsgType::OrderCanceled: {
        // Execute/Cancel carry only an order id; look up side/price.
        OrderEntry* e = orders_.find(m.order_id);
        if (!e) { ++unresolved_; applied = false; break; }
        side = e->side;
        price = e->price;
        book.reduce(side, price, m.qty);
        if (m.qty >= e->remaining) {
          orders_.erase(m.order_id);
        } else {
          e->remaining -= m.qty;
        }
        resulting_qty = book.qty_at(side, price);
        break;
      }
      case MsgType::OrderDeleted: {
        OrderEntry* e = orders_.find(m.order_id);
        if (!e) { ++unresolved_; applied = false; break; }
        side = e->side;
        price = e->price;
        book.reduce(side, price, e->remaining); // remove the full remaining qty
        orders_.erase(m.order_id);
        resulting_qty = book.qty_at(side, price);
        break;
      }
    }

    if (!applied) return;

    // Stamp the moment the book reflects the update, then measure
    // generation-to-book latency.
    const uint64_t now_ns = wall_clock_ns();
    const uint64_t lat = (now_ns > m.ts) ? (now_ns - m.ts) : 0;
    lat_.record(lat);

    ++applied_;

    if (emit) {
      L1Update u{};
      u.seq        = m.seq;
      u.ts         = m.ts;
      u.symbol     = m.symbol_id;
      u.side       = side;
      u.price      = price;
      u.qty        = resulting_qty;
      u.latency_ns = lat;
      emit(u);
    }
  }

  void set_gap_logger(std::function<void(const GapEvent&)> fn) {
    gap_log_ = std::move(fn);
  }

  const L1Book<kLevelsPerSymbol>& book(SymbolId s) const { return books_[s]; }
  L1Book<kLevelsPerSymbol>&       book(SymbolId s) { return books_[s]; }
  const OrderIndex&               orders() const noexcept { return orders_; }

  const SequenceChecker<kNumSymbols>& checker() const noexcept { return seq_; }
  const LatencyStats& latency() const noexcept { return lat_; }

  std::size_t applied() const noexcept { return applied_; }
  std::size_t gaps_reported() const noexcept { return gaps_reported_; }
  std::size_t out_of_range() const noexcept { return out_of_range_; }
  std::size_t unresolved() const noexcept { return unresolved_; }

  // Message timestamps are wall-clock nanoseconds, so the receipt stamp must be
  // too for the difference to be a meaningful generation-to-book latency.
  static uint64_t wall_clock_ns() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
  }

private:
  Price                                 tick_base_ = 0;
  std::vector<L1Book<kLevelsPerSymbol>> books_;
  SequenceChecker<kNumSymbols>          seq_;
  OrderIndex                            orders_;
  LatencyStats                          lat_;
  std::function<void(const GapEvent&)>  gap_log_;
  std::size_t applied_ = 0;
  std::size_t gaps_reported_ = 0;
  std::size_t out_of_range_ = 0;
  std::size_t unresolved_ = 0;
};

} // namespace feed
