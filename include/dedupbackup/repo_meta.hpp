#pragma once

#include <cstdint>
#include <string>

namespace dedupbackup {

// The chunking parameters a repository was committed to, persisted once
// and checked forever after. Written the first time a backup runs
// against a fresh repo; every subsequent backup (potentially from a
// different build, different machine, different CLI flags) must match
// exactly, or dedup silently degrades — chunks cut with a different
// avg_size, or by a gear table seeded differently, won't align with
// chunks already in the store, even though nothing is actually corrupt.
//
// On-disk format (§3.3), all integers little-endian, 32 bytes total:
//   magic        char[8]   "DEDUPBK1"
//   version      u32       = 1
//   min_size     u32
//   avg_size     u32
//   max_size     u32
//   gear_seed    u64
struct RepoMeta {
    uint32_t min_size = 0;
    uint32_t avg_size = 0;
    uint32_t max_size = 0;
    uint64_t gear_seed = 0;
};

// Writes `meta` to `path` and fsyncs. Called once, when a repo has no
// repo.meta yet.
void write_repo_meta(const std::string& path, const RepoMeta& meta);

// Reads and validates the header (magic/version) of a repo.meta file.
// Throws on I/O error, bad magic, or unsupported version.
RepoMeta read_repo_meta(const std::string& path);

} // namespace dedupbackup
