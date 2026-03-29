#include "../include/OrderEvent.h"
#include "../include/EventLog.h"
#include "../include/PositionTracker.h"
#include "../include/OrderManager.h"

#include <gtest/gtest.h>
#include <cassert>
#include <cstdio>

using namespace hft;

static int64_t fp(const double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

TEST(EventLog, structSize) {
    ASSERT_EQ(sizeof(LogHeader), 64u);
}

TEST(EventLog, basic) {
    const auto path = "/tmp/hft_test.bin";

    unlink(path);

    {
        EventLog log(path);
        ASSERT_EQ(log.size(), 0u);

        const uint64_t s0 = log.append(make_fill("AAPL", Side::Buy,   100, fp(175.00), "O1"));
        const uint64_t s1 = log.append(make_fill("AAPL", Side::Sell,   50, fp(176.00), "O2"));
        const uint64_t s2 = log.append(make_fill("TSLA", Side::Buy,    20, fp(250.00), "O3"));

        ASSERT_EQ(s0, 0u);
        ASSERT_EQ(s1, 1u);
        ASSERT_EQ(s2, 2u);
        ASSERT_EQ(log.size(), 3u);

        const OrderEvent* r = log.read(0);

        ASSERT_STREQ(r->symbol, "AAPL");
        ASSERT_EQ(r->side,      Side::Buy);
        ASSERT_EQ(r->fill_qty,  100);
    }

    // Durability: Reopen & Verify
    {
        const EventLog log2(path);
        ASSERT_EQ(log2.size(), 3u);

        int n = 0;
        log2.replay([&](const OrderEvent&) { ++n; });
        ASSERT_EQ(n, 3);
    }

    unlink(path);
}

TEST(EventLog, replayPositionTracker) {
    const auto path = "/tmp/hft_replay_test.bin";
    unlink(path);

    {
        EventLog log(path);
        log.append(make_fill("AAPL", Side::Buy,   100, fp(175.00), "O1"));
        log.append(make_fill("AAPL", Side::Sell,   40, fp(176.00), "O2"));
    }

    const EventLog log2(path);
    PositionTracker tracker;
    tracker.replay_from(log2);

    const Position* p = tracker.get("AAPL");
    ASSERT_EQ(p->net_qty, 60);
    ASSERT_EQ(p->realized_pnl, 40LL * (fp(176.00) - fp(175.00)));

    unlink(path);
}

TEST(EventLog, replayOrderManager) {
    const auto path = "/tmp/hft_om_test.bin";

    unlink(path);

    {
        EventLog log(path);
        log.append(make_new_order("TSLA", Side::Sell, 50, fp(250.00), "O1"));
        log.append(make_fill("TSLA", Side::Sell, 50, fp(250.10), "O1"));
    }

    const EventLog log2(path);
    OrderManager om;
    om.replay_from(log2);
    const Order* o = om.get("O1");

    ASSERT_TRUE(o != nullptr);
    ASSERT_TRUE(o->state == OrderState::Filled);
    ASSERT_EQ(o->filled_qty, 50);

    unlink(path);
}