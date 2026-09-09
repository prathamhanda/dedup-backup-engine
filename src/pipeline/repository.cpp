#include "dedupbackup/repository.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "dedupbackup/repo_meta.hpp"

namespace dedupbackup {

namespace {

void make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("Repository: mkdir " + path + " failed: " + std::strerror(errno));
    }
}

// Creates the repo's directory structure and returns the pack file path.
// Called as a mem-initializer-list argument expression (below) so the
// directories are guaranteed to exist BEFORE ChunkStore's constructor
// runs and tries to open a file inside packs/ — a plain function call in
// an initializer list, rather than stuffing directory creation into the
// constructor body, which would run too late (member construction has
// already happened by the time the body executes).
std::string prepare_repo_dirs(const std::string& repo_path) {
    make_dir(repo_path);
    make_dir(repo_path + "/packs");
    make_dir(repo_path + "/snapshots");
    return repo_path + "/packs/pack-000001.dat";
}

} // namespace

Repository::Repository(const std::string& repo_path)
    : repo_path_(repo_path), store_(prepare_repo_dirs(repo_path)), wal_(repo_path + "/wal.log") {
    wal_.replay(index_);
}

ChunkLocation Repository::store_chunk_if_absent(const Digest& digest, const uint8_t* data,
                                                  uint32_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index_.contains(digest)) {
        return index_.find(digest);
    }
    // Ordering is load-bearing (see wal.hpp): chunk bytes durable first,
    // WAL record durable second, in-memory index updated last.
    const ChunkLocation loc = store_.append(data, length);
    wal_.append_chunk_add(digest, loc);
    index_.insert(digest, loc);
    return loc;
}

bool Repository::has_chunk(const Digest& digest) const {
    return index_.contains(digest);
}

ChunkLocation Repository::locate_chunk(const Digest& digest) const {
    return index_.find(digest);
}

std::vector<uint8_t> Repository::read_chunk(const ChunkLocation& loc) const {
    return store_.read(loc);
}

void Repository::write_snapshot_manifest(const Manifest& manifest) {
    dedupbackup::write_manifest(repo_path_ + "/snapshots/" + manifest.snapshot_id + ".manifest",
                                 manifest);
}

Manifest Repository::read_snapshot_manifest(const std::string& snapshot_id) const {
    return dedupbackup::read_manifest(repo_path_ + "/snapshots/" + snapshot_id + ".manifest");
}

bool Repository::has_snapshot(const std::string& snapshot_id) const {
    std::error_code ec;
    return std::filesystem::exists(repo_path_ + "/snapshots/" + snapshot_id + ".manifest", ec);
}

ChunkSizeStats Repository::compute_chunk_size_stats() const {
    std::vector<uint32_t> sizes;
    sizes.reserve(index_.size());
    uint64_t total = 0;
    for (const auto& kv : index_) {
        sizes.push_back(kv.second.length);
        total += kv.second.length;
    }

    ChunkSizeStats stats;
    stats.physical_bytes = total;
    stats.unique_chunks = sizes.size();
    if (!sizes.empty()) {
        stats.mean_chunk_size = static_cast<double>(total) / static_cast<double>(sizes.size());
        std::sort(sizes.begin(), sizes.end());
        stats.median_chunk_size = sizes[sizes.size() / 2];
    }
    return stats;
}

void Repository::check_or_init_chunk_config(const FastCDCConfig& config) {
    const std::string meta_path = repo_path_ + "/repo.meta";
    std::error_code ec;
    const bool exists = std::filesystem::exists(meta_path, ec);

    if (!exists) {
        RepoMeta meta;
        meta.min_size = static_cast<uint32_t>(config.min_size);
        meta.avg_size = static_cast<uint32_t>(config.avg_size);
        meta.max_size = static_cast<uint32_t>(config.max_size);
        meta.gear_seed = kGearSeed;
        write_repo_meta(meta_path, meta);
        return;
    }

    const RepoMeta stored = read_repo_meta(meta_path);
    std::string mismatches;
    if (stored.min_size != config.min_size) {
        mismatches += "min_size (repo: " + std::to_string(stored.min_size) + ", requested: " +
                      std::to_string(config.min_size) + ") ";
    }
    if (stored.avg_size != config.avg_size) {
        mismatches += "avg_size (repo: " + std::to_string(stored.avg_size) + ", requested: " +
                      std::to_string(config.avg_size) + ") ";
    }
    if (stored.max_size != config.max_size) {
        mismatches += "max_size (repo: " + std::to_string(stored.max_size) + ", requested: " +
                      std::to_string(config.max_size) + ") ";
    }
    if (stored.gear_seed != kGearSeed) {
        mismatches += "gear_seed (repo: " + std::to_string(stored.gear_seed) + ", this build: " +
                      std::to_string(kGearSeed) + ") ";
    }

    if (!mismatches.empty()) {
        throw std::runtime_error(
            "chunk config mismatch against repo.meta -- refusing to proceed, since chunking "
            "with different parameters would silently stop deduplicating against what's "
            "already stored: " +
            mismatches);
    }
}

std::vector<std::string> Repository::list_snapshots() const {
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(repo_path_ + "/snapshots", ec)) {
        if (!entry.is_regular_file()) continue;
        const std::filesystem::path& p = entry.path();
        if (p.extension() == ".manifest") {
            ids.push_back(p.stem().string());
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace dedupbackup
