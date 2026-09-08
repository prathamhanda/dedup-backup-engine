#include "dedupbackup/chunk_index.hpp"

namespace dedupbackup {

bool ChunkIndex::contains(const Digest& digest) const {
    return map_.find(digest) != map_.end();
}

ChunkLocation ChunkIndex::find(const Digest& digest) const {
    const auto it = map_.find(digest);
    if (it == map_.end()) {
        return ChunkLocation{0, 0};
    }
    return it->second;
}

void ChunkIndex::insert(const Digest& digest, const ChunkLocation& loc) {
    map_[digest] = loc;
}

} // namespace dedupbackup
