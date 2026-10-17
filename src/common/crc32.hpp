#pragma once

#include <cstdint>
#include <cstddef>

namespace ast {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), matching zlib's crc32.
// Used for both on-disk record checksums and on-wire frame checksums.
uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32_combine(uint32_t crc, const uint8_t* data, size_t len);

} // namespace ast
