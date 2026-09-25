#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace feed {

using SymbolId  = uint16_t;
using Seq       = uint64_t;
using Price     = int64_t;   // integer ticks, never floating point
using Qty       = uint64_t;
using OrderId   = uint64_t;
using Timestamp = uint64_t;  // nanoseconds (generation time)

// Upper bound on addressable symbols, shared by the generator and the handler.
inline constexpr std::size_t kMaxSymbols = 256;

enum class MsgType : uint8_t {
  AddOrder      = 1,
  OrderExecuted = 2,
  OrderCanceled = 3,
  OrderDeleted  = 4,
};

enum class Side : uint8_t {
  Bid = 0,
  Ask = 1,
};

// Fixed-width wire record. Every message type shares this exact layout so the
// decoder is a reinterpret_cast over the byte stream, not a field-by-field
// parser. Which trailing field is meaningful is defined by `type`.
//
//   off  field     type   note
//   0    type      u8     MsgType
//   1    side      u8     Side (meaningful for AddOrder)
//   2    symbol_id u16
//   4    _pad      u32    keeps the 8-byte fields aligned
//   8    seq       u64    per-symbol monotonic sequence number
//   16   ts        u64    generation timestamp (ns)
//   24   order_id  u64
//   32   price     i64    price in ticks
//   40   qty       u64    add qty / exec qty / cancel qty (0 for delete)
//
// The struct is packed so there are no padding surprises relative to the wire
// format; every 8-byte field lands on an 8-byte boundary by construction.
#pragma pack(push, 1)
struct WireRecord {
  MsgType   type;
  Side      side;
  SymbolId  symbol_id;
  uint32_t  _pad;
  Seq       seq;
  Timestamp ts;
  OrderId   order_id;
  Price     price;
  Qty       qty;
};
#pragma pack(pop)

// Typed aliases: identical bytes, distinct meaning per message type.
using AddOrderMsg      = WireRecord;
using OrderExecutedMsg = WireRecord;
using OrderCanceledMsg = WireRecord;
using OrderDeletedMsg  = WireRecord;

constexpr std::size_t kWireRecordSize = sizeof(WireRecord);

// Layout tests: if the wire format and struct diverge, this is where a real
// feed handler would silently corrupt the book, so fail the build instead.
static_assert(kWireRecordSize == 48, "wire record must be fixed-width 48 bytes");
static_assert(offsetof(WireRecord, seq)      == 8,  "seq offset mismatch");
static_assert(offsetof(WireRecord, ts)       == 16, "ts offset mismatch");
static_assert(offsetof(WireRecord, order_id) == 24, "order_id offset mismatch");
static_assert(offsetof(WireRecord, price)    == 32, "price offset mismatch");
static_assert(offsetof(WireRecord, qty)      == 40, "qty offset mismatch");

// Serialise one record into a byte buffer (used by feed_gen and the tests).
inline void encode_record(const WireRecord& rec, void* dst) noexcept {
  std::memcpy(dst, &rec, kWireRecordSize);
}

// Decode one record from a byte buffer (reinterpret_cast, no parsing).
inline const WireRecord* decode_record(const void* src) noexcept {
  return reinterpret_cast<const WireRecord*>(src);
}

} // namespace feed
