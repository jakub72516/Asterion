#include "test_framework.hpp"
#include "test_util.hpp"

#include "core/store.hpp"
#include "storage/record.hpp"

#include <string>

using namespace ast;
using namespace ast::test;

static Store::Options store_opts(const std::string& dir, uint64_t index_max = 1000) {
    Store::Options o;
    o.log.dir = dir;
    o.log.segment_size = 4096;
    o.log.flush_mode = FlushMode::PerWrite;
    o.log.max_key_size = 1024;
    o.log.max_payload_size = 1 << 20;
    o.index_max_entries = index_max;
    o.max_key_size = 1024;
    o.max_payload_size = 1 << 20;
    return o;
}

static EventRecord make_event(uint64_t tenant, const std::string& key, uint64_t seq,
                              const std::string& payload, bool deleted = false) {
    EventRecord r;
    r.tenant_id = tenant;
    r.key = key;
    r.sequence = seq;
    r.timestamp = 1000 + seq;
    r.deleted = deleted;
    r.payload.assign(payload.begin(), payload.end());
    // Deterministic event_id derived from tenant/key/seq.
    for (int i = 0; i < 16; ++i)
        r.event_id[i] = (uint8_t)(tenant ^ (seq * 31) ^ i);
    return r;
}

TEST_CASE(store_write_and_get) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    auto w = (*store)->write(make_event(1, "k1", 1, "hello"));
    REQUIRE(w.ok());
    CHECK(!w->duplicate);

    auto g = (*store)->get(1, "k1");
    REQUIRE(g.ok());
    REQUIRE(g->has_value());
    CHECK_EQ(g->value().sequence, 1u);
    CHECK_EQ(std::string(g->value().payload.begin(), g->value().payload.end()), "hello");

    // Absent key.
    auto miss = (*store)->get(1, "nonexistent");
    REQUIRE(miss.ok());
    CHECK(!miss->has_value());
}

TEST_CASE(store_idempotent_retry) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    auto e = make_event(1, "k", 7, "v");
    auto w1 = (*store)->write(e);
    REQUIRE(w1.ok());
    CHECK(!w1->duplicate);

    auto w2 = (*store)->write(e); // same event_id, same seq
    REQUIRE(w2.ok());
    CHECK(w2->duplicate);

    CHECK_EQ((*store)->total_records(), 1u);
}

TEST_CASE(store_conflict_same_seq_different_id) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    auto e1 = make_event(1, "k", 5, "v1");
    REQUIRE((*store)->write(e1).ok());

    auto e2 = make_event(1, "k", 5, "v2");
    e2.event_id[0] ^= 0xFF; // different event_id
    auto w2 = (*store)->write(e2);
    CHECK(!w2.ok());
    CHECK(w2.code() == Err::Conflict);
}

TEST_CASE(store_autoassign_sequence) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    auto e1 = make_event(1, "k", 0, "a"); // seq 0 -> auto 1
    auto w1 = (*store)->write(e1);
    REQUIRE(w1.ok());
    CHECK_EQ(w1->sequence, 1u);

    auto e2 = make_event(1, "k", 0, "b"); // auto 2
    auto w2 = (*store)->write(e2);
    REQUIRE(w2.ok());
    CHECK_EQ(w2->sequence, 2u);
}

TEST_CASE(store_key_history_ordered) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    for (int s = 1; s <= 5; ++s)
        REQUIRE((*store)->write(make_event(1, "k", s, "v" + std::to_string(s))).ok());

    std::vector<uint64_t> seqs;
    auto e = (*store)->scan_key(1, "k", [&](const EventRecord& r) {
        seqs.push_back(r.sequence);
        return true;
    });
    REQUIRE(e.ok());
    REQUIRE(seqs.size() == 5u);
    for (int i = 0; i < 5; ++i) CHECK_EQ(seqs[i], (uint64_t)(i + 1));
}

TEST_CASE(store_tenant_time_range_scan) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    for (int s = 1; s <= 10; ++s) {
        auto e = make_event(1, "k" + std::to_string(s), s, "v");
        e.timestamp = 1000 + s;
        REQUIRE((*store)->write(e).ok());
    }

    uint64_t count = 0;
    ScanControl ctrl;
    auto e = (*store)->scan_tenant(1, 1003, 1007, [&](const EventRecord&) {
        count++;
        return true;
    }, &ctrl);
    REQUIRE(e.ok());
    CHECK_EQ(count, 5u); // ts 1003..1007 inclusive
}

TEST_CASE(store_scan_cancellation) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    for (int s = 1; s <= 100; ++s)
        REQUIRE((*store)->write(make_event(1, "k" + std::to_string(s), s, "v")).ok());

    uint64_t count = 0;
    ScanControl ctrl;
    ctrl.cancelled = [&] { return count >= 3; };
    auto e = (*store)->scan_tenant(1, 0, 1ULL << 60, [&](const EventRecord&) {
        count++;
        return true;
    }, &ctrl);
    REQUIRE(e.ok());
    CHECK(count <= 4); // cancelled near the start
}

TEST_CASE(store_deletion_marker) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str()));
    REQUIRE(store.ok());

    REQUIRE((*store)->write(make_event(1, "k", 1, "v")).ok());
    REQUIRE((*store)->write(make_event(1, "k", 2, "", true)).ok()); // tombstone

    auto g = (*store)->get(1, "k");
    REQUIRE(g.ok());
    REQUIRE(g->has_value());
    CHECK(g->value().deleted);
}

TEST_CASE(store_persistence_across_reopen) {
    TempDir tmp;
    {
        auto store = Store::open(store_opts(tmp.str()));
        REQUIRE(store.ok());
        for (int s = 1; s <= 50; ++s)
            REQUIRE((*store)->write(make_event(1, "k", s, "v" + std::to_string(s))).ok());
        (*store)->close();
    }
    {
        auto store = Store::open(store_opts(tmp.str()));
        REQUIRE(store.ok());
        CHECK_EQ((*store)->total_records(), 50u);
        auto g = (*store)->get(1, "k");
        REQUIRE(g.ok());
        REQUIRE(g->has_value());
        CHECK_EQ(g->value().sequence, 50u);
        (*store)->close();
    }
}

TEST_CASE(store_bounded_index) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str(), 8));
    REQUIRE(store.ok());

    for (int s = 1; s <= 100; ++s) {
        REQUIRE((*store)->write(make_event(1, "k" + std::to_string(s), 1, "v")).ok());
    }
    CHECK((*store)->index_size() <= 8u);

    // A cold key (evicted from the index) must still resolve via scan.
    auto g = (*store)->get(1, "k1");
    REQUIRE(g.ok());
    REQUIRE(g->has_value());
    CHECK_EQ(g->value().key, "k1");
}

TEST_CASE(store_compaction) {
    TempDir tmp;
    auto store = Store::open(store_opts(tmp.str(), 4096)); // tiny segments force many
    REQUIRE(store.ok());

    // Large payloads force multiple sealed segments.
    std::string big_payload(200, 'x');
    for (int key = 1; key <= 20; ++key) {
        for (int seq = 1; seq <= 4; ++seq) {
            REQUIRE((*store)->write(make_event(1, "k" + std::to_string(key), seq, big_payload)).ok());
        }
    }

    uint64_t before = (*store)->total_records();
    CHECK_EQ(before, 80u);

    auto st = (*store)->compact(0);
    REQUIRE(st.ok());
    CHECK(!st->nothing_to_do);
    CHECK(st->records_in > 0);
    CHECK(st->records_out < st->records_in); // dedup reclaimed space

    // The critical invariant: the latest version of every key is preserved.
    for (int key = 1; key <= 20; ++key) {
        auto g = (*store)->get(1, "k" + std::to_string(key));
        REQUIRE(g.ok());
        REQUIRE(g->has_value());
        CHECK_EQ(g->value().sequence, 4u);
    }

    CHECK((*store)->total_records() < before);
}
