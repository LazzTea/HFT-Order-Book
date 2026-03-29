#pragma once
#include "OrderEvent.h"
#include "PositionTracker.h"
#include "OrderManager.h"

#include <cstdint>
#include <string_view>

namespace hft {

enum class RiskResult : uint8_t {
    Pass               = 0,
    FailMaxQty,
    FailMaxNotional,
    FailPositionLimit,
    FailMaxLoss,
    FailPositionCross,
};

inline const char* to_string(RiskResult r) {
    switch (r) {
        case RiskResult::Pass:              return "PASS";
        case RiskResult::FailMaxQty:        return "FAIL_MAX_QTY";
        case RiskResult::FailMaxNotional:   return "FAIL_MAX_NOTIONAL";
        case RiskResult::FailPositionLimit: return "FAIL_POSITION_LIMIT";
        case RiskResult::FailMaxLoss:       return "FAIL_MAX_LOSS";
        case RiskResult::FailPositionCross: return "FAIL_POSITION_CROSS";
        default:                            return "UNKNOWN";
    }
}

struct RiskLimits {
    int32_t max_order_qty        = 10'000;
    int64_t max_order_notional   = 5'000'000LL * PRICE_SCALE;  // $5M
    int64_t max_net_position     = 50'000;
    int64_t max_loss_fp          = 100'000LL * PRICE_SCALE;    // $100k
    bool    allow_position_cross = false;
};

// All checks run inline, no heap allocation, no system calls.
// Target: < 200 ns per call.
class RiskChecker {
public:
    explicit RiskChecker(const RiskLimits &limits = {}) : limits_(limits) {}

    // Complete Check with both Position Tracker and Order Manager
    [[nodiscard]] RiskResult check(const std::string_view sym,
                                const Side side,
                                const int32_t qty,
                                const int64_t price_fp,
                                const PositionTracker& tracker,
                                const OrderManager& orders) const
    {
        if (qty > limits_.max_order_qty)
            return RiskResult::FailMaxQty;

        if (static_cast<int64_t>(qty) * price_fp > limits_.max_order_notional)
            return RiskResult::FailMaxNotional;

        const Position* pos = tracker.get(sym);

        // Combine filled exposure and pending open order exposure
        const int64_t filled_qty  = pos ? pos->net_qty : 0;
        const int64_t pending_qty = orders.open_exposure(sym);
        const int64_t true_qty    = filled_qty + pending_qty;

        // Uses true_qty instead of pos->net_qty
        const int64_t projected = true_qty + (side == Side::Buy ? qty : -qty);

        if (projected >  limits_.max_net_position ||
            projected < -limits_.max_net_position)
            return RiskResult::FailPositionLimit;

        // Position cross: would this order flip long↔short including open orders?
        // Using true_qty as the baseline catches the case where pending sells
        // would already flip you short before this order even lands.
        if (!limits_.allow_position_cross) {
            if ((true_qty > 0 && projected < 0) ||
                (true_qty < 0 && projected > 0))
                return RiskResult::FailPositionCross;
        }

        if (pos && pos->realized_pnl < -limits_.max_loss_fp)
            return RiskResult::FailMaxLoss;

        return RiskResult::Pass;
    }

    // Check using only Position
    RiskResult check(std::string_view /*sym*/,
                     const Side side,
                     const int32_t qty,
                     const int64_t price_fp,
                     const Position* pos) const
    {
        if (qty > limits_.max_order_qty)
            return RiskResult::FailMaxQty;

        if (static_cast<int64_t>(qty) * price_fp > limits_.max_order_notional)
            return RiskResult::FailMaxNotional;

        if (pos) {
            const int64_t projected = pos->net_qty + (side == Side::Buy ? qty : -qty);
            if (projected >  limits_.max_net_position ||
                projected < -limits_.max_net_position) // Min Net Position
                return RiskResult::FailPositionLimit;

            if (!limits_.allow_position_cross) {
                if ((pos->net_qty > 0 && projected < 0) ||
                    (pos->net_qty < 0 && projected > 0))
                    return RiskResult::FailPositionCross;
            }

            if (pos->realized_pnl < -limits_.max_loss_fp)
                return RiskResult::FailMaxLoss;
        }

        return RiskResult::Pass;
    }

    // Gets Position from Position Tracker, then calls another Check Function
    [[nodiscard]] RiskResult check(
                        const std::string_view sym,
                        const Side side,
                        const int32_t qty,
                        const int64_t price_fp,
                        const PositionTracker& tracker
                        ) const
    {
        return check(sym, side, qty, price_fp, tracker.get(sym));
    }

    void set_limits(const RiskLimits &l) { limits_ = l; }
    [[nodiscard]] const RiskLimits& limits() const { return limits_; }

private:
    RiskLimits limits_;
};

} // namespace hft
