#include "storage/log.hpp"

#include <algorithm>
#include <cstdio>

#include "common/clock.hpp"
#include "common/platform/file.hpp"
#include "common/crc32.hpp"

namespace ast {

namespace {
std::string hex_id(uint64_t id) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)id);
    return std::string(buf);
}
} // namespace

std::string Log::segment_path(const std::string& dir, uint64_t id) {
    return dir + "\\segment_" + hex_id(id) + ".astseg";
}

bool Log::parse_segment_id(const std::string& name, uint64_t* id) {
    // Expect "segment_<16 hex>.astseg"
    const std::string prefix = "segment_";
    const std::string suffix = ".astseg";
    if (name.size() != prefix.size() + 16 + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; ++i) {
        char c = name[prefix.size() + i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return false;
    }
    *id = v;
    return true;
}

Result<std::unique_ptr<Log>> Log::open(const Options& opts, RecoveryInfo* recovery_out) {
    Error err = make_dirs(opts.dir);
    if (!err.ok()) return err;

    auto log = std::make_unique<Log>();
    log->opts_ = opts;

    // Enumerate and open existing segments.
    std::vector<uint64_t> ids;
    for (auto& f : list_files(opts.dir, ".astseg")) {
        uint64_t id;
        if (parse_segment_id(f, &id)) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    RecoveryInfo info;
    info.segments_found = (uint64_t)ids.size();

    for (uint64_t id : ids) {
        std::string path = segment_path(opts.dir, id);
        SegmentHeader hdr;
        auto seg_res = SegmentFile::open(path, &hdr);
        if (!seg_res.ok()) {
            info.issues.push_back("segment " + path + ": " + seg_res.error().to_string());
            continue;
        }
        auto seg = std::make_unique<SegmentFile>(std::move(seg_res.value()));
        info.segments_opened++;

        // Scan to validate records and isolate any corrupt tail.
        uint64_t last_good = kSegmentHeaderSize;
        uint64_t rec_count = 0;
        bool corrupt = false;
        auto visitor = [&](const EventRecord& rec) -> bool {
            last_good = rec.offset + rec.frame_size;
            rec_count++;
            return true;
        };
        auto scan_err = seg->scan(visitor, opts.max_key_size, opts.max_payload_size);
        if (!scan_err.ok()) {
            // Truncate at the last good offset and continue.
            info.issues.push_back("segment " + path + ": " + scan_err.to_string() +
                                  " (truncating at " + std::to_string(last_good) + ")");
            auto tr = seg->truncate_to(last_good);
            if (tr.ok()) {
                corrupt = true;
            } else {
                info.issues.push_back("segment " + path + ": failed to truncate: " +
                                      tr.to_string());
            }
        }

        // Recover record counts (header may be stale after a crash).
        uint64_t good_records = 0;
        auto count_visitor = [&](const EventRecord&) -> bool { good_records++; return true; };
        if (corrupt) {
            auto r2 = seg->scan(count_visitor, opts.max_key_size, opts.max_payload_size);
            if (r2.ok()) rec_count = good_records;
        }
        info.records_recovered += rec_count;

        bool sealed = seg->sealed();
        if (!sealed && !log->active_) {
            // The highest non-sealed segment becomes active.
            log->active_ = seg.get();
        }
        log->total_records_ += rec_count;
        log->segments_.push_back(std::move(seg));
    }

    // The active segment is the last (highest id) unsealed segment; if all are
    // sealed (or none), create a fresh one.
    if (!log->active_) {
        err = log->ensure_active_segment();
        if (!err.ok()) return err;
    }

    // Start the batch flusher if needed. Capture a raw pointer: the Log lives
    // on the heap and is not moved after this point.
    if (opts.flush_mode == FlushMode::Batch) {
        Log* self = log.get();
        log->flusher_ = std::thread([self] { self->flusher_loop(); });
    }

    if (recovery_out) *recovery_out = std::move(info);
    return log;
}

void Log::close() {
    std::unique_lock<std::mutex> lk(mu_);
    if (closing_) return;
    closing_ = true;
    lk.unlock();

    if (flusher_.joinable()) {
        flush_cv_.notify_all();
        flusher_.join();
    }

    lk.lock();
    for (auto& s : segments_) s->close();
    segments_.clear();
    active_ = nullptr;
}

uint64_t Log::next_segment_id() const {
    if (segments_.empty()) return 1;
    return segments_.back()->segment_id() + 1;
}

Error Log::ensure_active_segment() {
    uint64_t id = next_segment_id();
    std::string path = segment_path(opts_.dir, id);
    auto seg_res = SegmentFile::create(path, id, wall_time_us());
    if (!seg_res.ok()) return seg_res.error();
    // Durably record the new segment's directory entry.
    flush_directory(opts_.dir);
    auto seg = std::make_unique<SegmentFile>(std::move(seg_res.value()));
    active_ = seg.get();
    segments_.push_back(std::move(seg));
    return ok();
}

Result<uint64_t> Log::append_frame(const uint8_t* frame, uint64_t len) {
    if (len == 0) return Error(Err::InvalidArgument, "empty frame");
    auto off = active_->append(frame, len);
    if (!off.ok()) return off.error();
    total_records_ += 1;

    // Rotate if the active segment exceeds the target size.
    if (active_->data_size() >= opts_.segment_size) {
        auto e = rotate();
        if (!e.ok()) return e;
    }
    return off.value();
}

Result<LogPos> Log::append(const EventRecord& rec) {
    auto frame = encode_frame(rec);
    if (!frame.ok()) return frame.error();

    std::lock_guard<std::mutex> lk(mu_);
    if (closing_) return Error(Err::Shutdown, "log is closed");

    uint64_t sid = active_->segment_id();
    auto off = append_frame(frame->data(), frame->size());
    if (!off.ok()) return off.error();

    LogPos pos;
    pos.segment_id = sid;
    pos.offset = off.value();
    pos.frame_size = frame->size();

    // Per-write flush: durability before returning. If append_frame rotated
    // the segment, the old segment was already sealed+flushed; flushing the
    // new (empty) active segment is harmless.
    if (opts_.flush_mode == FlushMode::PerWrite) {
        auto e = active_ ? active_->flush() : ok();
        if (!e.ok()) return e;
    }
    return pos;
}

Error Log::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (active_) return active_->flush();
    return ok();
}

Result<EventRecord> Log::read_at(const LogPos& pos) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : segments_) {
        if (s->segment_id() == pos.segment_id) {
            return s->read_record(pos.offset, opts_.max_key_size, opts_.max_payload_size);
        }
    }
    return Error(Err::NotFound, "segment not found");
}

Error Log::scan_all(const std::function<bool(const EventRecord&)>& visitor) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : segments_) {
        auto e = s->scan(visitor, opts_.max_key_size, opts_.max_payload_size);
        if (!e.ok()) return e;
    }
    return ok();
}

Error Log::rotate() {
    if (active_) {
        auto e = active_->seal();
        if (!e.ok()) return e;
        active_ = nullptr;
    }
    return ensure_active_segment();
}

std::vector<SegmentInfo> Log::segments() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SegmentInfo> out;
    out.reserve(segments_.size());
    for (auto& s : segments_) {
        SegmentInfo si;
        si.id = s->segment_id();
        si.sealed = s->sealed();
        si.data_size = s->data_size();
        si.path = s->path();
        out.push_back(si);
    }
    return out;
}

uint64_t Log::segment_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (uint64_t)segments_.size();
}

uint64_t Log::total_records() const {
    std::lock_guard<std::mutex> lk(mu_);
    return total_records_;
}

void Log::flusher_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(flush_mu_);
        flush_cv_.wait_for(lk, std::chrono::milliseconds(opts_.batch_flush_interval_ms));
        {
            std::lock_guard<std::mutex> gl(mu_);
            if (closing_) return;
        }
        auto e = flush();
        (void)e; // Flush errors surface on the next explicit flush/close.
    }
}

std::vector<uint64_t> Log::sealed_segment_ids() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint64_t> out;
    for (auto& s : segments_) {
        if (s.get() != active_) out.push_back(s->segment_id());
    }
    return out;
}

Error Log::scan_segments(const std::vector<uint64_t>& ids,
                         const std::function<bool(const EventRecord&)>& visitor) {
    std::lock_guard<std::mutex> lk(mu_);
    for (uint64_t id : ids) {
        for (auto& s : segments_) {
            if (s->segment_id() == id) {
                auto e = s->scan(visitor, opts_.max_key_size, opts_.max_payload_size);
                if (!e.ok()) return e;
                break;
            }
        }
    }
    return ok();
}

Error Log::replace_segments(const std::vector<uint64_t>& remove_ids, uint64_t new_id,
                            const std::vector<EventRecord>& records) {
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<uint64_t> remove = remove_ids;

    // Close and delete the source files first (their data is already in
    // `records`). This frees the filename for reuse as new_id.
    uint64_t removed_count = 0;
    std::vector<std::unique_ptr<SegmentFile>> remaining;
    remaining.reserve(segments_.size());
    for (auto& s : segments_) {
        bool is_remove = std::find(remove.begin(), remove.end(), s->segment_id()) != remove.end();
        if (is_remove) {
            removed_count += s->record_count();
            s->close();
            remove_file(s->path());
        } else {
            remaining.push_back(std::move(s));
        }
    }
    segments_ = std::move(remaining);

    // Create the new sealed segment (id == new_id, which is free now).
    std::string path = segment_path(opts_.dir, new_id);
    auto seg_res = SegmentFile::create(path, new_id, wall_time_us());
    if (!seg_res.ok()) return seg_res.error();
    auto seg = std::make_unique<SegmentFile>(std::move(seg_res.value()));
    for (auto& rec : records) {
        auto frame = encode_frame(rec);
        if (!frame.ok()) return frame.error();
        auto off = seg->append(frame->data(), frame->size());
        if (!off.ok()) return off.error();
    }
    auto seal_err = seg->seal();
    if (!seal_err.ok()) return seal_err;
    flush_directory(opts_.dir);

    // Insert in sorted (append) order.
    auto it = std::lower_bound(segments_.begin(), segments_.end(), new_id,
                               [](const std::unique_ptr<SegmentFile>& a, uint64_t id) {
                                   return a->segment_id() < id;
                               });
    segments_.insert(it, std::move(seg));

    total_records_ = total_records_ - removed_count + records.size();
    return ok();
}

} // namespace ast
