#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dedupbackup {

// A chunk boundary within a buffer, expressed as [offset, offset+length).
// This is deliberately *not* a copy of the bytes — chunkers only decide
// where the cuts go. Copying/hashing/storing that range is the caller's
// job (the hasher and chunk store, built in later stages).
struct ChunkSpan {
    size_t offset;
    size_t length;

    // True if this boundary was found by the content-defined test (the
    // gear-hash mask actually matched); false if it was forced by a
    // purely positional rule instead — hitting max_size with no match,
    // or the final too-short-to-scan remainder at end of buffer. A fixed
    // chunker's cuts are always positional, so it always reports false.
    // This is the diagnostic tools/chunk_identity.cpp uses to measure how
    // often max_size truncation (which breaks CDC's resync property — see
    // fastcdc_chunker.cpp) is actually happening on real data.
    bool content_defined = false;
};

// Common interface for anything that splits a buffer into chunks, so the
// pipeline (and --fixed-chunking) can swap strategies without caring which
// one it's talking to.
class IChunker {
public:
    virtual ~IChunker() = default;

    // Splits data[0, len) into a sequence of contiguous, non-overlapping
    // spans that cover the whole buffer.
    virtual std::vector<ChunkSpan> chunk(const uint8_t* data, size_t len) const = 0;
};

} // namespace dedupbackup
