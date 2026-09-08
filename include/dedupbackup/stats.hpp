#pragma once

#include <cstddef>
#include <cstdint>

#include "dedupbackup/repository.hpp"

namespace dedupbackup {

struct RepoStats {
    size_t snapshot_count = 0;
    uint64_t logical_bytes = 0;   // sum of file sizes across every snapshot's manifest -- what
                                    // total storage would be with zero deduplication
    uint64_t physical_bytes = 0;  // sum of unique chunk lengths actually stored on disk
    size_t unique_chunks = 0;
    double mean_chunk_size = 0.0;
    uint64_t median_chunk_size = 0;

    double percent_saved() const {
        if (logical_bytes == 0) return 0.0;
        return 100.0 * (1.0 - static_cast<double>(physical_bytes) /
                                   static_cast<double>(logical_bytes));
    }
    // "4.2x" style factor: how many times larger the logical data is
    // than what's actually stored.
    double dedup_factor() const {
        if (physical_bytes == 0) return 0.0;
        return static_cast<double>(logical_bytes) / static_cast<double>(physical_bytes);
    }
};

// Walks every snapshot's manifest to total logical bytes (a file backed
// up in N snapshots without changing counts N times here — that's the
// correct measure of "what would have been stored without dedup"), and
// combines it with Repository::compute_chunk_size_stats() for the
// physical side.
RepoStats compute_stats(const Repository& repo);

} // namespace dedupbackup
