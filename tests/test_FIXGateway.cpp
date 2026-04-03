#include <gtest/gtest.h>

#include "FIXGateway.h"
#include "EventLog.h"
#include "PositionTracker.h"
#include "OrderManager.h"
#include "RiskChecker.h"
#include "fixtures/sample_msgs.h"

#include <cstring>
#include <unistd.h>

using namespace hft;
using namespace hft::gateway;
using namespace hft::test::fixtures;

// ── Test fixture ──────────────────────────────────────────────────────────────
// Sets up all bookkeeping components and the gateway with a dummy SessionID.
// We call gateway.onMessage() directly — no network required.

class FIXGatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        log_     = std::make_unique<EventLog>(log_path_);
        tracker_ = std::make_unique<PositionTracker>();
        orders_  = std::make_unique<OrderManager>();
        risk_    = std::make_unique<RiskChecker>();
        gateway_ = std::make_unique<FIXGateway>(
            *log_, *tracker_, *orders_, *risk_);
    }

    void TearDown() override {
        gateway_.reset();
        log_.reset();
        unlink(log_path_);
    }

    // Simulate the full fromApp() → crack() → onMessage() path
    void deliver(const FIX42::ExecutionReport& report) {
        FIX::SessionID dummy("FIX.4.2", "CLIENT", "SERVER", "");
        gateway_->onMessage(report, dummy);
    }

    const char* log_path_ = "/tmp/test_gateway.bin";

    std::unique_ptr<EventLog>        log_;
    std::unique_ptr<PositionTracker> tracker_;
    std::unique_ptr<OrderManager>    orders_;
    std::unique_ptr<RiskChecker>     risk_;
    std::unique_ptr<FIXGateway>      gateway_;
};

// ── Ack does not change state ─────────────────────────────────────────────────

TEST_F(FIXGatewayTest, AckDoesNotUpdatePosition) {
    // First register the order
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         100'000'000LL, "ORD001"));

    deliver(make_ack("ORD001", "AAPL"));

    // Position should still be flat — ack carries no fill
    const Position* pos = tracker_->get("AAPL");
    EXPECT_EQ(pos, nullptr);
    EXPECT_EQ(log_->size(), 0u) << "Ack should not append to event log";
}

// ── Full fill updates all three components ────────────────────────────────────

TEST_F(FIXGatewayTest, FullFillUpdatesEventLog) {
    deliver(make_full_fill("ORD001", "AAPL", FIX::Side_BUY, 100, 175.50));
    EXPECT_EQ(log_->size(), 1u);

    const OrderEvent* evt = log_->read(0);
    ASSERT_NE(evt, nullptr);
    EXPECT_EQ(evt->type,     EventType::FullFill);
    EXPECT_EQ(evt->fill_qty, 100);
}

TEST_F(FIXGatewayTest, FullFillUpdatesPositionTracker) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_full_fill("ORD001", "AAPL", FIX::Side_BUY, 100, 175.50));

    const Position* pos = tracker_->get("AAPL");
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->net_qty, 100);
}

TEST_F(FIXGatewayTest, FullFillUpdatesOrderManager) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_full_fill("ORD001", "AAPL", FIX::Side_BUY, 100, 175.50));

    const Order* order = orders_->get("ORD001");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_EQ(order->filled_qty, 100);
}

// ── Partial fill ──────────────────────────────────────────────────────────────

TEST_F(FIXGatewayTest, PartialFillLeavesOrderOpen) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_partial_fill("ORD001", "AAPL", FIX::Side_BUY, 60, 175.50));

    const Order* order = orders_->get("ORD001");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::PartiallyFilled);
    EXPECT_EQ(order->filled_qty, 60);
    EXPECT_EQ(order->remaining_qty(), 40);
}

TEST_F(FIXGatewayTest, PartialThenFullFill) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_partial_fill("ORD001", "AAPL", FIX::Side_BUY, 60, 175.00));
    deliver(make_partial_fill("ORD001", "AAPL", FIX::Side_BUY, 40, 175.50));

    const Position* pos   = tracker_->get("AAPL");
    const Order*    order = orders_->get("ORD001");

    ASSERT_NE(pos,   nullptr);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(pos->net_qty,    100);
    EXPECT_EQ(order->state,    OrderState::Filled);
    EXPECT_EQ(log_->size(),    2u);
}

// ── Cancel ────────────────────────────────────────────────────────────────────

TEST_F(FIXGatewayTest, CancelClosesOrder) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_cancel("ORD001", "AAPL"));

    const Order* order = orders_->get("ORD001");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::Cancelled);
}

TEST_F(FIXGatewayTest, CancelDoesNotMovePosition) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_cancel("ORD001", "AAPL"));

    // Cancel event appended to log
    EXPECT_EQ(log_->size(), 1u);
    // But no position change — cancel never moves shares
    EXPECT_EQ(tracker_->get("AAPL"), nullptr);
}

// ── Reject ────────────────────────────────────────────────────────────────────

TEST_F(FIXGatewayTest, RejectClosesOrder) {
    orders_->apply_event(make_new_order("AAPL", Side::Buy, 100,
                                         175'000'000LL, "ORD001"));
    deliver(make_reject("ORD001", "AAPL"));

    const Order* order = orders_->get("ORD001");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::Rejected);
}

// ── Multi-symbol ──────────────────────────────────────────────────────────────

TEST_F(FIXGatewayTest, MultipleSymbolsTrackedIndependently) {
    deliver(make_full_fill("O1", "AAPL", FIX::Side_BUY,  100, 175.00));
    deliver(make_full_fill("O2", "TSLA", FIX::Side_BUY,   20, 250.00));
    deliver(make_sell_fill("O3", "AAPL", 40, 176.00));

    EXPECT_EQ(tracker_->get("AAPL")->net_qty, 60);
    EXPECT_EQ(tracker_->get("TSLA")->net_qty, 20);
    EXPECT_EQ(log_->size(), 3u);
}

// ── Event log replay consistency ──────────────────────────────────────────────

TEST_F(FIXGatewayTest, ReplayReconstructsState) {
    deliver(make_full_fill("O1", "AAPL", FIX::Side_BUY, 100, 175.00));
    deliver(make_sell_fill("O2", "AAPL", 40, 176.00));

    // Rebuild from the log
    EventLog        log2(log_path_);
    PositionTracker tracker2;
    tracker2.replay_from(log2);

    const Position* pos = tracker2.get("AAPL");
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->net_qty, 60);
}
