#include "dedupbackup/manifest.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "dedupbackup/byte_io.hpp"

namespace dedupbackup {

namespace {

constexpr char kMagic[8] = {'D', 'E', 'D', 'U', 'P', 'M', 'F', '1'};
constexpr uint32_t kVersion = 1;
constexpr size_t kSnapshotIdFieldSize = 16;  // 15 chars + 1 NUL pad byte

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

void append_bytes(std::vector<uint8_t>& buf, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}

void append_u16(std::vector<uint8_t>& buf, uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    append_bytes(buf, b, 2);
}

void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    uint8_t b[4];
    write_u32_le(b, v);
    append_bytes(buf, b, 4);
}

void append_u64(std::vector<uint8_t>& buf, uint64_t v) {
    uint8_t b[8];
    write_u64_le(b, v);
    append_bytes(buf, b, 8);
}

void append_i64(std::vector<uint8_t>& buf, int64_t v) {
    append_u64(buf, static_cast<uint64_t>(v));
}

// A small cursor over an in-memory buffer for parsing, so read_manifest
// can check bounds once per field and throw a clear "truncated manifest"
// error instead of reading past the end of a corrupt/short file.
class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}

    void need(size_t n) const {
        if (pos_ + n > size_) {
            throw std::runtime_error("manifest: truncated (unexpected end of file)");
        }
    }

    const uint8_t* take(size_t n) {
        need(n);
        const uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }

    uint16_t read_u16() {
        const uint8_t* p = take(2);
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    uint32_t read_u32() { return dedupbackup::read_u32_le(take(4)); }
    uint64_t read_u64() { return dedupbackup::read_u64_le(take(8)); }
    int64_t read_i64() { return static_cast<int64_t>(read_u64()); }

    bool at_end() const { return pos_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

} // namespace

void write_manifest(const std::string& path, const Manifest& manifest) {
    if (manifest.snapshot_id.size() >= kSnapshotIdFieldSize) {
        throw std::invalid_argument("write_manifest: snapshot_id too long for the fixed field "
                                     "(max 15 characters, got " +
                                     std::to_string(manifest.snapshot_id.size()) + ")");
    }

    // Sort a local copy by path — see the header comment for why this is
    // enforced here rather than trusted to callers.
    std::vector<ManifestFileEntry> files = manifest.files;
    std::sort(files.begin(), files.end(),
              [](const ManifestFileEntry& a, const ManifestFileEntry& b) { return a.path < b.path; });

    std::vector<uint8_t> buf;
    append_bytes(buf, kMagic, sizeof(kMagic));
    append_u32(buf, kVersion);

    // Fixed-width snapshot_id field: the string bytes, then zero-padded
    // to kSnapshotIdFieldSize. A fixed width (rather than a length
    // prefix, as paths use) is possible because the format guarantees a
    // canonical "YYYYMMDD-HHMMSS" shape — and it means a reader can
    // validate the entire fixed-size header (magic/version/snapshot_id/
    // created_unix/file_count) in one pass before it has to start
    // parsing variable-length file entries.
    {
        std::vector<uint8_t> id_field(kSnapshotIdFieldSize, 0);
        std::memcpy(id_field.data(), manifest.snapshot_id.data(), manifest.snapshot_id.size());
        append_bytes(buf, id_field.data(), id_field.size());
    }

    append_i64(buf, manifest.created_unix);
    append_u64(buf, static_cast<uint64_t>(files.size()));

    for (const ManifestFileEntry& f : files) {
        if (f.path.size() > 0xFFFF) {
            throw std::invalid_argument("write_manifest: path too long: " + f.path);
        }
        append_u16(buf, static_cast<uint16_t>(f.path.size()));
        append_bytes(buf, f.path.data(), f.path.size());
        append_u32(buf, f.mode);
        append_u64(buf, f.size);
        append_i64(buf, f.mtime_unix);
        append_bytes(buf, f.file_digest.data(), f.file_digest.size());
        append_u32(buf, static_cast<uint32_t>(f.chunk_digests.size()));
        for (const Digest& d : f.chunk_digests) {
            append_bytes(buf, d.data(), d.size());
        }
    }

    // Single buffered write + one fsync — see header comment for why
    // this doesn't need the WAL's incremental-durability machinery.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw_errno("write_manifest: failed to open " + path);
    }
    size_t written = 0;
    while (written < buf.size()) {
        const ssize_t n = ::write(fd, buf.data() + written, buf.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("write_manifest: write failed");
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("write_manifest: fsync failed");
    }
    ::close(fd);
}

Manifest read_manifest(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw_errno("read_manifest: failed to open " + path);
    }

    const off_t file_size = ::lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("read_manifest: failed to seek to end of " + path);
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    size_t got = 0;
    while (got < buf.size()) {
        const ssize_t n = ::pread(fd, buf.data() + got, buf.size() - got,
                                   static_cast<off_t>(got));
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("read_manifest: pread failed");
        }
        if (n == 0) break; // shouldn't happen given lseek above, but don't loop forever
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    if (got != buf.size()) {
        throw std::runtime_error("read_manifest: short read on " + path);
    }

    Cursor c(buf.data(), buf.size());

    const uint8_t* magic = c.take(sizeof(kMagic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("read_manifest: bad magic in " + path +
                                  " (not a dedup-backup manifest, or a different version)");
    }
    const uint32_t version = c.read_u32();
    if (version != kVersion) {
        throw std::runtime_error("read_manifest: unsupported manifest version " +
                                  std::to_string(version) + " in " + path);
    }

    Manifest manifest;
    {
        const uint8_t* id_field = c.take(kSnapshotIdFieldSize);
        const uint8_t* nul = static_cast<const uint8_t*>(
            std::memchr(id_field, 0, kSnapshotIdFieldSize));
        const size_t id_len = nul ? static_cast<size_t>(nul - id_field) : kSnapshotIdFieldSize;
        manifest.snapshot_id.assign(reinterpret_cast<const char*>(id_field), id_len);
    }
    manifest.created_unix = c.read_i64();

    const uint64_t file_count = c.read_u64();
    manifest.files.reserve(static_cast<size_t>(file_count));

    for (uint64_t i = 0; i < file_count; ++i) {
        ManifestFileEntry f;
        const uint16_t path_len = c.read_u16();
        const uint8_t* path_bytes = c.take(path_len);
        f.path.assign(reinterpret_cast<const char*>(path_bytes), path_len);
        f.mode = c.read_u32();
        f.size = c.read_u64();
        f.mtime_unix = c.read_i64();

        const uint8_t* digest_bytes = c.take(f.file_digest.size());
        std::memcpy(f.file_digest.data(), digest_bytes, f.file_digest.size());

        const uint32_t chunk_count = c.read_u32();
        f.chunk_digests.resize(chunk_count);
        for (uint32_t j = 0; j < chunk_count; ++j) {
            const uint8_t* cd = c.take(f.chunk_digests[j].size());
            std::memcpy(f.chunk_digests[j].data(), cd, f.chunk_digests[j].size());
        }

        manifest.files.push_back(std::move(f));
    }

    if (!c.at_end()) {
        throw std::runtime_error("read_manifest: trailing bytes after last file entry in " + path);
    }

    return manifest;
}

} // namespace dedupbackup
