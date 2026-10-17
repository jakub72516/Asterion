#include "test_framework.hpp"
#include "test_util.hpp"

#include "common/platform/file.hpp"
#include "core/store.hpp"
#include "fault/rng.hpp"
#include "storage/record.hpp"

#include <map>
#include <string>
#include <vector>

using namespace ast;
using namespace ast::test;

static Store::Options fault_opts(const std::string& dir, uint64_t seg_size = 1 << 20) {
    Store::Options o;
    o.log.dir = dir;
    o.log.segment_size = seg_size;
    o.log.flush_mode = FlushMode::PerWrite;
    o.log.max_key_size = 1024;
    o.log.max_payload_size = 1 << 20;
    o.index_max_entries = 100000;
    o.max_key_size = 1024;
    o.max_payload_size = 1 << 20;
    return o;
}

static EventRecord make_ev(uint64_t tenant, const std::string& key, uint64_t seq,
                           uint64_t event_id_lo) {
    EventRecord e;
    e.tenant_id = tenant;
    e.key = key;
    e.sequence = seq;
    e.timestamp = 1000 + seq;
    e.payload = {'v'};
    for (int i = 0; i < 16; ++i) e.event_id[i] = (uint8_t)((event_id_lo >> (8 * (i % 8))) ^ i);
    return e;
}

// Flip a byte past the first record of the newest segment (corrupting a later
// record), so earlier records survive and the corrupted tail is isolated.
static bool corrupt_newest_segment(const std::string& dir) {
    auto files = list_files(dir, ".astseg");
    if (files.empty()) return false;
    std::string path = dir + "\\" + files.back();
    auto f = File::open_rw(path);
    if (!f.ok()) return false;
    auto sz = f->size();
    if (!sz.ok() || sz.value() < kSegmentHeaderSize + 120) return false;
    uint64_t offset = kSegmentHeaderSize + 100;
    uint8_t byte = 0;
    auto r = f->read(&byte, 1, offset);
    if (!r.ok() || r.value() != 1) return false;
    byte ^= 0xFF;
    return f->write_at(&byte, 1, offset).ok();
}

TEST_CASE(fault_recovery_detects_corruption) {
    TempDir tmp;
    {
        auto store = Store::open(fault_opts(tmp.str(), 1 << 20));
        REQUIRE(store.ok());
        for (int i = 1; i <= 20; ++i)
            REQUIRE((*store)->write(make_ev(1, "k" + std::to_string(i), 1, i)).ok());
        (*store)->close();
    }

    REQUIRE(corrupt_newest_segment(tmp.str()));

    RecoveryInfo ri;
    auto store = Store::open(fault_opts(tmp.str(), 1 << 20), &ri);
    REQUIRE(store.ok());
    // Some records before the corruption survive; the corrupted tail is dropped.
    uint64_t total = (*store)->total_records();
    CHECK(total >= 1u);
    CHECK(total < 20u);
    CHECK(!ri.issues.empty());
    (*store)->close();
}

TEST_CASE(fault_recovery_truncation) {
    TempDir tmp;
    {
        auto store = Store::open(fault_opts(tmp.str(), 1 << 20));
        REQUIRE(store.ok());
        for (int i = 1; i <= 10; ++i)
            REQUIRE((*store)->write(make_ev(1, "k" + std::to_string(i), 1, i)).ok());
        (*store)->close();
    }
    // Truncate the newest segment to just past its header.
    auto files = list_files(tmp.str(), ".astseg");
    REQUIRE(!files.empty());
    std::string path = tmp.str() + "\\" + files.back();
    {
        auto f = File::open_rw(path);
        REQUIRE(f.ok());
        REQUIRE(f->truncate(kSegmentHeaderSize).ok());
        f->flush();
    }

    RecoveryInfo ri;
    auto store = Store::open(fault_opts(tmp.str(), 1 << 20), &ri);
    REQUIRE(store.ok());
    // The truncated segment holds no records; other segments (none) survive.
    CHECK_EQ((*store)->total_records(), 0u);
    (*store)->close();
}

struct RefKey {
    uint64_t tenant;
    std::string key;
    bool operator<(const RefKey& o) const {
        return tenant != o.tenant ? tenant < o.tenant : key < o.key;
    }
};

TEST_CASE(fault_randomized_reference_model) {
    TempDir tmp;
    Rng rng(0xC0FFEE123456ULL);

    auto opts = fault_opts(tmp.str(), 4096);
    auto store = Store::open(opts);
    REQUIRE(store.ok());

    std::map<RefKey, std::vector<EventRecord>> ref;
    const int NUM_KEYS = 40;
    const int OPS = 400;

    for (int op = 0; op < OPS; ++op) {
        std::string key = "k" + std::to_string(rng.next_below(NUM_KEYS));
        RefKey rk{1, key};
        int t = (int)rng.next_below(10);

        if (t < 6) {
            // Append a new version.
            uint64_t seq = ref[rk].size() + 1;
            EventRecord e = make_ev(1, key, seq, rng.next_u64());
            auto wr = (*store)->write(e);
            REQUIRE(wr.ok());
            CHECK(!wr->duplicate);
            ref[rk].push_back(e);
        } else if (t < 7 && !ref[rk].empty()) {
            // Idempotent retry of the last event for this key.
            EventRecord e = ref[rk].back();
            auto wr = (*store)->write(e);
            REQUIRE(wr.ok());
            CHECK(wr->duplicate);
        } else if (t < 9) {
            // Read latest and compare to the reference.
            auto got = (*store)->get(1, key);
            REQUIRE(got.ok());
            if (ref[rk].empty()) {
                CHECK(!got->has_value());
            } else {
                REQUIRE(got->has_value());
                CHECK_EQ(got->value().sequence, ref[rk].back().sequence);
            }
        } else {
            // Simulate a process restart: close and reopen (recovery).
            (*store)->close();
            auto reopened = Store::open(opts);
            REQUIRE(reopened.ok());
            store = std::move(reopened.value());
        }
    }

    // Full verification against the reference model.
    for (auto& kv : ref) {
        auto got = (*store)->get(1, kv.first.key);
        REQUIRE(got.ok());
        REQUIRE(got->has_value());
        CHECK_EQ(got->value().sequence, kv.second.back().sequence);
    }
    (*store)->close();
}

// Long-running stress variant of the randomized reference-model test. Runs only
// under `--all` / `--stress`; excluded from `--fast`.
TEST_CASE_STRESS(stress_randomized_reference_model) {
    TempDir tmp;
    Rng rng(0xDEADBEEF1234ULL);
    auto opts = fault_opts(tmp.str(), 64 * 1024);
    auto store = Store::open(opts);
    REQUIRE(store.ok());

    std::map<RefKey, std::vector<EventRecord>> ref;
    const int NUM_KEYS = 200;
    const int OPS = 5000;

    for (int op = 0; op < OPS; ++op) {
        std::string key = "k" + std::to_string(rng.next_below(NUM_KEYS));
        RefKey rk{1, key};
        int t = (int)rng.next_below(10);
        if (t < 6) {
            uint64_t seq = ref[rk].size() + 1;
            EventRecord e = make_ev(1, key, seq, rng.next_u64());
            auto wr = (*store)->write(e);
            REQUIRE(wr.ok());
            ref[rk].push_back(e);
        } else if (t < 7 && !ref[rk].empty()) {
            EventRecord e = ref[rk].back();
            auto wr = (*store)->write(e);
            REQUIRE(wr.ok());
            CHECK(wr->duplicate);
        } else if (t < 9) {
            auto got = (*store)->get(1, key);
            REQUIRE(got.ok());
            if (ref[rk].empty()) CHECK(!got->has_value());
            else {
                REQUIRE(got->has_value());
                CHECK_EQ(got->value().sequence, ref[rk].back().sequence);
            }
        } else {
            (*store)->close();
            auto reopened = Store::open(opts);
            REQUIRE(reopened.ok());
            store = std::move(reopened.value());
        }
    }

    for (auto& kv : ref) {
        auto got = (*store)->get(1, kv.first.key);
        REQUIRE(got.ok());
        REQUIRE(got->has_value());
        CHECK_EQ(got->value().sequence, kv.second.back().sequence);
    }
    (*store)->close();
}
