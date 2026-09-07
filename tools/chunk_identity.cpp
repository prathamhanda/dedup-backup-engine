// Chunk IDENTITY test: proves content-defined chunking survives edits by
// comparing the *set of chunk hashes* before and after, not just the size
// distribution (a histogram match is necessary but nowhere near
// sufficient — two unrelated files can share a histogram).
//
// Usage:
//   chunk_identity --gen <path> <size_mb>     generate a random test file
//   chunk_identity <file> [avg_kb]            run the full test suite
//
// The suite:
//   1. Two illustrative single-byte insertions (offset 0, midpoint).
//   2. A 50-trial resync-distance distribution: single-byte insertion at
//      pseudorandom offsets (fixed seed — reproducible), reporting
//      min/max/mean and a histogram of how many chunks differed.
//   3. Multi-byte edits (100-byte insert, 10 KiB insert, 100-byte delete)
//      at a fixed offset, since those are the realistic edit shapes in
//      actual backups (a config line added, a log rotated, etc).
//
// See src/chunker/fastcdc_chunker.cpp for why the resync distance is
// probabilistic rather than a guaranteed bound.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "dedupbackup/chunker.hpp"
#include "dedupbackup/fastcdc_chunker.hpp"
#include "dedupbackup/fixed_chunker.hpp"
#include "dedupbackup/sha256_hasher.hpp"

using namespace dedupbackup;

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    const std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

void generate(const char* path, size_t size_mb) {
    // std::random_device: real entropy, not a seeded PRNG — a weak
    // generator (e.g. a textbook LCG) has short-period low bits, and the
    // gear hash's boundary test is exactly a low-bits test, so weak
    // "randomness" here previously produced degenerate chunking (every
    // chunk hit max_size) that had nothing to do with the chunker itself.
    std::random_device rd;
    std::vector<uint8_t> buf(size_mb * 1024 * 1024);
    for (size_t i = 0; i < buf.size();) {
        uint32_t r = rd();
        for (int b = 0; b < 4 && i < buf.size(); ++b, ++i) {
            buf[i] = static_cast<uint8_t>(r >> (b * 8));
        }
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    std::printf("wrote %zu bytes to %s\n", buf.size(), path);
}

std::vector<uint8_t> insert_bytes(const std::vector<uint8_t>& src, size_t offset,
                                   const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(src.size() + payload.size());
    out.insert(out.end(), src.begin(), src.begin() + offset);
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), src.begin() + offset, src.end());
    return out;
}

std::vector<uint8_t> delete_bytes(const std::vector<uint8_t>& src, size_t offset, size_t count) {
    std::vector<uint8_t> out;
    out.reserve(src.size() - count);
    out.insert(out.end(), src.begin(), src.begin() + offset);
    out.insert(out.end(), src.begin() + offset + count, src.end());
    return out;
}

std::unordered_set<std::string> hash_all_chunks(const IChunker& chunker, const IHasher& hasher,
                                                 const std::vector<uint8_t>& buf) {
    std::unordered_set<std::string> digests;
    for (const ChunkSpan& c : chunker.chunk(buf.data(), buf.size())) {
        digests.insert(to_hex(hasher.hash(buf.data() + c.offset, c.length)));
    }
    return digests;
}

struct CompareResult {
    size_t total_a, total_b, shared, only_a, only_b;
};

CompareResult compare_chunks(const IChunker& chunker, const IHasher& hasher,
                              const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    auto sa = hash_all_chunks(chunker, hasher, a);
    auto sb = hash_all_chunks(chunker, hasher, b);
    size_t shared = 0;
    for (const auto& h : sa) {
        if (sb.count(h)) ++shared;
    }
    return CompareResult{sa.size(), sb.size(), shared, sa.size() - shared, sb.size() - shared};
}

struct BoundaryStats {
    size_t total;
    size_t content_defined;
    size_t truncated;  // forced by max_size or end-of-buffer, not by content
};

// The diagnostic for whether the resync argument in fastcdc_chunker.cpp
// actually holds on a given input: if truncated/total is near zero, cuts
// are essentially all content-derived and resync should be tight; if it
// climbs, max_size is firing often (sparse candidates — long low-entropy
// runs) and resync can no longer be assumed to behave the way it does on
// dense, high-entropy data like the synthetic random sample.
BoundaryStats boundary_stats(const IChunker& chunker, const std::vector<uint8_t>& buf) {
    BoundaryStats stats{0, 0, 0};
    for (const ChunkSpan& c : chunker.chunk(buf.data(), buf.size())) {
        ++stats.total;
        if (c.content_defined) {
            ++stats.content_defined;
        } else {
            ++stats.truncated;
        }
    }
    return stats;
}

void print_boundary_stats(const char* chunker_name, const BoundaryStats& s) {
    const double pct = s.total ? 100.0 * static_cast<double>(s.truncated) / s.total : 0.0;
    std::printf("[%s] boundary origin: %zu/%zu content-defined, %zu/%zu positional "
                "(max_size/EOF truncation) — %.2f%% truncated\n",
                chunker_name, s.content_defined, s.total, s.truncated, s.total, pct);
}

void print_result(const char* chunker_name, const char* case_name, const CompareResult& r) {
    std::printf("\n[%s / %s]\n", chunker_name, case_name);
    std::printf("  chunks in original:      %zu\n", r.total_a);
    std::printf("  chunks in modified:      %zu\n", r.total_b);
    std::printf("  shared (identical hash): %zu\n", r.shared);
    std::printf("  only in original:        %zu\n", r.only_a);
    std::printf("  only in modified:        %zu\n", r.only_b);
}

// Inserts a single byte at `trials` pseudorandom offsets (fixed seed, so
// the run is reproducible) and reports the distribution of how many
// chunks ended up unique to the original — i.e. the resync distance.
void run_offset_distribution(const IChunker& chunker, const char* chunker_name,
                              const IHasher& hasher, const std::vector<uint8_t>& original,
                              int trials, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> offset_dist(0, original.size() - 1);

    std::vector<size_t> only_a_counts;
    only_a_counts.reserve(trials);
    for (int t = 0; t < trials; ++t) {
        const size_t offset = offset_dist(rng);
        std::vector<uint8_t> modified = insert_bytes(original, offset, {0xAB});
        only_a_counts.push_back(compare_chunks(chunker, hasher, original, modified).only_a);
    }

    const size_t mn = *std::min_element(only_a_counts.begin(), only_a_counts.end());
    const size_t mx = *std::max_element(only_a_counts.begin(), only_a_counts.end());
    const double mean =
        std::accumulate(only_a_counts.begin(), only_a_counts.end(), size_t{0}) /
        static_cast<double>(trials);

    std::map<size_t, int> histogram;
    for (size_t v : only_a_counts) histogram[v]++;

    std::printf("\n[%s] single-byte insertion at %d pseudorandom offsets (seed=%u)\n",
                chunker_name, trials, seed);
    std::printf("  chunks-unique-to-original: min=%zu max=%zu mean=%.2f\n", mn, mx, mean);
    std::printf("  histogram (differing-chunk-count -> trial count):\n");
    for (const auto& kv : histogram) {
        std::printf("    %zu chunk%s differ: %2d trial%s\n", kv.first, kv.first == 1 ? "" : "s",
                    kv.second, kv.second == 1 ? "" : "s");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--gen") {
        generate(argv[2], std::stoul(argv[3]));
        return 0;
    }

    if (argc < 2) {
        std::fprintf(stderr, "usage: chunk_identity <file> [avg_kb]\n");
        std::fprintf(stderr, "       chunk_identity --gen <path> <size_mb>\n");
        return 1;
    }

    const size_t avg_kb = argc >= 3 ? std::stoul(argv[2]) : 8;
    std::vector<uint8_t> original = read_file(argv[1]);

    FastCDCConfig cfg;
    cfg.min_size = avg_kb * 1024 / 4;
    cfg.avg_size = avg_kb * 1024;
    cfg.max_size = avg_kb * 1024 * 8;

    FastCDCChunker fastcdc(cfg);
    FixedChunker fixed(avg_kb * 1024);
    Sha256Hasher hasher;

    std::printf("source file: %s (%zu bytes), avg_kb=%zu\n", argv[1], original.size(), avg_kb);

    // --- 0. Boundary-origin diagnostic (run first: explains the rest) ---
    print_boundary_stats("FastCDC", boundary_stats(fastcdc, original));

    // --- 1. Illustrative single cases ---
    for (const auto& [offset, label] :
         std::vector<std::pair<size_t, std::string>>{{0, "insert at offset 0"},
                                                       {original.size() / 2, "insert at midpoint"}}) {
        std::vector<uint8_t> modified = insert_bytes(original, offset, {0xAB});
        print_result("FastCDC", label.c_str(), compare_chunks(fastcdc, hasher, original, modified));
        print_result("Fixed-size", label.c_str(),
                      compare_chunks(fixed, hasher, original, modified));
    }

    // --- 2. Resync distance distribution over many offsets ---
    run_offset_distribution(fastcdc, "FastCDC", hasher, original, /*trials=*/50, /*seed=*/20240907);
    run_offset_distribution(fixed, "Fixed-size", hasher, original, /*trials=*/50, /*seed=*/20240907);

    // --- 3. Realistic multi-byte edit shapes ---
    const size_t edit_offset = original.size() / 3;
    std::mt19937 payload_rng(999);
    auto random_payload = [&](size_t n) {
        std::vector<uint8_t> v(n);
        for (auto& b : v) b = static_cast<uint8_t>(payload_rng() & 0xFF);
        return v;
    };

    {
        auto modified = insert_bytes(original, edit_offset, random_payload(100));
        print_result("FastCDC", "insert 100 bytes",
                      compare_chunks(fastcdc, hasher, original, modified));
        print_result("Fixed-size", "insert 100 bytes",
                      compare_chunks(fixed, hasher, original, modified));
    }
    {
        auto modified = insert_bytes(original, edit_offset, random_payload(10 * 1024));
        print_result("FastCDC", "insert 10 KiB",
                      compare_chunks(fastcdc, hasher, original, modified));
        print_result("Fixed-size", "insert 10 KiB",
                      compare_chunks(fixed, hasher, original, modified));
    }
    {
        auto modified = delete_bytes(original, edit_offset, 100);
        print_result("FastCDC", "delete 100 bytes",
                      compare_chunks(fastcdc, hasher, original, modified));
        print_result("Fixed-size", "delete 100 bytes",
                      compare_chunks(fixed, hasher, original, modified));
    }

    return 0;
}
