#include "storage/record.hpp"

#include <cstdio>
#include <random>

#include "common/crc32.hpp"

namespace ast {

Result<Buffer> encode_record_body(const EventRecord& rec) {
    Buffer b;
    b.reserve(64 + rec.key.size() + rec.payload.size());
    b.append_byte(kRecordKindEvent);
    b.append_u64(rec.tenant_id);
    b.append_string(rec.key);
    b.append_u64(rec.timestamp);
    b.append_u64(rec.sequence);
    b.append(rec.event_id.data(), 16);
    b.append_byte(rec.deleted ? 1 : 0);
    b.append_u32((uint32_t)rec.payload.size());
    b.append(rec.payload.data(), rec.payload.size());
    return b;
}

Result<Buffer> encode_frame(const EventRecord& rec) {
    auto body = encode_record_body(rec);
    if (!body.ok()) return body.error();

    const uint32_t len = (uint32_t)(4 + body->size());
    uint32_t c = crc32(body->data(), body->size());

    Buffer frame;
    frame.reserve(8 + body->size());
    frame.append_u32(len);
    frame.append_u32(c);
    frame.append(body->data(), body->size());
    return frame;
}

Result<EventRecord> decode_frame(const uint8_t* p, size_t n, size_t* consumed,
                                 uint64_t max_key_size, uint64_t max_payload_size) {
    if (n < 8) return Error(Err::Corrupt, "frame too short for length+crc");
    uint32_t len = get_le32(p);
    if (len < 4) return Error(Err::Corrupt, "frame length < 4");
    // A complete frame is len(field:4) + crc(4) + body(len-4) = len + 4 bytes.
    if ((size_t)len + 4 > n) return Error(Err::Corrupt, "frame length exceeds available bytes");

    uint32_t stored_crc = get_le32(p + 4);
    const uint8_t* body = p + 8;
    size_t body_len = (size_t)len - 4;

    if (crc32(body, body_len) != stored_crc)
        return Error(Err::Corrupt, "record checksum mismatch");

    Reader r(body, body_len);
    uint8_t kind;
    if (!r.read_u8(kind) || kind != kRecordKindEvent)
        return Error(Err::Corrupt, "unknown record kind");

    EventRecord rec;
    uint64_t tenant;
    if (!r.read_u64(tenant)) return Error(Err::Corrupt, "truncated tenant_id");
    rec.tenant_id = tenant;

    std::string_view key;
    if (!r.read_string(key, (uint32_t)max_key_size))
        return Error(Err::Corrupt, "bad or oversized key");
    if (key.size() > max_key_size) return Error(Err::Corrupt, "key exceeds limit");
    rec.key.assign(key);

    uint64_t ts, seq;
    if (!r.read_u64(ts)) return Error(Err::Corrupt, "truncated timestamp");
    if (!r.read_u64(seq)) return Error(Err::Corrupt, "truncated sequence");
    rec.timestamp = ts;
    rec.sequence = seq;

    const uint8_t* eid = nullptr;
    if (!r.read_bytes(eid, 16)) return Error(Err::Corrupt, "truncated event_id");
    for (int i = 0; i < 16; ++i) rec.event_id[i] = eid[i];

    uint8_t del;
    if (!r.read_u8(del)) return Error(Err::Corrupt, "truncated deleted flag");
    rec.deleted = del != 0;

    uint32_t plen;
    if (!r.read_u32(plen)) return Error(Err::Corrupt, "truncated payload length");
    if ((uint64_t)plen > max_payload_size) return Error(Err::Corrupt, "payload exceeds limit");
    const uint8_t* payload = nullptr;
    if (plen > 0 && !r.read_bytes(payload, plen)) return Error(Err::Corrupt, "truncated payload");
    if (payload) rec.payload.assign(payload, payload + plen);

    if (consumed) *consumed = (size_t)len + 4;
    return rec;
}

std::string event_id_to_hex(const EventId& id) {
    static const char* hex = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = hex[id[i] >> 4];
        out[2 * i + 1] = hex[id[i] & 0xF];
    }
    return out;
}

Result<EventId> event_id_from_hex(const std::string& hex) {
    if (hex.size() != 32) return Error(Err::InvalidArgument, "event_id must be 32 hex chars");
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    EventId id{};
    for (int i = 0; i < 16; ++i) {
        int hi = nib(hex[2 * i]);
        int lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return Error(Err::InvalidArgument, "invalid event_id hex");
        id[i] = (uint8_t)((hi << 4) | lo);
    }
    return id;
}

EventId make_random_event_id() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    EventId id{};
    for (int i = 0; i < 2; ++i) {
        uint64_t v = gen();
        for (int j = 0; j < 8; ++j) id[i * 8 + j] = (uint8_t)(v >> (8 * j));
    }
    return id;
}

} // namespace ast
