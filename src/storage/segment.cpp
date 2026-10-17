#include "storage/segment.hpp"

#include <cstring>

#include "common/bytes.hpp"
#include "common/crc32.hpp"

namespace ast {

void SegmentHeader::encode(uint8_t out[kSegmentHeaderSize]) const {
    std::memset(out, 0, kSegmentHeaderSize);
    put_le32(out + 0, magic);
    put_le32(out + 4, version);
    put_le64(out + 8, segment_id);
    put_le64(out + 16, created_ts);
    put_le64(out + 24, first_seq);
    put_le64(out + 32, record_count);
    put_le32(out + 40, flags);
    uint32_t c = crc32(out, 44);
    put_le32(out + 44, c);
}

Result<SegmentHeader> SegmentHeader::decode(const uint8_t in[kSegmentHeaderSize]) {
    SegmentHeader h;
    h.magic = get_le32(in + 0);
    h.version = get_le32(in + 4);
    h.segment_id = get_le64(in + 8);
    h.created_ts = get_le64(in + 16);
    h.first_seq = get_le64(in + 24);
    h.record_count = get_le64(in + 32);
    h.flags = get_le32(in + 40);
    h.header_crc = get_le32(in + 44);

    if (h.magic != kSegmentMagic) return Error(Err::Corrupt, "bad segment magic");
    if (h.version != kSegmentVersion) return Error(Err::Corrupt, "unsupported segment version");
    uint32_t expect = crc32(in, 44);
    if (h.header_crc != expect) return Error(Err::Corrupt, "segment header checksum mismatch");
    return h;
}

Result<SegmentFile> SegmentFile::create(const std::string& path, uint64_t segment_id,
                                        uint64_t created_ts) {
    auto f = File::create(path);
    if (!f.ok()) return f.error();

    SegmentFile s;
    s.path_ = path;
    s.file_ = std::move(f.value());
    s.header_.segment_id = segment_id;
    s.header_.created_ts = created_ts;
    s.header_.record_count = 0;

    uint8_t hdr[kSegmentHeaderSize];
    s.header_.encode(hdr);
    auto w = s.file_.write_at(hdr, kSegmentHeaderSize, 0);
    if (!w.ok()) return w.error();
    if (w.value() != kSegmentHeaderSize) return Error(Err::IoError, "short header write");
    auto fl = s.file_.flush();
    if (!fl.ok()) return fl;
    s.data_size_ = 0;
    return s;
}

Result<SegmentFile> SegmentFile::open(const std::string& path, SegmentHeader* out_header) {
    auto f = File::open_rw(path);
    if (!f.ok()) return f.error();

    uint8_t hdr[kSegmentHeaderSize];
    auto r = f.value().read(hdr, kSegmentHeaderSize, 0);
    if (!r.ok()) return r.error();
    if (r.value() != kSegmentHeaderSize) return Error(Err::Corrupt, "segment too short for header");

    auto parsed = SegmentHeader::decode(hdr);
    if (!parsed.ok()) return parsed.error();

    SegmentFile s;
    s.path_ = path;
    s.file_ = std::move(f.value());
    s.header_ = parsed.value();

    auto sz = s.file_.size();
    if (!sz.ok()) return sz.error();
    s.data_size_ = sz.value() - kSegmentHeaderSize;

    if (out_header) *out_header = s.header_;
    return s;
}

Result<uint64_t> SegmentFile::append(const uint8_t* frame, uint64_t frame_len) {
    auto w = file_.write(frame, frame_len);
    if (!w.ok()) return w.error();
    if (w.value() != frame_len) return Error(Err::IoError, "short append");
    data_size_ += frame_len;
    header_.record_count += 1;
    dirty_ = true;
    return (uint64_t)(data_size_ - frame_len) + kSegmentHeaderSize; // absolute offset
}

Error SegmentFile::flush() {
    auto e = file_.flush();
    if (e.ok()) dirty_ = false;
    return e;
}

Error SegmentFile::seal() {
    header_.flags |= kSegmentFlagSealed;
    uint8_t hdr[kSegmentHeaderSize];
    header_.encode(hdr);
    auto w = file_.write_at(hdr, kSegmentHeaderSize, 0);
    if (!w.ok()) return w.error();
    if (w.value() != kSegmentHeaderSize) return Error(Err::IoError, "short header write on seal");
    auto e = file_.flush();
    if (e.ok()) dirty_ = false;
    return e;
}

void SegmentFile::close() {
    if (file_.is_open()) {
        if (dirty_) file_.flush();
        file_.close();
    }
}

Result<EventRecord> SegmentFile::read_record(uint64_t offset, uint64_t max_key,
                                             uint64_t max_payload) {
    // Read the 4-byte length field first.
    uint8_t lenbuf[4];
    auto r = file_.read(lenbuf, 4, offset);
    if (!r.ok()) return r.error();
    if (r.value() < 4) return Error(Err::Corrupt, "short length field");

    uint32_t len = get_le32(lenbuf);
    if (len == 0) return Error(Err::NotFound, "end of segment");
    if (len < 4 || len > (1u << 30)) return Error(Err::Corrupt, "invalid record length");

    std::vector<uint8_t> frame(len + 4);
    auto r2 = file_.read(frame.data(), len + 4, offset);
    if (!r2.ok()) return r2.error();
    if (r2.value() < len + 4) return Error(Err::Corrupt, "truncated record");

    size_t consumed = 0;
    auto rec = decode_frame(frame.data(), frame.size(), &consumed, max_key, max_payload);
    if (!rec.ok()) return rec.error();
    rec->segment_id = header_.segment_id;
    rec->offset = offset;
    rec->frame_size = consumed;
    return rec;
}

Error SegmentFile::scan(const std::function<bool(const EventRecord&)>& visitor,
                        uint64_t max_key, uint64_t max_payload) {
    const uint64_t end = kSegmentHeaderSize + data_size_;
    uint64_t pos = kSegmentHeaderSize;
    std::vector<uint8_t> buf;

    while (pos < end) {
        // Ensure we have at least 8 bytes to read a length+crc.
        if (end - pos < 8) return Error(Err::Corrupt, "trailing bytes < 8 at end of segment");

        uint8_t lenbuf[8];
        auto r = file_.read(lenbuf, 8, pos);
        if (!r.ok()) return r.error();
        if (r.value() < 8) return Error(Err::Corrupt, "short record header");

        uint32_t len = get_le32(lenbuf);
        if (len == 0) return ok(); // sentinel: clean end
        if (len < 4 || len > (1u << 30)) return Error(Err::Corrupt, "invalid record length");
        uint64_t total = (uint64_t)len + 4;
        if (pos + total > end) return Error(Err::Corrupt, "record extends past EOF");

        buf.resize(total);
        auto r2 = file_.read(buf.data(), total, pos);
        if (!r2.ok()) return r2.error();
        if (r2.value() < total) return Error(Err::Corrupt, "truncated record body");

        size_t consumed = 0;
        auto rec = decode_frame(buf.data(), total, &consumed, max_key, max_payload);
        if (!rec.ok()) return rec.error();
        rec->segment_id = header_.segment_id;
        rec->offset = pos;
        rec->frame_size = consumed;

        if (!visitor(*rec)) return ok();
        pos += total;
    }
    return ok();
}

Error SegmentFile::truncate_to(uint64_t new_size) {
    auto e = file_.truncate(new_size);
    if (!e.ok()) return e;
    data_size_ = new_size > kSegmentHeaderSize ? new_size - kSegmentHeaderSize : 0;
    return file_.flush();
}

} // namespace ast
