#include "../include/OrderEvent.h"
#include "../include/EventLog.h"
#include "../include/PositionTracker.h"
#include "../include/RiskChecker.h"

#include <gtest/gtest.h>
#include <cassert>
#include <cstdio>

using namespace hft;

static int64_t fp(const double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

TEST(RiskChecker, basicChecks) {
    const RiskChecker risk;
    PositionTracker tracker;

    // Clean pass
    ASSERT_EQ(risk.check("AAPL", Side::Buy, 100, fp(175.00), tracker), RiskResult::Pass);

    // Fat-finger qty
    ASSERT_EQ(risk.check("AAPL", Side::Buy, 50'000, fp(175.00), tracker), RiskResult::FailMaxQty);

    // Build near limit (5 * 10,000 = 50,000)
    for (int i = 0; i < 5; ++i)
        tracker.apply_event(make_fill("AAPL", Side::Buy, 10'000, fp(175.00), "Ox"));

    ASSERT_EQ(tracker.get("AAPL")->net_qty, 50'000);

    // Another buy would breach position limit
    ASSERT_EQ(risk.check("AAPL", Side::Buy, 1, fp(175.00), tracker), RiskResult::FailPositionLimit);

    // Sell reduces exposure — should pass
    ASSERT_EQ(risk.check("AAPL", Side::Sell, 5'000, fp(176.00), tracker), RiskResult::Pass);
}

TEST(RiskChecker, blindSpot) {
    const RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    tracker.apply_event(make_fill("AAPL", Side::Buy, 40'000, fp(175.00), "F1"));

    const auto pending = make_new_order("AAPL", Side::Buy, 15'000, fp(175.00), "O1");
    orders.apply_event(pending);

    ASSERT_EQ(risk.check("AAPL", Side::Buy, 5'000, fp(175.00), tracker),
             RiskResult::Pass);

    ASSERT_EQ(risk.check("AAPL", Side::Buy, 5'000, fp(175.00), tracker, orders),
             RiskResult::FailPositionLimit);
}

TEST(RiskChecker, pendingSellCross) {
    const RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    tracker.apply_event(make_fill("TSLA", Side::Buy, 1'000, fp(250.00), "F1"));
    orders.apply_event(make_new_order("TSLA", Side::Sell, 500, fp(251.00), "O1"));

    ASSERT_EQ(risk.check("TSLA", Side::Sell, 600, fp(251.00), tracker),
             RiskResult::Pass);

    ASSERT_EQ(risk.check("TSLA", Side::Sell, 600, fp(251.00), tracker, orders),
             RiskResult::FailPositionCross);

    ASSERT_EQ(risk.check("TSLA", Side::Sell, 400, fp(251.00), tracker, orders),
             RiskResult::Pass);
}

TEST(RiskChecker, openOrdersReduceOnFill) {
    const RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    tracker.apply_event(make_fill("MSFT", Side::Buy, 30'000, fp(330.00), "F1"));
    orders.apply_event(make_new_order("MSFT", Side::Buy, 18'000, fp(330.00), "O1"));

    ASSERT_EQ(risk.check("MSFT", Side::Buy, 3'000, fp(330.00), tracker, orders),
             RiskResult::FailPositionLimit);

    const auto fill = make_fill("MSFT", Side::Buy, 18'000, fp(330.00), "O1");
    tracker.apply_event(fill);
    orders.apply_event(fill);

    ASSERT_EQ(risk.check("MSFT", Side::Buy, 3'000, fp(330.00), tracker, orders),
             RiskResult::FailPositionLimit);

    ASSERT_EQ(risk.check("MSFT", Side::Buy, 2'000, fp(330.00), tracker, orders),
             RiskResult::Pass);
}

TEST(RiskChecker, cancelledOrderReleasesExposure) {
    const RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    tracker.apply_event(make_fill("SPY", Side::Buy, 45'000, fp(450.00), "F1"));
    orders.apply_event(make_new_order("SPY", Side::Buy, 4'000, fp(450.00), "O1"));

    ASSERT_EQ(risk.check("SPY", Side::Buy, 2'000, fp(450.00), tracker, orders),
             RiskResult::FailPositionLimit);

    OrderEvent cancel{};
    cancel.type = EventType::Cancel;
    strncpy(cancel.order_id, "O1", sizeof(cancel.order_id));
    orders.apply_event(cancel);

    ASSERT_EQ(risk.check("SPY", Side::Buy, 2'000, fp(450.00), tracker, orders),
             RiskResult::Pass);
}