#include "dedupbackup/restore_pipeline.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "dedupbackup/hasher.hpp"
#include "dedupbackup/sha256_hasher.hpp"

namespace dedupbackup {

namespace {

namespace fs = std::filesystem;

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

// Reassembles one file's bytes from its manifest entry by fetching every
// chunk in order and concatenating. Throws immediately, naming the
// digest, if a referenced chunk isn't in the repository at all — a
// missing chunk means the pack file is missing data this snapshot
// depends on, which is not recoverable by continuing.
std::vector<uint8_t> reassemble(const Repository& repo, const ManifestFileEntry& entry) {
    std::vector<uint8_t> data;
    data.reserve(entry.size);
    for (const Digest& digest : entry.chunk_digests) {
        if (!repo.has_chunk(digest)) {
            throw std::runtime_error("restore: file '" + entry.path + "' references chunk " +
                                      to_hex(digest) +
                                      " which is not present in the repository (missing/corrupt "
                                      "pack file or index)");
        }
        const ChunkLocation loc = repo.locate_chunk(digest);
        const std::vector<uint8_t> chunk_bytes = repo.read_chunk(loc);
        data.insert(data.end(), chunk_bytes.begin(), chunk_bytes.end());
    }
    return data;
}

void write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // ec is deliberately not checked for "already exists" (not an
    // error); a genuine failure surfaces as the open() below failing.

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw_errno("restore: failed to open " + path.string() + " for writing");
    }
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("restore: write failed for " + path.string());
        }
        written += static_cast<size_t>(n);
    }
    ::close(fd);
    // No fsync here, deliberately: restore has no crash-durability
    // requirement the way backup's pack-file/WAL ordering does (§1.4) —
    // if restore is interrupted, simply re-running it reproduces the
    // same output, since it's not maintaining any incremental state a
    // partial run could leave inconsistent.
}

void apply_metadata(const fs::path& path, const ManifestFileEntry& entry) {
    // Permission bits only (mask off the file-type bits st_mode also
    // carries, e.g. S_IFREG) — chmod() only meaningfully accepts the
    // permission/setuid/setgid/sticky bits.
    if (::chmod(path.c_str(), entry.mode & 07777) != 0) {
        throw_errno("restore: chmod failed for " + path.string());
    }
    struct utimbuf times;
    times.actime = static_cast<time_t>(entry.mtime_unix);
    times.modtime = static_cast<time_t>(entry.mtime_unix);
    if (::utime(path.c_str(), &times) != 0) {
        throw_errno("restore: utime failed for " + path.string());
    }
}

} // namespace

size_t run_restore(const std::string& snapshot_id, const std::string& dest_dir, Repository& repo) {
    const Manifest manifest = repo.read_snapshot_manifest(snapshot_id);
    const Sha256Hasher hasher;
    const fs::path root(dest_dir);

    for (const ManifestFileEntry& entry : manifest.files) {
        const std::vector<uint8_t> data = reassemble(repo, entry);

        const fs::path out_path = root / entry.path;
        write_file(out_path, data);
        apply_metadata(out_path, entry);

        const Digest recomputed = hasher.hash(data.data(), data.size());
        if (recomputed != entry.file_digest) {
            throw std::runtime_error(
                "restore: VERIFICATION FAILED for '" + entry.path + "': recomputed digest " +
                to_hex(recomputed) + " does not match manifest digest " +
                to_hex(entry.file_digest) +
                " -- the restored bytes do not match what was backed up");
        }
    }

    return manifest.files.size();
}

} // namespace dedupbackup
