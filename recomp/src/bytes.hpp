// Byte-order reads. The PS3 is big-endian, and so are its executables;
// PARAM.SFO alone is little-endian.
#pragma once

#include <cstdint>

namespace qilin {

inline uint16_t read_be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t read_be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline uint64_t read_be64(const uint8_t* p) { return uint64_t(read_be32(p)) << 32 | read_be32(p + 4); }

inline uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[1] << 8 | p[0]); }
inline uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]);
}

}  // namespace qilin
