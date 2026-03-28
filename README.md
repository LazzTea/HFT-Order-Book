
OrderEvent.h -- the canonical 64-byte (exactly one cache line) event struct with fixed-point prices 
(int64_t * PRICE_SCALE), typed enums for EventType and Side, and factory helpers make_fill() / make_new_order(). This 
struct is written directly to disk -- no virtual methods, no heap.

EventLog.h -- memory-mapped append-only log. 
Opens or creates a file, writes events into mmap'd pages, keeps a durable sequence counter in the file header so you 
can recover exact state on restart. A single std::atomic<uint64_t> in memory handles the reader/writer ordering without 
putting an atomic inside the mmap'd struct (a subtle correctness trap).

PositionTracker.h -- projects the event stream into live Position slots (also 64 bytes each). Handles longs, shorts, 
partial closes, position crosses, and weighted average cost basis. replay_from(log) rebuilds full state from any 
EventLog on startup.

RiskChecker.h -- inline pre-trade checks (max qty, max notional, position limit, max loss, position cross guard) with no 
allocation and no system calls. Designed to run in under 200ns.

OrderManager.h -- Similar to PositionTracker, instead relies on Order ID for slots. Handles exposure and state
transitions while remaining invisible to PositionTracker.

To build (bash):

`mkdir build && cd build`

`cmake .. -DCMAKE_BUILD_TYPE=Debug`

`make && ./run_tests`

TODO:
- [ ] FIX Gateway (connecting a real order flow into EventLog::append)
- [ ] FIFO lot tracker for precise tax-lot realized PnL
- [ ] Reconciliation job that diffs log against exchange drop copies