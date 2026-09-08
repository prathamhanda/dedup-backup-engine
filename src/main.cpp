#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dedupbackup/backup_pipeline.hpp"
#include "dedupbackup/repository.hpp"

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

    // list/restore/verify/stats/bench land in components 8-9.
    std::fprintf(stderr, "dedup-backup: subcommand '%s' not implemented yet\n", command.c_str());
    return 1;
}
