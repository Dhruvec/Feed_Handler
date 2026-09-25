// feed_tests -- correctness suite for the feed handler.
//
// Three properties, in the spirit of lob-engine's "compare against a dumb but
// obviously-correct baseline":
//   1. Decode round-trip: every message type written decodes back bit-identically.
//   2. Gap detection: a feed with a known set of dropped sequences must be
//      reported with exactly that set of gaps -- no fewer, no extras.
//   3. Book correctness: the streamed L1 book must agree with a reference model
//      that rebuilds the book with plain maps after every single event.

#include "feed/gen.hpp"
#include "feed/handler.hpp"
#include "feed/level_bitmap.hpp"
#include "feed/order_index.hpp"
#include "feed/stats.hpp"
#include "feed/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

template <class A, class B>
void check_eq(const A& a, const B& b, const std::string& what) {
  ++g_checks;
  if (!(a == b)) {
    ++g_failures;
    std::printf("  FAIL: %s (got %lld, want %lld)\n", what.c_str(),
                static_cast<long long>(a), static_cast<long long>(b));
  }
}

// ---------------------------------------------------------------------------
// 1. Decode round-trip
// ---------------------------------------------------------------------------
void test_decode_roundtrip() {
  std::printf("[decode round-trip]\n");

  check(feed::kWireRecordSize == 48, "record is 48 bytes");
  check(offsetof(feed::WireRecord, seq) == 8, "seq at offset 8");
  check(offsetof(feed::WireRecord, ts) == 16, "ts at offset 16");
  check(offsetof(feed::WireRecord, order_id) == 24, "order_id at offset 24");
  check(offsetof(feed::WireRecord, price) == 32, "price at offset 32");
  check(offsetof(feed::WireRecord, qty) == 40, "qty at offset 40");

  struct Case {
    feed::MsgType type;
    feed::Side    side;
    feed::SymbolId sym;
    feed::Seq      seq;
    feed::Timestamp ts;
    feed::OrderId  oid;
    feed::Price    price;
    feed::Qty      qty;
  };
  const Case cases[] = {
      {feed::MsgType::AddOrder,      feed::Side::Bid, 7,   101, 111111, 42, 150, 300},
      {feed::MsgType::OrderExecuted, feed::Side::Ask, 9,   202, 222222, 43, 175, 25},
      {feed::MsgType::OrderCanceled, feed::Side::Bid, 1,   303, 333333, 44, 120, 10},
      {feed::MsgType::OrderDeleted,  feed::Side::Ask, 255, 404, 444444, 45, 480, 0},
  };

  std::vector<std::byte> buf(sizeof(cases));
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    const auto& c = cases[i];
    feed::WireRecord rec{};
    rec.type = c.type; rec.side = c.side; rec.symbol_id = c.sym;
    rec.seq = c.seq; rec.ts = c.ts; rec.order_id = c.oid;
    rec.price = c.price; rec.qty = c.qty;

    feed::encode_record(rec, buf.data() + i * feed::kWireRecordSize);

    const feed::WireRecord* back =
        feed::decode_record(buf.data() + i * feed::kWireRecordSize);
    check(back->type == c.type, "type round-trips");
    check(back->side == c.side, "side round-trips");
    check_eq(back->symbol_id, c.sym, "symbol round-trips");
    check_eq(back->seq, c.seq, "seq round-trips");
    check_eq(back->ts, c.ts, "ts round-trips");
    check_eq(back->order_id, c.oid, "order_id round-trips");
    check_eq(back->price, c.price, "price round-trips");
    check_eq(back->qty, c.qty, "qty round-trips");
  }

  // Whole-buffer stream decode via Decoder.
  feed::Decoder dec(buf.data(), buf.size());
  std::size_t n = 0;
  while (dec.next()) ++n;
  check_eq(n, std::size(cases), "Decoder yields every record");

  // A trailing partial record must be ignored, not mis-decoded.
  feed::Decoder truncated(buf.data(), buf.size() - 1);
  std::size_t tn = 0;
  while (truncated.next()) ++tn;
  check_eq(tn, std::size(cases) - 1, "partial trailing record ignored");
}

// ---------------------------------------------------------------------------
// 2. Gap detection
// ---------------------------------------------------------------------------
void test_gap_detection() {
  std::printf("[gap detection]\n");

  feed::gen::Config cfg;
  cfg.num_symbols = 8;
  cfg.num_messages = 50'000;
  cfg.loss_rate = 0.05;
  cfg.seed = 12345;
  cfg.base_ts = 1'800'000'000'000'000'000ull;

  feed::gen::Result r = feed::gen::generate(cfg);
  check(!r.expected_gaps.empty(), "generator produced at least one gap");
  check(r.dropped > 0, "generator dropped records");

  // Pack into a byte buffer and replay through the handler.
  std::vector<std::byte> buf(r.written.size() * feed::kWireRecordSize);
  for (std::size_t i = 0; i < r.written.size(); ++i) {
    feed::encode_record(r.written[i], buf.data() + i * feed::kWireRecordSize);
  }

  feed::FeedHandler handler;
  std::vector<feed::GapEvent> reported;
  handler.set_gap_logger([&](const feed::GapEvent& g) { reported.push_back(g); });
  handler.replay(buf.data(), buf.size(), nullptr);

  check_eq(reported.size(), r.expected_gaps.size(),
           "reported gap count matches generator's known gaps");
  check_eq(handler.gaps_reported(), r.expected_gaps.size(),
           "handler gap counter matches");

  const std::size_t n = std::min(reported.size(), r.expected_gaps.size());
  bool all_match = true;
  for (std::size_t i = 0; i < n; ++i) {
    if (reported[i].symbol != r.expected_gaps[i].symbol ||
        reported[i].expected != r.expected_gaps[i].expected ||
        reported[i].got != r.expected_gaps[i].got) {
      all_match = false;
      break;
    }
  }
  check(all_match, "each reported gap equals the known gap (symbol/expected/got)");

  // No loss -> no gaps.
  feed::gen::Config clean = cfg;
  clean.loss_rate = 0.0;
  feed::gen::Result r2 = feed::gen::generate(clean);
  check(r2.expected_gaps.empty(), "lossless feed has no gaps");

  std::vector<std::byte> buf2(r2.written.size() * feed::kWireRecordSize);
  for (std::size_t i = 0; i < r2.written.size(); ++i) {
    feed::encode_record(r2.written[i], buf2.data() + i * feed::kWireRecordSize);
  }
  feed::FeedHandler h2;
  h2.replay(buf2.data(), buf2.size(), nullptr);
  check_eq(h2.gaps_reported(), std::size_t{0}, "lossless replay reports no gaps");
}

// ---------------------------------------------------------------------------
// 3. Book correctness against a dumb-but-correct reference model
// ---------------------------------------------------------------------------
struct ReferenceModel {
  struct Order { feed::Side side; feed::Price price; feed::Qty rem; };

  std::unordered_map<feed::OrderId, Order> orders;
  std::vector<std::map<feed::Price, feed::Qty>> bid;
  std::vector<std::map<feed::Price, feed::Qty>> ask;

  explicit ReferenceModel(std::size_t symbols) : bid(symbols), ask(symbols) {}

  void reduce_level(bool is_bid, feed::SymbolId sym, feed::Price price, feed::Qty q) {
    auto& side = is_bid ? bid : ask;
    auto& lvlmap = side[sym];
    auto lit = lvlmap.find(price);
    if (lit == lvlmap.end()) return;
    lit->second = (q >= lit->second) ? 0 : (lit->second - q);
    if (lit->second == 0) lvlmap.erase(lit);
  }

  void apply(const feed::WireRecord& m) {
    switch (m.type) {
      case feed::MsgType::AddOrder: {
        auto& side = (m.side == feed::Side::Bid) ? bid : ask;
        side[m.symbol_id][m.price] += m.qty;
        orders[m.order_id] = Order{m.side, m.price, m.qty};
        break;
      }
      case feed::MsgType::OrderExecuted:
      case feed::MsgType::OrderCanceled: {
        auto it = orders.find(m.order_id);
        if (it == orders.end()) break;
        const bool is_bid = (it->second.side == feed::Side::Bid);
        const feed::Price price = it->second.price;
        reduce_level(is_bid, m.symbol_id, price, m.qty);
        if (m.qty >= it->second.rem) {
          orders.erase(it);
        } else {
          it->second.rem -= m.qty;
        }
        break;
      }
      case feed::MsgType::OrderDeleted: {
        auto it = orders.find(m.order_id);
        if (it == orders.end()) break;
        const bool is_bid = (it->second.side == feed::Side::Bid);
        const feed::Price price = it->second.price;
        const feed::Qty rem = it->second.rem;
        reduce_level(is_bid, m.symbol_id, price, rem);
        orders.erase(it);
        break;
      }
    }
  }

  struct Top { bool hb=false; feed::Price bid=0; feed::Qty bq=0;
               bool ha=false; feed::Price ask=0; feed::Qty aq=0; };

  Top top(feed::SymbolId s) const {
    Top t;
    if (!bid[s].empty()) {
      auto it = std::prev(bid[s].end());
      t.hb = true; t.bid = it->first; t.bq = it->second;
    }
    if (!ask[s].empty()) {
      auto it = ask[s].begin();
      t.ha = true; t.ask = it->first; t.aq = it->second;
    }
    return t;
  }
};

void test_book_matches_reference() {
  std::printf("[book correctness vs reference model]\n");

  feed::gen::Config cfg;
  cfg.num_symbols = 16;
  cfg.num_messages = 20'000;
  cfg.loss_rate = 0.0;              // loss would desync both by construction
  cfg.seed = 777;
  cfg.base_ts = 1'800'000'000'000'000'000ull;

  feed::gen::Result r = feed::gen::generate(cfg);
  check_eq(r.expected_gaps.size(), std::size_t{0}, "clean feed, no gaps");

  feed::FeedHandler handler(/*tick_base=*/0);
  ReferenceModel ref(cfg.num_symbols);

  bool all_agree = true;
  std::size_t mismatches = 0;
  for (const feed::WireRecord& m : r.written) {
    handler.process(m, nullptr);
    ref.apply(m);

    const auto ht = handler.book(m.symbol_id).top();
    const auto rt = ref.top(m.symbol_id);

    if (ht.has_bid != rt.hb || ht.has_ask != rt.ha) { all_agree = false; ++mismatches; break; }
    if (ht.has_bid && (ht.bid != rt.bid || ht.bid_qty != rt.bq)) { all_agree = false; ++mismatches; break; }
    if (ht.has_ask && (ht.ask != rt.ask || ht.ask_qty != rt.aq)) { all_agree = false; ++mismatches; break; }
  }
  check(all_agree, "streamed L1 top-of-book agrees with reference at every step");
  check_eq(mismatches, std::size_t{0}, "zero divergences");
  check_eq(handler.unresolved(), std::size_t{0}, "no unresolved order references");

  // Cross-check every level of every symbol, not just the top.
  bool levels_agree = true;
  for (feed::SymbolId s = 0; s < cfg.num_symbols && levels_agree; ++s) {
    for (const auto& [price, qty] : ref.bid[s]) {
      if (handler.book(s).qty_at(feed::Side::Bid, price) != qty) { levels_agree = false; break; }
    }
    for (const auto& [price, qty] : ref.ask[s]) {
      if (handler.book(s).qty_at(feed::Side::Ask, price) != qty) { levels_agree = false; break; }
    }
  }
  check(levels_agree, "every price level matches the reference, not just the top");
}

// ---------------------------------------------------------------------------
// Component tests: bitmap, index, stats
// ---------------------------------------------------------------------------
void test_level_bitmap() {
  std::printf("[level bitmap]\n");
  feed::LevelBitmap<2048> b;
  check(b.empty(), "starts empty");
  b.set(5); b.set(1000); b.set(77);
  check(!b.empty(), "non-empty after sets");
  check_eq(b.min(), std::size_t{5}, "min is lowest set bit");
  check_eq(b.max(), std::size_t{1000}, "max is highest set bit");
  b.clear(5);
  check_eq(b.min(), std::size_t{77}, "min updates after clear");
  b.clear(1000); b.clear(77);
  check(b.empty(), "empty after clearing all");

  // Cross word boundary.
  feed::LevelBitmap<2048> c;
  c.set(63); c.set(64);
  check_eq(c.min(), std::size_t{63}, "min across word boundary");
  check_eq(c.max(), std::size_t{64}, "max across word boundary");
}

void test_order_index() {
  std::printf("[order index]\n");
  feed::OrderIndex idx;
  check(idx.find(1) == nullptr, "absent id not found");

  idx.insert(feed::OrderEntry{1, feed::Side::Bid, 100, 50});
  idx.insert(feed::OrderEntry{2, feed::Side::Ask, 200, 30});
  check_eq(idx.size(), std::size_t{2}, "two inserts");

  auto* e = idx.find(1);
  check(e != nullptr, "id 1 found");
  check(e && e->price == 100 && e->remaining == 50, "id 1 payload correct");

  idx.insert(feed::OrderEntry{1, feed::Side::Bid, 100, 70}); // overwrite
  check_eq(idx.size(), std::size_t{2}, "overwrite does not grow");
  check(idx.find(1)->remaining == 70, "overwrite updates payload");

  check(idx.erase(1), "erase existing");
  check(idx.find(1) == nullptr, "erased id not found");
  check(!idx.erase(1), "erase missing returns false");
  check_eq(idx.size(), std::size_t{1}, "size after erase");

  // Insert after tombstone reuse.
  idx.insert(feed::OrderEntry{3, feed::Side::Bid, 300, 10});
  check(idx.find(3) != nullptr, "insert after tombstone");
  check(idx.find(2) != nullptr, "surviving entry intact");

  // Growth path.
  feed::OrderIndex big(4);
  for (feed::OrderId id = 0; id < 1000; ++id) {
    big.insert(feed::OrderEntry{id, feed::Side::Bid, static_cast<feed::Price>(id), 1});
  }
  check_eq(big.size(), std::size_t{1000}, "1000 entries after growth");
  bool all_found = true;
  for (feed::OrderId id = 0; id < 1000; ++id) {
    if (!big.find(id)) { all_found = false; break; }
  }
  check(all_found, "all entries findable after growth");
}

void test_stats() {
  std::printf("[latency stats]\n");
  feed::LatencyStats st;
  for (uint64_t v = 1; v <= 1000; ++v) st.record(v);
  check_eq(st.total(), std::size_t{1000}, "total recorded");
  check_eq(st.min(), uint64_t{1}, "min");
  check_eq(st.max(), uint64_t{1000}, "max");
  check(st.percentile(50.0) >= 499 && st.percentile(50.0) <= 501, "p50 ~ 500");
  check(st.percentile(99.0) >= 989 && st.percentile(99.0) <= 1000, "p99 ~ 990+");
  check_eq(st.percentile(100.0), uint64_t{1000}, "p100 is max");
}

} // namespace

int main() {
  std::printf("feed_tests: running correctness suite\n\n");

  test_decode_roundtrip();
  test_gap_detection();
  test_book_matches_reference();
  test_level_bitmap();
  test_order_index();
  test_stats();

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("TESTS FAILED\n");
  return 1;
}
