// Standalone check for the manifest format: builds a synthetic Manifest
// (fabricated paths/metadata, real SHA-256 digests over random buffers —
// no real filesystem access, since this component doesn't touch the
// filesystem at all), writes it, reads it back, and verifies every field
// round-trips exactly. Also exercises:
//   - internal sort-by-path (entries inserted out of order)
//   - a zero-chunk file (empty file -> chunk_count == 0)
//   - the snapshot_id fixed-width boundary (15 chars OK, 16 rejected)
//   - corruption detection (bad magic, truncated file)
//
// Usage: manifest_check <repo-dir>

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "dedupbackup/manifest.hpp"
#include "dedupbackup/sha256_hasher.hpp"

using namespace dedupbackup;

namespace {

void make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir %s failed: %s\n", path.c_str(), std::strerror(errno));
        std::exit(1);
    }
}

Digest fake_digest(Sha256Hasher& hasher, std::mt19937& rng) {
    std::vector<uint8_t> buf(32);
    for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFF);
    return hasher.hash(buf.data(), buf.size());
}

bool manifests_equal(const Manifest& a, const Manifest& b) {
    if (a.snapshot_id != b.snapshot_id || a.created_unix != b.created_unix) return false;
    if (a.files.size() != b.files.size()) return false;
    for (size_t i = 0; i < a.files.size(); ++i) {
        const auto& fa = a.files[i];
        const auto& fb = b.files[i];
        if (fa.path != fb.path || fa.mode != fb.mode || fa.size != fb.size ||
            fa.mtime_unix != fb.mtime_unix || fa.file_digest != fb.file_digest) {
            return false;
        }
        if (fa.chunk_digests != fb.chunk_digests) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: manifest_check <repo-dir>\n");
        return 1;
    }
    const std::string repo_dir = argv[1];
    make_dir(repo_dir);
    make_dir(repo_dir + "/snapshots");
    const std::string manifest_path = repo_dir + "/snapshots/20260907-143022.manifest";

    Sha256Hasher hasher;
    std::mt19937 rng(2026);

    // --- 1. Build a synthetic manifest, deliberately out of path order,
    //        including one zero-chunk (empty) file. ---
    Manifest original;
    original.snapshot_id = "20260907-143022"; // exactly 15 chars
    original.created_unix = 1799500222;

    const std::vector<std::string> paths = {"src/main.cpp", "README.md", "src/chunker/fastcdc.cpp",
                                             "empty.txt", "a/b/c/deep.bin"};
    for (const auto& p : paths) {
        ManifestFileEntry f;
        f.path = p;
        f.mode = 0100644;
        f.mtime_unix = 1799400000 + static_cast<int64_t>(rng() % 100000);
        f.file_digest = fake_digest(hasher, rng);

        if (p == "empty.txt") {
            f.size = 0;
            // no chunks at all -- an empty file reconstructs from zero chunks
        } else {
            const int chunk_count = 1 + static_cast<int>(rng() % 5);
            uint64_t size = 0;
            for (int i = 0; i < chunk_count; ++i) {
                f.chunk_digests.push_back(fake_digest(hasher, rng));
                size += 1000 + (rng() % 8000);
            }
            f.size = size;
        }
        original.files.push_back(std::move(f));
    }

    write_manifest(manifest_path, original);
    std::printf("wrote manifest: %s (%zu files, inserted out of path order)\n",
                manifest_path.c_str(), original.files.size());

    Manifest read_back = read_manifest(manifest_path);

    // Verify sort-by-path actually happened: read_back.files should be
    // in ascending path order even though `original.files` wasn't.
    for (size_t i = 1; i < read_back.files.size(); ++i) {
        if (!(read_back.files[i - 1].path < read_back.files[i].path)) {
            std::printf("SORT FAILED: entries not in ascending path order at index %zu\n", i);
            return 1;
        }
    }
    std::printf("sort-by-path OK: entries stored in ascending order regardless of insertion order\n");

    // Re-sort `original` the same way to compare field-for-field.
    Manifest original_sorted = original;
    std::sort(original_sorted.files.begin(), original_sorted.files.end(),
              [](const ManifestFileEntry& a, const ManifestFileEntry& b) { return a.path < b.path; });

    if (!manifests_equal(original_sorted, read_back)) {
        std::printf("ROUND-TRIP MISMATCH: read-back manifest doesn't match what was written\n");
        return 1;
    }
    std::printf("round-trip OK: all %zu file entries (including the zero-chunk empty file) match "
                "exactly\n",
                read_back.files.size());

    // --- 2. snapshot_id fixed-width boundary: 15 chars OK, 16 rejected. ---
    {
        Manifest m;
        m.snapshot_id = "123456789012345"; // exactly 15 chars
        m.created_unix = 0;
        write_manifest(repo_dir + "/snapshots/boundary_ok.manifest", m);
        std::printf("snapshot_id boundary OK: 15-character id accepted\n");
    }
    {
        Manifest m;
        m.snapshot_id = "1234567890123456"; // 16 chars -- must be rejected
        m.created_unix = 0;
        bool threw = false;
        try {
            write_manifest(repo_dir + "/snapshots/boundary_bad.manifest", m);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            std::printf("SNAPSHOT_ID BOUNDARY TEST FAILED: 16-char id should have been rejected\n");
            return 1;
        }
        std::printf("snapshot_id boundary OK: 16-character id correctly rejected\n");
    }

    // --- 3. Corruption detection: bad magic. ---
    {
        const std::string bad_path = repo_dir + "/snapshots/bad_magic.manifest";
        std::ofstream f(bad_path, std::ios::binary);
        const char junk[8] = {'N', 'O', 'T', 'A', 'M', 'F', '0', '0'};
        f.write(junk, sizeof(junk));
        f.close();

        bool threw = false;
        try {
            read_manifest(bad_path);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::printf("BAD MAGIC TEST FAILED: should have thrown\n");
            return 1;
        }
        std::printf("corruption detection OK: bad magic correctly rejected\n");
    }

    // --- 4. Corruption detection: truncated file (valid header, cut
    //        off partway through the first file entry). ---
    {
        const std::string trunc_path = repo_dir + "/snapshots/truncated.manifest";
        std::ifstream src(manifest_path, std::ios::binary);
        std::vector<char> full((std::istreambuf_iterator<char>(src)),
                                std::istreambuf_iterator<char>());
        std::ofstream dst(trunc_path, std::ios::binary);
        // Keep the fixed header (8+4+16+8+8 = 44 bytes) plus a few bytes
        // into the first file entry, then stop -- simulates a manifest
        // write that was interrupted mid-file-entry.
        const size_t cut = 44 + 5;
        dst.write(full.data(), static_cast<std::streamsize>(std::min(cut, full.size())));
        dst.close();

        bool threw = false;
        try {
            read_manifest(trunc_path);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::printf("TRUNCATION TEST FAILED: should have thrown\n");
            return 1;
        }
        std::printf("corruption detection OK: truncated manifest correctly rejected\n");
    }

    return 0;
}
