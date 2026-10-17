#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <string_view>

namespace ast {

// Byte-order helpers. All on-disk and on-wire integers are little-endian.
inline void put_le16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
inline void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (v >> (8 * i)) & 0xff;
}
inline uint16_t get_le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t get_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Growable byte buffer used for encoding records and frames.
class Buffer {
public:
    Buffer() = default;
    explicit Buffer(size_t reserve) { data_.reserve(reserve); }

    void reserve(size_t n) { data_.reserve(n); }
    void clear() { data_.clear(); }
    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }
    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }
    const std::vector<uint8_t>& vec() const { return data_; }
    std::vector<uint8_t>& vec() { return data_; }

    void append(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data_.insert(data_.end(), b, b + n);
    }
    void append_byte(uint8_t b) { data_.push_back(b); }
    void append_u16(uint16_t v) { uint8_t t[2]; put_le16(t, v); append(t, 2); }
    void append_u32(uint32_t v) { uint8_t t[4]; put_le32(t, v); append(t, 4); }
    void append_u64(uint64_t v) { uint8_t t[8]; put_le64(t, v); append(t, 8); }
    void append_string(std::string_view s) {
        append_u32((uint32_t)s.size());
        append(s.data(), s.size());
    }

    std::string_view as_string_view() const {
        return std::string_view(reinterpret_cast<const char*>(data_.data()), data_.size());
    }

private:
    std::vector<uint8_t> data_;
};

// Bounds-checked reader over a byte range. All reads advance a cursor.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n), pos_(0) {}
    explicit Reader(const Buffer& b) : Reader(b.data(), b.size()) {}
    Reader(const uint8_t* p, size_t n, size_t pos) : p_(p), n_(n), pos_(pos) {}

    size_t remaining() const { return n_ - pos_; }
    size_t position() const { return pos_; }
    bool eof() const { return pos_ >= n_; }

    bool read_u8(uint8_t& v) {
        if (remaining() < 1) return false;
        v = p_[pos_++];
        return true;
    }
    bool read_u16(uint16_t& v) {
        if (remaining() < 2) return false;
        v = get_le16(p_ + pos_); pos_ += 2;
        return true;
    }
    bool read_u32(uint32_t& v) {
        if (remaining() < 4) return false;
        v = get_le32(p_ + pos_); pos_ += 4;
        return true;
    }
    bool read_u64(uint64_t& v) {
        if (remaining() < 8) return false;
        v = get_le64(p_ + pos_); pos_ += 8;
        return true;
    }
    bool read_bytes(const uint8_t*& out, size_t n) {
        if (remaining() < n) return false;
        out = p_ + pos_; pos_ += n;
        return true;
    }
    bool read_string(std::string_view& s, uint32_t max_len = 1u << 30) {
        uint32_t len;
        if (!read_u32(len)) return false;
        if (len > max_len || remaining() < len) return false;
        s = std::string_view(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return true;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_;
};

} // namespace ast
