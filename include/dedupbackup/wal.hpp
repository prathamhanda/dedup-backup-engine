#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "dedupbackup/chunk_index.hpp"
#include "dedupbackup/chunk_store.hpp"
#include "dedupbackup/hasher.hpp"

namespace dedupbackup {

// CRC32 (IEEE 802.3 / the "zlib" polynomial, 0xEDB88320) implemented from
// scratch rather than pulled from a library — the only allowed
// dependencies are OpenSSL and the standard library, and CRC32 is small
// enough that writing it is both permitted and a better demonstration of
// understanding than a library call would be. Verified against the
// standard test vector: crc32("123456789") == 0xCBF43926.
uint32_t crc32(const uint8_t* data, size_t len);

enum class WalRecordType : uint8_t {
    ChunkAdd = 1,
};

// Append-only journal recording chunk-index insertions. This IS the
// index's persistence (see chunk_index.hpp) — there is no separate index
// file; on startup the WAL is replayed from byte 0 and every valid
// record repopulates the in-memory ChunkIndex from scratch.
//
// On-disk record format (§3.3), all integers little-endian:
//   length   u32     byte count of (type + payload) that follows
//   type     u8      1 = CHUNK_ADD
//   payload  bytes   for CHUNK_ADD: digest[32] | offset u64 | length u32
//   crc32    u32     over (type + payload) only — not the length prefix
//                     itself, since the length prefix is what tells you
//                     how many bytes to feed the CRC in the first place
//
// Durability ordering (load-bearing — see README §1.4 / handoff §4.6):
//   1. ChunkStore::append() writes chunk bytes and fsyncs.
//   2. Wal::append_chunk_add() writes this record and fsyncs.
// A crash between the two leaves an orphaned, unreferenced chunk in the
// pack file: wasted space, never corruption, because nothing durable
// ever claimed to know about it. The reverse order would be fatal — a
// WAL record could then durably claim a chunk exists that was never
// actually written, and replay would hand the index a location pointing
// at garbage or past-EOF.
class Wal {
public:
    explicit Wal(const std::string& wal_path);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Appends one CHUNK_ADD record and does not return until it is
    // fsync'd. Caller must only call this after the chunk bytes
    // themselves are already durable (ChunkStore::append already
    // enforces that on its own end, via its own fsync).
    void append_chunk_add(const Digest& digest, const ChunkLocation& loc);

    // Replays every valid record from the start of the file into
    // `index`. Stops at the first record that is truncated (fewer bytes
    // remain than its length prefix promises) or fails its CRC32 check,
    // and physically truncates the underlying file to discard that torn
    // tail — not just skipping it in memory. Physical truncation matters:
    // without it, a later append would write new valid bytes starting at
    // the last good offset, but any leftover garbage past the end of
    // that new record would still be on disk for a future replay to walk
    // into. Truncating removes that class of bug outright instead of
    // relying on the torn bytes never coincidentally look like a
    // record.
    //
    // Must be called once, right after construction, before any
    // append_chunk_add() calls — this is what lets a Wal opened against
    // an existing repo resume appending at the correct offset even if
    // the previous run left a torn tail.
    //
    // Returns the number of valid records replayed.
    size_t replay(ChunkIndex& index);

private:
    int fd_;
    uint64_t write_offset_;
};

} // namespace dedupbackup
