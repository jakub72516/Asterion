#include "protocol/protocol.hpp"

#include "common/crc32.hpp"

namespace ast {
namespace proto {

Buffer encode_frame(uint8_t type, uint16_t flags, const uint8_t* body, size_t body_len) {
    Buffer f;
    f.reserve(kHeaderSize + body_len);
    f.append_u32(kMagic);
    f.append_byte(kVersion);
    f.append_byte(type);
    f.append_u16(flags);
    f.append_u32((uint32_t)body_len);
    f.append_u32(crc32(body, body_len));
    f.append(body, body_len);
    return f;
}

Buffer encode_frame(uint8_t type, uint16_t flags, const Buffer& body) {
    return encode_frame(type, flags, body.data(), body.size());
}

Result<Frame> parse_frame(const uint8_t* data, size_t len, size_t* consumed,
                          uint64_t max_frame_size) {
    if (len < kHeaderSize) return Error(Err::InvalidArgument, "frame shorter than header");
    uint32_t magic = get_le32(data + 0);
    uint8_t version = data[4];
    uint8_t type = data[5];
    uint16_t flags = get_le16(data + 6);
    uint32_t body_len = get_le32(data + 8);
    uint32_t crc = get_le32(data + 12);

    if (magic != kMagic) return Error(Err::InvalidArgument, "bad frame magic");
    if (version != kVersion) return Error(Err::Unsupported, "unsupported protocol version");
    if ((uint64_t)body_len > max_frame_size)
        return Error(Err::LimitExceeded, "frame body exceeds max_frame_size");
    uint64_t total = (uint64_t)kHeaderSize + body_len;
    if (total > len) return Error(Err::InvalidArgument, "frame body truncated");
    if (crc32(data + kHeaderSize, body_len) != crc)
        return Error(Err::Corrupt, "frame checksum mismatch");

    Frame f;
    f.type = type;
    f.flags = flags;
    f.body.assign(data + kHeaderSize, data + kHeaderSize + body_len);
    if (consumed) *consumed = total;
    return f;
}

// ---------------------------------------------------------------------------
// Append
// ---------------------------------------------------------------------------

Buffer encode_append_request(const EventRecord& rec) {
    auto body = encode_record_body(rec);
    // body already holds the record encoding; wrap directly.
    Buffer b;
    b.append(body->data(), body->size());
    return b;
}

Result<EventRecord> decode_append_request(const uint8_t* body, size_t len,
                                          uint64_t max_key, uint64_t max_payload) {
    Reader r(body, len);
    uint8_t kind;
    if (!r.read_u8(kind) || kind != kRecordKindEvent)
        return Error(Err::InvalidArgument, "append body must be an event record");

    EventRecord rec;
    uint64_t tenant;
    if (!r.read_u64(tenant)) return Error(Err::InvalidArgument, "truncated tenant_id");
    rec.tenant_id = tenant;

    std::string_view key;
    if (!r.read_string(key, (uint32_t)max_key)) return Error(Err::InvalidArgument, "bad key");
    rec.key.assign(key);

    if (!r.read_u64(rec.timestamp)) return Error(Err::InvalidArgument, "truncated timestamp");
    if (!r.read_u64(rec.sequence)) return Error(Err::InvalidArgument, "truncated sequence");

    const uint8_t* eid = nullptr;
    if (!r.read_bytes(eid, 16)) return Error(Err::InvalidArgument, "truncated event_id");
    for (int i = 0; i < 16; ++i) rec.event_id[i] = eid[i];

    uint8_t del;
    if (!r.read_u8(del)) return Error(Err::InvalidArgument, "truncated deleted");
    rec.deleted = del != 0;

    uint32_t plen;
    if (!r.read_u32(plen)) return Error(Err::InvalidArgument, "truncated payload length");
    if ((uint64_t)plen > max_payload) return Error(Err::LimitExceeded, "payload too large");
    const uint8_t* payload = nullptr;
    if (plen > 0 && !r.read_bytes(payload, plen)) return Error(Err::InvalidArgument, "truncated payload");
    if (payload) rec.payload.assign(payload, payload + plen);
    return rec;
}

Buffer encode_append_response(const AppendResponse& r) {
    Buffer b;
    b.append_byte(r.code);
    b.append_u64(r.sequence);
    return b;
}

Result<AppendResponse> decode_append_response(const uint8_t* body, size_t len) {
    Reader r(body, len);
    AppendResponse a;
    if (!r.read_u8(a.code)) return Error(Err::Corrupt, "truncated append response");
    if (!r.read_u64(a.sequence)) return Error(Err::Corrupt, "truncated append seq");
    return a;
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------

Buffer encode_get_request(const GetRequest& r) {
    Buffer b;
    b.append_u64(r.tenant);
    b.append_string(r.key);
    return b;
}

Result<GetRequest> decode_get_request(const uint8_t* body, size_t len, uint64_t max_key) {
    Reader r(body, len);
    GetRequest g;
    if (!r.read_u64(g.tenant)) return Error(Err::InvalidArgument, "truncated tenant");
    std::string_view key;
    if (!r.read_string(key, (uint32_t)max_key)) return Error(Err::InvalidArgument, "bad key");
    g.key.assign(key);
    return g;
}

Buffer encode_get_response(uint8_t code, const EventRecord* rec) {
    Buffer b;
    b.append_byte(code);
    if (rec) {
        auto body = encode_record_body(*rec);
        b.append(body->data(), body->size());
    }
    return b;
}

Result<uint8_t> decode_get_response_code(const uint8_t* body, size_t len) {
    if (len < 1) return Error(Err::Corrupt, "truncated get response");
    return body[0];
}

Result<EventRecord> decode_get_response_event(const uint8_t* body, size_t len,
                                              uint64_t max_key, uint64_t max_payload) {
    if (len < 1) return Error(Err::Corrupt, "truncated get response");
    return decode_append_request(body + 1, len - 1, max_key, max_payload);
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

Buffer encode_scan_request(const ScanRequest& r) {
    Buffer b;
    b.append_u64(r.tenant);
    b.append_u64(r.from_ts);
    b.append_u64(r.to_ts);
    b.append_u32(r.max_results);
    return b;
}

Result<ScanRequest> decode_scan_request(const uint8_t* body, size_t len) {
    Reader r(body, len);
    ScanRequest s;
    if (!r.read_u64(s.tenant)) return Error(Err::InvalidArgument, "truncated tenant");
    if (!r.read_u64(s.from_ts)) return Error(Err::InvalidArgument, "truncated from_ts");
    if (!r.read_u64(s.to_ts)) return Error(Err::InvalidArgument, "truncated to_ts");
    if (!r.read_u32(s.max_results)) return Error(Err::InvalidArgument, "truncated max_results");
    return s;
}

Buffer encode_scan_response(const EventRecord& rec) {
    auto body = encode_record_body(rec);
    Buffer b;
    b.append(body->data(), body->size());
    return b;
}

Result<EventRecord> decode_scan_response(const uint8_t* body, size_t len,
                                         uint64_t max_key, uint64_t max_payload) {
    return decode_append_request(body, len, max_key, max_payload);
}

Buffer encode_scan_end(uint8_t reason) {
    Buffer b;
    b.append_byte(reason);
    return b;
}

Result<uint8_t> decode_scan_end(const uint8_t* body, size_t len) {
    if (len < 1) return Error(Err::Corrupt, "truncated scan end");
    return body[0];
}

Buffer encode_error_response(uint8_t code, const std::string& msg) {
    Buffer b;
    b.append_byte(code);
    b.append_string(msg);
    return b;
}

Buffer encode_status_response(const std::string& json) {
    Buffer b;
    b.append_string(json);
    return b;
}

// ---------------------------------------------------------------------------
// Replication
// ---------------------------------------------------------------------------

Buffer encode_replicate_request(uint32_t partition, const EventRecord& rec) {
    Buffer b;
    b.append_u32(partition);
    auto body = encode_record_body(rec);
    b.append(body->data(), body->size());
    return b;
}

Result<std::pair<uint32_t, EventRecord>> decode_replicate_request(const uint8_t* body, size_t len,
                                                                  uint64_t max_key, uint64_t max_payload) {
    if (len < 4) return Error(Err::Corrupt, "truncated replicate partition");
    uint32_t partition = get_le32(body);
    auto rec = decode_append_request(body + 4, len - 4, max_key, max_payload);
    if (!rec.ok()) return rec.error();
    return std::make_pair(partition, rec.value());
}

Buffer encode_replicate_ack(uint32_t partition, uint8_t okflag) {
    Buffer b;
    b.append_u32(partition);
    b.append_byte(okflag);
    return b;
}

Result<uint8_t> decode_replicate_ack(const uint8_t* body, size_t len) {
    if (len < 5) return Error(Err::Corrupt, "truncated replicate ack");
    return body[4];
}

Buffer encode_sync_request(uint32_t partition) {
    Buffer b;
    b.append_u32(partition);
    return b;
}

Result<uint32_t> decode_sync_request(const uint8_t* body, size_t len) {
    if (len < 4) return Error(Err::Corrupt, "truncated sync request");
    return get_le32(body);
}

Buffer encode_sync_response(const EventRecord& rec) {
    auto body = encode_record_body(rec);
    Buffer b;
    b.append(body->data(), body->size());
    return b;
}

Result<EventRecord> decode_sync_response(const uint8_t* body, size_t len,
                                         uint64_t max_key, uint64_t max_payload) {
    return decode_append_request(body, len, max_key, max_payload);
}

Buffer encode_sync_end(uint32_t count) {
    Buffer b;
    b.append_u32(count);
    return b;
}

} // namespace proto
} // namespace ast
