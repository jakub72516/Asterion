#pragma once

#include <cstdint>

namespace ast {

// Monotonic ticks in nanoseconds. Used for timeouts and latency.
// Implemented over QueryPerformanceCounter (the steady clock on Windows).
class SteadyClock {
public:
    static SteadyClock& instance();

    uint64_t now_ns() const;
    uint64_t ticks_per_sec() const { return freq_; }

    void init(); // call once at startup

private:
    SteadyClock() = default;
    bool initialized_ = false;
    uint64_t freq_ = 1;
};

// Wall-clock time in microseconds since the Unix epoch (UTC).
// Implemented over GetSystemTimePreciseAsFileTime when available.
uint64_t wall_time_us();
uint64_t wall_time_ms();
uint64_t wall_time_s();

// A small, injectable clock interface for tests.
class Clock {
public:
    virtual ~Clock() = default;
    virtual uint64_t now_ns() const = 0; // monotonic
    virtual uint64_t now_wall_us() const = 0;
};

class SystemClock final : public Clock {
public:
    uint64_t now_ns() const override;
    uint64_t now_wall_us() const override;
};

} // namespace ast
