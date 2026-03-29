#pragma once
#include "OrderEvent.h"
#include "EventLog.h"

#include <array>
// #include <cstdint>
#include <cstring>
#include <string_view>

namespace hft {

// Lifecycle of an order as seen by the exchange.
// Transitions are strictly forward:
//
//  NewOrder ──► Pending ──► PartiallyFilled ──► Filled     (terminal)
//                    │              └──────────► Cancelled  (terminal)
//                    └──────────────────────────► Cancelled (terminal)
//                    └──────────────────────────► Rejected  (terminal)
enum class OrderState : uint8_t {
    Pending         = 1,
    PartiallyFilled = 2,
    Filled          = 3,
    Cancelled       = 4,
    Rejected        = 5,
};

inline const char* to_string(const OrderState s) {
    switch (s) {
        case OrderState::Pending:         return "Pending";
        case OrderState::PartiallyFilled: return "PartiallyFilled";
        case OrderState::Filled:          return "Filled";
        case OrderState::Cancelled:       return "Cancelled";
        case OrderState::Rejected:        return "Rejected";
        default:                          return "Unknown";
    }
}

// Live view of a single order. Exactly 64 bytes (one cache line).
//
// Field layout:
//   8  order_id[8]
//   8  symbol[8]
//   8  limit_price
//   8  avg_fill_price
//   8  opened_seq
//   8  last_seq
//   4  original_qty
//   4  filled_qty
//   1  state
//   1  side
//   6  _pad
//  ──
//  64
//
// Note: remaining_qty is not stored — always derived as (original_qty - filled_qty).
// Avoids the field and keeps the struct at exactly 64 bytes.
struct Order {
    char       order_id[8];
    char       symbol[8];
    int64_t    limit_price;
    int64_t    avg_fill_price;
    uint64_t   opened_seq;
    uint64_t   last_seq;
    int32_t    original_qty;
    int32_t    filled_qty;
    OrderState state;
    Side       side;
    uint8_t    _pad[6];

    [[nodiscard]] std::string_view order_id_view() const {
        return {order_id, strnlen(order_id, sizeof(order_id))};
    }

    [[nodiscard]] std::string_view symbol_view() const {
        return {symbol, strnlen(symbol, sizeof(symbol))};
    }

    [[nodiscard]] int32_t remaining_qty() const { return original_qty - filled_qty; }

    [[nodiscard]] bool is_open() const {
        return state == OrderState::Pending ||
               state == OrderState::PartiallyFilled;
    }

    [[nodiscard]] bool is_terminal() const { return !is_open(); }

    [[nodiscard]] double limit_price_d()    const { return static_cast<double>(limit_price)    / PRICE_SCALE; }
    [[nodiscard]] double avg_fill_price_d() const { return static_cast<double>(avg_fill_price) / PRICE_SCALE; }
};

static_assert(sizeof(Order) == 64, "Order must be 64 bytes");

static constexpr size_t MAX_ORDERS = 512;

// OrderManager is the PositionTracker equivalent for order lifecycle.
// It maintains a live projection of every order seen in the event stream,
// indexed by order_id rather than symbol.
//
// apply_event() — single writer thread only, must be called in sequence order.
// get(), for_each(), open_exposure() — read-only, safe from other threads.
class OrderManager {
public:
    OrderManager() {
        memset(slots_.data(), 0, sizeof(Order) * MAX_ORDERS);
    }

    void replay_from(const EventLog& log) {
        memset(slots_.data(), 0, sizeof(Order) * MAX_ORDERS);
        count_ = 0;
        log.replay([this](const OrderEvent& e) { apply_event(e); });
    }

    void apply_event(const OrderEvent& e) {
        switch (e.type) {
            case EventType::NewOrder:    handle_new(e);    break;
            case EventType::PartialFill:; // handle_fill(e);   break;
            case EventType::FullFill:    handle_fill(e);   break;
            case EventType::Cancel:      handle_cancel(e); break;
            case EventType::Reject:      handle_reject(e); break;
            case EventType::Amend:       handle_amend(e);  break;
        }
    }

    [[nodiscard]] const Order* get(const std::string_view oid) const {
        for (size_t i = 0; i < count_; ++i)
            if (strncmp(slots_[i].order_id, oid.data(), sizeof(Order::order_id)) == 0)
                return &slots_[i];
        return nullptr;
    }

    template<typename Fn>
    void for_each(Fn fn) const {
        for (size_t i = 0; i < count_; ++i) fn(slots_[i]);
    }

    template<typename Fn>
    void for_each_open(Fn fn) const {
        for (size_t i = 0; i < count_; ++i)
            if (slots_[i].is_open()) fn(slots_[i]);
    }

    // Net open exposure for a symbol across all live orders.
    // Combine with PositionTracker::net_qty for true total exposure:
    //   total = net_qty + open_exposure("SYM")
    [[nodiscard]] int32_t open_exposure(const std::string_view sym) const {
        int32_t exp = 0;
        for (size_t i = 0; i < count_; ++i) {
            const Order& o = slots_[i];
            if (!o.is_open()) continue;
            if (strncmp(o.symbol, sym.data(), sizeof(Order::symbol)) != 0) continue;
            exp += (o.side == Side::Buy) ? o.remaining_qty() : -o.remaining_qty();
        }
        return exp;
    }

    [[nodiscard]] size_t order_count() const { return count_; }

    [[nodiscard]] size_t open_order_count() const {
        size_t n = 0;
        for (size_t i = 0; i < count_; ++i)
            if (slots_[i].is_open()) ++n;
        return n;
    }

private:
    void handle_new(const OrderEvent& e) {
        Order* o = get_or_create(e.order_id_view());
        if (!o) return;
        strncpy(o->symbol, e.symbol, sizeof(o->symbol));
        o->state          = OrderState::Pending;
        o->side           = e.side;
        o->limit_price    = e.price;
        o->original_qty   = e.qty;
        o->filled_qty     = 0;
        o->avg_fill_price = 0;
        o->opened_seq     = e.sequence;
        o->last_seq       = e.sequence;
    }

    void handle_fill(const OrderEvent& e) {
        Order* o = find(e.order_id_view());
        if (!o) return;
        const int64_t prev_cost  = o->avg_fill_price * o->filled_qty;
        const int64_t fill_cost  = e.fill_price      * e.fill_qty;
        const int32_t new_filled = o->filled_qty      + e.fill_qty;
        o->avg_fill_price = (new_filled > 0) ? (prev_cost + fill_cost) / new_filled : 0;
        o->filled_qty     = new_filled;
        o->last_seq       = e.sequence;
        o->state = (o->remaining_qty() <= 0)
            ? OrderState::Filled : OrderState::PartiallyFilled;
    }

    void handle_cancel(const OrderEvent& e) {
        Order* o = find(e.order_id_view());
        if (!o) return;
        o->state    = OrderState::Cancelled;
        o->last_seq = e.sequence;
    }

    void handle_reject(const OrderEvent& e) {
        Order* o = find(e.order_id_view());
        if (!o) return;
        o->state    = OrderState::Rejected;
        o->last_seq = e.sequence;
    }

    void handle_amend(const OrderEvent& e) {
        Order* o = find(e.order_id_view());
        if (!o) return;
        if (e.qty   > 0) o->original_qty = e.qty;
        if (e.price > 0) o->limit_price  = e.price;
        o->last_seq = e.sequence;
    }

    [[nodiscard]] Order* find(const std::string_view oid) {
        for (size_t i = 0; i < count_; ++i)
            if (strncmp(slots_[i].order_id, oid.data(), sizeof(Order::order_id)) == 0)
                return &slots_[i];
        return nullptr;
    }

    [[nodiscard]] Order* get_or_create(const std::string_view oid) {
        if (Order* p = find(oid)) return p;
        if (count_ >= MAX_ORDERS) return nullptr;
        Order& slot = slots_[count_++];
        memset(&slot, 0, sizeof(Order));
        strncpy(slot.order_id, oid.data(), sizeof(slot.order_id));
        return &slot;
    }

    std::array<Order, MAX_ORDERS> slots_{};
    size_t count_ = 0;
};

} // namespace hft