// CTest test 2/4 (per the original spec) — "the test that proves CDC
// works": back up a file, insert one byte at the front, back up again,
// assert the second snapshot added only a small number of new chunks.
//
// Unlike tools/chunk_identity.cpp (which tests FastCDCChunker directly),
// this goes through the REAL backup pipeline end to end — Repository,
// dedup, WAL, index, manifest — so it's proof the whole stack delivers
// the property, not just the chunker in isolation.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "dedupbackup/backup_pipeline.hpp"
#include "dedupbackup/repository.hpp"

using namespace dedupbackup;
namespace fs = std::filesystem;

namespace {
int fail(const std::string& msg) {
    std::fprintf(stderr, "INSERTION IDENTITY TEST FAILED: %s\n", msg.c_str());
    return 1;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "dedupbackup_insertion_identity_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    const fs::path src = base / "src";
    const fs::path repo_dir = base / "repo";
    fs::create_directories(src);

    // 2 MB of real entropy -- see the component 1/3 history in the
    // handoff doc for why a weak PRNG here would produce misleading
    // results (a bad generator's low bits interact badly with the gear
    // hash's boundary test).
    std::random_device rd;
    std::vector<uint8_t> data(2 * 1024 * 1024);
    for (size_t i = 0; i < data.size();) {
        uint32_t r = rd();
        for (int b = 0; b < 4 && i < data.size(); ++b, ++i) data[i] = static_cast<uint8_t>(r >> (b * 8));
    }
    const fs::path target_file = src / "growing.bin";
    {
        std::ofstream f(target_file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    Repository repo(repo_dir.string());
    BackupOptions options;

    run_backup(src.string(), repo, options);
    const size_t chunks_after_first = repo.unique_chunk_count();

    // Insert one byte at the very front.
    data.insert(data.begin(), 0xAB);
    {
        std::ofstream f(target_file, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    run_backup(src.string(), repo, options);
    const size_t chunks_after_second = repo.unique_chunk_count();

    if (chunks_after_second < chunks_after_first) {
        return fail("chunk count DECREASED after second backup (" +
                    std::to_string(chunks_after_first) + " -> " +
                    std::to_string(chunks_after_second) + ") -- should never happen");
    }

    const size_t new_chunks = chunks_after_second - chunks_after_first;

    // Threshold: the resync distribution measured in components 1-3
    // (50/50 pseudorandom trials landing on exactly 1 differing chunk on
    // random data) predicts this should be 1, occasionally 2 from the
    // documented second-order normalized-chunking effect. Allowing up to
    // 3 leaves margin without being so loose the test stops meaning
    // anything -- fixed-size chunking on the same edit would cost
    // hundreds of chunks (proven in tools/chunk_identity.cpp), so even a
    // generous small-N threshold cleanly separates "CDC working" from
    // "CDC broken".
    constexpr size_t kMaxAcceptableNewChunks = 3;
    if (new_chunks > kMaxAcceptableNewChunks) {
        return fail("second backup added " + std::to_string(new_chunks) +
                    " new chunks after a single-byte insertion (expected <= " +
                    std::to_string(kMaxAcceptableNewChunks) +
                    ") -- content-defined chunking does not appear to be working");
    }

    std::printf(
        "insertion identity test PASSED: %zu chunks -> %zu chunks (+%zu) after a 1-byte insertion "
        "into a 2 MB file\n",
        chunks_after_first, chunks_after_second, new_chunks);
    return 0;
}
