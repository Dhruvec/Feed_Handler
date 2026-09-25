#pragma once

#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feed::gen {

// Deterministic RNG (splitmix64). Deterministic seeding means a given
// (seed, config) always produces byte-identical output, so a dropped-sequence
// set is reproducible and testable.
class Rng {
public:
  explicit Rng(uint64_t seed) noexcept : s_(seed) {}

  uint64_t next() noexcept {
    s_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = s_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  uint64_t range(uint64_t lo, uint64_t hi) noexcept { // inclusive
    if (hi <= lo) return lo;
    return lo + (next() % (hi - lo + 1));
  }

  // Returns true with probability p.
  bool chance(double p) noexcept {
    const uint64_t t = static_cast<uint64_t>(p * 1000000.0);
    return (next() % 1000000u) < t;
  }

private:
  uint64_t s_;
};

struct Config {
  std::size_t num_symbols   = 8;
  std::size_t num_messages  = 1'000'000;
  double      loss_rate     = 0.0;
  // Message mix (relative weights, normalised internally).
  double      w_add     = 0.55;
  double      w_exec    = 0.20;
  double      w_cancel  = 0.15;
  double      w_delete  = 0.10;
  Price       price_min = 100;
  Price       price_max = 500;
  uint64_t    seed      = 0xC0FFEEull;
  // Generation time base. 0 means "stamp the wall clock at generation time",
  // which makes the handler's generation-to-book latency a real measurement
  // when the file is replayed shortly after generation. Pin a nonzero value
  // (as the tests do) for byte-reproducible output; in that case timestamps
  // advance by 1us per generated event.
  uint64_t    base_ts   = 0;
};

inline uint64_t now_ns() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

struct GapEvent {
  SymbolId symbol;
  Seq      expected;
  Seq      got;
};

struct Result {
  std::vector<WireRecord> written;        // what lands in feed.bin
  std::vector<GapEvent>   expected_gaps;  // independently derived from written seqs
  std::size_t             dropped = 0;
  std::size_t             generated = 0;  // pre-loss event count
};

// Generates a coherent multi-symbol event stream. Live per-symbol order state
// is tracked so Execute/Cancel/Delete only ever reference orders that exist,
// and quantities never underflow. When loss_rate > 0, events are dropped from
// the output but their sequence numbers are still consumed, producing real
// per-symbol sequence gaps.
inline Result generate(const Config& cfg) {
  Result out;
  out.written.reserve(cfg.num_messages);
  out.written.reserve(cfg.num_messages);

  Rng rng(cfg.seed);
  const uint64_t base_ts = cfg.base_ts ? cfg.base_ts : now_ns();

  struct Live {
    OrderId id;
    Side    side;
    Price   price;
    Qty     remaining;
  };

  std::vector<std::vector<Live>> orders(cfg.num_symbols);
  std::vector<Seq>               next_seq(cfg.num_symbols, 1);
  std::vector<Seq>               prev_written(cfg.num_symbols, 0);
  std::vector<bool>              seen_written(cfg.num_symbols, false);

  const double total_w = cfg.w_add + cfg.w_exec + cfg.w_cancel + cfg.w_delete;
  const double cut_add    = cfg.w_add / total_w;
  const double cut_exec   = cut_add + cfg.w_exec / total_w;
  const double cut_cancel = cut_exec + cfg.w_cancel / total_w;

  OrderId next_order_id = 1;

  auto emit = [&](const WireRecord& rec) {
    const SymbolId s = rec.symbol_id;
    // Independently derive the gap this written record implies.
    if (seen_written[s]) {
      if (rec.seq != prev_written[s] + 1) {
        out.expected_gaps.push_back(GapEvent{s, prev_written[s] + 1, rec.seq});
      }
    } else {
      seen_written[s] = true;
    }
    prev_written[s] = rec.seq;
    out.written.push_back(rec);
  };

  auto push_event = [&](const WireRecord& rec) {
    ++out.generated;
    if (cfg.loss_rate > 0.0 && rng.chance(cfg.loss_rate)) {
      ++out.dropped;
      return; // dropped from output; seq already consumed
    }
    emit(rec);
  };

  for (std::size_t i = 0; i < cfg.num_messages; ++i) {
    const SymbolId sym = static_cast<SymbolId>(rng.range(0, cfg.num_symbols - 1));
    auto& live = orders[sym];

    const double roll = static_cast<double>(rng.next() % 1000000u) / 1000000.0;
    enum { ADD, EXEC, CANCEL, DELETE } kind;
    if (live.empty() || roll < cut_add) {
      kind = ADD;
    } else if (roll < cut_exec) {
      kind = EXEC;
    } else if (roll < cut_cancel) {
      kind = CANCEL;
    } else {
      kind = DELETE;
    }

    const Seq seq = next_seq[sym]++;
    const Timestamp ts = cfg.base_ts
        ? (base_ts + static_cast<uint64_t>(i) * 1000ull) // deterministic: +1us
        : now_ns();                                      // real generation time

    WireRecord rec{};
    rec.symbol_id = sym;
    rec.seq = seq;
    rec.ts  = ts;

    if (kind == ADD) {
      const Side side = (rng.next() & 1u) ? Side::Bid : Side::Ask;
      // Bids live in the lower half of the price range, asks in the upper half,
      // so the reconstructed book is never crossed (best bid < best ask) --
      // the way a real venue's book behaves.
      const Price mid = (cfg.price_min + cfg.price_max) / 2;
      const Price price = (side == Side::Bid)
          ? static_cast<Price>(rng.range(static_cast<uint64_t>(cfg.price_min),
                                         static_cast<uint64_t>(mid)))
          : static_cast<Price>(rng.range(static_cast<uint64_t>(mid) + 1u,
                                         static_cast<uint64_t>(cfg.price_max)));
      const Qty qty = rng.range(1, 1000);
      const OrderId oid = next_order_id++;

      rec.type = MsgType::AddOrder;
      rec.side = side;
      rec.order_id = oid;
      rec.price = price;
      rec.qty = qty;
      live.push_back(Live{oid, side, price, qty});
    } else if (kind == EXEC) {
      const std::size_t k = static_cast<std::size_t>(rng.range(0, live.size() - 1));
      Live& o = live[k];
      const Qty exec = rng.range(1, o.remaining);

      rec.type = MsgType::OrderExecuted;
      rec.side = o.side;
      rec.order_id = o.id;
      rec.price = o.price;
      rec.qty = exec;

      o.remaining -= exec;
      if (o.remaining == 0) {
        live[k] = live.back();
        live.pop_back();
      }
    } else if (kind == CANCEL) {
      const std::size_t k = static_cast<std::size_t>(rng.range(0, live.size() - 1));
      Live& o = live[k];
      const Qty cancel = rng.range(1, o.remaining);

      rec.type = MsgType::OrderCanceled;
      rec.side = o.side;
      rec.order_id = o.id;
      rec.price = o.price;
      rec.qty = cancel;

      o.remaining -= cancel;
      if (o.remaining == 0) {
        live[k] = live.back();
        live.pop_back();
      }
    } else { // DELETE
      const std::size_t k = static_cast<std::size_t>(rng.range(0, live.size() - 1));
      Live& o = live[k];

      rec.type = MsgType::OrderDeleted;
      rec.side = o.side;
      rec.order_id = o.id;
      rec.price = o.price;
      rec.qty = 0;

      live[k] = live.back();
      live.pop_back();
    }

    push_event(rec);
  }

  return out;
}

} // namespace feed::gen
