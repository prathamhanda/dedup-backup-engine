// Standalone check for Wal:
//   1. CRC32 self-test against the standard CRC-32/ISO-HDLC test vector.
//   2. Append N records, close, reopen, replay into a fresh ChunkIndex,
//      verify every record recovered correctly.
//   3. Simulate a torn write — append a few well-formed records, then
//      hand-corrupt the file with bytes that look like the start of
//      another record but aren't complete/valid (as a crash mid-write
//      would leave behind) — and confirm replay stops exactly at the
//      last good record, physically truncates the file, and that
//      appending afterward produces a log that replays cleanly again.
//
// This is the foundation for the formal WAL-torn-write CTest test in
// component 10; the logic here is what that test will exercise via
// CTest instead of a standalone binary.
//
// Usage: wal_check <repo-dir>

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "dedupbackup/chunk_index.hpp"
#include "dedupbackup/sha256_hasher.hpp"
#include "dedupbackup/wal.hpp"

using namespace dedupbackup;

namespace {

void make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir %s failed: %s\n", path.c_str(), std::strerror(errno));
        std::exit(1);
    }
}

uint64_t file_size(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        std::fprintf(stderr, "stat %s failed: %s\n", path.c_str(), std::strerror(errno));
        std::exit(1);
    }
    return static_cast<uint64_t>(st.st_size);
}

Digest fake_digest(Sha256Hasher& hasher, std::mt19937& rng) {
    std::vector<uint8_t> buf(32);
    for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFF);
    return hasher.hash(buf.data(), buf.size());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: wal_check <repo-dir>\n");
        return 1;
    }

    // --- 1. CRC32 self-test ---
    {
        const char* test = "123456789";
        const uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(test), 9);
        if (crc != 0xCBF43926u) {
            std::fprintf(stderr, "CRC32 SELF-TEST FAILED: got 0x%08x, expected 0xcbf43926\n", crc);
            return 1;
        }
        std::printf("CRC32 self-test OK (0x%08x matches standard test vector)\n", crc);
    }

    const std::string repo_dir = argv[1];
    // Idempotent/repeatable under CTest -- see store_check.cpp's comment.
    std::error_code rm_ec;
    std::filesystem::remove_all(repo_dir, rm_ec);
    make_dir(repo_dir);
    const std::string wal_path = repo_dir + "/wal.log";

    Sha256Hasher hasher;
    std::mt19937 rng(11);

    // --- 2. Write N records, close, reopen, replay, verify ---
    const int kFirstBatch = 50;
    std::vector<std::pair<Digest, ChunkLocation>> batch1;
    {
        Wal wal(wal_path);
        ChunkIndex empty;
        wal.replay(empty); // fresh file: replays 0 records, establishes write_offset_ = 0
        for (int i = 0; i < kFirstBatch; ++i) {
            const Digest d = fake_digest(hasher, rng);
            const ChunkLocation loc{static_cast<uint64_t>(i) * 1000, static_cast<uint32_t>(i + 1)};
            wal.append_chunk_add(d, loc);
            batch1.push_back({d, loc});
        }
    } // wal destructs, fd closed

    {
        Wal wal(wal_path);
        ChunkIndex index;
        const size_t replayed = wal.replay(index);
        if (replayed != static_cast<size_t>(kFirstBatch) ||
            index.size() != static_cast<size_t>(kFirstBatch)) {
            std::printf("REPLAY MISMATCH: replayed=%zu index.size=%zu expected=%d\n", replayed,
                        index.size(), kFirstBatch);
            return 1;
        }
        for (const auto& [d, loc] : batch1) {
            const ChunkLocation found = index.find(d);
            if (found.offset != loc.offset || found.length != loc.length) {
                std::printf("REPLAY DATA MISMATCH for a record\n");
                return 1;
            }
        }
        std::printf("clean replay OK: %zu/%d records recovered correctly after reopen\n", replayed,
                    kFirstBatch);
    }

    // --- 3. Torn-write simulation ---
    const int kSecondBatch = 5;
    {
        Wal wal(wal_path);
        ChunkIndex tmp;
        wal.replay(tmp); // repositions write_offset_ at end of the 50 good records
        for (int i = 0; i < kSecondBatch; ++i) {
            const Digest d = fake_digest(hasher, rng);
            const ChunkLocation loc{static_cast<uint64_t>(i) * 2000 + 1, static_cast<uint32_t>(i)};
            wal.append_chunk_add(d, loc);
        }
    }
    const uint64_t good_size = file_size(wal_path);

    // Hand-corrupt: append bytes that look like the START of a valid
    // record (a plausible length prefix) but are truncated partway
    // through — exactly what a crash mid-pwrite of a record would leave
    // on disk, since fsync (and therefore any durability guarantee)
    // never completed for it.
    {
        std::ofstream raw(wal_path, std::ios::binary | std::ios::app);
        const uint8_t garbage[10] = {0x2D, 0, 0, 0,          // claims body_len = 45 (0x2D)
                                      0x01,                    // type = CHUNK_ADD
                                      0xDE, 0xAD, 0xBE, 0xEF,  // only 4 of the 44 payload bytes
                                      0x00};                   // present before the "crash"
        raw.write(reinterpret_cast<const char*>(garbage), sizeof(garbage));
    }
    const uint64_t corrupted_size = file_size(wal_path);
    if (corrupted_size <= good_size) {
        std::printf("TEST SETUP FAILED: corruption bytes weren't appended\n");
        return 1;
    }

    // Replay must recover exactly the 55 good records and discard the
    // torn tail — both in what it reports AND physically, on disk.
    {
        Wal wal(wal_path);
        ChunkIndex index;
        const size_t replayed = wal.replay(index);
        const size_t expected = static_cast<size_t>(kFirstBatch + kSecondBatch);
        if (replayed != expected) {
            std::printf("TORN-WRITE TEST FAILED: replayed=%zu expected=%zu\n", replayed, expected);
            return 1;
        }
        std::printf(
            "torn-write replay OK: recovered exactly %zu good records, discarded the torn tail\n",
            replayed);
    }

    const uint64_t truncated_size = file_size(wal_path);
    if (truncated_size != good_size) {
        std::printf("TRUNCATION FAILED: file is %llu bytes, expected exactly %llu (the torn "
                    "garbage should have been physically removed, not just skipped in memory)\n",
                    static_cast<unsigned long long>(truncated_size),
                    static_cast<unsigned long long>(good_size));
        return 1;
    }
    std::printf("physical truncation OK: file shrank from %llu to %llu bytes on disk\n",
                static_cast<unsigned long long>(corrupted_size),
                static_cast<unsigned long long>(truncated_size));

    // Finally: appending after a recovered torn write must produce a log
    // that replays cleanly, with no trace of the discarded garbage.
    {
        Wal wal(wal_path);
        ChunkIndex tmp;
        wal.replay(tmp);
        const Digest d = fake_digest(hasher, rng);
        wal.append_chunk_add(d, ChunkLocation{123456, 789});
    }
    {
        Wal wal(wal_path);
        ChunkIndex index;
        const size_t replayed = wal.replay(index);
        const size_t expected = static_cast<size_t>(kFirstBatch + kSecondBatch + 1);
        if (replayed != expected) {
            std::printf("POST-RECOVERY APPEND TEST FAILED: replayed=%zu expected=%zu\n", replayed,
                        expected);
            return 1;
        }
        std::printf("post-recovery append OK: %zu total records replay cleanly\n", replayed);
    }

    return 0;
}
