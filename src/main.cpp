#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "dedupbackup/backup_pipeline.hpp"
#include "dedupbackup/repository.hpp"
#include "dedupbackup/restore_pipeline.hpp"
#include "dedupbackup/verify.hpp"

using namespace dedupbackup;

namespace {

// Minimal ad hoc argument parsing -- the CLI surface (§3.4) is small
// enough (one positional value or two, a handful of --flags) that a
// full argument-parsing library would be more machinery than the
// problem needs.
struct Args {
    std::vector<std::string> positional;
    std::string repo;
    size_t threads = 0;
    size_t avg_chunk_kb = 8;
    bool fixed_chunking = false;
};

Args parse_args(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--repo" && i + 1 < argc) {
            a.repo = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            a.threads = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--avg-chunk-kb" && i + 1 < argc) {
            a.avg_chunk_kb = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--fixed-chunking") {
            a.fixed_chunking = true;
        } else {
            a.positional.push_back(arg);
        }
    }
    return a;
}

int cmd_init(const Args& args) {
    if (args.positional.empty()) {
        std::fprintf(stderr, "usage: dedup-backup init <repo-path>\n");
        return 1;
    }
    Repository repo(args.positional[0]);
    std::printf("initialized repository at %s\n", repo.path().c_str());
    return 0;
}

int cmd_backup(const Args& args) {
    if (args.positional.empty() || args.repo.empty()) {
        std::fprintf(stderr,
                     "usage: dedup-backup backup <source-dir> --repo <repo-path> "
                     "[--threads N] [--avg-chunk-kb K] [--fixed-chunking]\n");
        return 1;
    }

    Repository repo(args.repo);

    BackupOptions options;
    options.thread_count = args.threads;
    options.fixed_chunking = args.fixed_chunking;
    options.chunk_config.min_size = args.avg_chunk_kb * 1024 / 4;
    options.chunk_config.avg_size = args.avg_chunk_kb * 1024;
    options.chunk_config.max_size = args.avg_chunk_kb * 1024 * 8;

    const auto start = std::chrono::steady_clock::now();
    const std::string snapshot_id = run_backup(args.positional[0], repo, options);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();

    const Manifest manifest = repo.read_snapshot_manifest(snapshot_id);
    std::printf("snapshot %s created (%zu files) in %.2fs\n", snapshot_id.c_str(),
                manifest.files.size(), seconds);
    std::printf("repository now holds %zu unique chunks\n", repo.unique_chunk_count());
    return 0;
}

int cmd_restore(const Args& args) {
    if (args.positional.size() < 2 || args.repo.empty()) {
        std::fprintf(stderr, "usage: dedup-backup restore <snapshot-id> <dest-dir> --repo <repo-path>\n");
        return 1;
    }
    Repository repo(args.repo);
    try {
        const size_t count = run_restore(args.positional[0], args.positional[1], repo);
        std::printf("restored and verified %zu files from snapshot %s into %s\n", count,
                    args.positional[0].c_str(), args.positional[1].c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "restore failed: %s\n", e.what());
        return 1;
    }
}

int cmd_verify(const Args& args) {
    if (args.repo.empty()) {
        std::fprintf(stderr, "usage: dedup-backup verify --repo <repo-path>\n");
        return 1;
    }
    Repository repo(args.repo);
    const VerifyReport report = run_verify(repo);

    std::printf("checked %zu snapshots, %zu files, %zu unique chunks (%zu chunk references)\n",
                report.snapshots_checked, report.files_checked, report.unique_chunks_verified,
                report.chunk_references_walked);

    if (report.ok()) {
        std::printf("verify OK: no issues found\n");
        return 0;
    }

    std::printf("verify FAILED: %zu issue(s) found\n", report.issues.size());
    for (const VerifyIssue& issue : report.issues) {
        std::printf("  [%s]%s%s: %s\n", issue.snapshot_id.c_str(),
                    issue.file_path.empty() ? "" : " ", issue.file_path.c_str(),
                    issue.message.c_str());
    }
    return 1;
}

int cmd_list(const Args& args) {
    if (args.repo.empty()) {
        std::fprintf(stderr, "usage: dedup-backup list --repo <repo-path>\n");
        return 1;
    }
    Repository repo(args.repo);
    for (const std::string& id : repo.list_snapshots()) {
        std::printf("%s\n", id.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "dedup-backup: no subcommand given\n");
        return 1;
    }

    const std::string command = argv[1];
    const Args args = parse_args(argc, argv, 2);

    if (command == "init") return cmd_init(args);
    if (command == "backup") return cmd_backup(args);
    if (command == "restore") return cmd_restore(args);
    if (command == "verify") return cmd_verify(args);
    if (command == "list") return cmd_list(args);

    // stats/bench land in component 9.
    std::fprintf(stderr, "dedup-backup: subcommand '%s' not implemented yet\n", command.c_str());
    return 1;
}
