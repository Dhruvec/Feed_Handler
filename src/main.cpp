// feed_handler -- replays a synthetic binary feed and reconstructs a per-symbol
// L1 (best bid/ask) book, reporting sequence gaps and tick-to-book latency.

#include "feed/handler.hpp"
#include "feed/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct ReplayBuffer {
  std::vector<std::byte> data;
  std::size_t records = 0;
};

bool load_file(const char* path, ReplayBuffer& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "error: cannot open '%s'\n", path);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fprintf(stderr, "error: cannot size '%s'\n", path);
    std::fclose(f);
    return false;
  }
  out.data.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    const std::size_t got = std::fread(out.data.data(), 1, static_cast<std::size_t>(size), f);
    if (got != static_cast<std::size_t>(size)) {
      std::fprintf(stderr, "error: short read from '%s'\n", path);
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  out.records = out.data.size() / feed::kWireRecordSize;
  return true;
}

const char* side_name(feed::Side s) {
  return s == feed::Side::Bid ? "BID" : "ASK";
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  bool verbose = false;
  std::size_t print_limit = 20;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      std::printf("usage: %s [--verbose] [--print N] <feed.bin>\n", argv[0]);
      return 0;
    } else if (a == "--verbose") {
      verbose = true;
    } else if (a == "--print") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --print requires a value\n");
        return 2;
      }
      print_limit = std::strtoull(argv[++i], nullptr, 10);
    } else if (a.rfind("--", 0) == 0) {
      std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
      return 2;
    } else {
      path = a;
    }
  }

  if (path.empty()) {
    std::fprintf(stderr, "usage: %s [--verbose] [--print N] <feed.bin>\n", argv[0]);
    return 2;
  }

  ReplayBuffer buf;
  if (!load_file(path.c_str(), buf)) return 1;

  if (buf.data.size() % feed::kWireRecordSize != 0) {
    std::fprintf(stderr, "warning: trailing %zu bytes are not a whole record; ignoring\n",
                 buf.data.size() % feed::kWireRecordSize);
  }

  feed::FeedHandler handler(/*tick_base=*/0);

  const std::size_t gap_limit = 20;
  std::size_t gaps_logged = 0;
  handler.set_gap_logger([&](const feed::GapEvent& g) {
    if (gaps_logged < gap_limit) {
      std::printf("[GAP] symbol=%u expected=%llu got=%llu (missing %llu)\n",
                  static_cast<unsigned>(g.symbol),
                  static_cast<unsigned long long>(g.expected),
                  static_cast<unsigned long long>(g.got),
                  static_cast<unsigned long long>(g.got - g.expected));
    } else if (gaps_logged == gap_limit) {
      std::printf("[GAP] ... further gaps suppressed\n");
    }
    ++gaps_logged;
  });

  std::size_t updates_printed = 0;
  auto emit = [&](const feed::L1Update& u) {
    if (updates_printed < print_limit) {
      std::printf("[L1] sym=%u %s px=%lld qty=%llu seq=%llu lat=%lluns\n",
                  static_cast<unsigned>(u.symbol), side_name(u.side),
                  static_cast<long long>(u.price),
                  static_cast<unsigned long long>(u.qty),
                  static_cast<unsigned long long>(u.seq),
                  static_cast<unsigned long long>(u.latency_ns));
    }
    ++updates_printed;
  };

  const uint64_t start = feed::FeedHandler::wall_clock_ns();
  handler.replay(buf.data.data(), buf.data.size(), emit);
  const uint64_t end = feed::FeedHandler::wall_clock_ns();

  const double secs = static_cast<double>(end - start) / 1e9;
  const auto& lat = handler.latency();

  std::printf("\n=== feed_handler summary ===\n");
  std::printf("file                : %s\n", path.c_str());
  std::printf("records read        : %zu\n", buf.records);
  std::printf("updates applied     : %zu\n", handler.applied());
  std::printf("unresolved refs     : %zu (events for orders dropped earlier)\n",
              handler.unresolved());
  std::printf("out-of-range prices : %zu\n", handler.out_of_range());
  std::printf("sequence gaps       : %zu\n", handler.gaps_reported());
  std::printf("replay wall time    : %.3f ms\n", secs * 1e3);
  if (secs > 0) {
    std::printf("throughput          : %.2f M msg/s\n",
                static_cast<double>(handler.applied()) / secs / 1e6);
  }

  std::printf("\n--- tick-to-book latency (generation -> book update) ---\n");
  std::printf("samples             : %zu (min %llu ns, mean %.0f ns, max %llu ns)\n",
              lat.total(),
              static_cast<unsigned long long>(lat.min()),
              lat.mean(),
              static_cast<unsigned long long>(lat.max()));
  std::printf("p50                 : %llu ns\n", (unsigned long long)lat.percentile(50.0));
  std::printf("p90                 : %llu ns\n", (unsigned long long)lat.percentile(90.0));
  std::printf("p99                 : %llu ns\n", (unsigned long long)lat.percentile(99.0));
  std::printf("p99.9               : %llu ns\n", (unsigned long long)lat.percentile(99.9));

  if (verbose) {
    std::printf("\n--- top of book per symbol ---\n");
    for (feed::SymbolId s = 0; s < feed::kNumSymbols; ++s) {
      const auto& book = handler.book(s);
      if (book.empty()) continue;
      const auto top = book.top();
      std::printf("sym=%u  ", static_cast<unsigned>(s));
      if (top.has_bid) {
        std::printf("bid=%lld x %llu  ", (long long)top.bid,
                    (unsigned long long)top.bid_qty);
      } else {
        std::printf("bid=--  ");
      }
      if (top.has_ask) {
        std::printf("ask=%lld x %llu", (long long)top.ask,
                    (unsigned long long)top.ask_qty);
      } else {
        std::printf("ask=--");
      }
      std::printf("\n");
    }
  }

  return 0;
}
