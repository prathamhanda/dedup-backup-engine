// Standalone demo/debug tool: chunk a file with FastCDC (and, for
// comparison, the fixed-size chunker) and print the resulting size
// distribution. Not part of the dedup-backup CLI — just a way to eyeball
// that the chunker is behaving before it's wired into the real pipeline.
//
// Usage: chunk_stats <file> [avg_kb]

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <vector>

#include "dedupbackup/fastcdc_chunker.hpp"
#include "dedupbackup/fixed_chunker.hpp"

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

void print_stats(const char* label, const std::vector<ChunkSpan>& chunks, size_t total_bytes) {
    std::vector<size_t> sizes;
    sizes.reserve(chunks.size());
    for (const auto& c : chunks) sizes.push_back(c.length);
    std::sort(sizes.begin(), sizes.end());

    const size_t n = sizes.size();
    const size_t sum = std::accumulate(sizes.begin(), sizes.end(), size_t{0});
    const double mean = n ? static_cast<double>(sum) / n : 0.0;
    const size_t median = n ? sizes[n / 2] : 0;
    const size_t min = n ? sizes.front() : 0;
    const size_t max = n ? sizes.back() : 0;

    std::printf("\n=== %s ===\n", label);
    std::printf("file size:    %zu bytes\n", total_bytes);
    std::printf("chunk count:  %zu\n", n);
    std::printf("mean size:    %.0f bytes\n", mean);
    std::printf("median size:  %zu bytes\n", median);
    std::printf("min size:     %zu bytes\n", min);
    std::printf("max size:     %zu bytes\n", max);

    // Coarse histogram in 4 KiB buckets, capped so pathological inputs
    // don't print an unbounded number of rows.
    std::printf("distribution (4 KiB buckets):\n");
    const size_t bucket_kib = 4;
    std::vector<size_t> hist(17, 0);  // 0..64KiB in 4KiB steps, last = overflow
    for (size_t s : sizes) {
        size_t bucket = (s / 1024) / bucket_kib;
        if (bucket >= hist.size()) bucket = hist.size() - 1;
        hist[bucket]++;
    }
    for (size_t i = 0; i < hist.size(); ++i) {
        if (hist[i] == 0) continue;
        std::printf("  %3zu-%3zu KiB | %6zu | ", i * bucket_kib,
                    (i + 1) * bucket_kib, hist[i]);
        const size_t bar = std::min<size_t>(hist[i], 60);
        for (size_t b = 0; b < bar; ++b) std::putchar('#');
        std::putchar('\n');
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: chunk_stats <file> [avg_kb]\n");
        return 1;
    }

    const size_t avg_kb = argc >= 3 ? std::stoul(argv[2]) : 8;

    std::vector<uint8_t> data = read_file(argv[1]);

    FastCDCConfig cfg;
    cfg.min_size = avg_kb * 1024 / 4;
    cfg.avg_size = avg_kb * 1024;
    cfg.max_size = avg_kb * 1024 * 8;

    std::printf("config: min=%zu avg=%zu max=%zu bytes\n", cfg.min_size, cfg.avg_size,
                cfg.max_size);

    FastCDCChunker fastcdc(cfg);
    auto fastcdc_chunks = fastcdc.chunk(data.data(), data.size());
    print_stats("FastCDC", fastcdc_chunks, data.size());

    FixedChunker fixed(avg_kb * 1024);
    auto fixed_chunks = fixed.chunk(data.data(), data.size());
    print_stats("Fixed-size", fixed_chunks, data.size());

    return 0;
}
