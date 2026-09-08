#include "dedupbackup/repository.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

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

} // namespace dedupbackup
