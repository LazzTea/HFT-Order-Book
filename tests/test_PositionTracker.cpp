#include "../OrderEvent.h"
#include "../PositionTracker.h"

#include <gtest/gtest.h>
#include <cassert>
#include <cstdio>

using namespace hft;

static int64_t fp(double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

TEST(PositionTracker, structSize) {
    ASSERT_EQ(sizeof(Position), 64u);
}

TEST(PositionTracker, simpleLong) {
    PositionTracker tracker;

    const auto o1 = make_fill("AAPL", Side::Buy, 100, fp(175.50), "O1");

    tracker.apply_event(o1);
    const Position* p = tracker.get("AAPL");

    ASSERT_EQ(p != nullptr,         true);
    ASSERT_EQ(p->net_qty,           100);
    ASSERT_EQ(p->avg_entry_price,   fp(175.50));
    ASSERT_EQ(p->realized_pnl,      0);
}

TEST(PositionTracker, partialClose) {
    PositionTracker tracker;
    const auto o1 = make_fill("AAPL", Side::Buy, 100, fp(175.50), "O1");
    const auto o2 = make_fill("AAPL", Side::Sell, 50, fp(176.00), "O2");
    tracker.apply_event(o1);
    tracker.apply_event(o2);

    const Position* p = tracker.get("AAPL");
    ASSERT_EQ(p->net_qty,       50);
    ASSERT_EQ(p->realized_pnl,  50LL * (fp(176.00) - fp(175.50)));
}

TEST(PositionTracker, fullRoundTrip) {
    PositionTracker tracker;
    const auto o1 = make_fill("AAPL", Side::Buy, 100, fp(175.50), "O1");
    const auto o2 = make_fill("AAPL", Side::Sell, 100, fp(176.00), "O2");
    tracker.apply_event(o1);
    tracker.apply_event(o2);

    const Position* p = tracker.get("AAPL");
    ASSERT_EQ(p->net_qty,       0);
    ASSERT_EQ(p->realized_pnl,  100LL * (fp(176.00) - fp(175.50)));
}

TEST(PositionTracker, multipleSymbols) {
    PositionTracker tracker;
    const auto o1 = make_fill("AAPL", Side::Buy, 100, fp(175.50), "O1");
    const auto o2 = make_fill("TSLA", Side::Buy, 20, fp(250.00), "O2");
    const auto o3 = make_fill("MSFT", Side::Sell, 30, fp(330.00), "O3");

    tracker.apply_event(o1);
    tracker.apply_event(o2);
    tracker.apply_event(o3);

    ASSERT_EQ(tracker.symbol_count(), 3u);

    ASSERT_EQ(tracker.get("AAPL")->net_qty, 100);
    ASSERT_EQ(tracker.get("TSLA")->net_qty, 20);
    ASSERT_EQ(tracker.get("MSFT")->net_qty, -30);
}

TEST(PositionTracker, shortPosition) {
    PositionTracker tracker;
    const auto o1 = make_fill("SPY", Side::Sell, 100, fp(200.00), "O1");
    tracker.apply_event(o1);
    ASSERT_EQ(tracker.get("SPY")->net_qty, -100);

    const auto o2 = make_fill("SPY", Side::Buy, 100, fp(198.00), "O2");
    tracker.apply_event(o2);

    const Position* p = tracker.get("SPY");
    ASSERT_EQ(p->is_flat(),     true);
    ASSERT_EQ(p->realized_pnl,  100LL * (fp(200.00) - fp(198.00)));
}