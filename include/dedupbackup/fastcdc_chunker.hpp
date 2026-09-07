#pragma once

#include "dedupbackup/chunker.hpp"

namespace dedupbackup {

struct FastCDCConfig {
    size_t min_size = 2 * 1024;   // hard floor: never cut before this many bytes
    size_t avg_size = 8 * 1024;   // target mean chunk size
    size_t max_size = 64 * 1024;  // hard ceiling: force a cut at this many bytes
};

// FastCDC (Xia et al., USENIX ATC 2016) content-defined chunker.
//
// A gear-hash rolling hash is evaluated at every byte position; a boundary
// is declared where the low bits of that hash are all zero. Because the
// decision depends only on the local byte window (not on absolute offset),
// inserting or deleting bytes anywhere in the file re-aligns the chunk
// boundaries after the edit instead of shifting every chunk downstream —
// see the accompanying discussion for why that matters for dedup ratio.
//
// This implementation includes FastCDC's "normalized chunking": two masks
// of different strictness (mask_s_ before avg_size, mask_l_ at/after
// avg_size) that pull the chunk-size distribution toward avg_size instead
// of letting it spread out geometrically.
class FastCDCChunker final : public IChunker {
public:
    explicit FastCDCChunker(FastCDCConfig config = {});

    std::vector<ChunkSpan> chunk(const uint8_t* data, size_t len) const override;

private:
    FastCDCConfig config_;
    uint64_t mask_s_;  // stricter mask (more 1-bits) — used while pos < avg_size
    uint64_t mask_l_;  // looser mask (fewer 1-bits)  — used while pos >= avg_size
};

} // namespace dedupbackup
