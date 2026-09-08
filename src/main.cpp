#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "dedupbackup/backup_pipeline.hpp"
#include "dedupbackup/repository.hpp"
#include "dedupbackup/restore_pipeline.hpp"
#include "dedupbackup/stats.hpp"
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

int cmd_stats(const Args& args) {
    if (args.repo.empty()) {
        std::fprintf(stderr, "usage: dedup-backup stats --repo <repo-path>\n");
        return 1;
    }
    Repository repo(args.repo);
    const RepoStats stats = compute_stats(repo);

    std::printf("snapshots:          %zu\n", stats.snapshot_count);
    std::printf("logical bytes:      %llu\n",
                static_cast<unsigned long long>(stats.logical_bytes));
    std::printf("physical bytes:     %llu\n",
                static_cast<unsigned long long>(stats.physical_bytes));
    std::printf("dedup saved:        %.1f%% (%.2fx)\n", stats.percent_saved(),
                stats.dedup_factor());
    std::printf("unique chunks:      %zu\n", stats.unique_chunks);
    std::printf("mean chunk size:    %.0f bytes\n", stats.mean_chunk_size);
    std::printf("median chunk size:  %llu bytes\n",
                static_cast<unsigned long long>(stats.median_chunk_size));
    return 0;
}

int cmd_bench(const Args& args) {
    if (args.positional.empty() || args.repo.empty()) {
        std::fprintf(stderr,
                     "usage: dedup-backup bench <source-dir> --repo <repo-path> "
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

    PipelineTimings timings;
    options.timings = &timings;

    const auto t0 = std::chrono::steady_clock::now();
    const std::string snapshot_id = run_backup(args.positional[0], repo, options);
    const auto t1 = std::chrono::steady_clock::now();
    const double wall_seconds = std::chrono::duration<double>(t1 - t0).count();

    const Manifest manifest = repo.read_snapshot_manifest(snapshot_id);
    uint64_t total_bytes = 0;
    for (const ManifestFileEntry& f : manifest.files) total_bytes += f.size;

    const double mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    const double mbps = wall_seconds > 0.0 ? mb / wall_seconds : 0.0;

    std::printf("bench: snapshot %s, %zu files, %.1f MB logical, %.2fs wall\n",
                snapshot_id.c_str(), manifest.files.size(), mb, wall_seconds);
    std::printf("sustained throughput: %.1f MB/s\n", mbps);

    const auto to_seconds = [](uint64_t ns) { return static_cast<double>(ns) / 1e9; };
    std::printf("\nphase breakdown (aggregate ACROSS ALL WORKER THREADS, not wall-clock --\n");
    std::printf("a fully-parallel run's phases sum to roughly threads * wall time, so this\n");
    std::printf("shows relative cost / where the bottleneck is, not a wall-time budget):\n");
    std::printf("  file I/O:       %.2fs\n", to_seconds(timings.read_ns.load()));
    std::printf("  chunking:       %.2fs\n", to_seconds(timings.chunk_ns.load()));
    std::printf("  hashing:        %.2fs\n", to_seconds(timings.hash_ns.load()));
    std::printf("  store+wal:      %.2fs  (single global mutex -- known secondary bottleneck)\n",
                to_seconds(timings.store_ns.load()));
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
    if (command == "stats") return cmd_stats(args);
    if (command == "bench") return cmd_bench(args);

    std::fprintf(stderr, "dedup-backup: unknown subcommand '%s'\n", command.c_str());
    return 1;
}
