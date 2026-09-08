#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "dedupbackup/fastcdc_chunker.hpp"
#include "dedupbackup/repository.hpp"

namespace dedupbackup {

// Optional phase-timing accumulator for `bench`. Each field sums
// nanoseconds spent in that phase, ADDED ACROSS EVERY WORKER THREAD —
// this is aggregate thread-time, not wall-clock time. A fully-parallel
// run's phases will sum to roughly (thread_count * wall_time), not to
// wall_time itself; that's intentional; it shows the relative cost of
// each phase (and therefore where the bottleneck is) rather than a
// budget that adds up to the run's actual duration. `bench` reports wall
// time separately, from its own single start/end measurement around the
// whole run_backup() call.
//
// std::atomic members make this non-copyable, so BackupOptions holds a
// pointer to a caller-owned instance rather than one by value — the
// normal `backup` command passes nullptr and pays zero instrumentation
// cost (see worker_loop's ternary pattern in the .cpp: clock::now() is
// only ever called when a non-null PipelineTimings* is present).
struct PipelineTimings {
    std::atomic<uint64_t> read_ns{0};
    std::atomic<uint64_t> chunk_ns{0};
    std::atomic<uint64_t> hash_ns{0};
    std::atomic<uint64_t> store_ns{0};  // time inside Repository::store_chunk_if_absent()
                                          // (the single global mutex) -- the known secondary
                                          // bottleneck per §3.5.
};

struct BackupOptions {
    size_t thread_count = 0;  // 0 => std::thread::hardware_concurrency()
    FastCDCConfig chunk_config;
    bool fixed_chunking = false;    // use FixedChunker instead of FastCDCChunker
    PipelineTimings* timings = nullptr;  // optional; non-null only for `bench`
};

// Walks `source_dir` (one walker thread), chunks + hashes + dedups every
// regular file into `repo` (a pool of worker threads, per §3.5), writes a
// snapshot manifest, and returns the assigned snapshot id
// ("YYYYMMDD-HHMMSS", UTC).
//
// Scope, per the project's reduced-scope plan (§2.2): regular files and
// directories only. Symlinks, hardlinks, device files, sockets, etc. are
// skipped and logged to stderr, not backed up. Empty directories are not
// separately recorded — the manifest only has file entries — so restore
// will not recreate a directory that contains no files at all; documented
// as a known limitation, not a bug.
std::string run_backup(const std::string& source_dir, Repository& repo,
                        const BackupOptions& options = {});

} // namespace dedupbackup
