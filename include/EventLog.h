#pragma once
#include "OrderEvent.h"

#include <atomic>
// #include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>

namespace hft {

// Plain POD header at byte 0 of the file.
// next_sequence is maintained here for durability across restarts.
struct LogHeader {
    uint64_t magic;           // 0xBAADF00D1337CAFE
    uint64_t next_sequence;   // plain uint64; we manage ordering ourselves
    uint64_t _pad[6];         // pad to 64 bytes
};
static_assert(sizeof(LogHeader) == 64, "LogHeader must be 64 bytes");

static constexpr uint64_t LOG_MAGIC  = 0xBAADF00D1337CAFEULL;
static constexpr size_t   MAX_EVENTS = 10'000'000;
static constexpr size_t   FILE_SIZE  = sizeof(LogHeader) + MAX_EVENTS * sizeof(OrderEvent);

// Append-only, memory-mapped event log.
//
// Single-writer, multi-reader. The writer calls append() from one thread.
// Readers call read() / replay() concurrently — they load next_seq_ with
// acquire semantics to get a consistent view.
//
// On restart: reopen the same file; the header's next_sequence tells you
// where to resume. Call replay() to rebuild in-memory position state.
class EventLog {
public:
    explicit EventLog(const std::string_view path) : path_(path) { open_or_create(); }

    ~EventLog() {
        if (mmap_base_ != MAP_FAILED) munmap(mmap_base_, FILE_SIZE);
        if (fd_ >= 0) close(fd_);
    }

    EventLog(const EventLog&)            = delete;
    EventLog& operator=(const EventLog&) = delete;

    // Append one event. Assigns sequence + timestamp. Returns sequence number.
    // Must be called from the single writer thread only.
    uint64_t append(OrderEvent evt) {
        const uint64_t seq = next_seq_.load(std::memory_order_relaxed);
        if (seq >= MAX_EVENTS)
            throw std::overflow_error("EventLog full");

        evt.sequence     = seq;
        evt.timestamp_ns = now_ns();

        events_[seq] = evt;

        // Release fence: ensure the event write is visible before we bump seq
        std::atomic_thread_fence(std::memory_order_release);
        next_seq_.store(seq + 1, std::memory_order_release);

        // Also persist to the header (readers recovering from disk use this)
        header_->next_sequence = seq + 1;

        return seq;
    }

    [[nodiscard]] const OrderEvent* read(const uint64_t seq) const {
        if (seq >= next_seq_.load(std::memory_order_acquire)) return nullptr;
        return events_ + seq;
    }

    void replay(const std::function<void(const OrderEvent&)>& fn) const {
        const uint64_t head = next_seq_.load(std::memory_order_acquire);
        for (uint64_t i = 0; i < head; ++i) fn(events_[i]);
    }

    [[nodiscard]] uint64_t size()  const { return next_seq_.load(std::memory_order_acquire); }
    [[nodiscard]] bool     empty() const { return size() == 0; }

private:
    void open_or_create() {
        const bool exists = (access(path_.c_str(), F_OK) == 0);
        fd_ = open(path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) throw std::runtime_error("EventLog: cannot open " + path_);

        if (!exists && ftruncate(fd_, static_cast<off_t>(FILE_SIZE)) != 0)
            throw std::runtime_error("EventLog: ftruncate failed");

        mmap_base_ = mmap(nullptr, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mmap_base_ == MAP_FAILED) throw std::runtime_error("EventLog: mmap failed");

        madvise(mmap_base_, FILE_SIZE, MADV_SEQUENTIAL);

        header_ = static_cast<LogHeader*>(mmap_base_);
        events_ = reinterpret_cast<OrderEvent*>(
            static_cast<uint8_t*>(mmap_base_) + sizeof(LogHeader));

        if (!exists) {
            header_->magic         = LOG_MAGIC;
            header_->next_sequence = 0;
            next_seq_.store(0, std::memory_order_relaxed);
        } else {
            if (header_->magic != LOG_MAGIC)
                throw std::runtime_error("EventLog: corrupt file");
            next_seq_.store(header_->next_sequence, std::memory_order_relaxed);
        }
    }

    static uint64_t now_ns() {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
             + static_cast<uint64_t>(ts.tv_nsec);
    }

    std::string          path_;
    int                  fd_        = -1;
    void*                mmap_base_ = MAP_FAILED;
    LogHeader*           header_    = nullptr;
    OrderEvent*          events_    = nullptr;
    std::atomic<uint64_t> next_seq_{0};
};

} // namespace hft
