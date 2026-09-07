#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dedupbackup {

// A content address: the output of whatever hash function is backing the
// store. Fixed at 32 bytes (256 bits) rather than a variable-length
// container — every hash function we'd realistically swap in (SHA-256,
// BLAKE2s, BLAKE3, SHA3-256) produces exactly this size, and a fixed
// std::array avoids a heap allocation per chunk on what is the hottest
// path in the whole pipeline. The cost is that a hash with a different
// natural output size (e.g. SHA-1's 20 bytes) wouldn't fit without either
// truncating or padding — an acceptable constraint given we'd never choose
// a weaker/smaller hash for content addressing anyway (see IHasher below
// for why).
using Digest = std::array<uint8_t, 32>;

std::string to_hex(const Digest& d);

// Common interface for anything that maps chunk bytes to a content
// address, so the store/index code never has to know or care which hash
// function is behind it.
class IHasher {
public:
    virtual ~IHasher() = default;
    virtual Digest hash(const uint8_t* data, size_t len) const = 0;
};

} // namespace dedupbackup
