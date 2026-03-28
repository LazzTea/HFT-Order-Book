// bench_main.cpp
// ──────────────
// Benchmark definitions for the HFT bookkeeping hot path.
//
// To add a new benchmark:
//   1. Pick or create a group name (used for section headers).
//   2. Write a BENCH_CASE block — setup outside RUN, timed code inside RUN.
//   3. Done. The registry picks it up automatically.
//
// To run a single group:
//   ./run_bench EventLog
//   ./run_bench OrderManager
//
// Build:
//   g++ -std=c++17 -O3 -march=native -Iinclude bench/bench_main.cpp -o run_bench

#include "../OrderEvent.h"
#include "../EventLog.h"
#include "../PositionTracker.h"
#include "../OrderManager.h"
#include "../RiskChecker.h"
#include "bench_harness.h"

#include <cstring>
#include <unistd.h>

using namespace hft;

//      Event Log

BENCH_CASE("EventLog", "append() — mmap write", 100'000) {
    const auto path = "/tmp/hft_bench_log.bin";
    unlink(path);
    EventLog log(path);

    // Pre-touch pages to avoid page-fault spikes in the timed loop
    const auto warmup = make_fill("AAPL", Side::Buy, 1, fp(175.00), "W");
    for (size_t i = 0; i < 1000; ++i) log.append(warmup);

    const auto evt = make_fill("AAPL", Side::Buy, 100, fp(175.00), "O");
    RUN {
        do_not_optimise(log.append(evt));
    }
    unlink(path);
}

//      Position Tracker

BENCH_CASE("PositionTracker", "apply_event() buy — warm cache", 100'000) {
    PositionTracker tracker;
    tracker.apply_event(make_fill("AAPL", Side::Buy, 1, fp(175.00), "W"));
    const auto evt = make_fill("AAPL", Side::Buy, 100, fp(175.00), "O");
    RUN {
        tracker.apply_event(evt);
        do_not_optimise(tracker.get("AAPL")->net_qty);
    }
}

BENCH_CASE("PositionTracker", "apply_event() alternating buy/sell", 100'000) {
    PositionTracker tracker;
    tracker.apply_event(make_fill("AAPL", Side::Buy, 1, fp(175.00), "W"));
    RUN {
        const Side side = (_i % 2 == 0) ? Side::Buy : Side::Sell;
        const auto evt  = make_fill("AAPL", side, 100, fp(175.00), "O");
        tracker.apply_event(evt);
        do_not_optimise(tracker.get("AAPL")->net_qty);
    }
}

BENCH_CASE("PositionTracker", "mark_to_market() single symbol", 100'000) {
    PositionTracker tracker;
    const char* syms[] = {"AAPL", "TSLA", "MSFT", "GOOGL", "AMZN"};
    for (const auto* s : syms)
        tracker.apply_event(make_fill(s, Side::Buy, 1'000, fp(200.00), "W"));
    int64_t px = fp(200.00);
    RUN {
        px += (_i % 2 == 0) ? 1 : -1;
        tracker.mark_to_market("AAPL", px);
        do_not_optimise(tracker.get("AAPL")->unrealized_pnl);
    }
}

BENCH_CASE("PositionTracker", "get() — symbol lookup hit", 100'000) {
    PositionTracker tracker;
    tracker.apply_event(make_fill("AAPL", Side::Buy, 100, fp(175.00), "W"));
    RUN {
        do_not_optimise(tracker.get("AAPL"));
    }
}

//      Order Manager

BENCH_CASE("OrderManager", "apply_event() NewOrder", 100'000) {
    // Each iteration creates a new order — tests get_or_create path.
    // We reset the manager each batch to avoid hitting MAX_ORDERS.
    OrderManager orders;
    char oid[8];
    RUN {
        // Rotate through 256 order IDs to keep the slot count bounded
        snprintf(oid, sizeof(oid), "O%05zu", _i % 256);
        orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), oid));
        do_not_optimise(orders.get(oid));
    }
}

BENCH_CASE("OrderManager", "apply_event() FullFill — existing order", 100'000) {
    // Tests the fill path on a known order — no get_or_create, just find + update.
    OrderManager orders;
    orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
    const auto fill = make_fill("AAPL", Side::Buy, 100, fp(175.00), "O1");
    RUN {
        // Re-open the order each iteration so fill always has work to do
        orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
        orders.apply_event(fill);
        do_not_optimise(orders.get("O1")->state);
    }
}

BENCH_CASE("OrderManager", "apply_event() PartialFill — avg price update", 100'000) {
    // Weighted average fill price recalculation on every partial.
    OrderManager orders;
    orders.apply_event(make_new_order("AAPL", Side::Buy, 10'000, fp(175.00), "O1"));
    RUN {
        // Simulate a stream of small partials at varying prices
        const int64_t price = fp(175.00) + static_cast<int64_t>(_i % 100) * 1000;
        const auto partial  = make_fill("AAPL", Side::Buy, 10, price, "O1", "", true);
        orders.apply_event(partial);
        do_not_optimise(orders.get("O1")->avg_fill_price);
    }
}

BENCH_CASE("OrderManager", "apply_event() Cancel", 100'000) {
    // Cancel path: find order, flip state. Simplest handler.
    OrderManager orders;
    OrderEvent cancel{};
    cancel.type = EventType::Cancel;
    strncpy(cancel.order_id, "O1", sizeof(cancel.order_id));
    RUN {
        // Re-open each iteration so cancel always has a live order to close
        orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), "O1"));
        orders.apply_event(cancel);
        do_not_optimise(orders.get("O1")->state);
    }
}

BENCH_CASE("OrderManager", "open_exposure() — 10 open orders", 100'000) {
    // open_exposure() scans all slots linearly — cost grows with open order count.
    // 10 open orders is realistic for a single-symbol strategy.
    OrderManager orders;
    for (int i = 0; i < 10; ++i) {
        char oid[8];
        snprintf(oid, sizeof(oid), "O%d", i);
        orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), oid));
    }
    RUN {
        do_not_optimise(orders.open_exposure("AAPL"));
    }
}

BENCH_CASE("OrderManager", "open_exposure() — 50 open orders", 100'000) {
    // Same as above but with 50 open orders — shows linear scan cost.
    OrderManager orders;
    for (int i = 0; i < 50; ++i) {
        char oid[8];
        snprintf(oid, sizeof(oid), "O%d", i);
        orders.apply_event(make_new_order("AAPL", Side::Buy, 100, fp(175.00), oid));
    }
    RUN {
        do_not_optimise(orders.open_exposure("AAPL"));
    }
}

//      Risk Checker

BENCH_CASE("RiskChecker", "check() pass — position-only (legacy)", 100'000) {
    RiskChecker risk;
    PositionTracker tracker;
    tracker.apply_event(make_fill("AAPL", Side::Buy, 20'000, fp(175.00), "F1"));
    RUN {
        do_not_optimise(risk.check("AAPL", Side::Buy, 100, fp(175.00), tracker));
    }
}

BENCH_CASE("RiskChecker", "check() pass — full (tracker + orders)", 100'000) {
    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;
    tracker.apply_event(make_fill("AAPL", Side::Buy, 20'000, fp(175.00), "F1"));
    orders.apply_event(make_new_order("AAPL", Side::Buy, 5'000, fp(175.00), "O1"));
    RUN {
        do_not_optimise(risk.check("AAPL", Side::Buy, 100, fp(175.00), tracker, orders));
    }
}

BENCH_CASE("RiskChecker", "check() FailPositionLimit — full", 100'000) {
    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;
    tracker.apply_event(make_fill("AAPL", Side::Buy, 40'000, fp(175.00), "F1"));
    orders.apply_event(make_new_order("AAPL", Side::Buy, 15'000, fp(175.00), "O1"));
    RUN {
        // true_qty = 55,000 - over the 50,000 limit
        do_not_optimise(risk.check("AAPL", Side::Buy, 100, fp(175.00), tracker, orders));
    }
}

BENCH_CASE("RiskChecker", "check() FailMaxQty — early exit", 100'000) {
    RiskChecker risk;
    PositionTracker tracker;
    OrderManager orders;
    RUN {
        // Fails on the first check - no position lookup at all
        do_not_optimise(risk.check("AAPL", Side::Buy, 50'000, fp(175.00), tracker, orders));
    }
}

//      Full Hot Path

BENCH_CASE("FullHotPath", "append + apply_event x2 (no risk)", 100'000) {
    const auto path = "/tmp/hft_bench_full.bin";
    unlink(path);
    EventLog log(path);
    PositionTracker tracker;
    OrderManager orders;

    // Warm up
    const auto w = make_fill("AAPL", Side::Buy, 1, fp(175.00), "W");
    log.append(w); tracker.apply_event(w); orders.apply_event(w);

    RUN {
        const Side side = (_i % 2 == 0) ? Side::Buy : Side::Sell;
        const auto evt  = make_fill("AAPL", side, 100, fp(175.00), "O");
        log.append(evt);
        tracker.apply_event(evt);
        orders.apply_event(evt);
        do_not_optimise(tracker.get("AAPL")->net_qty);
    }
    unlink(path);
}

BENCH_CASE("FullHotPath", "risk check + append + apply_event x2", 100'000) {
    const auto path = "/tmp/hft_bench_full2.bin";
    unlink(path);
    EventLog log(path);
    PositionTracker tracker;
    OrderManager orders;
    RiskChecker risk;

    const auto w = make_fill("AAPL", Side::Buy, 1, fp(175.00), "W");
    log.append(w); tracker.apply_event(w); orders.apply_event(w);

    RUN {
        const Side side = (_i % 2 == 0) ? Side::Buy : Side::Sell;
        const auto result = risk.check("AAPL", side, 100, fp(175.00), tracker, orders);
        if (result == RiskResult::Pass) {
            const auto evt = make_fill("AAPL", side, 100, fp(175.00), "O");
            log.append(evt);
            tracker.apply_event(evt);
            orders.apply_event(evt);
        }
        do_not_optimise(result);
    }
    unlink(path);
}

//      Replay
// A one-shot measurement (not a loop), so it doesn't use BENCH_CASE.
// It's reported separately at the end of main().

static void run_replay_bench(const size_t event_count) {
    printf("\n[Replay — startup cost]\n");

    const auto path = "/tmp/hft_bench_replay.bin";
    unlink(path);
    {
        EventLog log(path);
        for (size_t i = 0; i < event_count; ++i) {
            const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
            log.append(make_fill("AAPL", side, 100, fp(175.00), "O"));
        }
    }

    const EventLog log(path);
    PositionTracker tracker;
    OrderManager orders;

    const uint64_t t0 = rdtsc();
    tracker.replay_from(log);
    const uint64_t t1 = rdtsc();

    const uint64_t t2 = rdtsc();
    orders.replay_from(log);
    const uint64_t t3 = rdtsc();

    const uint64_t pos_ns = cycles_to_ns(t1 - t0);
    const uint64_t ord_ns = cycles_to_ns(t3 - t2);

    printf("    Events:                   %zu\n",          event_count);
    printf("    PositionTracker rebuild:  %llu ms  (%llu ns/event)\n",
           static_cast<unsigned long long>(pos_ns / 1'000'000),
           static_cast<unsigned long long>(pos_ns / event_count));
    printf("    OrderManager rebuild:     %llu ms  (%llu ns/event)\n",
           static_cast<unsigned long long>(ord_ns / 1'000'000),
           static_cast<unsigned long long>(ord_ns / event_count));

    unlink(path);
}

//      Main

int main(const int argc, const char* argv[]) {
    printf("=== HFT bookkeeping — latency benchmark ===\n");
    printf("Measuring TSC frequency...\n");
    g_tsc_ghz = measure_tsc_ghz();
    printf("TSC: %.3f GHz\n", g_tsc_ghz);

    const auto& reg = BenchRegistry::get();

    if (argc > 1) {
        printf("\nRunning group: %s\n", argv[1]);
        reg.run_group(argv[1]);
    } else {
        printf("\nRunning all %zu cases...\n",  reg.cases().size());
        reg.run_all();
        run_replay_bench(100'000);
    }

    printf("\n=== Targets (bare-metal server) ===\n");
    printf("    append():                      <  100ns\n");
    printf("    apply_event() PositionTracker: <   50ns\n");
    printf("    apply_event() OrderManager:    <   50ns\n");
    printf("    open_exposure() 10 orders:     <   50ns\n");
    printf("    check() pass (full):           <  150ns\n");
    printf("    full hot path (with risk):     <  300ns\n");
    printf("    replay 100k events:            <   50ms\n");
    printf("\n    p99 > 1us = cache pressure or OS jitter.\n");

    return 0;
}
