#include "dedupbackup/fixed_chunker.hpp"

#include <algorithm>
#include <stdexcept>

namespace dedupbackup {

FixedChunker::FixedChunker(size_t chunk_size) : chunk_size_(chunk_size) {
    if (chunk_size_ == 0) {
        throw std::invalid_argument("FixedChunker chunk_size must be > 0");
    }
}

std::vector<ChunkSpan> FixedChunker::chunk(const uint8_t* /*data*/, size_t len) const {
    std::vector<ChunkSpan> chunks;
    size_t offset = 0;
    while (offset < len) {
        const size_t length = std::min(chunk_size_, len - offset);
        chunks.push_back({offset, length});
        offset += length;
    }
    return chunks;
}

} // namespace dedupbackup
