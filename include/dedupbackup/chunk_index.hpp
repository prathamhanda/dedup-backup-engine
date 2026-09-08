#pragma once

#include <cstring>
#include <unordered_map>

#include "dedupbackup/chunk_store.hpp"
#include "dedupbackup/hasher.hpp"

namespace dedupbackup {

// Hashes a Digest for use as an unordered_map key by taking its first 8
// bytes as a size_t, rather than re-hashing all 32 bytes with something
// like std::hash<std::string> or FNV. A Digest IS a SHA-256 output —
// already uniformly-distributed, cryptographically strong bits — so
// computing a second, weaker hash on top of it would only throw away
// quality for no benefit. Hash-TABLE collisions (two different digests
// landing in the same bucket, resolved by chaining) are a different
// concern from cryptographic collisions (two different chunks producing
// the same digest, which would corrupt the store) — bucket collisions
// cost a little lookup time, nothing more, so truncating to 64 bits is
// safe here even though it would be a disaster for the digest's actual
// job of proving chunk identity.
struct DigestHash {
    size_t operator()(const Digest& d) const noexcept {
        size_t h;
        std::memcpy(&h, d.data(), sizeof(h));
        return h;
    }
};

// In-memory map from a chunk's content digest to where it lives in a
// pack file. This is the whole answer to "do we already have this
// chunk?" — everything else (persistence, crash-safety) is deliberately
// NOT this class's job:
//
//   - No load/save of its own. Component 5 (the WAL) is the index's
//     persistence: on startup, the WAL is replayed from scratch and
//     insert() is called for each valid record. There is no separate
//     index file to keep in sync or get out of sync with the WAL — the
//     WAL IS the source of truth, and this class is just what you get
//     from replaying it into memory. (A real system would checkpoint
//     periodically to bound replay time; this project's WAL is small
//     enough that full replay on open is fine — see README.)
//   - No internal locking. Per the concurrency design, all mutation
//     happens under one caller-held global mutex alongside the pack
//     write and the WAL append, so adding a second lock here would be
//     redundant at best and a deadlock risk at worst. This class is
//     NOT safe to call from multiple threads without external
//     synchronization — that is a deliberate simplicity choice, not an
//     oversight.
class ChunkIndex {
public:
    // True if this digest is already known — i.e. its chunk bytes are
    // already stored somewhere and don't need to be written again. This
    // is the dedup check itself.
    bool contains(const Digest& digest) const;

    // Looks up where a known digest's bytes live. Only meaningful after
    // contains() returned true (or after this process itself inserted
    // it) — returns {0, 0} if the digest isn't present, since a
    // 0-length location can never be a real chunk.
    ChunkLocation find(const Digest& digest) const;

    // Records that `digest`'s bytes live at `loc`. Callers must only
    // call this AFTER durably writing both the chunk bytes (ChunkStore::
    // append, which itself blocks until fsync'd) and the WAL record
    // that claims to know about it — this method has no I/O of its own
    // and enforces none of that ordering itself.
    void insert(const Digest& digest, const ChunkLocation& loc);

    size_t size() const { return map_.size(); }

    using const_iterator = std::unordered_map<Digest, ChunkLocation, DigestHash>::const_iterator;
    const_iterator begin() const { return map_.begin(); }
    const_iterator end() const { return map_.end(); }

private:
    std::unordered_map<Digest, ChunkLocation, DigestHash> map_;
};

} // namespace dedupbackup
