#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/bytes.hpp"
#include "common/result.hpp"

namespace ast {

using EventId = std::array<uint8_t, 16>;

// A logical event, matching the spec's (tenant_id, key, timestamp,
// sequence_number, event_id, payload). Immutable once written.
struct EventRecord {
    uint64_t tenant_id = 0;
    std::string key;
    uint64_t timestamp = 0;  // wall-clock microseconds (UTC)
    uint64_t sequence = 0;   // per-key ordering number
    EventId event_id{};      // 16-byte unique id (idempotency key)
    bool deleted = false;    // true => tombstone (payload empty)
    std::vector<uint8_t> payload;

    // Location after append (not part of the encoding).
    uint64_t segment_id = 0;
    uint64_t offset = 0;     // byte offset of the frame's length field
    uint64_t frame_size = 0; // total bytes of the framed record on disk
};

// Record body kind tags.
constexpr uint8_t kRecordKindEvent = 1;

// Encode a record body (everything after the frame length field).
// Returns the encoded body bytes (crc NOT included).
Result<Buffer> encode_record_body(const EventRecord& rec);

// Encode a full framed record: [len:u32][crc:u32][body...] where
// len = 4 + body.size(). This is what gets appended to a segment.
Result<Buffer> encode_frame(const EventRecord& rec);

// Decode a framed record starting at `p`. On success, `*consumed` is set to
// the total frame size and the record (without segment/offset) is returned.
// Verifies the length field and the CRC; returns Err::Corrupt on mismatch.
Result<EventRecord> decode_frame(const uint8_t* p, size_t n, size_t* consumed,
                                 uint64_t max_key_size, uint64_t max_payload_size);

// event_id <-> hex string (32 hex chars, no separators).
std::string event_id_to_hex(const EventId& id);
Result<EventId> event_id_from_hex(const std::string& hex);

// Generate a random (non-cryptographic for tests / CLI defaults) event id.
EventId make_random_event_id();

} // namespace ast
