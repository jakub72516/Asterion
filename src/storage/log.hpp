#pragma once

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "common/result.hpp"
#include "storage/record.hpp"
#include "storage/segment.hpp"

namespace ast {

// Location of a stored record within the log.
struct LogPos {
    uint64_t segment_id = 0;
    uint64_t offset = 0;      // absolute file offset of the frame
    uint64_t frame_size = 0;
};

struct SegmentInfo {
    uint64_t id = 0;
    bool sealed = false;
    uint64_t data_size = 0;
    std::string path;
};

// Information returned by recovery, reported for observability.
struct RecoveryInfo {
    uint64_t segments_found = 0;
    uint64_t segments_opened = 0;
    uint64_t records_recovered = 0;
    uint64_t records_dropped = 0;      // corrupt/truncated records isolated
    std::vector<std::string> issues;   // human-readable descriptions
};

// The append-only log: a sequence of segment files plus the active segment.
// Serializes appends internally. Flush semantics follow the configured
// FlushMode (see docs/on-disk-format.md for durability guarantees).
class Log {
public:
    struct Options {
        std::string dir;
        uint64_t segment_size = 64 * 1024 * 1024;
        FlushMode flush_mode = FlushMode::PerWrite;
        uint64_t batch_flush_interval_ms = 10;
        uint64_t max_key_size = 1024;
        uint64_t max_payload_size = 1 * 1024 * 1024;
    };

    Log() = default;
    ~Log() { close(); }

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // Open (and recover) the log at opts.dir. The Log is heap-allocated so its
    // address stays stable for the background flusher thread.
    static Result<std::unique_ptr<Log>> open(const Options& opts, RecoveryInfo* recovery_out = nullptr);
    void close();

    // Append a record. Returns its location. Flushes per FlushMode.
    Result<LogPos> append(const EventRecord& rec);

    // Force a flush of the active segment (used by batch mode / explicit ack).
    Error flush();

    Result<EventRecord> read_at(const LogPos& pos);

    // Scan all records across all segments in append order.
    Error scan_all(const std::function<bool(const EventRecord&)>& visitor);

    // Rotate the active segment now (seal + create new). Used by compaction.
    Error rotate();

    std::vector<SegmentInfo> segments() const;
    uint64_t segment_count() const;
    uint64_t total_records() const;

    const Options& options() const { return opts_; }

    // Compaction support (see docs/on-disk-format.md for crash-safety notes).
    std::vector<uint64_t> sealed_segment_ids() const;
    // Scan only the records in the given (sealed) segment ids, in order.
    Error scan_segments(const std::vector<uint64_t>& ids,
                        const std::function<bool(const EventRecord&)>& visitor);
    // Replace `remove_ids` (sealed, non-active) with one new sealed segment of
    // id `new_id` containing `records`. Callers must already have read the
    // removed segments' data. Removes the old files.
    Error replace_segments(const std::vector<uint64_t>& remove_ids, uint64_t new_id,
                           const std::vector<EventRecord>& records);

private:
    Result<uint64_t> append_frame(const uint8_t* frame, uint64_t len);
    Error ensure_active_segment();
    uint64_t next_segment_id() const;
    static std::string segment_path(const std::string& dir, uint64_t id);
    static bool parse_segment_id(const std::string& name, uint64_t* id);

    void flusher_loop();

    Options opts_;
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<SegmentFile>> segments_;
    SegmentFile* active_ = nullptr;
    uint64_t total_records_ = 0;
    bool closing_ = false;
    std::thread flusher_;
    std::mutex flush_mu_;
    std::condition_variable flush_cv_;
    bool flush_requested_ = false;
};

} // namespace ast
