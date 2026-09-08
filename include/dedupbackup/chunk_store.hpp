#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dedupbackup {

// Where a chunk's bytes live inside a pack file. This is the value the
// chunk index (component 4) maps a content digest to.
struct ChunkLocation {
    uint64_t offset;
    uint32_t length;
};

// Owns a single append-only pack file holding raw chunk bytes,
// concatenated with no framing — (offset, length) is tracked entirely
// out-of-band by the index/WAL, not stored inline in the pack file.
// ChunkStore has no notion of digests or dedup: "have we already stored
// this content?" is the index's question, not this class's. ChunkStore
// only answers "put these bytes somewhere durable and tell me where" and
// "give me the bytes at this (offset, length)".
//
// Reduced scope for this project: one pack file that grows without
// bound, rather than rotating to pack-000002.dat at a size threshold.
// Rotation matters for real operational concerns (bounding a single
// file's size for parallel compaction, restore locality) that this
// project doesn't exercise — documented as future work in the README.
class ChunkStore {
public:
    // `pack_path` must name a file whose parent directory already
    // exists — ChunkStore manages one file, not the repository's
    // directory tree (that's the caller's job, e.g. `init`). Opens the
    // file if present (future appends land after its existing contents,
    // so a store correctly resumes across separate process runs against
    // the same repo) or creates it if not.
    explicit ChunkStore(const std::string& pack_path);
    ~ChunkStore();

    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;

    // Appends `length` bytes to the pack file and does not return until
    // they are fsync'd to durable storage. This durability guarantee is
    // load-bearing: the WAL (component 5) relies on being able to write
    // a record that references this chunk immediately after append()
    // returns, confident the bytes it points to already survive a crash.
    ChunkLocation append(const uint8_t* data, uint32_t length);

    // Reads back exactly loc.length bytes starting at loc.offset.
    std::vector<uint8_t> read(const ChunkLocation& loc) const;

    // Current size of the pack file — equivalently, the offset the next
    // append() will land at. Used by stats/bench.
    uint64_t size() const { return write_offset_; }

private:
    int fd_;
    uint64_t write_offset_;
};

} // namespace dedupbackup
