#pragma once

#include <cstdint>

namespace dedupbackup {

// Explicit little-endian encode/decode for the fixed on-disk formats
// (WAL records here, the manifest in component 6). Deliberately NOT a
// memcpy of a struct: struct layout depends on padding/alignment rules
// the compiler controls, and raw integer bytes depend on host
// endianness — neither is something the on-disk format spec (§3.3) can
// rely on. Byte-at-a-time shift/mask is fully portable and well-defined
// regardless of host byte order, at negligible cost for records this
// small.
inline void write_u32_le(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v);
    out[1] = static_cast<uint8_t>(v >> 8);
    out[2] = static_cast<uint8_t>(v >> 16);
    out[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t read_u32_le(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

inline void write_u64_le(uint8_t* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint64_t read_u64_le(const uint8_t* in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[i]) << (8 * i);
    return v;
}

} // namespace dedupbackup
