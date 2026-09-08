#include "dedupbackup/stats.hpp"

namespace dedupbackup {

RepoStats compute_stats(const Repository& repo) {
    RepoStats stats;

    for (const std::string& snapshot_id : repo.list_snapshots()) {
        ++stats.snapshot_count;
        const Manifest manifest = repo.read_snapshot_manifest(snapshot_id);
        for (const ManifestFileEntry& file : manifest.files) {
            stats.logical_bytes += file.size;
        }
    }

    const ChunkSizeStats chunk_stats = repo.compute_chunk_size_stats();
    stats.physical_bytes = chunk_stats.physical_bytes;
    stats.unique_chunks = chunk_stats.unique_chunks;
    stats.mean_chunk_size = chunk_stats.mean_chunk_size;
    stats.median_chunk_size = chunk_stats.median_chunk_size;

    return stats;
}

} // namespace dedupbackup
