// CTest test 1/4 (per the original spec): back up a generated tree,
// restore it, assert byte-exact equality (and mode bits) — through the
// REAL backup_pipeline/restore_pipeline, not any lower-level shortcut.
//
// Exit 0 = pass, non-zero = fail, with a clear message identifying what
// didn't match.

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "dedupbackup/backup_pipeline.hpp"
#include "dedupbackup/repository.hpp"
#include "dedupbackup/restore_pipeline.hpp"

using namespace dedupbackup;
namespace fs = std::filesystem;

namespace {

void write_random_file(const fs::path& path, size_t size, std::mt19937& rng) {
    fs::create_directories(path.parent_path());
    std::vector<uint8_t> data(size);
    for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

int fail(const std::string& msg) {
    std::fprintf(stderr, "ROUNDTRIP TEST FAILED: %s\n", msg.c_str());
    return 1;
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "dedupbackup_roundtrip_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    const fs::path src = base / "src";
    const fs::path repo_dir = base / "repo";
    const fs::path dst = base / "restored";

    std::mt19937 rng(4242);
    write_random_file(src / "small.txt", 37, rng);
    write_random_file(src / "medium.bin", 300 * 1024, rng);
    write_random_file(src / "sub" / "deep" / "nested.bin", 50 * 1024, rng);
    {
        // A zero-byte file: an edge case worth covering explicitly (no
        // chunks at all, per manifest.hpp's contract).
        fs::create_directories(src / "sub");
        std::ofstream f(src / "sub" / "empty.txt", std::ios::binary);
    }
    fs::permissions(src / "small.txt", fs::perms::owner_read | fs::perms::owner_write, ec);

    Repository repo(repo_dir.string());
    BackupOptions options; // defaults: FastCDC, hardware_concurrency() threads
    const std::string snapshot_id = run_backup(src.string(), repo, options);

    const size_t restored_count = run_restore(snapshot_id, dst.string(), repo);
    if (restored_count != 4) {
        return fail("expected 4 files restored, got " + std::to_string(restored_count));
    }

    // Byte-exact comparison, file by file, plus confirming no files are
    // missing or extra on either side.
    size_t src_file_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        ++src_file_count;
        const fs::path rel = entry.path().lexically_relative(src);
        const fs::path restored_path = dst / rel;
        if (!fs::exists(restored_path)) {
            return fail("missing in restore: " + rel.string());
        }
        const std::vector<uint8_t> original = read_file(entry.path());
        const std::vector<uint8_t> restored = read_file(restored_path);
        if (original != restored) {
            return fail("byte mismatch: " + rel.string() + " (original " +
                        std::to_string(original.size()) + " bytes, restored " +
                        std::to_string(restored.size()) + " bytes)");
        }
    }

    size_t dst_file_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dst)) {
        if (entry.is_regular_file()) ++dst_file_count;
    }
    if (dst_file_count != src_file_count) {
        return fail("restored tree has " + std::to_string(dst_file_count) +
                    " files, source has " + std::to_string(src_file_count) +
                    " (extra or missing files)");
    }

    // Mode bits: check the one file we deliberately set non-default
    // permissions on.
    struct stat st_src{}, st_dst{};
    ::stat((src / "small.txt").c_str(), &st_src);
    ::stat((dst / "small.txt").c_str(), &st_dst);
    if ((st_src.st_mode & 07777) != (st_dst.st_mode & 07777)) {
        return fail("mode bits not preserved for small.txt");
    }

    std::printf("roundtrip test PASSED: %zu files, byte-exact, mode bits preserved\n",
                src_file_count);
    return 0;
}
