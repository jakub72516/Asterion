#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/bytes.hpp"
#include "common/result.hpp"
#include "storage/record.hpp"

namespace ast {
namespace proto {

// Wire frame layout (all integers little-endian):
//   magic:u32 version:u8 type:u8 flags:u16 length:u32 crc:u32 body...
// length = body size; crc = crc32(body). Total header = 16 bytes.
constexpr uint32_t kMagic = 0x41535431; // "AST1"
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 16;

enum MsgType : uint8_t {
    Ping = 0x01,
    Pong = 0x02,
    StatusReq = 0x03,
    StatusResp = 0x04,
    AppendReq = 0x10,
    AppendResp = 0x11,
    GetReq = 0x12,
    GetResp = 0x13,
    ScanReq = 0x14,
    ScanResp = 0x15,
    ScanEnd = 0x16,
    DeleteReq = 0x17,
    ShutdownReq = 0x20,
    ShutdownResp = 0x21,

    // Replication (leader <-> follower).
    ReplicateReq = 0x30,
    ReplicateAck = 0x31,
    SyncReq = 0x32,
    SyncResp = 0x33,
    SyncEnd = 0x34,

    ErrorResp = 0x7F,
};

// Message flags (u16).
constexpr uint16_t kFlagMore = 0x0001; // more scan results follow

struct Frame {
    uint8_t type = 0;
    uint16_t flags = 0;
    std::vector<uint8_t> body;
};

// Encode a full frame (header + body) ready to send.
Buffer encode_frame(uint8_t type, uint16_t flags, const uint8_t* body, size_t body_len);
Buffer encode_frame(uint8_t type, uint16_t flags, const Buffer& body);

// Parse one frame from a byte stream. Sets *consumed to total frame bytes
// (header + body) on success. Validates magic, version, crc, and the max size.
Result<Frame> parse_frame(const uint8_t* data, size_t len, size_t* consumed,
                          uint64_t max_frame_size);

// --- Request/response body codecs ---

// AppendReq / AppendResp use the record body encoding for events.
Buffer encode_append_request(const EventRecord& rec);
Result<EventRecord> decode_append_request(const uint8_t* body, size_t len,
                                          uint64_t max_key, uint64_t max_payload);

struct AppendResponse {
    uint8_t code = 0;    // 0 = ok, 1 = duplicate, otherwise error code
    uint64_t sequence = 0;
};
Buffer encode_append_response(const AppendResponse& r);
Result<AppendResponse> decode_append_response(const uint8_t* body, size_t len);

struct GetRequest {
    uint64_t tenant = 0;
    std::string key;
};
Buffer encode_get_request(const GetRequest& r);
Result<GetRequest> decode_get_request(const uint8_t* body, size_t len, uint64_t max_key);

// GetResp: code u8 (0 = found, 1 = not found, 2 = tombstone), then event body if found.
Buffer encode_get_response(uint8_t code, const EventRecord* rec);
Result<uint8_t> decode_get_response_code(const uint8_t* body, size_t len);
Result<EventRecord> decode_get_response_event(const uint8_t* body, size_t len,
                                              uint64_t max_key, uint64_t max_payload);

struct ScanRequest {
    uint64_t tenant = 0;
    uint64_t from_ts = 0;
    uint64_t to_ts = 0;
    uint32_t max_results = 1000;
};
Buffer encode_scan_request(const ScanRequest& r);
Result<ScanRequest> decode_scan_request(const uint8_t* body, size_t len);

// ScanResp: one event body (record body encoding).
Buffer encode_scan_response(const EventRecord& rec);
Result<EventRecord> decode_scan_response(const uint8_t* body, size_t len,
                                         uint64_t max_key, uint64_t max_payload);

// ScanEnd: reason u8 (0 = complete, 1 = limit reached, 2 = cancelled).
Buffer encode_scan_end(uint8_t reason);
Result<uint8_t> decode_scan_end(const uint8_t* body, size_t len);

// ErrorResp: code u8 + message string.
Buffer encode_error_response(uint8_t code, const std::string& msg);

// StatusResp body (JSON-ish text) for the CLI. Encoded as a UTF-8 string.
Buffer encode_status_response(const std::string& json);

// Build the payload field of an EventRecord from raw bytes (CLI helper).
} // namespace proto

// Replication codecs.
namespace proto {

// ReplicateReq: partition u32 + event record body.
Buffer encode_replicate_request(uint32_t partition, const EventRecord& rec);
Result<std::pair<uint32_t, EventRecord>> decode_replicate_request(const uint8_t* body, size_t len,
                                                                  uint64_t max_key, uint64_t max_payload);
// ReplicateAck: partition u32 + ok u8.
Buffer encode_replicate_ack(uint32_t partition, uint8_t ok);
Result<uint8_t> decode_replicate_ack(const uint8_t* body, size_t len);

// SyncReq: partition u32.
Buffer encode_sync_request(uint32_t partition);
Result<uint32_t> decode_sync_request(const uint8_t* body, size_t len);
// SyncResp: one event body.
Buffer encode_sync_response(const EventRecord& rec);
Result<EventRecord> decode_sync_response(const uint8_t* body, size_t len,
                                         uint64_t max_key, uint64_t max_payload);
// SyncEnd: count u32.
Buffer encode_sync_end(uint32_t count);

} // namespace proto
} // namespace ast
