// Standalone round-trip check for ChunkStore: appends a batch of random
// chunks, reads each back by its returned location, and verifies
// byte-for-byte equality. Then closes and reopens the store against the
// same file to confirm append() correctly resumes after the existing
// contents — simulating what happens across two separate `backup`
// invocations against the same repo.
//
// Usage: store_check <repo-dir> [chunk_count]

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "dedupbackup/chunk_store.hpp"

using namespace dedupbackup;

namespace {

void make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir %s failed: %s\n", path.c_str(), std::strerror(errno));
        std::exit(1);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: store_check <repo-dir> [chunk_count]\n");
        return 1;
    }
    const std::string repo_dir = argv[1];
    const int chunk_count = argc >= 3 ? std::atoi(argv[2]) : 200;

    // Plain POSIX mkdir, not std::filesystem — ChunkStore doesn't manage
    // directories itself, and this tool keeps the same pre-C++17-filesystem
    // dependency footprint as everything built so far (that arrives in
    // component 6).
    make_dir(repo_dir);
    const std::string packs_dir = repo_dir + "/packs";
    make_dir(packs_dir);
    const std::string pack_path = packs_dir + "/pack-000001.dat";

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> size_dist(1, 64 * 1024);

    struct Written {
        std::vector<uint8_t> data;
        ChunkLocation loc;
    };
    std::vector<Written> written;
    written.reserve(chunk_count);

    uint64_t size_before_reopen = 0;
    {
        ChunkStore store(pack_path);

        for (int i = 0; i < chunk_count; ++i) {
            std::vector<uint8_t> data(static_cast<size_t>(size_dist(rng)));
            for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);
            ChunkLocation loc = store.append(data.data(), static_cast<uint32_t>(data.size()));
            written.push_back(Written{std::move(data), loc});
        }

        std::printf("appended %d chunks, pack file now %llu bytes\n", chunk_count,
                    static_cast<unsigned long long>(store.size()));

        size_t mismatches = 0;
        for (const auto& w : written) {
            std::vector<uint8_t> back = store.read(w.loc);
            if (back != w.data) {
                ++mismatches;
                std::fprintf(stderr, "MISMATCH at offset=%llu length=%u\n",
                             static_cast<unsigned long long>(w.loc.offset), w.loc.length);
            }
        }

        if (mismatches != 0) {
            std::printf("round-trip FAILED: %zu/%d chunks mismatched\n", mismatches, chunk_count);
            return 1;
        }
        std::printf("round-trip OK: all %d chunks read back byte-identical\n", chunk_count);
        size_before_reopen = store.size();
    } // store destructs here — fd closed, nothing left open

    // Reopen against the same file: append() must pick up exactly where
    // the previous instance left off.
    {
        ChunkStore reopened(pack_path);
        if (reopened.size() != size_before_reopen) {
            std::printf("REOPEN FAILED: expected size %llu, got %llu\n",
                        static_cast<unsigned long long>(size_before_reopen),
                        static_cast<unsigned long long>(reopened.size()));
            return 1;
        }

        std::vector<uint8_t> data(1234);
        for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);
        ChunkLocation loc = reopened.append(data.data(), static_cast<uint32_t>(data.size()));
        if (loc.offset != size_before_reopen) {
            std::printf("REOPEN FAILED: new chunk landed at %llu, expected %llu\n",
                        static_cast<unsigned long long>(loc.offset),
                        static_cast<unsigned long long>(size_before_reopen));
            return 1;
        }

        std::vector<uint8_t> back = reopened.read(loc);
        if (back != data) {
            std::printf("REOPEN FAILED: post-reopen chunk did not round-trip\n");
            return 1;
        }
        std::printf("reopen OK: appended after existing %llu bytes, round-tripped correctly\n",
                    static_cast<unsigned long long>(size_before_reopen));
    }

    return 0;
}
