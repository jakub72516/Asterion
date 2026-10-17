#include "metrics/metrics.hpp"

#include <algorithm>

namespace ast {

const std::vector<uint64_t>& Histogram::bounds() {
    // Microsecond upper bounds.
    static const std::vector<uint64_t> b = {
        50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000,
        100000, 250000, 500000, 1000000, 2500000, 5000000, 10000000,
    };
    return b;
}

Histogram::Histogram() : buckets_(bounds().size() + 1, 0) {}

void Histogram::record(uint64_t us) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto& b = bounds();
    size_t i = 0;
    while (i < b.size() && us > b[i]) ++i;
    buckets_[i]++;
    count_++;
    sum_ += us;
    if (count_ == 1) { min_ = us; max_ = us; }
    else { min_ = std::min(min_, us); max_ = std::max(max_, us); }
}

uint64_t Histogram::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return count_;
}

uint64_t Histogram::sum_us() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sum_;
}

double Histogram::mean_us() const {
    std::lock_guard<std::mutex> lk(mu_);
    return count_ ? (double)sum_ / (double)count_ : 0.0;
}

uint64_t Histogram::percentile(double p) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (count_ == 0) return 0;
    uint64_t target = (uint64_t)(p * (double)count_);
    if (target >= count_) target = count_ - 1;
    const auto& b = bounds();
    uint64_t acc = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
        acc += buckets_[i];
        if (acc > target) {
            return (i < b.size()) ? b[i] : (b.back() + 1);
        }
    }
    return max_;
}

void Histogram::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    buckets_.assign(buckets_.size(), 0);
    count_ = sum_ = min_ = max_ = 0;
}

void Metrics::reset() {
    appends = 0;
    reads = 0;
    scans = 0;
    deletes = 0;
    errors = 0;
    rejected = 0;
    retries = 0;
    bytes_written = 0;
    latency_.reset();
}

Json Metrics::to_json() const {
    std::map<std::string, Json> o;
    o["appends"] = Json((int64_t)appends.load());
    o["reads"] = Json((int64_t)reads.load());
    o["scans"] = Json((int64_t)scans.load());
    o["deletes"] = Json((int64_t)deletes.load());
    o["errors"] = Json((int64_t)errors.load());
    o["rejected"] = Json((int64_t)rejected.load());
    o["retries"] = Json((int64_t)retries.load());
    o["bytes_written"] = Json((int64_t)bytes_written.load());
    o["latency_mean_us"] = Json(latency_.mean_us());
    o["latency_p50_us"] = Json((int64_t)latency_.percentile(0.50));
    o["latency_p95_us"] = Json((int64_t)latency_.percentile(0.95));
    o["latency_p99_us"] = Json((int64_t)latency_.percentile(0.99));
    o["requests_total"] = Json((int64_t)latency_.count());
    return Json(std::move(o));
}

} // namespace ast
