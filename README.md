# HFT Bookkeeping Engine

A high-performance, low-latency order bookkeeping system for High-Frequency Trading written in C++17. 
Tracks positions, PnL, and order lifecycle in real time with sub-50ns hot-path updates, connected to live or 
simulated exchanges via a FIX 4.2 gateway.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Performance](#performance)
- [Project Structure](#project-structure)
- [Core Components](#core-components)
- [FIX Gateway](#fix-gateway)
- [Getting Started](#getting-started)
- [Running Tests](#running-tests)
- [Benchmarks](#benchmarks)
- [CI/CD](#cicd)

---

## Overview

This system solves the bookkeeping layer of an HFT stack — the component responsible for maintaining accurate position 
state and risk exposure across thousands of fills per second. Every design decision is driven by two constraints that 
are in direct tension: **speed** (sub-microsecond position updates) and **correctness** (every fill must reconcile 
perfectly, with no silent data loss across restarts).

**Key design principles:**

- **Event sourcing** — an append-only, memory-mapped event log is the single source of truth. All position and order 
state is a pure projection over this log. Crash recovery is a full replay.
- **Cache-line discipline** — all hot-path structs are exactly 64 bytes and cache-line aligned. No false sharing, no 
padding waste.
- **Zero allocation on the hot path** — no heap allocation in `apply_event()`, `check()`, or `mark_to_market()`. 
All state lives in flat arrays.
- **True exposure risk** — pre-trade risk checks operate on filled position _and_ open order exposure simultaneously, 
closing a blind spot that position-only checks leave open.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Trading Strategy                       │
└─────────────────────────┬───────────────────────────────────┘
                          │ send_order()
┌─────────────────────────▼───────────────────────────────────┐
│                      FIX Gateway                            │
│  ┌─────────────┐  ┌──────────────────┐  ┌────────────────┐  │
│  │ Session Mgr │  │ MessageTranslator│  │   Killswitch   │  │
│  │ Logon/Hbeat │  │  FIX ↔ OrderEvent│  │ cancel_all()   │  │
│  └─────────────┘  └──────────────────┘  └────────────────┘  │
└──────────────┬──────────────────────────────────────────────┘
               │ append() + apply_event() x2
┌──────────────▼──────────────────────────────────────────────┐
│                    Hot Path (single writer thread)          │
│                                                             │
│  ┌──────────────┐  ┌─────────────────┐  ┌────────────────┐  │
│  │   EventLog   │  │ PositionTracker │  │  OrderManager  │  │
│  │ mmap, atomic │  │ flat array proj │  │ lifecycle proj │  │
│  │ seq numbers  │  │ avg cost basis  │  │ open exposure  │  │
│  └──────────────┘  └─────────────────┘  └────────────────┘  │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              RiskChecker (inline, pre-trade)          │  │
│  │  max qty · max notional · position limit · max loss   │  │
│  │  fat-finger · position cross · open order exposure    │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────────┐
│                 Exchange / Simulator (FIX 4.2)              │
└─────────────────────────────────────────────────────────────┘
```

---

## Performance

Measured on a 2.8 GHz server using RDTSC hardware timing with 100,000 iterations per case:

| Operation | p50 | p99 | p999 |
|---|---|---|---|
| `EventLog::append()` | 53 ns | 1,359 ns* | 2,624 ns* |
| `PositionTracker::apply_event()` | 29 ns | 43 ns | 73 ns |
| `OrderManager::apply_event()` FullFill | 33 ns | 50 ns | 78 ns |
| `OrderManager::apply_event()` Cancel | 31 ns | 46 ns | 74 ns |
| `RiskChecker::check()` pass (full) | 28 ns | 38 ns | 63 ns |
| `RiskChecker::check()` FailMaxQty | 22 ns | 23 ns | 36 ns |
| `mark_to_market()` single symbol | 26 ns | 36 ns | 55 ns |
| `open_exposure()` 10 open orders | 46 ns | 82 ns | 120 ns |
| Full hot path (risk + append + 2× apply) | 75 ns | 1,472 ns* | 2,999 ns* |
| Replay 100k events (startup) | — | — | ~2 ms |

\* p99 spikes on `append()` are OS page writeback to the mmap file, not code latency. On a RAM-backed tmpfs or tuned 
kernel this tightens significantly. On bare-metal production hardware with `MADV_SEQUENTIAL` and pre-faulted pages, 
the p99 is expected under 200 ns.

> **Target** — p99 > 1 µs indicates cache pressure or OS jitter and should be investigated with `perf`.

---

## Project Structure

```
hft_bookkeeping/
│
├── include/                       # Core bookkeeping layer — header only, no external deps
│   ├── OrderEvent.h               # 64-byte cache-line aligned event struct
│   ├── EventLog.h                 # Append-only memory-mapped event log
│   ├── PositionTracker.h          # Position projection over the event stream
│   ├── OrderManager.h             # Order lifecycle projection
│   └── RiskChecker.h              # Inline pre-trade risk checks
│
├── gateway/                       # FIX gateway — requires QuickFIX
│   ├── FIXGateway.h / .cpp        # QuickFIX Application subclass
│   ├── MessageTranslator.h        # FIX tag-value pairs ↔ OrderEvent (pure, no session)
│   └── Killswitch.h               # cancel_all() + flatten() with time budget
│
├── src/
│   └── main.cpp                   # Entry point — wires components, starts FIX initiator
│
├── config/
│   ├── simulator.cfg              # Local ordermatch simulator (localhost:9878)
│   ├── uat.cfg                    # Broker sandbox template
│   └── production.cfg             # Live session template (no credentials) (Optional)
│
├── tests/
│   ├── test_OrderEvent.cpp        # Struct layout, factory helpers
│   ├── test_EventLog.cpp          # Append, replay, durability
│   ├── test_PositionTracker.cpp   # Long/short/PnL/mark-to-market
│   ├── test_OrderManager.cpp      # Order lifecycle, open_exposure
│   ├── test_RiskChecker.cpp       # All fail cases + blind spot scenarios
│   ├── test_MessageTranslator.cpp # FIX tag → OrderEvent conversion
│   ├── test_FIXGateway.cpp        # Full callback chain, no network required
│   ├── test_Killswitch.cpp        # cancel_all, flatten, arm/execute
│   └── fixtures/
│       └── sample_msgs.h          # Pre-built FIX42::ExecutionReport objects
│
├── benchmarks/
│   ├── bench_harness.h            # BENCH_CASE/RUN macros, registry, percentile stats
│   └── bench_main.cpp             # 17 registered benchmark cases
│
├── Dockerfile                     # Development + CI environment
├── CMakeLists.txt
├── .github/workflows/ci.yml
└── .gitignore                     # fix_store/ fix_logs/ *.bin
```

---

## Core Components

### OrderEvent

The canonical 64-byte (one cache line) event struct written directly into the memory-mapped log. Every state change in 
the system — new order, partial fill, full fill, cancel, reject, amend — is represented as an `OrderEvent`.

```cpp
struct OrderEvent {              // exactly 64 bytes
    uint64_t  sequence;          // monotonic, gapless
    uint64_t  timestamp_ns;      // hardware clock (RDTSC / clock_gettime)
    int64_t   price;             // fixed-point: actual_price × 1,000,000
    int64_t   fill_price;        // fixed-point; 0 if not a fill
    int32_t   qty;
    int32_t   fill_qty;
    char      symbol[8];
    char      order_id[8];
    EventType type;              // NewOrder | PartialFill | FullFill | Cancel | Reject | Amend
    Side      side;              // Buy | Sell
    uint8_t   _pad[6];
};
```

> All prices are stored as `int64_t` fixed-point (`price × 1,000,000`). Floating-point is never used in any 
> calculation — only for display.

---

### EventLog

An append-only, memory-mapped event log. The single source of truth for all state. Every other component is a 
projection over this log.

```
File layout:
┌──────────────────────────┐
│  LogHeader (64 bytes)    │  magic | next_sequence | padding
├──────────────────────────┤
│  OrderEvent[0] (64 bytes)│
│  OrderEvent[1] (64 bytes)│
│  ...                     │
│  OrderEvent[N] (64 bytes)│
└──────────────────────────┘
Capacity: 10,000,000 events (~640 MB)
```

- Writes are appends only — never updates
- `next_sequence` is atomic, enabling concurrent reads with a single writer
- On restart, replay from sequence 0 to rebuild all in-memory state
- Sequence gaps are impossible by construction

---

### PositionTracker

Projects the event stream into live per-symbol positions, indexed by symbol in a flat cache-line aligned array.

```
EventLog  ──replay──►  PositionTracker
                            │
                    ┌───────▼────────┐
                    │ symbol: "AAPL" │
                    │ net_qty: +500  │
                    │ avg_px: 175.42 │
                    │ realized_pnl   │
                    │ unrealized_pnl │
                    └────────────────┘
```

Supports longs, shorts, partial closes, position crosses, weighted average cost basis, and real-time `mark_to_market()` 
on every market data tick.

---

### OrderManager

Projects the event stream into live per-order state, indexed by `order_id`. The equivalent of `PositionTracker` for 
order lifecycle.

```
NewOrder ──► Pending ──► PartiallyFilled ──► Filled      (terminal)
                  │              └──────────► Cancelled  (terminal)
                  └──────────────────────────► Rejected  (terminal)
```

`open_exposure(symbol)` returns the net quantity of all live orders for a symbol — used by `RiskChecker` to close the 
open-order blind spot.

---

### RiskChecker

All checks run inline on the hot path — no heap allocation, no system calls. Target: under 200 ns per check.

| Check | Description |
|---|---|
| `FailMaxQty` | Single order quantity exceeds fat-finger limit |
| `FailMaxNotional` | Single order notional value (qty × price) too large |
| `FailPositionLimit` | Would breach max net position including open orders |
| `FailPositionCross` | Would flip long↔short in one order |
| `FailMaxLoss` | Cumulative realized loss exceeds threshold |

**The open-order blind spot** — the primary overload takes both `PositionTracker` and `OrderManager` to compute true 
exposure:

```
true_exposure = net_qty (filled)  +  open_exposure (pending at exchange)
```

Without the second term, a limit of 50,000 shares with 40,000 filled and 15,000 pending appears to have 10,000 headroom 
when it actually has none.

---

## FIX Gateway

Connects the bookkeeping core to a real or simulated exchange via FIX 4.2 using QuickFIX.

### MessageTranslator

Pure conversion layer with no session dependency — every method is a static function. Fully unit-testable without a 
live session.

```
FIX42::ExecutionReport  ──►  OrderEvent  ──►  EventLog + Trackers
OrderEvent              ──►  FIX42::NewOrderSingle  ──►  Exchange
```

### Killswitch

Hardened emergency shutdown. Safely callable from any thread or signal handler.

```cpp
Killswitch ks(orders, tracker, session_id);
ks.arm();          // set from SIGINT/SIGTERM handler — non-blocking

// On next strategy loop iteration:
if (ks.is_armed()) {
    ks.execute();  // cancel_all() with time budget, then flatten()
}
```

Both `cancel_all()` and `flatten()` catch `SessionNotFound` internally and continue — a dropped session mid-killswitch 
does not abort the remaining cancels.

### Connecting to the local simulator

```bash
# Terminal 1 — start the ordermatch simulator (from QuickFIX examples)
./ordermatch ordermatch.cfg

# Terminal 2 — start the gateway
./run_gateway config/simulator.cfg
```

---

## Getting Started

### Prerequisites

- Docker (recommended — zero host setup)
- Or: `build-essential`, `cmake`, `libgtest-dev`, QuickFIX built from master

### Docker setup (recommended)

```bash
# Build the development image (installs all dependencies including QuickFIX from source)
docker build -t hft-clion .

# Start the container with SSH for CLion remote development
docker run -d --cap-add sys_ptrace \
    -p 127.0.0.1:2222:22 \
    --restart unless-stopped \
    --name hft_dev \
    hft-clion
```

### CLion Docker toolchain

1. **Settings → Build, Execution, Deployment → Toolchains** → `+` → Remote Host
2. Name: `hft-docker`, Credentials: `localhost:2222`, user: `user`, password: `password`
3. Click **Test Connection**
4. **Settings → CMake** → add profile `Debug-Docker` using the `hft-docker` toolchain

### Building natively

```bash
# Install QuickFIX from master (C++17 compatible)
git clone https://github.com/quickfix/quickfix.git
cmake -B quickfix/build quickfix -DHAVE_SSL=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build quickfix/build -j$(nproc)
sudo cmake --install quickfix/build

# Build the project
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

---

## Running Tests

Tests are split into two suites — bookkeeping (no external dependencies) and gateway (requires QuickFIX).

```bash
# All tests via CTest
ctest --test-dir build --output-on-failure

# Individual suite
ctest --test-dir build --output-on-failure -R "test_Position"
ctest --test-dir build --output-on-failure -R "test_FIXGateway"

# Full run inside Docker (as used in CI)
docker run --rm hft-test:latest bash -c "
    cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja &&
    cmake --build build &&
    ctest --test-dir build --output-on-failure
"
```

### Test suites

| Suite | Tests | Covers |
|---|---|---|
| `test_OrderEvent` | Struct layout, factory helpers, fixed-point | `OrderEvent.h` |
| `test_EventLog` | Append, replay, durability across restarts | `EventLog.h` |
| `test_PositionTracker` | Long/short, partial close, PnL, mark-to-market | `PositionTracker.h` |
| `test_OrderManager` | Full lifecycle, avg fill price, open exposure | `OrderManager.h` |
| `test_RiskChecker` | All 5 fail cases + 4 blind spot scenarios | `RiskChecker.h` |
| `test_MessageTranslator` | FIX tag → OrderEvent for all ExecTypes | `MessageTranslator.h` |
| `test_FIXGateway` | Full callback chain into bookkeeping, no network | `FIXGateway.h/.cpp` |
| `test_Killswitch` | arm/execute, cancel_all, flatten | `Killswitch.h` |

**78 tests, all passing.**

---

## Benchmarks

The benchmark harness uses RDTSC hardware timing for sub-nanosecond precision and a macro-based auto-registration 
system — adding a new benchmark requires only a `BENCH_CASE` block, no changes to the runner.

```bash
# Build and run all 17 cases
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target run_bench
./build/run_bench

# Run a single group
./build/run_bench OrderManager
./build/run_bench RiskChecker
./build/run_bench FullHotPath
```

Adding a new benchmark:

```cpp
BENCH_CASE("MyComponent", "my operation", 100'000) {
    MyFixture fixture;        // setup — runs once, not timed
    RUN {
        do_not_optimise(fixture.hot_fn());   // timed
    }
}
```

---

## CI/CD

GitHub Actions runs two parallel jobs on every push and pull request:

```
push / pull_request
       │
       ├── bookkeeping (ubuntu-latest, ~30s)
       │     apt-get install cmake libgtest-dev
       │     cmake + build bookkeeping targets
       │     ctest -R "test_Order|test_Event|test_Position|test_Risk"
       │
       ├── gateway (ubuntu-latest, ~2min first run / ~30s cached)
       │     Docker Buildx with layer cache keyed on Dockerfile hash
       │     docker run bash -c "cmake + build gateway targets + ctest"
       │
       └── bench smoke (ubuntu-latest, needs: bookkeeping)
             cmake Release + build run_bench
             ./run_bench — verify exit 0
```

The Docker layer cache means QuickFIX is only compiled from source when the `Dockerfile` changes — every other push 
hits the cache and the gateway job runs in well under a minute.

---

## Design Notes

**Why header-only for the core?** The bookkeeping layer (`include/`) has no external dependencies. Every hot-path 
function needs to be visible to the compiler at the call site for inlining. Header-only guarantees inlining without 
relying on LTO. The gateway (`gateway/`) has a `.cpp` because QuickFIX headers are expensive to compile and should only 
be paid by targets that actually need them.

**Why fixed-point arithmetic?** Floating-point rounding silently corrupts PnL at scale. A fill at $175.001 repeated 
10,000 times accumulates error. `int64_t` with a scale of 1,000,000 gives six decimal places of precision with exact 
arithmetic.

**Why two trackers?** `PositionTracker` answers "what do I own" (backward-looking, fills only). `OrderManager` answers 
"what have I asked the exchange to do" (forward-looking, all event types). The gap between them — orders live at the 
exchange but not yet filled — is where risk blind spots live. Both are required for correct pre-trade risk checks.
