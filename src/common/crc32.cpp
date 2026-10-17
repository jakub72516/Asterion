#include "common/crc32.hpp"

namespace ast {

namespace {
struct CrcTable {
    uint32_t table[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
    }
};
const CrcTable& crc_table() {
    static CrcTable t;
    return t;
}
} // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    const uint32_t* t = crc_table().table;
    for (size_t i = 0; i < len; ++i) {
        c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t crc32_combine(uint32_t crc, const uint8_t* data, size_t len) {
    uint32_t c = crc ^ 0xFFFFFFFFu;
    const uint32_t* t = crc_table().table;
    for (size_t i = 0; i < len; ++i) {
        c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

} // namespace ast
