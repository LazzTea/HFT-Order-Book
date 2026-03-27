#include "../OrderEvent.h"
#include "../EventLog.h"
#include "../PositionTracker.h"
#include "../RiskChecker.h"

#include <gtest/gtest.h>
#include <cassert>
#include <cstdio>

using namespace hft;

static int64_t fp(const double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

TEST(OrderManager, newOrder) {
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    const Order* o = om.get("O1");

    ASSERT_TRUE(o != nullptr);
    ASSERT_TRUE(o->state == OrderState::Pending);

    ASSERT_EQ(o->original_qty,      100);
    ASSERT_EQ(o->remaining_qty(),   100);
    ASSERT_EQ(o->filled_qty,        0);

    ASSERT_TRUE(o->symbol_view()    == "AAPL");
    ASSERT_TRUE(o->side             == Side::Buy);
}

TEST(OrderManager, orderPartialFill) {
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy, 60, fp(174.98), "O1", "", true));
    const Order* o = om.get("O1");

    ASSERT_TRUE(o->state == OrderState::PartiallyFilled);
    ASSERT_EQ(o->filled_qty,        60);
    ASSERT_EQ(o->remaining_qty(),   40);
    ASSERT_TRUE(o->is_open());
}

TEST(OrderManager, orderFullFill) {
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy, 60, fp(174.98), "O1", "", true));
    om.apply_event(make_fill("AAPL", Side::Buy, 40, fp(175.00), "O1"));
    const Order* o = om.get("O1");

    ASSERT_TRUE(o->state == OrderState::Filled);
    ASSERT_EQ(o->filled_qty,        100);
    ASSERT_EQ(o->remaining_qty(),   0);
    ASSERT_TRUE(o->is_terminal());
}

TEST(OrderManager, orderAvgFillPrice) {
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy,  60, fp(174.00), "O1", "", true));
    om.apply_event(make_fill("AAPL", Side::Buy,  40, fp(176.50), "O1"));

    const Order* o = om.get("O1");
    const int64_t expected = (fp(174.00) * 60 + fp(176.50) * 40) / 100;

    ASSERT_EQ(o->avg_fill_price, expected);
}

TEST(OrderManager, cancelOrder) {
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy, 30, fp(175.00), "O1", "", true));
    OrderEvent cancel{};
    cancel.type = EventType::Cancel;
    strncpy(cancel.order_id, "O1", sizeof(cancel.order_id));
    om.apply_event(cancel);
    const Order* o = om.get("O1");

    ASSERT_TRUE(o->state == OrderState::Cancelled);
    ASSERT_TRUE(o->is_terminal());
    ASSERT_EQ(o->filled_qty,        30);
    ASSERT_EQ(o->remaining_qty(),   70);
}

TEST(OrderManager, openExposure) {
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy,  100, fp(175.00), "O1"));
    om.apply_event(make_new_order("AAPL", Side::Buy,   50, fp(174.50), "O2"));
    om.apply_event(make_new_order("AAPL", Side::Sell,  30, fp(176.00), "O3"));

    ASSERT_EQ(om.open_exposure("AAPL"), 120);
    om.apply_event(make_fill("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    ASSERT_EQ(om.open_exposure("AAPL"), 20);
}


