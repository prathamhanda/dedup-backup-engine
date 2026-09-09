#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "dedupbackup/chunk_index.hpp"
#include "dedupbackup/chunk_store.hpp"
#include "dedupbackup/fastcdc_chunker.hpp"
#include "dedupbackup/manifest.hpp"
#include "dedupbackup/wal.hpp"

namespace dedupbackup {

struct ChunkSizeStats {
    uint64_t physical_bytes = 0;
    size_t unique_chunks = 0;
    double mean_chunk_size = 0.0;
    uint64_t median_chunk_size = 0;
};

// Ties ChunkStore + ChunkIndex + Wal together into the one object the
// backup pipeline actually talks to. This is where §3.5's "single global
// mutex" lives: rather than have pipeline/worker code manage a shared
// mutex directly, Repository owns it and exposes one thread-safe
// operation, store_chunk_if_absent(), that performs the whole
// check-index / store-bytes / log-WAL / update-index sequence
// atomically. Worker threads never touch locking at all.
//
// Originally sketched as component 3 in the project plan, before the
// index and WAL existed to tie together — genuinely belongs here, once
// all three pieces it wraps actually exist.
class Repository {
public:
    // Creates <repo_path>/packs and <repo_path>/snapshots if they don't
    // already exist, opens (or creates) packs/pack-000001.dat and
    // wal.log, and replays the WAL to rebuild the in-memory index —
    // exactly the bootstrap sequence described in chunk_index.hpp's
    // "no persistence of its own" contract.
    explicit Repository(const std::string& repo_path);

    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    // Thread-safe — this is the pipeline's one synchronized entry point,
    // called concurrently from every worker thread. Returns the chunk's
    // location whether it was already present (a dedup hit — nothing is
    // written) or newly stored.
    ChunkLocation store_chunk_if_absent(const Digest& digest, const uint8_t* data,
                                         uint32_t length);

    // Read paths. NOT synchronized against a concurrently-running
    // backup in the same process: this project's CLI runs one operation
    // per process invocation (backup, then separately restore/verify),
    // so there is never a concurrent writer while these are in use. A
    // future caller needing backup and restore concurrently in one
    // process would need to route these through the same mutex as
    // store_chunk_if_absent().
    bool has_chunk(const Digest& digest) const;
    ChunkLocation locate_chunk(const Digest& digest) const;
    std::vector<uint8_t> read_chunk(const ChunkLocation& loc) const;
    size_t unique_chunk_count() const { return index_.size(); }

    void write_snapshot_manifest(const Manifest& manifest);
    Manifest read_snapshot_manifest(const std::string& snapshot_id) const;
    bool has_snapshot(const std::string& snapshot_id) const;

    // All snapshot ids present in the repo, sorted ascending — which is
    // also chronological order, since ids are "YYYYMMDD-HHMMSS".
    std::vector<std::string> list_snapshots() const;

    // Physical storage stats derived from the index in one pass: total
    // bytes actually stored (unique chunks only), count, mean/median
    // chunk size. Returned as one struct (rather than several separate
    // accessors) so `stats` gets a single, internally-consistent
    // snapshot of the index instead of computing each field from a
    // separately-timed pass.
    ChunkSizeStats compute_chunk_size_stats() const;

    // Enforces repo.meta (§3.3): if this repo has never been backed up
    // to before, writes `config` (+ the build's kGearSeed) as the
    // committed parameters. If repo.meta already exists, compares
    // `config` against it and throws a clear std::runtime_error naming
    // the mismatched field(s) if they differ — refusing to silently
    // start producing chunks that won't dedup against what's already
    // stored. Called once, at the top of run_backup(), before any work
    // starts.
    //
    // Known gap, named rather than silently left: this only covers
    // FastCDCConfig's three sizes (avg_size doubles as FixedChunker's
    // chunk_size, so --avg-chunk-kb drift is still caught even in
    // --fixed-chunking mode) plus the gear seed. It does NOT detect
    // switching between FastCDC and --fixed-chunking with the same
    // avg_chunk_kb on the same repo — the sizes would match even though
    // the chunking STRATEGY differs, which also breaks dedup alignment.
    // repo.meta's on-disk format (§3.3) has no field for that; extending
    // it was judged out of scope for closing this specific gap.
    void check_or_init_chunk_config(const FastCDCConfig& config);

    const std::string& path() const { return repo_path_; }

private:
    std::string repo_path_;
    ChunkStore store_;
    Wal wal_;
    ChunkIndex index_;
    mutable std::mutex mutex_;
};

} // namespace dedupbackup
