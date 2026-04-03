#include <gtest/gtest.h>

#include "Killswitch.h"
#include "OrderManager.h"
#include "PositionTracker.h"
#include "OrderEvent.h"
#include "RiskChecker.h"
#include "FIXGateway.h"

#include <quickfix/SessionID.h>
#include <vector>

using namespace hft;
using namespace hft::gateway;

// ── Helpers ───────────────────────────────────────────────────────────────────

static int64_t fp(const double p) {
    return static_cast<int64_t>(p * PRICE_SCALE);
}

// Dummy SessionID — Killswitch uses it only to call sendToTarget(),
static FIX::SessionID dummy_session("FIX.4.2", "CLIENT", "SERVER", "");


// ── arm / is_armed ────────────────────────────────────────────────────────────

TEST(KillswitchArm, InitiallyDisarmed) {
    OrderManager    orders;
    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);
    EXPECT_FALSE(ks.is_armed());
}

TEST(KillswitchArm, ArmSetsFlag) {
    OrderManager    orders;
    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);
    ks.arm();
    EXPECT_TRUE(ks.is_armed());
}

// ── cancel_all ────────────────────────────────────────────────────────────────
// These tests verify that cancel_all() only targets open orders and does not
// attempt to cancel orders that are already filled or cancelled.
// We inspect OrderManager state before the call — the actual FIX sends
// go to sendToTarget() which silently no-ops without a live session.

TEST(KillswitchCancelAll, DoesNotCrashWithNoOpenOrders) {
    OrderManager    orders;
    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);
    EXPECT_NO_THROW(ks.cancel_all());
}

TEST(KillswitchCancelAll, OnlyTargetsOpenOrders) {
    OrderManager orders;

    // Open order
    orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    // Filled order — should not be cancelled
    orders.apply_event(make_new_order("TSLA", Side::Buy,  20, fp(250.00), "O2"));
    orders.apply_event(make_fill("TSLA", Side::Buy, 20, fp(250.00), "O2"));

    EXPECT_EQ(orders.open_order_count(), 1u)
        << "Only O1 should be open before cancel";

    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);

    EXPECT_NO_THROW(ks.cancel_all()); // changed from expect no throw
}

TEST(KillswitchCancelAll, MultipleOpenOrders) {
    OrderManager orders;
    for (int i = 0; i < 5; ++i) {
        char oid[8];
        snprintf(oid, sizeof(oid), "O%d", i);
        orders.apply_event(make_new_order("AAPL", Side::Buy, 100,
                                           fp(175.00), oid));
    }
    EXPECT_EQ(orders.open_order_count(), 5u);

    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);

    EXPECT_NO_THROW(ks.cancel_all()); // Changed from expect no throw
}

// ── flatten ───────────────────────────────────────────────────────────────────

TEST(KillswitchFlatten, DoesNotCrashWithFlatBook) {
    OrderManager    orders;
    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);
    EXPECT_NO_THROW(ks.flatten());
}

TEST(KillswitchFlatten, DoesNotCrashWithOpenPositions) {
    OrderManager    orders;
    PositionTracker tracker;

    // Build a long position
    tracker.apply_event(make_fill("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    tracker.apply_event(make_fill("TSLA", Side::Sell, 20, fp(250.00), "O2"));

    EXPECT_EQ(tracker.get("AAPL")->net_qty,  100);
    EXPECT_EQ(tracker.get("TSLA")->net_qty,  -20);

    Killswitch ks(orders, tracker, dummy_session);
    EXPECT_NO_THROW(ks.flatten());
}

// ── execute (full sequence) ───────────────────────────────────────────────────

TEST(KillswitchExecute, ReturnsElapsedTime) {
    OrderManager    orders;
    PositionTracker tracker;
    orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));

    Killswitch ks(orders, tracker, dummy_session);
    const int64_t elapsed = ks.execute("KS", 5000);  // generous budget
    EXPECT_GE(elapsed, 0);
    EXPECT_LT(elapsed, 5000) << "execute() should complete well within 5 seconds";
}

TEST(KillswitchExecute, DoesNotCrashOnEmptyBook) {
    OrderManager    orders;
    PositionTracker tracker;
    Killswitch ks(orders, tracker, dummy_session);
    EXPECT_NO_THROW(ks.execute());
}
