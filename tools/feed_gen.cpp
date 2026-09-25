// feed_gen -- synthetic multi-symbol binary feed generator.
//
// Writes a fixed-width record stream (feed.bin) with a configurable message mix
// and packet-loss rate. Loss is applied by dropping records from the output
// while still consuming their per-symbol sequence numbers, so the file contains
// real, verifiable sequence gaps for the feed handler to detect.

#include "feed/gen.hpp"
#include "feed/types.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s [options]\n"
    "  --symbols N        number of symbols (default 8)\n"
    "  --messages N       number of events to generate (default 1000000)\n"
    "  --loss-rate F      packet-loss probability in [0,1) (default 0.0)\n"
    "  --seed N           RNG seed (default 0xC0FFEE)\n"
    "  -o FILE            output file (default feed.bin)\n"
    "  --quiet            suppress summary output\n"
    "  -h, --help         show this help\n",
    argv0);
}

} // namespace

int main(int argc, char** argv) {
  feed::gen::Config cfg;
  std::string out_path = "feed.bin";
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };

    if (a == "--symbols") {
      cfg.num_symbols = std::strtoull(need("--symbols"), nullptr, 10);
    } else if (a == "--messages") {
      cfg.num_messages = std::strtoull(need("--messages"), nullptr, 10);
    } else if (a == "--loss-rate") {
      cfg.loss_rate = std::atof(need("--loss-rate"));
    } else if (a == "--seed") {
      cfg.seed = std::strtoull(need("--seed"), nullptr, 10);
    } else if (a == "-o") {
      out_path = need("-o");
    } else if (a == "--quiet") {
      quiet = true;
    } else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
      usage(argv[0]);
      return 2;
    }
  }

  if (cfg.num_symbols == 0 || cfg.num_symbols > feed::kMaxSymbols) {
    std::fprintf(stderr, "error: --symbols must be in [1, %zu]\n",
                 static_cast<std::size_t>(feed::kMaxSymbols));
    return 2;
  }

  feed::gen::Result r = feed::gen::generate(cfg);

  std::FILE* f = std::fopen(out_path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "error: cannot open '%s' for writing\n", out_path.c_str());
    return 1;
  }
  const std::size_t bytes = r.written.size() * feed::kWireRecordSize;
  if (bytes > 0 &&
      std::fwrite(r.written.data(), 1, bytes, f) != bytes) {
    std::fprintf(stderr, "error: short write to '%s'\n", out_path.c_str());
    std::fclose(f);
    return 1;
  }
  std::fclose(f);

  if (!quiet) {
    std::printf("feed_gen: symbols=%zu events=%zu records=%zu dropped=%zu gaps=%zu\n",
                cfg.num_symbols, r.generated, r.written.size(),
                r.dropped, r.expected_gaps.size());
    std::printf("feed_gen: wrote %zu bytes (%zu records x %zu B) -> %s\n",
                bytes, r.written.size(), feed::kWireRecordSize, out_path.c_str());
    if (cfg.loss_rate > 0.0) {
      std::printf("feed_gen: loss-rate=%.6f produced %zu sequence gap(s)\n",
                  cfg.loss_rate, r.expected_gaps.size());
    }
  }
  return 0;
}
