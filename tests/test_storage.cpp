#include "test_framework.hpp"
#include "test_util.hpp"

#include "common/bytes.hpp"
#include "common/clock.hpp"
#include "common/crc32.hpp"
#include "common/json.hpp"
#include "common/config.hpp"
#include "common/platform/file.hpp"
#include "storage/record.hpp"
#include "storage/segment.hpp"
#include "storage/log.hpp"
#include "storage/key_index.hpp"

#include <cstring>
#include <fstream>
#include <string>

using namespace ast;
using namespace ast::test;

// ---------------------------------------------------------------------------
// crc32
// ---------------------------------------------------------------------------

TEST_CASE(crc32_known_value) {
    const char* s = "123456789";
    CHECK_EQ(crc32((const uint8_t*)s, 9), 0xCBF43926u);
}

TEST_CASE(crc32_empty) {
    CHECK_EQ(crc32(nullptr, 0), 0u);
}

// ---------------------------------------------------------------------------
// record encoding
// ---------------------------------------------------------------------------

static EventRecord sample_record(uint64_t seq) {
    EventRecord r;
    r.tenant_id = 42;
    r.key = "orders/1234";
    r.timestamp = 1'700'000'000'000'000ULL;
    r.sequence = seq;
    for (int i = 0; i < 16; ++i) r.event_id[i] = (uint8_t)(i * 7 + seq);
    r.payload = {0xde, 0xad, 0xbe, 0xef};
    return r;
}

TEST_CASE(record_roundtrip) {
    auto rec = sample_record(1);
    auto frame = encode_frame(rec);
    REQUIRE(frame.ok());

    size_t consumed = 0;
    auto dec = decode_frame(frame->data(), frame->size(), &consumed, 1024, 1 << 20);
    REQUIRE(dec.ok());
    CHECK_EQ(consumed, frame->size());
    CHECK_EQ(dec->tenant_id, 42);
    CHECK_EQ(dec->key, std::string("orders/1234"));
    CHECK_EQ(dec->sequence, 1u);
    CHECK(dec->event_id == rec.event_id);
    CHECK(dec->payload == rec.payload);
    CHECK(!dec->deleted);
}

TEST_CASE(record_corruption_detected) {
    auto rec = sample_record(2);
    auto frame = encode_frame(rec);
    REQUIRE(frame.ok());

    // Flip a byte in the payload area.
    frame->vec()[frame->size() - 1] ^= 0xFF;

    size_t consumed = 0;
    auto dec = decode_frame(frame->data(), frame->size(), &consumed, 1024, 1 << 20);
    CHECK(!dec.ok());
    CHECK(dec.code() == Err::Corrupt);
}

TEST_CASE(record_truncation_detected) {
    auto rec = sample_record(3);
    auto frame = encode_frame(rec);
    REQUIRE(frame.ok());

    size_t consumed = 0;
    auto dec = decode_frame(frame->data(), frame->size() - 3, &consumed, 1024, 1 << 20);
    CHECK(!dec.ok());
}

TEST_CASE(event_id_hex_roundtrip) {
    auto id = sample_record(1).event_id;
    std::string hex = event_id_to_hex(id);
    CHECK_EQ(hex.size(), 32u);
    auto back = event_id_from_hex(hex);
    REQUIRE(back.ok());
    CHECK(back.value() == id);
}

TEST_CASE(event_id_hex_invalid) {
    auto bad = event_id_from_hex("zz");
    CHECK(!bad.ok());
}

// ---------------------------------------------------------------------------
// json / config
// ---------------------------------------------------------------------------

TEST_CASE(json_parse_basic) {
    auto j = parse_json(R"({"a":1,"b":[true,null,"x"],"c":{"d":2.5}})");
    REQUIRE(j.ok());
    CHECK(j->is_object());
    CHECK_EQ(j->get_int("a").value_or(0), 1);
    CHECK(j->find("b")->is_array());
    CHECK_EQ(j->find("b")->as_array().size(), 3u);
    CHECK_EQ(j->find("c")->get_double("d").value_or(0), 2.5);
}

TEST_CASE(json_parse_invalid) {
    auto j = parse_json("{a:1}");
    CHECK(!j.ok());
}

TEST_CASE(config_validation) {
    TempDir tmp;
    std::string path = tmp.str() + "\\cfg.json";
    std::string json = R"({
        "node_id": 1,
        "data_dir": "data",
        "network": { "listen_port": 7100, "io_threads": 2 },
        "replication": {
            "peers": [
                {"node_id":1,"host":"127.0.0.1","port":7100},
                {"node_id":2,"host":"127.0.0.1","port":7101}
            ]
        }
    })";
    {
        std::ofstream f(path);
        f << json;
    }
    auto cfg = Config::load(path);
    REQUIRE(cfg.ok());
    CHECK_EQ(cfg->node_id, 1u);
    CHECK_EQ(cfg->network.listen_port, 7100u);
    CHECK_EQ(cfg->replication.peers.size(), 2u);
    CHECK(cfg->validate().ok());
}

TEST_CASE(config_rejects_bad_node_id) {
    TempDir tmp;
    std::string path = tmp.str() + "\\cfg.json";
    {
        std::ofstream f(path);
        f << R"({"node_id":0,"data_dir":"d"})";
    }
    auto cfg = Config::load(path);
    CHECK(!cfg.ok());
}

// ---------------------------------------------------------------------------
// segment file
// ---------------------------------------------------------------------------

TEST_CASE(segment_append_and_read) {
    TempDir tmp;
    std::string path = tmp.str() + "\\segment_0000000000000001.astseg";
    auto seg = SegmentFile::create(path, 1, wall_time_us());
    REQUIRE(seg.ok());

    auto rec = sample_record(1);
    auto frame = encode_frame(rec);
    REQUIRE(frame.ok());

    auto off = seg->append(frame->data(), frame->size());
    REQUIRE(off.ok());

    auto got = seg->read_record(off.value(), 1024, 1 << 20);
    REQUIRE(got.ok());
    CHECK_EQ(got->sequence, 1u);
    CHECK_EQ(got->key, std::string("orders/1234"));

    CHECK_EQ(seg->record_count(), 1u);
    seg->close();
}

TEST_CASE(segment_seal_and_reopen) {
    TempDir tmp;
    std::string path = tmp.str() + "\\segment_0000000000000002.astseg";
    {
        auto seg = SegmentFile::create(path, 2, wall_time_us());
        REQUIRE(seg.ok());
        auto rec = sample_record(1);
        auto frame = encode_frame(rec);
        REQUIRE(frame.ok());
        REQUIRE(seg->append(frame->data(), frame->size()).ok());
        REQUIRE(seg->seal().ok());
        CHECK(seg->sealed());
    }

    SegmentHeader hdr;
    auto reopened = SegmentFile::open(path, &hdr);
    REQUIRE(reopened.ok());
    CHECK(reopened->sealed());
    CHECK_EQ(hdr.segment_id, 2u);
    CHECK_EQ(reopened->record_count(), 1u);
}

TEST_CASE(segment_scan_all) {
    TempDir tmp;
    std::string path = tmp.str() + "\\segment_0000000000000003.astseg";
    auto seg = SegmentFile::create(path, 3, wall_time_us());
    REQUIRE(seg.ok());
    for (int i = 1; i <= 5; ++i) {
        auto rec = sample_record(i);
        auto frame = encode_frame(rec);
        REQUIRE(frame.ok());
        REQUIRE(seg->append(frame->data(), frame->size()).ok());
    }
    int count = 0;
    uint64_t last_seq = 0;
    auto e = seg->scan([&](const EventRecord& r) {
        count++;
        last_seq = r.sequence;
        return true;
    }, 1024, 1 << 20);
    CHECK(e.ok());
    CHECK_EQ(count, 5);
    CHECK_EQ(last_seq, 5u);
}

// ---------------------------------------------------------------------------
// log (segmented, append + recovery)
// ---------------------------------------------------------------------------

static Log::Options log_opts(const std::string& dir, uint64_t seg_size = 4096) {
    Log::Options o;
    o.dir = dir;
    o.segment_size = seg_size;
    o.flush_mode = FlushMode::PerWrite;
    o.max_key_size = 1024;
    o.max_payload_size = 1 << 20;
    return o;
}

TEST_CASE(log_append_and_reopen) {
    TempDir tmp;
    auto log = Log::open(log_opts(tmp.str()));
    REQUIRE(log.ok());
    for (int i = 1; i <= 20; ++i) {
        auto pos = (*log)->append(sample_record(i));
        REQUIRE(pos.ok());
    }
    CHECK_EQ((*log)->total_records(), 20u);
    (*log)->close();

    RecoveryInfo ri;
    auto reopened = Log::open(log_opts(tmp.str()), &ri);
    REQUIRE(reopened.ok());
    CHECK_EQ((*reopened)->total_records(), 20u);
    CHECK_EQ(ri.records_recovered, 20u);
    (*reopened)->close();
}

TEST_CASE(log_rotation_occurs) {
    TempDir tmp;
    auto log = Log::open(log_opts(tmp.str(), 1024)); // tiny segments force rotation
    REQUIRE(log.ok());
    for (int i = 1; i <= 50; ++i) {
        REQUIRE((*log)->append(sample_record(i)).ok());
    }
    CHECK((*log)->segment_count() > 1);
    (*log)->close();
}

TEST_CASE(log_recovery_isolates_corrupt_tail) {
    TempDir tmp;
    // Create a log with data, close, then manually corrupt the tail.
    {
        auto log = Log::open(log_opts(tmp.str(), 1 << 20));
        REQUIRE(log.ok());
        for (int i = 1; i <= 5; ++i) REQUIRE((*log)->append(sample_record(i)).ok());
        (*log)->close();
    }
    // Find the single segment and append garbage.
    auto files = list_files(tmp.str(), ".astseg");
    REQUIRE(files.size() == 1);
    std::string path = tmp.str() + "\\" + files[0];
    {
        auto f = File::open_rw(path);
        REQUIRE(f.ok());
        uint8_t garbage[64];
        memset(garbage, 0xAB, sizeof(garbage));
        REQUIRE(f->write(garbage, sizeof(garbage)).ok());
        f->flush();
    }

    RecoveryInfo ri;
    auto reopened = Log::open(log_opts(tmp.str()), &ri);
    REQUIRE(reopened.ok());
    CHECK_EQ((*reopened)->total_records(), 5u);
    CHECK(ri.records_dropped >= 1 || !ri.issues.empty());
    (*reopened)->close();
}

// ---------------------------------------------------------------------------
// key index (bounded memory)
// ---------------------------------------------------------------------------

TEST_CASE(key_index_lru_eviction) {
    KeyIndex idx(3);
    for (int i = 0; i < 5; ++i) {
        IndexKey k{(uint64_t)i, "k"};
        KeyIndexEntry e;
        e.latest_seq = i + 1;
        idx.put(k, e);
    }
    CHECK_EQ(idx.size(), 3u);
    CHECK(idx.evictions() >= 1);
    // The first two keys should have been evicted.
    CHECK(!idx.contains(IndexKey{0, "k"}));
    CHECK(!idx.contains(IndexKey{1, "k"}));
    CHECK(idx.contains(IndexKey{4, "k"}));
}

TEST_CASE(key_index_get_updates_recency) {
    KeyIndex idx(2);
    idx.put(IndexKey{1, "a"}, KeyIndexEntry{});
    idx.put(IndexKey{2, "b"}, KeyIndexEntry{});
    // Touch key a, making b least-recently-used.
    auto got = idx.get(IndexKey{1, "a"});
    CHECK(got.has_value());
    idx.put(IndexKey{3, "c"}, KeyIndexEntry{});
    CHECK(idx.contains(IndexKey{1, "a"}));
    CHECK(!idx.contains(IndexKey{2, "b"}));
}
