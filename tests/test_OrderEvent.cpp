#include "../OrderEvent.h"

#include <gtest/gtest.h>
#include <cassert>
#include <cstdio>

using namespace hft;

static int64_t fp(const double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

TEST(OrderEvent, structSize) {
    ASSERT_EQ(sizeof(OrderEvent), 64u);
}

TEST(OrderEvent, makeFill) {
    const OrderEvent e = make_fill("AAPL", Side::Buy, 100, fp(175.00), "O1");
    ASSERT_EQ(e.type,       EventType::FullFill);
    ASSERT_STREQ(e.symbol,  "AAPL");
    ASSERT_EQ(e.side,       Side::Buy);
    ASSERT_EQ(e.fill_qty,   100);
    ASSERT_EQ(e.fill_price, fp(175.00));
    ASSERT_STREQ(e.order_id,"O1");

    ASSERT_STREQ(e.symbol_view().data(), "AAPL");
}

TEST(OrderEvent, makeNewOrder) {
    const OrderEvent e = make_new_order("AAPL", Side::Buy, 100, fp(135.00), "O1");
    ASSERT_EQ(e.type,       EventType::NewOrder);
    ASSERT_STREQ(e.symbol,  "AAPL");
    ASSERT_EQ(e.side,       Side::Buy);
    ASSERT_EQ(e.qty,        100);
    ASSERT_EQ(e.price,      fp(135.00));
    ASSERT_STREQ(e.order_id,"O1");
}

