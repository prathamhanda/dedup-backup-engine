// Standalone check for ChunkIndex, exercised together with ChunkStore and
// Sha256Hasher — this is a miniature rehearsal of the actual dedup
// decision the backup pipeline will make (component 7), just without a
// WAL or directory walk yet:
//
//   digest = hash(chunk bytes)
//   if index.contains(digest): dedup hit, store nothing
//   else: store bytes, insert(digest -> location)
//
// Generates a set of buffers where some are deliberate exact duplicates,
// runs them through that logic, then verifies every original buffer's
// bytes can still be recovered via index.find() + store.read() — proving
// the dedup path doesn't lose data, not just that it saves space.
//
// Usage: index_check <repo-dir>

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "dedupbackup/chunk_index.hpp"
#include "dedupbackup/chunk_store.hpp"
#include "dedupbackup/sha256_hasher.hpp"

using namespace dedupbackup;

namespace {

void make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir %s failed: %s\n", path.c_str(), std::strerror(errno));
        std::exit(1);
    }
}

std::vector<uint8_t> random_buffer(std::mt19937& rng, size_t size) {
    std::vector<uint8_t> buf(size);
    for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFF);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: index_check <repo-dir>\n");
        return 1;
    }
    const std::string repo_dir = argv[1];
    make_dir(repo_dir);
    const std::string packs_dir = repo_dir + "/packs";
    make_dir(packs_dir);

    ChunkStore store(packs_dir + "/pack-000001.dat");
    ChunkIndex index;
    Sha256Hasher hasher;

    // Build 300 "logical" chunks where roughly 2/3 are genuinely unique
    // and 1/3 are exact duplicates of earlier ones — a rough stand-in
    // for what a second, mostly-similar backup would look like.
    std::mt19937 rng(7);
    std::vector<std::vector<uint8_t>> unique_pool;
    for (int i = 0; i < 100; ++i) {
        unique_pool.push_back(random_buffer(rng, 1 + (rng() % (64 * 1024))));
    }

    std::vector<std::vector<uint8_t>> logical_chunks;
    std::uniform_int_distribution<int> pick_existing(0, 99);
    for (int i = 0; i < 300; ++i) {
        if (i < 100) {
            logical_chunks.push_back(unique_pool[static_cast<size_t>(i)]);
        } else {
            // Re-use a previously seen buffer verbatim -> guaranteed dedup hit.
            logical_chunks.push_back(unique_pool[static_cast<size_t>(pick_existing(rng))]);
        }
    }

    size_t hits = 0, misses = 0, logical_bytes = 0;
    for (const auto& chunk : logical_chunks) {
        logical_bytes += chunk.size();
        const Digest digest = hasher.hash(chunk.data(), chunk.size());
        if (index.contains(digest)) {
            ++hits;
            continue;
        }
        ++misses;
        ChunkLocation loc = store.append(chunk.data(), static_cast<uint32_t>(chunk.size()));
        index.insert(digest, loc);
    }

    std::printf("processed %zu logical chunks: %zu dedup hits, %zu newly stored\n",
                logical_chunks.size(), hits, misses);
    std::printf("index size (unique chunks): %zu\n", index.size());
    if (index.size() != unique_pool.size()) {
        std::printf("MISMATCH: expected %zu unique chunks, index has %zu\n", unique_pool.size(),
                    index.size());
        return 1;
    }

    size_t physical_bytes = 0;
    for (const auto& entry : index) {
        physical_bytes += entry.second.length;
    }
    std::printf("logical bytes: %zu, physical bytes stored: %zu, saved: %.1f%%\n", logical_bytes,
                physical_bytes, 100.0 * (1.0 - static_cast<double>(physical_bytes) /
                                                     static_cast<double>(logical_bytes)));

    // Round-trip every logical chunk through the index + store, proving
    // dedup doesn't cost correctness: even a chunk that was a hit (never
    // separately stored) must still resolve, via its digest, back to the
    // one copy that WAS stored, and produce identical bytes.
    size_t mismatches = 0;
    for (const auto& chunk : logical_chunks) {
        const Digest digest = hasher.hash(chunk.data(), chunk.size());
        ChunkLocation loc = index.find(digest);
        if (loc.length == 0 && !chunk.empty()) {
            std::fprintf(stderr, "LOOKUP FAILED for a chunk that should be indexed\n");
            ++mismatches;
            continue;
        }
        std::vector<uint8_t> back = store.read(loc);
        if (back != chunk) {
            std::fprintf(stderr, "MISMATCH: recovered bytes don't match original\n");
            ++mismatches;
        }
    }

    if (mismatches != 0) {
        std::printf("round-trip FAILED: %zu/%zu chunks mismatched\n", mismatches,
                    logical_chunks.size());
        return 1;
    }
    std::printf("round-trip OK: all %zu logical chunks resolve to correct bytes via the index\n",
                logical_chunks.size());
    return 0;
}
