#pragma once
#include "OrderEvent.h"
#include "OrderManager.h"
#include "PositionTracker.h"
#include "MessageTranslator.h"

#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/fix42/OrderCancelRequest.h>
#include <quickfix/fix42/NewOrderSingle.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

namespace hft::gateway {

// Killswitch
// ──────────
// Hardened emergency shutdown path. Designed to be callable from any thread
// at any time — including signal handlers (via the armed_ flag) and the
// strategy thread.
//
// Two operations:
//   cancel_all()  — sends OrderCancelRequest for every open order.
//                   Does not wait for confirms — fire and forget.
//   flatten()     — sends market orders to close every non-zero position.
//                   Called after cancel_all() once open orders are cleared.
//
// In production, budget 100ms for the full sequence. Track wall time
// from the first call to the last send — if you exceed the budget,
// log it as a breach even if all sends succeeded.
//
// The armed_ flag lets a signal handler (SIGINT, SIGTERM) trigger the
// killswitch without blocking — set armed_ = true in the handler, then
// the strategy loop checks it on every iteration.

class Killswitch {
public:
    Killswitch(OrderManager&    orders,
               PositionTracker& tracker,
               FIX::SessionID  session_id)
        : orders_(orders)
        , tracker_(tracker)
        , session_id_(std::move(session_id))
    {}

    // Arm the killswitch for deferred execution from a signal handler.
    // The strategy loop should call execute() when it sees armed_ == true.
    void arm() noexcept { armed_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_armed() const noexcept {
        return armed_.load(std::memory_order_acquire);
    }

    // Execute the full killswitch sequence.
    // Returns elapsed milliseconds. Logs a warning if over budget.
    int64_t execute(std::string_view cancel_id_prefix = "KS",
                    int64_t budget_ms = 100)
    {
        const auto t0 = std::chrono::steady_clock::now();

        cancel_all(cancel_id_prefix);
        // flatten() is intentionally separate — call it once you've confirmed
        // all cancels are acknowledged, or after a fixed timeout.

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (elapsed > budget_ms)
            printf("[KILLSWITCH] WARNING: cancel_all took %llums (budget %llums)\n",
                   (long long)elapsed, (long long)budget_ms);
        else
            printf("[KILLSWITCH] cancel_all sent in %llums\n", (long long)elapsed);

        return elapsed;
    }

    // Send OrderCancelRequest for every open order.
    // Thread-safe: reads OrderManager (read-only) and sends via QuickFIX.
    void cancel_all(std::string_view id_prefix = "KS") {
        uint32_t count = 0;
        orders_.for_each_open([&](const Order& o) {
            const std::string cancel_id =
                std::string(id_prefix) + std::to_string(++count);

            // Reconstruct a minimal OrderEvent for the translator
            OrderEvent ref{};
            memcpy(ref.order_id, o.order_id, sizeof(ref.order_id));
            memcpy(ref.symbol,   o.symbol,   sizeof(ref.symbol));
            ref.side = o.side;

            auto cancel = MessageTranslator::to_cancel_request(ref, cancel_id);

            // Added try catch block to handle session not found
            try {
                FIX::Session::sendToTarget(cancel, session_id_);
            } catch (const FIX::SessionNotFound&) {
                printf("[KILLSWITCH] warning: session not found for cancel %s\n",
                       cancel_id.c_str());
            }
        });
        printf("[KILLSWITCH] sent %u cancel requests\n", count);
    }

    // Send market orders to close every non-zero position.
    // Call this after cancel_all() confirms have arrived, or after a timeout.
    void flatten(std::string_view id_prefix = "FL") {
        uint32_t count = 0;
        tracker_.for_each([&](const Position& pos) {
            if (pos.is_flat()) return;

            // Close opposite to net direction
            const Side close_side = (pos.net_qty > 0) ? Side::Sell : Side::Buy;
            const int32_t close_qty = static_cast<int32_t>(
                pos.net_qty > 0 ? pos.net_qty : -pos.net_qty);

            const std::string flat_id =
                std::string(id_prefix) + std::to_string(++count);

            OrderEvent flat_evt{};
            flat_evt.type  = EventType::NewOrder;
            flat_evt.side  = close_side;
            flat_evt.qty   = close_qty;
            flat_evt.price = 0;  // market order — price not used
            memcpy(flat_evt.symbol,   pos.symbol,    sizeof(flat_evt.symbol));
            strncpy(flat_evt.order_id, flat_id.c_str(), sizeof(flat_evt.order_id));

            auto mkt_order = MessageTranslator::to_new_order_single(flat_evt);
            // Override to market type
            mkt_order.set(FIX::OrdType(FIX::OrdType_MARKET));
            // mkt_order.remove(FIX::FIELD::Price);
            mkt_order.removeField(FIX::FIELD::Price);

            try {
                FIX::Session::sendToTarget(mkt_order, session_id_);
            } catch (const FIX::SessionNotFound&) {
                printf("[KILLSWITCH] warning: session not found for flatten %s\n",
                       flat_id.c_str());
            }
        });
        printf("[KILLSWITCH] sent %u flatten orders\n", count);
    }

private:
    OrderManager&    orders_;
    PositionTracker& tracker_;
    FIX::SessionID   session_id_;
    std::atomic<bool> armed_{false};
};

} // namespace hft::gateway
