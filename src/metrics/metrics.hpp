#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "common/json.hpp"

namespace ast {

// Latency histogram with fixed log-ish buckets (microseconds). Tracks count,
// sum, min, max; supports approximate percentile queries.
class Histogram {
public:
    Histogram();

    void record(uint64_t us);
    void merge(const Histogram& o);
    void reset();

    uint64_t count() const;
    uint64_t sum_us() const;
    uint64_t percentile(double p) const; // p in [0,1]
    double mean_us() const;

private:
    // Bucket upper bounds (us).
    static const std::vector<uint64_t>& bounds();
    mutable std::mutex mu_;
    std::vector<uint64_t> buckets_;
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = 0;
    uint64_t max_ = 0;
};

// Operational metrics reported by a node. All counters are atomics; the
// histogram is independently synchronized.
class Metrics {
public:
    void reset();

    std::atomic<uint64_t> appends{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> scans{0};
    std::atomic<uint64_t> deletes{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> retries{0};
    std::atomic<uint64_t> bytes_written{0};

    void record_latency(uint64_t us) { latency_.record(us); }
    const Histogram& latency() const { return latency_; }

    Json to_json() const;

private:
    Histogram latency_;
};

} // namespace ast
