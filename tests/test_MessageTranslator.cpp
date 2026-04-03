#include <gtest/gtest.h>

#include "MessageTranslator.h"
#include "fixtures/sample_msgs.h"

using namespace hft;
using namespace hft::gateway;
using namespace hft::test::fixtures;

// ── Fixed-point conversion ────────────────────────────────────────────────────

TEST(MessageTranslatorFixedPoint, RoundTrip) {
    const double price = 175.50;
    const int64_t fp   = MessageTranslator::to_fixed(price);
    EXPECT_EQ(fp, 175'500'000LL);
    EXPECT_DOUBLE_EQ(MessageTranslator::from_fixed(fp), price);
}

TEST(MessageTranslatorFixedPoint, ZeroPrice) {
    EXPECT_EQ(MessageTranslator::to_fixed(0.0), 0);
    EXPECT_DOUBLE_EQ(MessageTranslator::from_fixed(0), 0.0);
}

TEST(MessageTranslatorFixedPoint, LargePrice) {
    // $50,000 per share (e.g. BRK.A)
    const int64_t fp = MessageTranslator::to_fixed(50000.00);
    EXPECT_EQ(fp, 50'000'000'000LL);
}

// ── Ack — no booking consequence ─────────────────────────────────────────────

TEST(MessageTranslatorFromReport, AckReturnsNullopt) {
    const auto report = make_ack("ORD001", "AAPL");
    const auto result = MessageTranslator::from_execution_report(report);
    EXPECT_FALSE(result.has_value())
        << "New Order Ack should produce no OrderEvent";
}

// ── Partial fill ──────────────────────────────────────────────────────────────

TEST(MessageTranslatorFromReport, PartialFillType) {
    const auto report = make_partial_fill("ORD001", "AAPL",
                                          FIX::Side_BUY, 60, 175.50);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, EventType::PartialFill);
}

TEST(MessageTranslatorFromReport, PartialFillSymbol) {
    const auto report = make_partial_fill("ORD001", "TSLA",
                                          FIX::Side_BUY, 60, 250.00);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_view(), "TSLA");
}

TEST(MessageTranslatorFromReport, PartialFillSideBuy) {
    const auto report = make_partial_fill("ORD001", "AAPL",
                                          FIX::Side_BUY, 60, 175.50);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->side, Side::Buy);
}

TEST(MessageTranslatorFromReport, PartialFillSideSell) {
    const auto report = make_partial_fill("ORD002", "AAPL",
                                          FIX::Side_SELL, 40, 176.00);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->side, Side::Sell);
}

TEST(MessageTranslatorFromReport, PartialFillQtyAndPrice) {
    const auto report = make_partial_fill("ORD001", "AAPL",
                                          FIX::Side_BUY, 60, 175.50);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->fill_qty,   60);
    EXPECT_EQ(result->fill_price, MessageTranslator::to_fixed(175.50));
}

TEST(MessageTranslatorFromReport, PartialFillOrderId) {
    const auto report = make_partial_fill("ORD999", "AAPL",
                                          FIX::Side_BUY, 60, 175.50);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->order_id_view(), "ORD999");
}

// ── Full fill ─────────────────────────────────────────────────────────────────

TEST(MessageTranslatorFromReport, FullFillType) {
    const auto report = make_full_fill("ORD001", "AAPL",
                                       FIX::Side_BUY, 100, 175.50);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, EventType::FullFill);
}

TEST(MessageTranslatorFromReport, FullFillQtyAndPrice) {
    const auto report = make_full_fill("ORD001", "AAPL",
                                       FIX::Side_BUY, 100, 175.50);
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->fill_qty,   100);
    EXPECT_EQ(result->fill_price, MessageTranslator::to_fixed(175.50));
}

// ── Cancel ────────────────────────────────────────────────────────────────────

TEST(MessageTranslatorFromReport, CancelType) {
    const auto report = make_cancel("ORD001", "AAPL");
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, EventType::Cancel);
}

TEST(MessageTranslatorFromReport, CancelHasNoFillData) {
    const auto report = make_cancel("ORD001", "AAPL");
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->fill_qty,   0);
    EXPECT_EQ(result->fill_price, 0);
}

// ── Reject ────────────────────────────────────────────────────────────────────

TEST(MessageTranslatorFromReport, RejectType) {
    const auto report = make_reject("ORD001", "AAPL");
    const auto result = MessageTranslator::from_execution_report(report);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, EventType::Reject);
}

// ── ToNewOrderSingle ──────────────────────────────────────────────────────────

TEST(MessageTranslatorToOrder, SymbolAndSideAndQty) {
    const auto evt = make_new_order("AAPL", Side::Buy, 100,
                                     MessageTranslator::to_fixed(175.00), "O1");
    const auto msg = MessageTranslator::to_new_order_single(evt);

    FIX::Symbol   sym;  msg.get(sym);
    FIX::Side     side; msg.get(side);
    FIX::OrderQty qty;  msg.get(qty);

    EXPECT_EQ(sym.getString(),      "AAPL");
    EXPECT_EQ(side.getValue(),      FIX::Side_BUY);
    EXPECT_DOUBLE_EQ(qty.getValue(), 100.0);
}

TEST(MessageTranslatorToOrder, Price) {
    const auto evt = make_new_order("AAPL", Side::Buy, 100,
                                     MessageTranslator::to_fixed(175.50), "O1");
    const auto msg = MessageTranslator::to_new_order_single(evt);

    FIX::Price px; msg.get(px);
    EXPECT_DOUBLE_EQ(px.getValue(), 175.50);
}

TEST(MessageTranslatorToOrder, SellSide) {
    const auto evt = make_new_order("AAPL", Side::Sell, 50,
                                     MessageTranslator::to_fixed(176.00), "O2");
    const auto msg = MessageTranslator::to_new_order_single(evt);

    FIX::Side side; msg.get(side);
    EXPECT_EQ(side.getValue(), FIX::Side_SELL);
}
