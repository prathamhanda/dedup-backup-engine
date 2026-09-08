#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "dedupbackup/chunk_index.hpp"
#include "dedupbackup/chunk_store.hpp"
#include "dedupbackup/manifest.hpp"
#include "dedupbackup/wal.hpp"

namespace dedupbackup {

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

    const std::string& path() const { return repo_path_; }

private:
    std::string repo_path_;
    ChunkStore store_;
    Wal wal_;
    ChunkIndex index_;
    mutable std::mutex mutex_;
};

} // namespace dedupbackup
