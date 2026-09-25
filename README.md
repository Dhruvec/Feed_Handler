# feed-handler

A standalone, file-replayed market-data feed handler in C++20. Reconstructs a
per-symbol L1 (best bid/ask) book from a synthetic binary feed, with
sequence-gap detection and tick-to-book latency measurement. Designed as a
companion to `lob-engine` — same allocation and layout discipline, applied to
the consuming side of the wire instead of the matching side.

```
   feed.bin (synthetic, multi-symbol)
        │
        ▼
 ┌─────────────┐
 │   Decoder    │  fixed-width wire format → typed messages (reinterpret_cast,
 └──────┬──────┘   no parsing, no heap allocation)
        ▼
 ┌─────────────────┐
 │ SequenceChecker  │  per-symbol monotonic seq check → flags gaps, no recovery
 └──────┬──────────┘   (this build detects and reports gaps; it does not
        ▼               request snapshots or replay — see "Extending this")
 ┌─────────────────┐
 │  L1 BookBuilder  │  applies Add/Execute/Cancel/Delete → best bid/ask + qty
 └──────┬──────────┘   per symbol (array + bitmap, aggregate-only, no FIFO)
        ▼
 ┌─────────────────┐
 │  Stats / Output  │  L1 update stream + tick-to-book latency percentiles
 └─────────────────┘
```

## Why this exists

Real venues (NASDAQ ITCH, CME MDP3, NYSE XDP) don't hand you a book — they
hand you a stream of discrete events (order added, executed, canceled,
replaced) over unreliable, unordered UDP multicast. A feed handler's whole job
is turning that stream back into a correct, low-latency mirror of the book
while surviving the ugly realities of the wire: dropped packets, out-of-order
arrival, and — in this scope — nothing more exotic than noticing when a packet
went missing.

This build is intentionally narrower than a production feed handler so it stays
honest about what it demonstrates:

- **Standalone, file-replayed** — no live multicast socket, no network capture
  layer. `feed_gen` writes a synthetic binary file; `feed_handler` reads it
  back. This isolates decode/book-building/latency measurement from
  network-stack concerns, which is its own (different) engineering problem.
- **L1 only** — best bid/ask + aggregate quantity per symbol. No per-order book
  (L3) and no full depth-of-book (L2), so there's no FIFO/price-time-priority
  machinery to maintain. An order-id index is kept so Execute/Cancel/Delete —
  which carry only an order id on the wire — can be mapped back to their price
  level.
- **Sequence checking, not recovery** — the handler detects a gap and
  reports/flags it. It does not request a snapshot or replay to heal the gap.
  See "Extending this".

## Wire protocol

Fixed-width, no padding surprises — decode is a `reinterpret_cast`, not a
parser. Every message carries a per-symbol monotonic sequence number and a
generation timestamp.

| Message         | Fields                                                 |
|-----------------|--------------------------------------------------------|
| `AddOrder`      | `seq, ts, symbol_id, order_id, side, price, qty`        |
| `OrderExecuted` | `seq, ts, symbol_id, order_id, exec_qty`                |
| `OrderCanceled` | `seq, ts, symbol_id, order_id, cancel_qty`              |
| `OrderDeleted`  | `seq, ts, symbol_id, order_id`                          |

All four share one 48-byte packed layout ([`WireRecord`](include/feed/types.hpp:47)):

```
   off  field      type   note
   0    type       u8     MsgType
   1    side       u8     Side (meaningful for AddOrder)
   2    symbol_id  u16
   4    _pad       u32    keeps the 8-byte fields aligned
   8    seq        u64    per-symbol monotonic sequence number
   16   ts         u64    generation timestamp (ns)
   24   order_id   u64
   32   price      i64    price in integer ticks
   40   qty        u64    add / exec / cancel qty (0 for delete)
```

`feed_gen` writes a realistic mix of these (configurable ratio) across multiple
symbols, with an optional packet-loss rate that drops messages from the output
file — the mechanism that produces real, verifiable sequence gaps rather than
gaps that only exist in a test's imagination.

## Core pieces

- **Decoder** ([`decoder.hpp`](include/feed/decoder.hpp)) — reads fixed-width
  records directly into typed structs. No per-field parsing, no allocation, no
  intermediate representation. The struct is packed and layout-tested with
  `static_assert`s, so a wire/layout divergence fails the build instead of
  silently corrupting the book.
- **SequenceChecker** ([`sequence_checker.hpp`](include/feed/sequence_checker.hpp))
  — one expected-next-sequence counter per symbol. A mismatch increments a gap
  counter and logs `(symbol, expected, got)`, then resynchronises to the
  received sequence so a single gap doesn't cascade into permanent desync.
- **OrderIndex** ([`order_index.hpp`](include/feed/order_index.hpp)) — an
  open-addressing `order_id → (side, price, remaining)` table with tombstone
  deletion and load-factor growth. Execute/Cancel/Delete carry only an order id,
  so this is what lets an aggregate book find the right price level.
- **L1Book** ([`l1_book.hpp`](include/feed/l1_book.hpp)) — array-indexed by
  price tick + occupied-tick bitmap (shared with `lob-engine`'s `BookSide`),
  stripped to aggregate-only. Best bid/ask is the same
  `countr_zero`/`countl_zero`-on-bitmap trick as the matching engine.
- **Latency measurement** ([`stats.hpp`](include/feed/stats.hpp), driven from
  [`handler.hpp`](include/feed/handler.hpp)) — every message carries its
  generation timestamp; the handler stamps wall-clock time when the book
  reflects the update and reports generation-to-book percentiles.

## Layout

```
include/feed/
  types.hpp            fundamental types + packed wire struct (48 B)
  decoder.hpp          fixed-width record → typed message (reinterpret_cast)
  sequence_checker.hpp per-symbol expected-seq tracking, gap counting
  level_bitmap.hpp     occupied-tick bitset, O(1) min/max scan
  l1_book.hpp          per-symbol aggregate book: qty-per-tick + bitmap
  order_index.hpp      order_id → resting (side, price, remaining)
  stats.hpp            latency percentile accumulator (p50/p90/p99/p99.9/max)
  gen.hpp              synthetic feed generator (shared by tool + tests)
  handler.hpp          decode → seq check → index → book → stats pipeline
tools/
  feed_gen.cpp         CLI: synthetic multi-symbol feed generator
src/main.cpp           feed_handler: replays a file, prints L1 updates + stats
tests/test_main.cpp    correctness suite
```

## Build & run

Requires CMake ≥ 3.16 and a C++20 compiler.

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j
./feed_gen --symbols 8 --messages 1000000 --loss-rate 0.001 -o feed.bin
./feed_handler feed.bin --verbose
./feed_tests
```

On Windows with MinGW:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build\feed_gen.exe --symbols 8 --messages 1000000 --loss-rate 0.001 -o feed.bin
build\feed_handler.exe feed.bin --verbose
build\feed_tests.exe
```

The generator stamps each message with the wall clock at generation time, so
the reported generation-to-book latency is the real elapsed time between a
message being written and the book reflecting it — replay the file right after
generating it for a representative number.

Always benchmark a Release (`-O3`) build — Debug numbers are not representative
of the decode/book-update path's real cost.

## Correctness testing

`feed_tests` runs three properties, plus component tests:

- **Decode round-trip**: every message type written by `feed_gen` decodes back
  to bit-identical field values, and a trailing partial record is ignored rather
  than mis-decoded.
- **Gap detection**: a feed generated with a *known* set of dropped sequence
  numbers must produce exactly that set of reported gaps — same symbol,
  same `expected`, same `got` — no fewer, no extras. A lossless feed must
  report zero gaps.
- **Book correctness**: the L1 top-of-book produced by streaming replay is
  checked against a reference model rebuilt from scratch with plain `std::map`s
  after every event (slow, obviously correct). After the full replay, every
  price level of every symbol is cross-checked, not just the top. This is the
  same "compare against a dumb-but-correct baseline" approach `lob-engine` uses.

## Extending this

- **Gap recovery**: on a detected gap, request a snapshot (full current book
  state) or replay the missing range from a recovery feed, instead of just
  resynchronising and moving on. This is the biggest gap (no pun intended)
  between this build and a production feed handler.
- **L2 / L3 reconstruction**: swap the aggregate-only `L1Book` for a
  price-level structure that keeps `lob-engine`'s intrusive per-order FIFO list,
  turning this into a full depth-of-book (L2) or order-by-order (L3) handler.
  With the `OrderIndex` already present, the `Add`/`Execute`/`Cancel`/`Delete`
  message set doesn't need to change — only what the book does with each one.
- **Live multicast transport**: replace `feed_gen`'s file with a UDP multicast
  socket (kernel bypass — DPDK/AF_XDP — for a real deployment, a raw socket for
  a demo). This is the piece that makes gaps and out-of-order arrival *actually*
  happen instead of being synthetically injected.
- **Conflation for slow consumers**: publish L1 updates to a lock-free ring and
  let a slow downstream consumer coalesce to "latest state per symbol" instead
  of blocking the hot path or processing every tick.
- **End-to-end tie-in with `lob-engine`**: add a market-data publisher hook to
  `MatchingEngine::submit_limit`/`submit_market`/`cancel` that emits this
  project's wire format directly, then run this feed handler against that live
  stream and assert its reconstructed book matches the matching engine's
  source-of-truth book at every step.
