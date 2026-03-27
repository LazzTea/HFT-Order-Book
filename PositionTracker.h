#pragma once
#include "OrderEvent.h"
#include "EventLog.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hft {

// Per-symbol position. 64 bytes (one cache line).
//
// Field layout:
//   8  symbol[8]
//   8  net_qty
//   8  avg_entry_price
//   8  realized_pnl
//   8  unrealized_pnl
//   4  total_buy_qty
//   4  total_sell_qty
//   8  last_price
//   8  last_updated_seq
struct Position {
    char    symbol[8];
    int64_t net_qty;
    int64_t avg_entry_price;   // fixed-point
    int64_t realized_pnl;      // fixed-point, cumulative
    int64_t unrealized_pnl;    // fixed-point, updated by mark_to_market()
    int32_t total_buy_qty;
    int32_t total_sell_qty;
    int64_t last_price;        // most recent market price (fixed-point)
    int64_t last_updated_seq;

    [[nodiscard]] std::string_view symbol_view() const {
        return {symbol, strnlen(symbol, sizeof(symbol))};
    }

    [[nodiscard]] bool is_flat() const { return net_qty == 0; }

    [[nodiscard]] double net_qty_d()          const { return static_cast<double>(net_qty); }
    [[nodiscard]] double avg_entry_price_d()  const { return static_cast<double>(avg_entry_price) / PRICE_SCALE; }
    [[nodiscard]] double realized_pnl_d()     const { return static_cast<double>(realized_pnl)    / PRICE_SCALE; }
    [[nodiscard]] double unrealized_pnl_d()   const { return static_cast<double>(unrealized_pnl)  / PRICE_SCALE; }
    [[nodiscard]] double total_pnl_d()        const { return realized_pnl_d() + unrealized_pnl_d(); }
};

static_assert(sizeof(Position) == 64, "Position must be 64 bytes");

static constexpr size_t MAX_SYMBOLS = 1024;

// PositionTracker maintains a flat array of Position slots.
// apply_event() must be called from a single writer thread.
// get() and for_each() are safe for concurrent readers.
class PositionTracker {
public:
    PositionTracker() {
        memset(slots_.data(), 0, sizeof(Position) * MAX_SYMBOLS);
    }

    void replay_from(const EventLog& log) {
        memset(slots_.data(), 0, sizeof(Position) * MAX_SYMBOLS);
        count_ = 0;
        log.replay([this](const OrderEvent& e) { apply_event(e); });
    }

    void apply_event(const OrderEvent& e) {
        if (!e.is_fill()) return;
        Position* p = get_or_create(e.symbol_view());
        if (!p) return;

        if (e.side == Side::Buy)
            apply_buy(*p, e.fill_qty, e.fill_price, e.sequence);
        else
            apply_sell(*p, e.fill_qty, e.fill_price, e.sequence);
    }

    [[nodiscard]] const Position* get(const std::string_view sym) const {
        for (size_t i = 0; i < count_; ++i)
            if (strncmp(slots_[i].symbol, sym.data(), sizeof(Position::symbol)) == 0)
                return &slots_[i];
        return nullptr;
    }

    void mark_to_market(const std::string_view sym, const int64_t mkt_price_fp) {
        Position* p = find(sym);
        if (!p || p->is_flat()) return;
        p->last_price     = mkt_price_fp;
        p->unrealized_pnl = (mkt_price_fp - p->avg_entry_price) * p->net_qty;
    }

    template<typename Fn>
    void for_each(Fn fn) const {
        for (size_t i = 0; i < count_; ++i) fn(slots_[i]);
    }

    [[nodiscard]] size_t symbol_count() const { return count_; }

private:
    static void apply_buy(Position& p, const int32_t qty, const int64_t price, const uint64_t seq) {
        p.total_buy_qty += qty;
        if (p.net_qty >= 0) {
            const int64_t new_qty  = p.net_qty + qty;
            const int64_t new_cost = p.avg_entry_price * p.net_qty + price * qty;
            p.avg_entry_price = new_qty > 0 ? new_cost / new_qty : 0;
            p.net_qty         = new_qty;
        } else {
            const int64_t cover = (qty <= -p.net_qty) ? qty : -p.net_qty;
            p.realized_pnl   += (p.avg_entry_price - price) * cover;
            p.net_qty         += qty;
            if      (p.net_qty > 0) p.avg_entry_price = price;
            else if (p.net_qty == 0) p.avg_entry_price = 0;
        }
        refresh_unrealized(p);
        p.last_updated_seq = static_cast<int64_t>(seq);
    }

    static void apply_sell(Position& p, const int32_t qty, const int64_t price, const uint64_t seq) {
        p.total_sell_qty += qty;
        if (p.net_qty > 0) {
            const int64_t close = (qty <= p.net_qty) ? qty : p.net_qty;
            p.realized_pnl   += (price - p.avg_entry_price) * close;
            p.net_qty         -= qty;
            if      (p.net_qty < 0) p.avg_entry_price = price;
            else if (p.net_qty == 0) p.avg_entry_price = 0;
        } else {
            const int64_t new_qty  = (-p.net_qty) + qty;
            const int64_t new_cost = p.avg_entry_price * (-p.net_qty) + price * qty;
            p.avg_entry_price = new_qty > 0 ? new_cost / new_qty : 0;
            p.net_qty        -= qty;
        }
        refresh_unrealized(p);
        p.last_updated_seq = static_cast<int64_t>(seq);
    }

    static void refresh_unrealized(Position& p) {
        p.unrealized_pnl = (p.last_price > 0 && p.net_qty != 0)
            ? (p.last_price - p.avg_entry_price) * p.net_qty : 0;
    }

    Position* find(const std::string_view sym) {
        for (size_t i = 0; i < count_; ++i)
            if (strncmp(slots_[i].symbol, sym.data(), sizeof(Position::symbol)) == 0)
                return &slots_[i];
        return nullptr;
    }

    Position* get_or_create(const std::string_view sym) {
        if (Position* p = find(sym)) return p;
        if (count_ >= MAX_SYMBOLS) return nullptr;
        Position& slot = slots_[count_++];
        memset(&slot, 0, sizeof(Position));
        strncpy(slot.symbol, sym.data(), sizeof(slot.symbol));
        return &slot;
    }

    std::array<Position, MAX_SYMBOLS> slots_{};
    size_t count_ = 0;
};

} // namespace hft
