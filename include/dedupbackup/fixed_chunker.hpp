#pragma once

#include "dedupbackup/chunker.hpp"

namespace dedupbackup {

// Baseline chunker for --fixed-chunking: cuts every `chunk_size` bytes with
// no regard for content. Exists so the benchmark can quantify how much
// dedup ratio content-defined chunking actually buys you over the naive
// approach on the same dataset.
class FixedChunker final : public IChunker {
public:
    explicit FixedChunker(size_t chunk_size = 8 * 1024);

    std::vector<ChunkSpan> chunk(const uint8_t* data, size_t len) const override;

private:
    size_t chunk_size_;
};

} // namespace dedupbackup
