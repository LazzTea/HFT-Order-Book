#include "../OrderEvent.h"
#include "../EventLog.h"
#include "../PositionTracker.h"
#include "../RiskChecker.h"
#include "../OrderManager.h"

#include <cassert>
#include <cstdio>
// #include <cstring>

using namespace hft;

static int tests_run = 0, tests_passed = 0;

#define CHECK_EQ(a, b) do { \
    ++tests_run; \
    if ((a) == (b)) { ++tests_passed; } \
    else { printf("  FAIL %s:%d  got %lld  expected %lld\n", \
                  __FILE__, __LINE__, (long long)(a), (long long)(b)); } \
} while(0)

#define CHECK(expr) do { \
    ++tests_run; \
    if (expr) { ++tests_passed; } \
    else { printf("  FAIL %s:%d  " #expr "\n", __FILE__, __LINE__); } \
} while(0)

static int64_t fp(double p) { return static_cast<int64_t>(p * PRICE_SCALE); }


//      Testing Functions


void test_struct_sizes() {
    printf("test_struct_sizes\n");
    CHECK_EQ(sizeof(OrderEvent), 64u);
    CHECK_EQ(sizeof(Position),   64u);
    CHECK_EQ(sizeof(LogHeader),  64u);
}

void test_make_fill() {
    printf("test_make_fill\n");
    auto e = make_fill("AAPL", Side::Buy, 100, fp(175.50), "O1");
    CHECK(e.is_fill());
    CHECK_EQ(e.fill_qty,   100);
    CHECK_EQ(e.fill_price, fp(175.50));
    CHECK_EQ(e.side,       Side::Buy);
    CHECK(e.symbol_view() == "AAPL");
}

void test_event_log() {
    printf("test_event_log\n");
    const auto path = "/tmp/hft_test.bin";
    unlink(path);

    {
        EventLog log(path);
        CHECK_EQ(log.size(), 0u);

        const uint64_t s0 = log.append(make_fill("AAPL", Side::Buy,  100, fp(175.00), "O1"));
        const uint64_t s1 = log.append(make_fill("AAPL", Side::Sell,  50, fp(176.00), "O2"));
        const uint64_t s2 = log.append(make_fill("TSLA", Side::Buy,   20, fp(250.00), "O3"));

        CHECK_EQ(s0, 0u);
        CHECK_EQ(s1, 1u);
        CHECK_EQ(s2, 2u);
        CHECK_EQ(log.size(), 3u);

        const OrderEvent* r = log.read(0);
        CHECK(r != nullptr);
        CHECK_EQ(r->fill_qty, 100);
    }

    // Durability: reopen and verify
    {
        const EventLog log2(path);
        CHECK_EQ(log2.size(), 3u);
        int n = 0;
        log2.replay([&](const OrderEvent&) { ++n; });
        CHECK_EQ(n, 3);
    }

    unlink(path);
}

void test_simple_long() {
    printf("test_simple_long\n");
    PositionTracker t;
    t.apply_event(make_fill("AAPL", Side::Buy, 100, fp(175.50), "O1"));

    const Position* p = t.get("AAPL");
    CHECK(p != nullptr);
    CHECK_EQ(p->net_qty,         100);
    CHECK_EQ(p->avg_entry_price, fp(175.50));
    CHECK_EQ(p->realized_pnl,    0);
}

void test_partial_close() {
    printf("test_partial_close\n");
    PositionTracker t;
    t.apply_event(make_fill("AAPL", Side::Buy,  100, fp(175.50), "O1"));
    t.apply_event(make_fill("AAPL", Side::Sell,  50, fp(176.00), "O2"));

    const Position* p = t.get("AAPL");
    CHECK_EQ(p->net_qty, 50);
    // Realized: 50 * (176.00 - 175.50)
    CHECK_EQ(p->realized_pnl, 50LL * (fp(176.00) - fp(175.50)));
}

void test_full_round_trip() {
    printf("test_full_round_trip\n");
    PositionTracker t;
    t.apply_event(make_fill("AAPL", Side::Buy,  100, fp(175.00), "O1"));
    t.apply_event(make_fill("AAPL", Side::Sell, 100, fp(177.00), "O2"));

    const Position* p = t.get("AAPL");
    CHECK(p->is_flat());
    // Realized: 100 * (177 - 175) = $200
    CHECK_EQ(p->realized_pnl, 100LL * (fp(177.00) - fp(175.00)));
}

void test_mark_to_market() {
    printf("test_mark_to_market\n");
    PositionTracker t;
    t.apply_event(make_fill("TSLA", Side::Buy, 50, fp(250.00), "O1"));
    t.mark_to_market("TSLA", fp(255.00));

    const Position* p = t.get("TSLA");
    // Unrealized: 50 * (255 - 250) = $250
    CHECK_EQ(p->unrealized_pnl, 50LL * (fp(255.00) - fp(250.00)));
}

void test_multi_symbol() {
    printf("test_multi_symbol\n");
    PositionTracker t;
    t.apply_event(make_fill("AAPL", Side::Buy,  100, fp(175.00), "O1"));
    t.apply_event(make_fill("TSLA", Side::Buy,   20, fp(250.00), "O2"));
    t.apply_event(make_fill("MSFT", Side::Sell,  30, fp(330.00), "O3"));

    CHECK_EQ(t.symbol_count(), 3u);
    CHECK_EQ(t.get("AAPL")->net_qty,  100);
    CHECK_EQ(t.get("TSLA")->net_qty,   20);
    CHECK_EQ(t.get("MSFT")->net_qty,  -30);
}

void test_short_position() {
    printf("test_short_position\n");
    PositionTracker t;

    // Open short: sell 100 @ $200
    t.apply_event(make_fill("SPY", Side::Sell, 100, fp(200.00), "O1"));
    CHECK_EQ(t.get("SPY")->net_qty, -100);

    // Cover: buy 100 @ $198 -> profit $200
    t.apply_event(make_fill("SPY", Side::Buy, 100, fp(198.00), "O2"));
    const Position* p = t.get("SPY");
    CHECK(p->is_flat());
    CHECK_EQ(p->realized_pnl, 100LL * (fp(200.00) - fp(198.00)));
}

void test_replay_rebuilds_state() {
    printf("test_replay_rebuilds_state\n");
    const auto path = "/tmp/hft_replay_test.bin";
    unlink(path);

    {
        EventLog log(path);
        log.append(make_fill("AAPL", Side::Buy,  100, fp(175.00), "O1"));
        log.append(make_fill("AAPL", Side::Sell,  40, fp(176.00), "O2"));
    }

    const EventLog log2(path);
    PositionTracker t;
    t.replay_from(log2);

    const Position* p = t.get("AAPL");
    CHECK(p != nullptr);
    CHECK_EQ(p->net_qty, 60);
    CHECK_EQ(p->realized_pnl, 40LL * (fp(176.00) - fp(175.00)));

    unlink(path);
}

void test_risk_checks() {
    printf("test_risk_checks\n");
    const RiskChecker risk;
    PositionTracker t;

    // Clean pass
    CHECK_EQ(risk.check("AAPL", Side::Buy, 100, fp(175.00), t), RiskResult::Pass);

    // Fat-finger qty── risk + order manager integration tests ────────────────────────────────────
    CHECK_EQ(risk.check("AAPL", Side::Buy, 50'000, fp(175.00), t), RiskResult::FailMaxQty);

    // Build near limit (5 * 10,000 = 50,000)
    for (int i = 0; i < 5; ++i)
        t.apply_event(make_fill("AAPL", Side::Buy, 10'000, fp(175.00), "Ox"));

    CHECK_EQ(t.get("AAPL")->net_qty, 50'000);

    // Another buy would breach position limit
    CHECK_EQ(risk.check("AAPL", Side::Buy, 1, fp(175.00), t), RiskResult::FailPositionLimit);

    // Sell reduces exposure — should pass
    CHECK_EQ(risk.check("AAPL", Side::Sell, 5'000, fp(176.00), t), RiskResult::Pass);
}


//      Order Manager Test


void test_order_new() {
    printf("test_order_new\n");
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    const Order* o = om.get("O1");
    CHECK(o != nullptr);
    CHECK(o->state == OrderState::Pending);
    CHECK_EQ(o->original_qty,  100);
    CHECK_EQ(o->remaining_qty(), 100);
    CHECK_EQ(o->filled_qty,      0);
    CHECK(o->symbol_view()   == "AAPL");
    CHECK(o->side            == Side::Buy);
}

void test_order_partial_fill() {
    printf("test_order_partial_fill\n");
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy, 60, fp(174.98), "O1", "", true));
    const Order* o = om.get("O1");
    CHECK(o->state == OrderState::PartiallyFilled);
    CHECK_EQ(o->filled_qty,    60);
    CHECK_EQ(o->remaining_qty(), 40);
    CHECK(o->is_open());
}

void test_order_full_fill() {
    printf("test_order_full_fill\n");
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy, 60, fp(174.98), "O1", "", true));
    om.apply_event(make_fill("AAPL", Side::Buy, 40, fp(175.00), "O1"));
    const Order* o = om.get("O1");
    CHECK(o->state == OrderState::Filled);
    CHECK_EQ(o->filled_qty,    100);
    CHECK_EQ(o->remaining_qty(),   0);
    CHECK(o->is_terminal());
}

void test_order_avg_fill_price() {
    printf("test_order_avg_fill_price\n");
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy,  60, fp(174.00), "O1", "", true));
    om.apply_event(make_fill("AAPL", Side::Buy,  40, fp(176.50), "O1"));
    const Order* o = om.get("O1");
    const int64_t expected = (fp(174.00) * 60 + fp(176.50) * 40) / 100;
    CHECK_EQ(o->avg_fill_price, expected);
}

void test_order_cancel() {
    printf("test_order_cancel\n");
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    om.apply_event(make_fill("AAPL", Side::Buy, 30, fp(175.00), "O1", "", true));
    OrderEvent cancel{};
    cancel.type = EventType::Cancel;
    strncpy(cancel.order_id, "O1", sizeof(cancel.order_id));
    om.apply_event(cancel);
    const Order* o = om.get("O1");
    CHECK(o->state == OrderState::Cancelled);
    CHECK(o->is_terminal());
    CHECK_EQ(o->filled_qty,    30);
    CHECK_EQ(o->remaining_qty(), 70);
}

void test_open_exposure() {
    printf("test_open_exposure\n");
    OrderManager om;
    om.apply_event(make_new_order("AAPL", Side::Buy,  100, fp(175.00), "O1"));
    om.apply_event(make_new_order("AAPL", Side::Buy,   50, fp(174.50), "O2"));
    om.apply_event(make_new_order("AAPL", Side::Sell,  30, fp(176.00), "O3"));
    CHECK_EQ(om.open_exposure("AAPL"), 120);
    om.apply_event(make_fill("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    CHECK_EQ(om.open_exposure("AAPL"), 20);
}

void test_order_replay() {
    printf("test_order_replay\n");
    const char* path = "/tmp/hft_om_test.bin";
    unlink(path);
    {
        EventLog log(path);
        log.append(make_new_order("TSLA", Side::Sell, 50, fp(250.00), "O1"));
        log.append(make_fill("TSLA", Side::Sell, 50, fp(250.10), "O1"));
    }
    EventLog log2(path);
    OrderManager om;
    om.replay_from(log2);
    const Order* o = om.get("O1");
    CHECK(o != nullptr);
    CHECK(o->state == OrderState::Filled);
    CHECK_EQ(o->filled_qty, 50);
    unlink(path);
}

//      Risk & Order Manager

void test_risk_blind_spot() {
    printf("test_risk_blind_spot\n");

    // This test demonstrates the exact scenario the old check() missed.
    // Position limit = 50,000. Filled = 40,000. Open buy orders = 15,000.
    // True exposure = 55,000 - already over the limit.
    // The old check saw only 40,000 and let more buys through.

    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    // Fill 40,000 shares
    tracker.apply_event(make_fill("AAPL", Side::Buy, 40'000, fp(175.00), "F1"));

    // Open buy order for 15,000 (pending, not yet filled)
    const auto pending = make_new_order("AAPL", Side::Buy, 15'000, fp(175.00), "O1");
    orders.apply_event(pending);

    // Old position-only check: 40,000 + 5,000 = 45,000 — incorrectly passes
    CHECK_EQ(risk.check("AAPL", Side::Buy, 5'000, fp(175.00), tracker),
             RiskResult::Pass);

    // Full check: true exposure = 55,000 - correctly blocks
    CHECK_EQ(risk.check("AAPL", Side::Buy, 5'000, fp(175.00), tracker, orders),
             RiskResult::FailPositionLimit);
}

void test_risk_pending_sell_cross() {
    printf("test_risk_pending_sell_cross\n");

    // This demonstrates the cross blind spot.
    //
    // Setup: filled +1,000, pending sell 500 → true_qty = +500
    //
    // Old check (position-only) sees net_qty = +1,000:
    //   sell 600 → projects to +400 → no cross detected (WRONG)
    //
    // New check sees true_qty = +500:
    //   sell 600 → projects to -100 → cross detected (CORRECT)

    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    tracker.apply_event(make_fill("TSLA", Side::Buy, 1'000, fp(250.00), "F1"));
    orders.apply_event(make_new_order("TSLA", Side::Sell, 500, fp(251.00), "O1"));

    // Old position-only check: net=+1000, sell 600 → +400, no cross — passes (blind spot)
    CHECK_EQ(risk.check("TSLA", Side::Sell, 600, fp(251.00), tracker),
             RiskResult::Pass);

    // Full check: true=+500, sell 600 → -100 — correctly blocked
    CHECK_EQ(risk.check("TSLA", Side::Sell, 600, fp(251.00), tracker, orders),
             RiskResult::FailPositionCross);

    // A smaller sell that stays positive passes fine
    CHECK_EQ(risk.check("TSLA", Side::Sell, 400, fp(251.00), tracker, orders),
             RiskResult::Pass);
}

void test_risk_open_orders_reduce_on_fill() {
    printf("test_risk_open_orders_reduce_on_fill\n");

    // Start near the limit with a mix of filled and pending.
    // After a fill lands, open exposure drops and a new order fits.

    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    // Fill 30,000, open buy 18,000 → true = 48,000 (close to limit of 50,000)
    tracker.apply_event(make_fill("MSFT", Side::Buy, 30'000, fp(330.00), "F1"));
    orders.apply_event(make_new_order("MSFT", Side::Buy, 18'000, fp(330.00), "O1"));

    // Only 2,000 headroom — a 3,000 order should fail
    CHECK_EQ(risk.check("MSFT", Side::Buy, 3'000, fp(330.00), tracker, orders),
             RiskResult::FailPositionLimit);

    // The open order fills — exposure stays the same in PositionTracker
    // but open_exposure in OrderManager drops to 0
    const auto fill = make_fill("MSFT", Side::Buy, 18'000, fp(330.00), "O1");
    tracker.apply_event(fill);
    orders.apply_event(fill);

    // Now filled = 48,000, pending = 0 → true = 48,000, same headroom
    // 3,000 still fails
    CHECK_EQ(risk.check("MSFT", Side::Buy, 3'000, fp(330.00), tracker, orders),
             RiskResult::FailPositionLimit);

    // But 2,000 passes
    CHECK_EQ(risk.check("MSFT", Side::Buy, 2'000, fp(330.00), tracker, orders),
             RiskResult::Pass);
}

void test_risk_cancelled_order_releases_exposure() {
    printf("test_risk_cancelled_order_releases_exposure\n");

    // Open order holds exposure. Cancelling it frees the headroom.

    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;

    // Fill 45,000, open buy 4,000 → true = 49,000
    tracker.apply_event(make_fill("SPY", Side::Buy, 45'000, fp(450.00), "F1"));
    orders.apply_event(make_new_order("SPY", Side::Buy, 4'000, fp(450.00), "O1"));

    // Only 1,000 headroom — 2,000 fails
    CHECK_EQ(risk.check("SPY", Side::Buy, 2'000, fp(450.00), tracker, orders),
             RiskResult::FailPositionLimit);

    // Cancel the open order — open_exposure drops back to 0
    OrderEvent cancel{};
    cancel.type = EventType::Cancel;
    strncpy(cancel.order_id, "O1", sizeof(cancel.order_id));
    orders.apply_event(cancel);

    // Now true = 45,000 — 5,000 headroom, 2,000 passes
    CHECK_EQ(risk.check("SPY", Side::Buy, 2'000, fp(450.00), tracker, orders),
             RiskResult::Pass);
}


//      Main Function


int main() {
    printf("\n=== HFT Order Book - Unit Tests ===\n\n");

    test_struct_sizes();
    test_make_fill();
    test_event_log();
    test_simple_long();
    test_partial_close();
    test_full_round_trip();
    test_mark_to_market();
    test_multi_symbol();
    test_short_position();
    test_replay_rebuilds_state();
    test_risk_checks();

    printf("\n--- OrderManager ---\n\n");

    test_order_new();
    test_order_partial_fill();
    test_order_full_fill();
    test_order_avg_fill_price();
    test_order_cancel();
    test_open_exposure();
    test_order_replay();

    printf("\n--- RiskChecker w/ OrderManager ---\n\n");

    test_risk_blind_spot();
    test_risk_pending_sell_cross();
    test_risk_open_orders_reduce_on_fill();
    test_risk_cancelled_order_releases_exposure();


    printf("\n%d / %d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
