#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dedupbackup/hasher.hpp"

namespace dedupbackup {

// One file's entry in a snapshot manifest. This struct is deliberately
// opaque to *how* these values were obtained — populating it from a real
// directory walk (std::filesystem::recursive_directory_iterator, stat())
// is component 7's job. This component only knows how to serialize and
// deserialize the format; it never looks at the filesystem itself.
struct ManifestFileEntry {
    std::string path;                   // relative to backup root, '/' separated
    uint32_t mode;                      // POSIX mode bits (st_mode)
    uint64_t size;                      // file size in bytes
    int64_t mtime_unix;                 // modification time, seconds since epoch
    Digest file_digest;                 // SHA-256 of the whole file, for restore verification
    std::vector<Digest> chunk_digests;  // ordered list that reconstructs the file
};

struct Manifest {
    std::string snapshot_id;  // "YYYYMMDD-HHMMSS" — exactly 15 characters
    int64_t created_unix;
    std::vector<ManifestFileEntry> files;
};

// On-disk format (§3.3), all integers little-endian:
//   magic        char[8]   "DEDUPMF1"
//   version      u32       = 1
//   snapshot_id  char[16]  "YYYYMMDD-HHMMSS" + 1 NUL pad byte
//   created_unix i64
//   file_count   u64
//   --- repeated file_count times, sorted by path ---
//   path_len     u16
//   path         bytes
//   mode         u32
//   size         u64
//   mtime_unix   i64
//   file_digest  byte[32]
//   chunk_count  u32
//   chunk_digests byte[32 * chunk_count]
//
// Serializes `manifest` to `path` and fsyncs before returning. Entries
// are sorted by path INTERNALLY before writing, regardless of the order
// they arrive in — determinism (§3.2: "sorted by path for determinism")
// must not depend on every caller remembering to sort; enforcing it here,
// once, closes off a whole class of "forgot to sort" bugs.
//
// Unlike ChunkStore/Wal, this is a single buffered write with one fsync
// at the end rather than incremental appends — a manifest has one entry
// per FILE, not per byte of data, so even a huge backup produces a
// manifest small enough to build entirely in memory before writing. The
// WAL's incremental-durability design exists because a backup can run
// for a long time and a mid-run crash must not lose already-completed
// work; a manifest is only written once, at the very end of a
// successful backup, so there's nothing to make incrementally durable.
void write_manifest(const std::string& path, const Manifest& manifest);

// Reads and parses a manifest written by write_manifest(). Throws on
// I/O error, bad magic/version, or a truncated/malformed file.
Manifest read_manifest(const std::string& path);

} // namespace dedupbackup
