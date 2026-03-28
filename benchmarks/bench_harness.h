#pragma once

// Adding a new benchmark
// 1. Write a BENCH_CASE block anywhere in bench_main.cpp.
// 2. That's it — the registry picks it up automatically.
//
// Example:
//   BENCH_CASE("MyGroup", "description", 100'000) {
//       MyFixture f;        // setup — runs once, not timed
//       RUN {
//           do_not_optimise(f.hot_fn());   // timed
//       }
//   }
//
// Run one group:   ./run_bench OrderManager
// Run all:         ./run_bench

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numeric>
#include <string>
#include <vector>
#include <ctime>

//      TSC Timings

static uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline double g_tsc_ghz = 1.0;

static uint64_t cycles_to_ns(const uint64_t cycles) {
    return static_cast<uint64_t>(static_cast<double>(cycles) / g_tsc_ghz);
}

inline double measure_tsc_ghz() {
    timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const uint64_t c0 = rdtsc();
    timespec target = t0;
    target.tv_nsec += 10'000'000;
    if (target.tv_nsec >= 1'000'000'000) { target.tv_sec++; target.tv_nsec -= 1'000'000'000; }
    while (true) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec > target.tv_sec ||
           (t1.tv_sec == target.tv_sec && t1.tv_nsec >= target.tv_nsec)) break;
    }
    const uint64_t c1 = rdtsc();
    return static_cast<double>(c1 - c0) /
           ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec));
}

//      Dead-Code-Guard

template<typename T>
static void do_not_optimise(const T& val) {
    __asm__ __volatile__("" : : "r,m"(val) : "memory");
}

//      fixed-point helper

static int64_t fp(const double p) {
    return static_cast<int64_t>(p * 1'000'000LL);
}

//      LatencyStats

class LatencyStats {
public:
    explicit LatencyStats(const size_t cap) { samples_.reserve(cap); }

    void record(const uint64_t ns) { samples_.push_back(ns); }

    void print(const char* label) const {
        if (samples_.empty()) { printf("    %-40s  no samples\n", label); return; }

        std::vector<uint64_t> s = samples_;
        std::sort(s.begin(), s.end());
        const size_t n   = s.size();
        const double avg = static_cast<double>(
            std::accumulate(s.begin(), s.end(), uint64_t{0})) / static_cast<double>(n);

        printf("    %-42s  n=%zu\n", label, n);
        printf("      min=%5lluns  avg=%6.1fns  p50=%5lluns"
               "  p90=%5lluns  p99=%6lluns  p999=%7lluns  max=%7lluns\n",
               static_cast<unsigned long long>(s[0]), avg,
               static_cast<unsigned long long>(s[n * 50 / 100]),
               static_cast<unsigned long long>(s[n * 90 / 100]),
               static_cast<unsigned long long>(s[n * 99 / 100]),
               static_cast<unsigned long long>(s[n * 999 / 1000]),
               static_cast<unsigned long long>(s[n - 1]));
    }

private:
    std::vector<uint64_t> samples_;
};

//      BenchRegistry

struct BenchCase {
    const char* group;
    const char* name;
    size_t      iterations;
    std::function<void(LatencyStats&, size_t)> fn;
};

struct BenchRegistry {
    static BenchRegistry& get() { static BenchRegistry r; return r; }

    void add(BenchCase c) { cases_.push_back(std::move(c)); }

    void run_all() const {
        const char* cur = nullptr;
        for (const auto& c : cases_) {
            if (!cur || std::string(c.group) != cur) {
                cur = c.group;
                printf("\n[%s]\n", cur);
            }
            LatencyStats stats(c.iterations);
            c.fn(stats, c.iterations);
            stats.print(c.name);
        }
    }

    void run_group(const char* prefix) const {
        bool found = false;
        const char* cur = nullptr;
        for (const auto& c : cases_) {
            if (std::string(c.group).find(prefix) != 0) continue;
            if (!cur || std::string(c.group) != cur) {
                cur = c.group;
                printf("\n[%s]\n", cur);
            }
            found = true;
            LatencyStats stats(c.iterations);
            c.fn(stats, c.iterations);
            stats.print(c.name);
        }
        if (!found) printf("No benchmarks matching: %s\n", prefix);
    }

    [[nodiscard]] const std::vector<BenchCase>& cases() const { return cases_; }

private:
    std::vector<BenchCase> cases_;
};

struct BenchAutoRegister {
    BenchAutoRegister(const char* g, const char* n, size_t i,
                      std::function<void(LatencyStats&, size_t)> fn) {
        BenchRegistry::get().add({g, n, i, std::move(fn)});
    }
};

//      BENCH_CASE / RUN macros
//
// Unique symbol generation: ## suppresses macro expansion of its operands,
// so passing __COUNTER__ directly to ## gives literal "_bench___COUNTER__".
// The fix is two levels of indirection — BENCH_PASTE forces __LINE__ to expand
// to its integer value before BENCH_PASTE_IMPL does the actual concatenation.
// One BENCH_CASE per source line is all we ever need.

#define BENCH_PASTE_IMPL(a, b) a##b
#define BENCH_PASTE(a, b)      BENCH_PASTE_IMPL(a, b)

#define BENCH_CASE(group_, name_, iters_)                                         \
    static void BENCH_PASTE(_bench_body_, __LINE__)(LatencyStats&, size_t);       \
    static BenchAutoRegister BENCH_PASTE(_bench_reg_, __LINE__)(                  \
        group_, name_, iters_,                                                    \
        [](LatencyStats& s, size_t n) {                                           \
            BENCH_PASTE(_bench_body_, __LINE__)(s, n);                            \
        });                                                                       \
    static void BENCH_PASTE(_bench_body_, __LINE__)(LatencyStats& _stats,         \
                                                     size_t        _iters)

// RUN: wraps the hot loop. _i = current iteration index (0-based).
#define RUN                                                              \
    for (size_t _i = 0; _i < _iters; ++_i)                              \
        if (const uint64_t _t0 = rdtsc(); true)                          \
            for (bool _done = false; !_done; _done = true,               \
                 _stats.record(cycles_to_ns(rdtsc() - _t0)))
