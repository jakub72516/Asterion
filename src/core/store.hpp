#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.hpp"
#include "storage/key_index.hpp"
#include "storage/log.hpp"
#include "storage/record.hpp"

namespace ast {

// Streaming scan control: lets a client cancel a long scan and enforces limits.
struct ScanControl {
    std::function<bool()> cancelled; // returns true to stop early
    uint64_t max_results = 100000;   // hard cap on emitted results
    uint64_t emitted = 0;            // filled by the scanner
};

// A single-node append-only event store: Log (durability) + KeyIndex (bounded
// memory). Enforces per-key ordering and idempotent writes by event_id.
class Store {
public:
    struct Options {
        Log::Options log;
        uint64_t index_max_entries = 1'000'000;
        uint64_t max_key_size = 1024;
        uint64_t max_payload_size = 1 * 1024 * 1024;
    };

    struct WriteResult {
        bool duplicate = false; // idempotent retry of an existing event
        uint64_t sequence = 0;
        uint64_t timestamp = 0; // assigned timestamp (for replication)
        LogPos pos;
    };

    struct CompactionStats {
        uint64_t segments_merged = 0;
        uint64_t records_in = 0;
        uint64_t records_out = 0;
        bool nothing_to_do = false;
    };

    static Result<std::unique_ptr<Store>> open(const Options& opts, RecoveryInfo* ri = nullptr);
    void close();

    // Write one event. Enforces per-key ordering by sequence (auto-assigned
    // when 0) and idempotency by event_id. Durable per the log flush mode.
    Result<WriteResult> write(const EventRecord& rec);

    // Latest event for a key (highest sequence). Returns nullopt if absent.
    Result<std::optional<EventRecord>> get(uint64_t tenant, std::string_view key);

    // All events for a key, in sequence order (streaming; visitor returns false
    // to stop). O(total records) fallback, memory-bounded.
    Error scan_key(uint64_t tenant, std::string_view key,
                   const std::function<bool(const EventRecord&)>& visitor);

    // All events in the store, in append order (used for sync/backup).
    Error scan_all(const std::function<bool(const EventRecord&)>& visitor);

    // Time-range scan over a tenant, in append (and thus per-key sequence)
    // order. Honors cancellation and result limits.
    Error scan_tenant(uint64_t tenant, uint64_t from_ts, uint64_t to_ts,
                      const std::function<bool(const EventRecord&)>& visitor,
                      ScanControl* control);

    // Compaction: merge the oldest sealed segments, keeping only the latest
    // version per key and dropping tombstones. max_segments==0 means "all
    // sealed". Memory use is bounded by the records in the merged segments.
    Result<CompactionStats> compact(uint64_t max_segments = 0);

    uint64_t total_records() const;
    uint64_t index_size() const;
    uint64_t index_max() const;

private:
    uint64_t find_latest_seq(uint64_t tenant, std::string_view key);
    Result<std::optional<EventRecord>> find_record(uint64_t tenant, std::string_view key,
                                                   uint64_t seq);

    Options opts_;
    std::unique_ptr<Log> log_;
    KeyIndex index_;
    mutable std::mutex mu_; // serializes writes and scans
};

} // namespace ast
