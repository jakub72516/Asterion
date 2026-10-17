#include <windows.h>
#include <psapi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/clock.hpp"
#include "core/store.hpp"
#include "fault/rng.hpp"
#include "metrics/metrics.hpp"
#include "storage/record.hpp"

#pragma comment(lib, "psapi.lib")

using namespace ast;

namespace {

struct Args {
    uint64_t ops = 1'000'000;
    uint64_t working_set = 200'000;
    uint64_t logical_keys = 100'000'000; // modeled address space (not stored)
    uint64_t tenants = 4;
    double read_fraction = 0.7;
    double skew = 0.0;      // 0 = uniform, >0 = zipfian (higher = more skewed)
    double retry_fraction = 0.01;
    double scan_fraction = 0.005;
    double delete_fraction = 0.02;
    uint64_t seed = 42;
    std::string data_dir;
    uint64_t segment_size = 64 * 1024 * 1024;
    uint64_t index_max = 1'000'000;
    std::string flush_mode = "per_write";
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> uint64_t { return strtoull(argv[++i], nullptr, 10); };
        if (k == "--ops") a.ops = next();
        else if (k == "--working-set") a.working_set = next();
        else if (k == "--logical-keys") a.logical_keys = next();
        else if (k == "--tenants") a.tenants = next();
        else if (k == "--reads") a.read_fraction = strtod(argv[++i], nullptr);
        else if (k == "--skew") a.skew = strtod(argv[++i], nullptr);
        else if (k == "--retries") a.retry_fraction = strtod(argv[++i], nullptr);
        else if (k == "--scans") a.scan_fraction = strtod(argv[++i], nullptr);
        else if (k == "--deletes") a.delete_fraction = strtod(argv[++i], nullptr);
        else if (k == "--seed") a.seed = next();
        else if (k == "--data-dir") a.data_dir = argv[++i];
        else if (k == "--segment-size") a.segment_size = next();
        else if (k == "--index-max") a.index_max = next();
        else if (k == "--flush") a.flush_mode = argv[++i];
    }
    return a;
}

uint64_t process_working_set_bytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize;
    return 0;
}

class KeyGen {
public:
    KeyGen(uint64_t working_set, double skew, Rng& rng)
        : W_(working_set), skew_(skew), rng_(rng) {
        if (skew_ > 0) build_zipf();
    }
    uint64_t next() {
        if (skew_ <= 0) return rng_.next_below(W_);
        // Inverse-CDF via binary search on the zipf CDF.
        double u = rng_.next_double();
        size_t lo = 0, hi = cdf_.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (cdf_[mid] < u) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
private:
    void build_zipf() {
        cdf_.resize(W_);
        double sum = 0;
        for (uint64_t i = 0; i < W_; ++i) sum += 1.0 / std::pow((double)(i + 1), skew_);
        double acc = 0;
        for (uint64_t i = 0; i < W_; ++i) {
            acc += (1.0 / std::pow((double)(i + 1), skew_)) / sum;
            cdf_[i] = acc;
        }
    }
    uint64_t W_;
    double skew_;
    Rng& rng_;
    std::vector<double> cdf_;
};

} // namespace

int main(int argc, char** argv) {
    SteadyClock::instance().init();
    Args a = parse_args(argc, argv);

    if (a.data_dir.empty()) {
        a.data_dir = "bench_data_" + std::to_string(GetCurrentProcessId());
    }
    bool cleanup = a.data_dir.rfind("bench_data_", 0) == 0;

    Store::Options so;
    so.log.dir = a.data_dir;
    so.log.segment_size = a.segment_size;
    so.log.flush_mode = (a.flush_mode == "batch") ? FlushMode::Batch
                        : (a.flush_mode == "none") ? FlushMode::None
                                                   : FlushMode::PerWrite;
    so.log.batch_flush_interval_ms = 10;
    so.log.max_key_size = 1024;
    so.log.max_payload_size = 1 << 20;
    so.index_max_entries = a.index_max;
    so.max_key_size = 1024;
    so.max_payload_size = 1 << 20;

    auto store = Store::open(so);
    if (!store.ok()) {
        fprintf(stderr, "store open failed: %s\n", store.error().to_string().c_str());
        return 2;
    }

    Rng rng(a.seed);
    KeyGen kg(a.working_set, a.skew, rng);
    Histogram hist;

    uint64_t writes = 0, reads = 0, scans = 0, deletes = 0, retries = 0, duplicates = 0;

    uint64_t t0 = SteadyClock::instance().now_ns();

    for (uint64_t op = 0; op < a.ops; ++op) {
        uint64_t tenant = rng.next_below(a.tenants);
        uint64_t kid = kg.next();
        std::string key = "key-" + std::to_string(kid);
        double r = rng.next_double();

        uint64_t s = SteadyClock::instance().now_ns();

        if (r < a.read_fraction) {
            auto got = (*store)->get(tenant, key);
            (void)got;
            reads++;
        } else if (r < a.read_fraction + a.scan_fraction) {
            ScanControl ctrl;
            ctrl.max_results = 100;
            uint64_t n = 0;
            (*store)->scan_tenant(tenant, 0, ~0ULL, [&](const EventRecord&) {
                n++;
                return n < 100;
            }, &ctrl);
            scans++;
        } else if (r < a.read_fraction + a.scan_fraction + a.delete_fraction) {
            EventRecord e;
            e.tenant_id = tenant;
            e.key = key;
            e.sequence = 0; // auto-assigned
            e.deleted = true;
            for (int i = 0; i < 16; ++i) e.event_id[i] = (uint8_t)rng.next_u32();
            (*store)->write(e);
            deletes++;
        } else if (r < a.read_fraction + a.scan_fraction + a.delete_fraction + a.retry_fraction) {
            // Retry: issue the same write twice (idempotency exercise).
            EventRecord e;
            e.tenant_id = tenant;
            e.key = key;
            e.sequence = 0;
            for (int i = 0; i < 16; ++i) e.event_id[i] = (uint8_t)rng.next_u32();
            auto w1 = (*store)->write(e);
            if (w1.ok()) {
                e.sequence = w1->sequence; // retry with the assigned sequence
                auto w2 = (*store)->write(e);
                if (w2.ok() && w2->duplicate) duplicates++;
            }
            retries++;
        } else {
            EventRecord e;
            e.tenant_id = tenant;
            e.key = key;
            e.sequence = 0;
            for (int i = 0; i < 16; ++i) e.event_id[i] = (uint8_t)rng.next_u32();
            e.payload = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
            (*store)->write(e);
            writes++;
        }

        uint64_t e2 = SteadyClock::instance().now_ns();
        hist.record((e2 - s) / 1000);
    }

    uint64_t t1 = SteadyClock::instance().now_ns();
    double secs = (double)(t1 - t0) / 1e9;
    double ops_s = secs > 0 ? (double)a.ops / secs : 0;

    uint64_t ws_bytes = process_working_set_bytes();

    printf("{\n");
    printf("  \"logical_keys\": %llu,\n", (unsigned long long)a.logical_keys);
    printf("  \"working_set\": %llu,\n", (unsigned long long)a.working_set);
    printf("  \"tenants\": %llu,\n", (unsigned long long)a.tenants);
    printf("  \"ops\": %llu,\n", (unsigned long long)a.ops);
    printf("  \"writes\": %llu,\n", (unsigned long long)writes);
    printf("  \"reads\": %llu,\n", (unsigned long long)reads);
    printf("  \"scans\": %llu,\n", (unsigned long long)scans);
    printf("  \"deletes\": %llu,\n", (unsigned long long)deletes);
    printf("  \"retries\": %llu,\n", (unsigned long long)retries);
    printf("  \"duplicates_detected\": %llu,\n", (unsigned long long)duplicates);
    printf("  \"skew\": %.3f,\n", a.skew);
    printf("  \"flush_mode\": \"%s\",\n", a.flush_mode.c_str());
    printf("  \"seed\": %llu,\n", (unsigned long long)a.seed);
    printf("  \"elapsed_s\": %.3f,\n", secs);
    printf("  \"throughput_ops_s\": %.1f,\n", ops_s);
    printf("  \"latency_mean_us\": %.1f,\n", hist.mean_us());
    printf("  \"latency_p50_us\": %llu,\n", (unsigned long long)hist.percentile(0.50));
    printf("  \"latency_p95_us\": %llu,\n", (unsigned long long)hist.percentile(0.95));
    printf("  \"latency_p99_us\": %llu,\n", (unsigned long long)hist.percentile(0.99));
    printf("  \"memory_working_set_bytes\": %llu,\n", (unsigned long long)ws_bytes);
    printf("  \"store_total_records\": %llu,\n", (unsigned long long)(*store)->total_records());
    printf("  \"store_index_entries\": %llu,\n", (unsigned long long)(*store)->index_size());
    printf("  \"store_index_max\": %llu\n", (unsigned long long)(*store)->index_max());
    printf("}\n");

    (*store)->close();
    if (cleanup) {
        std::error_code ec;
        std::filesystem::remove_all(a.data_dir, ec);
    }
    return 0;
}
