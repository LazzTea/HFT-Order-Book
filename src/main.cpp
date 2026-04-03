#include "EventLog.h"
#include "PositionTracker.h"
#include "OrderManager.h"
#include "RiskChecker.h"
#include "FIXGateway.h"

#include <quickfix/FileStore.h>
#include <quickfix/FileLog.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <thread>
#include <chrono>

using namespace hft;
using namespace hft::gateway;

// ── Signal handling ───────────────────────────────────────────────────────────

static std::atomic<bool>      g_shutdown{false};
static FIXGateway*            g_gateway = nullptr;

static void signal_handler(int sig) {
    printf("\n[MAIN] signal %d received — initiating shutdown\n", sig);
    g_shutdown.store(true, std::memory_order_release);
    if (g_gateway) g_gateway->emergency_cancel_all();
}

// ── Usage ─────────────────────────────────────────────────────────────────────

static void print_usage(const char* argv0) {
    printf("Usage: %s <config_file> [session_log_path]\n", argv0);
    printf("  config_file    path to QuickFIX .cfg (e.g. config/simulator.cfg)\n");
    printf("  session_log_path  optional override for FileStorePath\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const std::string cfg_path = argv[1];

    // ── Bookkeeping layer ─────────────────────────────────────────────────

    EventLog        log("session.bin");
    PositionTracker tracker;
    OrderManager    orders;
    RiskChecker     risk;

    // Rebuild state from any prior session before connecting.
    // If the log is empty this is a no-op; if it has events from a crash
    // or restart, positions and open orders are reconstructed correctly.
    printf("[MAIN] replaying event log (%llu events)...\n",
           (unsigned long long)log.size());
    tracker.replay_from(log);
    orders.replay_from(log);
    printf("[MAIN] replay complete — %zu symbols, %zu orders\n",
           tracker.symbol_count(), orders.order_count());

    // ── FIX gateway ───────────────────────────────────────────────────────

    FIXGateway gateway(log, tracker, orders, risk);
    g_gateway = &gateway;

    // Register signal handlers after g_gateway is set
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        FIX::SessionSettings  settings(cfg_path);
        FIX::FileStoreFactory store(settings);
        FIX::FileLogFactory   logger(settings);
        FIX::SocketInitiator  initiator(gateway, store, settings, logger);

        initiator.start();
        printf("[MAIN] FIX initiator started — waiting for logon\n");

        // ── Strategy loop ─────────────────────────────────────────────────
        // Replace this with your actual strategy logic.
        // The loop checks for shutdown on every iteration.

        while (!g_shutdown.load(std::memory_order_acquire)) {
            // Example: send a test order once connected
            // if (gateway.is_connected()) {
            //     gateway.send_order("AAPL", Side::Buy, 100, 175.00);
            // }

            // Check killswitch arm flag (set asynchronously by signal handler)
            // In production you'd also check risk limits, market data, etc.

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        printf("[MAIN] shutdown signal received — stopping initiator\n");
        initiator.stop();

    } catch (const FIX::ConfigError& e) {
        printf("[MAIN] FIX config error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        printf("[MAIN] fatal: %s\n", e.what());
        return 1;
    }

    printf("[MAIN] clean shutdown complete\n");
    return 0;
}
