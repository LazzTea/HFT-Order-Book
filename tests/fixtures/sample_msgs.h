#pragma once
// tests/fixtures/sample_msgs.h
// ─────────────────────────────
// Pre-built FIX 4.2 ExecutionReport message objects for use across
// test_MessageTranslator.cpp, test_FIXGateway.cpp, and test_Killswitch.cpp.
//
// Using FIX::Message objects (not raw strings) keeps tests independent of
// the parser and focused on the translation and bookkeeping logic.

#include <quickfix/fix42/ExecutionReport.h>
#include <quickfix/fix42/NewOrderSingle.h>
#include <quickfix/FixFields.h>

namespace hft::test::fixtures {

// ── ExecutionReport factories ─────────────────────────────────────────────────

// New Order Acknowledgment — ExecType=0 (New), no fill data.
// Should produce nullopt from MessageTranslator — no booking consequence.
inline FIX42::ExecutionReport make_ack(
    const std::string& cl_ord_id = "ORD001",
    const std::string& symbol    = "AAPL",
    char               side      = FIX::Side_BUY)
{
    FIX42::ExecutionReport r(
        FIX::OrderID("EXCH-001"),
        FIX::ExecID("EXEC-001"),
        FIX::ExecTransType(FIX::ExecTransType_NEW),
        FIX::ExecType(FIX::ExecType_NEW),
        FIX::OrdStatus(FIX::OrdStatus_NEW),
        FIX::Symbol(symbol),
        FIX::Side(side),
        FIX::LeavesQty(100),
        FIX::CumQty(0),
        FIX::AvgPx(0)
    );
    r.set(FIX::ClOrdID(cl_ord_id));
    r.set(FIX::OrderQty(100));
    return r;
}

// Partial fill — ExecType=1.
// Should produce EventType::PartialFill with fill_qty and fill_price set.
inline FIX42::ExecutionReport make_partial_fill(
    const std::string& cl_ord_id  = "ORD001",
    const std::string& symbol     = "AAPL",
    char               side       = FIX::Side_BUY,
    int                fill_qty   = 60,
    double             fill_price = 175.50)
{
    FIX42::ExecutionReport r(
        FIX::OrderID("EXCH-001"),
        FIX::ExecID("EXEC-002"),
        FIX::ExecTransType(FIX::ExecTransType_NEW),
        FIX::ExecType(FIX::ExecType_PARTIAL_FILL),
        FIX::OrdStatus(FIX::OrdStatus_PARTIALLY_FILLED),
        FIX::Symbol(symbol),
        FIX::Side(side),
        FIX::LeavesQty(100 - fill_qty),
        FIX::CumQty(fill_qty),
        FIX::AvgPx(fill_price)
    );
    r.set(FIX::ClOrdID(cl_ord_id));
    r.set(FIX::LastShares(fill_qty));
    r.set(FIX::LastPx(fill_price));
    r.set(FIX::OrderQty(100));
    return r;
}

// Full fill — ExecType=2.
// Should produce EventType::FullFill.
inline FIX42::ExecutionReport make_full_fill(
    const std::string& cl_ord_id  = "ORD001",
    const std::string& symbol     = "AAPL",
    char               side       = FIX::Side_BUY,
    int                fill_qty   = 100,
    double             fill_price = 175.50)
{
    FIX42::ExecutionReport r(
        FIX::OrderID("EXCH-001"),
        FIX::ExecID("EXEC-003"),
        FIX::ExecTransType(FIX::ExecTransType_NEW),
        FIX::ExecType(FIX::ExecType_FILL),
        FIX::OrdStatus(FIX::OrdStatus_FILLED),
        FIX::Symbol(symbol),
        FIX::Side(side),
        FIX::LeavesQty(0),
        FIX::CumQty(fill_qty),
        FIX::AvgPx(fill_price)
    );
    r.set(FIX::ClOrdID(cl_ord_id));
    r.set(FIX::LastShares(fill_qty));
    r.set(FIX::LastPx(fill_price));
    r.set(FIX::OrderQty(fill_qty));
    return r;
}

// Cancel confirmation — ExecType=4.
// Should produce EventType::Cancel. No fill data.
inline FIX42::ExecutionReport make_cancel(
    const std::string& cl_ord_id = "ORD001",
    const std::string& symbol    = "AAPL",
    char               side      = FIX::Side_BUY)
{
    FIX42::ExecutionReport r(
        FIX::OrderID("EXCH-001"),
        FIX::ExecID("EXEC-004"),
        FIX::ExecTransType(FIX::ExecTransType_NEW),
        FIX::ExecType(FIX::ExecType_CANCELED),
        FIX::OrdStatus(FIX::OrdStatus_CANCELED),
        FIX::Symbol(symbol),
        FIX::Side(side),
        FIX::LeavesQty(0),
        FIX::CumQty(0),
        FIX::AvgPx(0)
    );
    r.set(FIX::ClOrdID(cl_ord_id));
    return r;
}

// Reject — ExecType=8.
// Should produce EventType::Reject. No fill data.
inline FIX42::ExecutionReport make_reject(
    const std::string& cl_ord_id = "ORD001",
    const std::string& symbol    = "AAPL",
    char               side      = FIX::Side_BUY,
    const std::string& reason    = "Margin insufficient")
{
    FIX42::ExecutionReport r(
        FIX::OrderID(""),
        FIX::ExecID("EXEC-005"),
        FIX::ExecTransType(FIX::ExecTransType_NEW),
        FIX::ExecType(FIX::ExecType_REJECTED),
        FIX::OrdStatus(FIX::OrdStatus_REJECTED),
        FIX::Symbol(symbol),
        FIX::Side(side),
        FIX::LeavesQty(0),
        FIX::CumQty(0),
        FIX::AvgPx(0)
    );
    r.set(FIX::ClOrdID(cl_ord_id));
    r.set(FIX::Text(reason));
    return r;
}

// Sell-side variants (convenience wrappers)
inline FIX42::ExecutionReport make_sell_fill(
    const std::string& cl_ord_id  = "ORD002",
    const std::string& symbol     = "AAPL",
    int                fill_qty   = 100,
    double             fill_price = 176.00)
{
    return make_full_fill(cl_ord_id, symbol, FIX::Side_SELL,
                          fill_qty, fill_price);
}

} // namespace hft::test::fixtures
