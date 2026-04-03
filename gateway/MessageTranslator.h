#pragma once
#include "OrderEvent.h"

#include <quickfix/fix42/ExecutionReport.h>
#include <quickfix/fix42/NewOrderSingle.h>
#include <quickfix/fix42/OrderCancelRequest.h>
#include <quickfix/FixFields.h>

#include <cstring>
#include <optional>

namespace hft::gateway {

// MessageTranslator
// ─────────────────
// Pure conversion layer between FIX tag-value pairs and internal OrderEvent
// structs. Has no dependency on a live FIX session — every method is a pure
// function taking a FIX message and returning an OrderEvent or nothing.
//
// This isolation is deliberate: the translator can be unit-tested by
// constructing FIX::Message objects in-process, with no network required.
// All session mechanics (heartbeats, sequence numbers, logon) stay in
// FIXGateway and never touch this layer.
//
// All methods are [[nodiscard]] — the caller must check the result.
// A nullopt return means the message was valid but carries no bookkeeping
// consequence (e.g. a New Order Acknowledgment with ExecType=0 does not
// change position state and should not produce an OrderEvent).

class MessageTranslator {
public:

    // Translate an inbound ExecutionReport (MsgType=8) into an OrderEvent.
    // Returns nullopt for ack-only reports that carry no booking consequence.
    [[nodiscard]] static std::optional<OrderEvent>
    from_execution_report(const FIX42::ExecutionReport& report)
    {
        FIX::ExecType exec_type;
        report.get(exec_type);

        // Map ExecType to EventType — only fill/cancel/reject change state
        EventType etype;
        switch (exec_type.getValue()) {
            case FIX::ExecType_PARTIAL_FILL: etype = EventType::PartialFill; break;
            case FIX::ExecType_FILL:         etype = EventType::FullFill;    break;
            case FIX::ExecType_CANCELED:     etype = EventType::Cancel;      break;
            case FIX::ExecType_REJECTED:     etype = EventType::Reject;      break;
            case FIX::ExecType_NEW:          return std::nullopt; // ack only
            default:                         return std::nullopt;
        }

        OrderEvent evt{};
        evt.type = etype;

        // Symbol
        FIX::Symbol symbol;
        report.get(symbol);
        strncpy(evt.symbol, symbol.getString().c_str(), sizeof(evt.symbol));

        // ClOrdID — our internal order identifier
        FIX::ClOrdID cl_ord_id;
        report.get(cl_ord_id);
        strncpy(evt.order_id, cl_ord_id.getString().c_str(), sizeof(evt.order_id));

        // Side
        FIX::Side side;
        report.get(side);
        evt.side = (side == FIX::Side_BUY) ? Side::Buy : Side::Sell;

        // Fill fields — only present on fill reports
        if (etype == EventType::PartialFill || etype == EventType::FullFill) {
            FIX::LastShares last_qty;
            FIX::LastPx     last_px;
            if (report.isSet(last_qty) && report.isSet(last_px)) {
                report.get(last_qty);
                report.get(last_px);
                evt.fill_qty   = static_cast<int32_t>(last_qty.getValue());
                evt.fill_price = to_fixed(last_px.getValue());
            }
        }

        return evt;
    }

    // Build a NewOrderSingle (MsgType=D) from an internal OrderEvent.
    // The caller sends this to the exchange via FIX::Session::sendToTarget().
    [[nodiscard]] static FIX42::NewOrderSingle
    to_new_order_single(const OrderEvent& evt)
    {
        FIX42::NewOrderSingle order(
            FIX::ClOrdID(std::string(evt.order_id,
                         strnlen(evt.order_id, sizeof(evt.order_id)))),
            // FIX::HandlInst(FIX::HandlInst_AUTOMATED_EXECUTION_ORDER_PRIVATE),
            FIX::HandlInst('1'),
            FIX::Symbol(std::string(evt.symbol,
                        strnlen(evt.symbol, sizeof(evt.symbol)))),
            FIX::Side(evt.side == Side::Buy ? FIX::Side_BUY : FIX::Side_SELL),
            FIX::TransactTime(),
            FIX::OrdType(FIX::OrdType_LIMIT)
        );

        order.set(FIX::OrderQty(static_cast<double>(evt.qty)));
        order.set(FIX::Price(from_fixed(evt.price)));
        order.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
        return order;
    }

    // Build an OrderCancelRequest (MsgType=F) for a known open order.
    [[nodiscard]] static FIX42::OrderCancelRequest
    to_cancel_request(const OrderEvent& original, std::string_view new_cl_ord_id)
    {
        FIX42::OrderCancelRequest cancel(
            FIX::OrigClOrdID(std::string(original.order_id,
                             strnlen(original.order_id, sizeof(original.order_id)))),
            FIX::ClOrdID(std::string(new_cl_ord_id)),
            FIX::Symbol(std::string(original.symbol,
                        strnlen(original.symbol, sizeof(original.symbol)))),
            FIX::Side(original.side == Side::Buy ? FIX::Side_BUY : FIX::Side_SELL),
            FIX::TransactTime()
        );
        return cancel;
    }

    // Fixed-point conversion helpers.
    // price_fp = double_price * PRICE_SCALE (stored as int64_t)
    [[nodiscard]] static int64_t to_fixed(const double price) noexcept {
        return static_cast<int64_t>(price * static_cast<double>(PRICE_SCALE));
    }

    [[nodiscard]] static double from_fixed(const int64_t price_fp) noexcept {
        return static_cast<double>(price_fp) / static_cast<double>(PRICE_SCALE);
    }
};

} // namespace hft::gateway
