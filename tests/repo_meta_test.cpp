// Tests Repository::check_or_init_chunk_config(): a repo commits to its
// chunking parameters on first backup, and a later backup with different
// parameters must be refused rather than silently corrupting dedup
// alignment.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "dedupbackup/backup_pipeline.hpp"
#include "dedupbackup/repository.hpp"

using namespace dedupbackup;
namespace fs = std::filesystem;

namespace {
int fail(const std::string& msg) {
    std::fprintf(stderr, "REPO_META TEST FAILED: %s\n", msg.c_str());
    return 1;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "dedupbackup_repo_meta_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    const fs::path src = base / "src";
    const fs::path repo_dir = base / "repo";
    fs::create_directories(src);
    {
        std::ofstream f(src / "a.txt", std::ios::binary);
        f << "some content for the repo_meta test, doesn't matter what";
    }

    Repository repo(repo_dir.string());

    // First backup: avg_chunk_kb=8 (default FastCDCConfig). This must
    // write repo.meta and succeed.
    BackupOptions options8kb;
    run_backup(src.string(), repo, options8kb);
    if (!fs::exists(repo_dir / "repo.meta")) {
        return fail("repo.meta was not created after the first backup");
    }

    // Second backup, same repo, DIFFERENT avg_chunk_kb: must be refused.
    BackupOptions options16kb;
    options16kb.chunk_config.min_size = 16 * 1024 / 4;
    options16kb.chunk_config.avg_size = 16 * 1024;
    options16kb.chunk_config.max_size = 16 * 1024 * 8;

    bool threw = false;
    std::string what;
    try {
        run_backup(src.string(), repo, options16kb);
    } catch (const std::runtime_error& e) {
        threw = true;
        what = e.what();
    }
    if (!threw) {
        return fail("mismatched avg_chunk_kb was NOT rejected -- repo.meta enforcement isn't working");
    }
    if (what.find("avg_size") == std::string::npos) {
        return fail("exception was thrown but didn't mention avg_size -- got: " + what);
    }
    std::printf("mismatch correctly rejected: %s\n", what.c_str());

    // Third backup, matching the ORIGINAL config exactly: must succeed
    // (confirms this isn't a false-positive rejection of every repeat
    // backup, only genuine mismatches).
    BackupOptions options8kb_again;
    try {
        run_backup(src.string(), repo, options8kb_again);
    } catch (const std::exception& e) {
        return fail(std::string("matching config was incorrectly rejected: ") + e.what());
    }

    std::printf("repo_meta test PASSED: mismatch rejected, matching config accepted\n");
    return 0;
}
