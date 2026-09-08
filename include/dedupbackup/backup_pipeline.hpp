#pragma once

#include <cstddef>
#include <string>

#include "dedupbackup/fastcdc_chunker.hpp"
#include "dedupbackup/repository.hpp"

namespace dedupbackup {

struct BackupOptions {
    size_t thread_count = 0;  // 0 => std::thread::hardware_concurrency()
    FastCDCConfig chunk_config;
    bool fixed_chunking = false;  // use FixedChunker instead of FastCDCChunker
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
