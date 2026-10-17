#include "core/store.hpp"

#include <algorithm>

#include "common/clock.hpp"

namespace ast {

Result<std::unique_ptr<Store>> Store::open(const Options& opts, RecoveryInfo* ri) {
    auto log = Log::open(opts.log, ri);
    if (!log.ok()) return log.error();

    auto store = std::make_unique<Store>();
    store->opts_ = opts;
    store->log_ = std::move(log.value());
    store->index_ = KeyIndex(opts.index_max_entries);

    // Rebuild the bounded index from the log (drops entries once full; cold
    // keys are re-resolved by scan at query time).
    store->log_->scan_all([&](const EventRecord& rec) {
        IndexKey k{rec.tenant_id, rec.key};
        KeyIndexEntry e;
        e.latest_seq = rec.sequence;
        e.pos.segment_id = rec.segment_id;
        e.pos.offset = rec.offset;
        e.pos.frame_size = rec.frame_size;
        e.deleted = rec.deleted;
        e.timestamp = rec.timestamp;
        store->index_.put(k, e);
        return true;
    });

    return store;
}

void Store::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (log_) log_->close();
}

uint64_t Store::find_latest_seq(uint64_t tenant, std::string_view key) {
    // Forward scan keeping the last match; memory-bounded, O(total records).
    uint64_t latest = 0;
    log_->scan_all([&](const EventRecord& rec) {
        if (rec.tenant_id == tenant && rec.key == key) latest = rec.sequence;
        return true;
    });
    return latest;
}

Result<std::optional<EventRecord>> Store::find_record(uint64_t tenant, std::string_view key,
                                                      uint64_t seq) {
    std::optional<EventRecord> found;
    log_->scan_all([&](const EventRecord& rec) {
        if (rec.tenant_id == tenant && rec.key == key && rec.sequence == seq) {
            found = rec;
            return false;
        }
        return true;
    });
    return found;
}

Result<Store::WriteResult> Store::write(const EventRecord& rec) {
    if (rec.key.empty()) return Error(Err::InvalidArgument, "key must not be empty");
    if (rec.key.size() > opts_.max_key_size) return Error(Err::LimitExceeded, "key too large");
    if (rec.payload.size() > opts_.max_payload_size)
        return Error(Err::LimitExceeded, "payload too large");

    std::lock_guard<std::mutex> lk(mu_);

    IndexKey ikey{rec.tenant_id, rec.key};
    uint64_t latest_seq = 0;
    auto cached = index_.get(ikey);
    if (cached) {
        latest_seq = cached->latest_seq;
    } else {
        latest_seq = find_latest_seq(rec.tenant_id, rec.key);
    }

    EventRecord to_write = rec;

    if (to_write.sequence == 0) {
        to_write.sequence = latest_seq + 1;
    } else if (to_write.sequence <= latest_seq) {
        // Possible idempotent retry or a conflict.
        auto existing = find_record(rec.tenant_id, rec.key, to_write.sequence);
        if (existing.ok() && existing->has_value()) {
            const EventRecord& e = existing->value();
            if (e.event_id == rec.event_id) {
                WriteResult wr;
                wr.duplicate = true;
                wr.sequence = e.sequence;
                wr.pos.segment_id = e.segment_id;
                wr.pos.offset = e.offset;
                wr.pos.frame_size = e.frame_size;
                return wr;
            }
            return Error(Err::Conflict,
                         "sequence already used for key with a different event_id");
        }
        return Error(Err::Conflict, "sequence <= latest for key");
    }

    if (to_write.timestamp == 0) to_write.timestamp = wall_time_us();

    auto pos = log_->append(to_write);
    if (!pos.ok()) return pos.error();

    IndexKey uk{to_write.tenant_id, to_write.key};
    KeyIndexEntry e;
    e.latest_seq = to_write.sequence;
    e.pos = pos.value();
    e.deleted = to_write.deleted;
    e.timestamp = to_write.timestamp;
    index_.put(uk, e);

    WriteResult wr;
    wr.duplicate = false;
    wr.sequence = to_write.sequence;
    wr.timestamp = to_write.timestamp;
    wr.pos = pos.value();
    return wr;
}

Result<std::optional<EventRecord>> Store::get(uint64_t tenant, std::string_view key) {
    std::lock_guard<std::mutex> lk(mu_);

    IndexKey ikey{tenant, std::string(key)};
    auto cached = index_.get(ikey);
    if (cached) {
        auto rec = log_->read_at(cached->pos);
        if (rec.ok()) return std::optional<EventRecord>(rec.value());
        if (rec.code() == Err::NotFound || rec.code() == Err::Corrupt) {
            // Index stale; fall through to a scan.
        } else {
            return rec.error();
        }
    }
    // Cold key: forward scan keeping the latest match.
    std::optional<EventRecord> latest;
    log_->scan_all([&](const EventRecord& rec) {
        if (rec.tenant_id == tenant && rec.key == key) latest = rec;
        return true;
    });
    return latest;
}

Error Store::scan_key(uint64_t tenant, std::string_view key,
                      const std::function<bool(const EventRecord&)>& visitor) {
    std::lock_guard<std::mutex> lk(mu_);
    return log_->scan_all([&](const EventRecord& rec) {
        if (rec.tenant_id == tenant && rec.key == key) return visitor(rec);
        return true;
    });
}

Error Store::scan_all(const std::function<bool(const EventRecord&)>& visitor) {
    std::lock_guard<std::mutex> lk(mu_);
    return log_->scan_all(visitor);
}

Error Store::scan_tenant(uint64_t tenant, uint64_t from_ts, uint64_t to_ts,
                         const std::function<bool(const EventRecord&)>& visitor,
                         ScanControl* control) {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t emitted = 0;
    return log_->scan_all([&](const EventRecord& rec) {
        if (control && control->cancelled && control->cancelled()) return false;
        if (rec.tenant_id != tenant) return true;
        if (rec.timestamp < from_ts || rec.timestamp > to_ts) return true;
        if (control && emitted >= control->max_results) {
            if (control) control->emitted = emitted;
            return false; // stop; caller can page with a higher from_ts
        }
        emitted++;
        if (control) control->emitted = emitted;
        return visitor(rec);
    });
}

uint64_t Store::total_records() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_->total_records();
}

uint64_t Store::index_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return index_.size();
}

uint64_t Store::index_max() const {
    return index_.max_entries();
}

Result<Store::CompactionStats> Store::compact(uint64_t max_segments) {
    std::lock_guard<std::mutex> lk(mu_);

    CompactionStats stats;
    auto sealed = log_->sealed_segment_ids();
    if (sealed.size() < 2) {
        stats.nothing_to_do = true;
        return stats;
    }

    uint64_t n = (max_segments == 0) ? sealed.size() : std::min<uint64_t>(sealed.size(), max_segments);
    if (n < 2) {
        stats.nothing_to_do = true;
        return stats;
    }

    std::vector<uint64_t> source(sealed.begin(), sealed.begin() + n);
    uint64_t min_id = source.front();

    // Latest version per key across the source segments.
    std::unordered_map<IndexKey, EventRecord, IndexKeyHash> latest;
    uint64_t records_in = 0;
    auto e = log_->scan_segments(source, [&](const EventRecord& rec) {
        records_in++;
        IndexKey k{rec.tenant_id, rec.key};
        auto it = latest.find(k);
        if (it == latest.end() || rec.sequence > it->second.sequence) {
            latest[k] = rec;
        }
        return true;
    });
    if (!e.ok()) return e;

    std::vector<EventRecord> survivors;
    survivors.reserve(latest.size());
    for (auto& kv : latest) {
        if (!kv.second.deleted) survivors.push_back(std::move(kv.second));
    }

    auto repl = log_->replace_segments(source, min_id, survivors);
    if (!repl.ok()) return repl;

    // Positions changed; rebuild the (bounded) index from the log.
    index_.clear();
    log_->scan_all([&](const EventRecord& rec) {
        IndexKey k{rec.tenant_id, rec.key};
        KeyIndexEntry e2;
        e2.latest_seq = rec.sequence;
        e2.pos.segment_id = rec.segment_id;
        e2.pos.offset = rec.offset;
        e2.pos.frame_size = rec.frame_size;
        e2.deleted = rec.deleted;
        e2.timestamp = rec.timestamp;
        index_.put(k, e2);
        return true;
    });

    stats.segments_merged = n;
    stats.records_in = records_in;
    stats.records_out = survivors.size();
    return stats;
}

} // namespace ast
