#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "common/platform/file.hpp"
#include "common/result.hpp"
#include "storage/record.hpp"

namespace ast {

// Segment file layout (see docs/on-disk-format.md):
//   [SegmentHeader: 48 bytes][record frames...]
constexpr uint32_t kSegmentMagic = 0x41534547;   // "ASEG"
constexpr uint32_t kSegmentVersion = 1;
constexpr uint32_t kSegmentHeaderSize = 48;
constexpr uint32_t kSegmentFlagSealed = 1u << 0;

struct SegmentHeader {
    uint32_t magic = kSegmentMagic;
    uint32_t version = kSegmentVersion;
    uint64_t segment_id = 0;
    uint64_t created_ts = 0;
    uint64_t first_seq = 0;
    uint64_t record_count = 0;
    uint32_t flags = 0;
    uint32_t header_crc = 0;

    // Serialize to 48 bytes (bytes [0..43]) and compute header_crc over them.
    void encode(uint8_t out[kSegmentHeaderSize]) const;
    // Parse + validate magic/version/crc. Returns Err::Corrupt on mismatch.
    static Result<SegmentHeader> decode(const uint8_t in[kSegmentHeaderSize]);
};

// A single append-only segment file. Not thread-safe by itself; the Log
// serializes access to the active segment.
class SegmentFile {
public:
    SegmentFile() = default;
    ~SegmentFile() { close(); }

    SegmentFile(const SegmentFile&) = delete;
    SegmentFile& operator=(const SegmentFile&) = delete;
    SegmentFile(SegmentFile&&) noexcept = default;
    SegmentFile& operator=(SegmentFile&&) noexcept = default;

    // Create a fresh segment file (writes and flushes the header).
    static Result<SegmentFile> create(const std::string& path, uint64_t segment_id,
                                      uint64_t created_ts);
    // Open an existing segment file for read/write; returns the parsed header.
    static Result<SegmentFile> open(const std::string& path, SegmentHeader* out_header);

    // Append a full framed record (already encoded by encode_frame).
    Result<uint64_t> append(const uint8_t* frame, uint64_t frame_len);

    Error flush();
    // Seal the segment: set the sealed flag, rewrite the header, and flush.
    Error seal();
    void close();

    uint64_t segment_id() const { return header_.segment_id; }
    uint64_t record_count() const { return header_.record_count; }
    uint64_t data_size() const { return data_size_; } // bytes after the header
    bool sealed() const { return (header_.flags & kSegmentFlagSealed) != 0; }
    const std::string& path() const { return path_; }

    // Read a single record at `offset` (absolute file offset of the frame).
    Result<EventRecord> read_record(uint64_t offset, uint64_t max_key, uint64_t max_payload);

    // Scan all records in order; visitor returns false to stop early.
    Error scan(const std::function<bool(const EventRecord&)>& visitor,
               uint64_t max_key, uint64_t max_payload);

    // Truncate the file back to `new_size` (to isolate a corrupt tail).
    Error truncate_to(uint64_t new_size);

private:
    std::string path_;
    File file_;
    SegmentHeader header_;
    uint64_t data_size_ = 0; // cached; equals file size - header size
    bool dirty_ = false;
};

} // namespace ast
