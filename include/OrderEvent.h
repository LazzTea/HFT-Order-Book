#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hft {

static constexpr int64_t PRICE_SCALE = 1'000'000;

enum class EventType : uint8_t {
    NewOrder    = 1,
    PartialFill = 2,
    FullFill    = 3,
    Cancel      = 4,
    Reject      = 5,
    Amend       = 6,
};

enum class Side : uint8_t {
    Buy  = 1,
    Sell = 2,
};

// Exactly 64 bytes (one cache line). Never add virtual methods or non-trivial
// constructors — this struct is written directly into a memory-mapped file.
//
// Field layout (totals 64 bytes):
//   8  sequence
//   8  timestamp_ns
//   8  price
//   8  fill_price
//   4  qty
//   4  fill_qty
//   8  symbol[8]
//   8  order_id[8]
//   1  type
//   1  side
//   6  _pad
struct OrderEvent {
    uint64_t  sequence;
    uint64_t  timestamp_ns;
    int64_t   price;
    int64_t   fill_price;
    int32_t   qty;
    int32_t   fill_qty;
    char      symbol[8];
    char      order_id[8];
    EventType type;
    Side      side;
    uint8_t   _pad[6];

    [[nodiscard]] std::string_view symbol_view() const {
        return {symbol, strnlen(symbol, sizeof(symbol))};
    }

    [[nodiscard]] std::string_view order_id_view() const {
        return {order_id, strnlen(order_id, sizeof(order_id))};
    }

    [[nodiscard]] bool is_fill() const {
        return type == EventType::PartialFill || type == EventType::FullFill;
    }

    [[nodiscard]] double price_d()      const { return static_cast<double>(price)      / PRICE_SCALE; }
    [[nodiscard]] double fill_price_d() const { return static_cast<double>(fill_price) / PRICE_SCALE; }
};

static_assert(sizeof(OrderEvent)  == 64, "OrderEvent must be 64 bytes");
static_assert(alignof(OrderEvent) ==  8, "OrderEvent alignment");

inline OrderEvent make_fill(    const std::string_view sym,
                                const Side side,
                                const int32_t fqty,
                                const int64_t fprice,
                                const std::string_view oid,
                                const std::string_view /*exchange_id*/ = "",
                                const bool partial = false
                                )
{
    OrderEvent e{};
    e.type       = partial ? EventType::PartialFill : EventType::FullFill;
    e.side       = side;
    e.qty        = fqty;
    e.fill_qty   = fqty;
    e.fill_price = fprice;
    strncpy(e.symbol,   sym.data(), sizeof(e.symbol));
    strncpy(e.order_id, oid.data(), sizeof(e.order_id));
    return e;
}

inline OrderEvent make_new_order(   const std::string_view sym,
                                    const Side side,
                                    const int32_t qty,
                                    const int64_t price_fp,
                                    const std::string_view oid
                                    )
{
    OrderEvent e{};
    e.type  = EventType::NewOrder;
    e.side  = side;
    e.qty   = qty;
    e.price = price_fp;
    strncpy(e.symbol,   sym.data(), sizeof(e.symbol));
    strncpy(e.order_id, oid.data(), sizeof(e.order_id));
    return e;
}

} // namespace hft
