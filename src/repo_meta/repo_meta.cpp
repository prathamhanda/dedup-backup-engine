#include "dedupbackup/repo_meta.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "dedupbackup/byte_io.hpp"

namespace dedupbackup {

namespace {

constexpr char kMagic[8] = {'D', 'E', 'D', 'U', 'P', 'B', 'K', '1'};
constexpr uint32_t kVersion = 1;
constexpr size_t kFileSize = 8 + 4 + 4 + 4 + 4 + 8; // magic+version+min+avg+max+gear_seed = 32

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

} // namespace

void write_repo_meta(const std::string& path, const RepoMeta& meta) {
    std::vector<uint8_t> buf(kFileSize);
    size_t off = 0;
    std::memcpy(&buf[off], kMagic, sizeof(kMagic));
    off += sizeof(kMagic);
    write_u32_le(&buf[off], kVersion);
    off += 4;
    write_u32_le(&buf[off], meta.min_size);
    off += 4;
    write_u32_le(&buf[off], meta.avg_size);
    off += 4;
    write_u32_le(&buf[off], meta.max_size);
    off += 4;
    write_u64_le(&buf[off], meta.gear_seed);
    off += 8;

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw_errno("write_repo_meta: failed to open " + path);
    size_t written = 0;
    while (written < buf.size()) {
        const ssize_t n = ::write(fd, buf.data() + written, buf.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("write_repo_meta: write failed");
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("write_repo_meta: fsync failed");
    }
    ::close(fd);
}

RepoMeta read_repo_meta(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw_errno("read_repo_meta: failed to open " + path);

    std::vector<uint8_t> buf(kFileSize);
    size_t got = 0;
    while (got < buf.size()) {
        const ssize_t n = ::pread(fd, buf.data() + got, buf.size() - got, static_cast<off_t>(got));
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("read_repo_meta: pread failed");
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    if (got != buf.size()) {
        throw std::runtime_error("read_repo_meta: " + path +
                                  " is truncated (not a valid repo.meta)");
    }

    if (std::memcmp(buf.data(), kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("read_repo_meta: bad magic in " + path);
    }
    size_t off = sizeof(kMagic);
    const uint32_t version = read_u32_le(&buf[off]);
    if (version != kVersion) {
        throw std::runtime_error("read_repo_meta: unsupported version " +
                                  std::to_string(version) + " in " + path);
    }
    off += 4;

    RepoMeta meta;
    meta.min_size = read_u32_le(&buf[off]);
    off += 4;
    meta.avg_size = read_u32_le(&buf[off]);
    off += 4;
    meta.max_size = read_u32_le(&buf[off]);
    off += 4;
    meta.gear_seed = read_u64_le(&buf[off]);
    return meta;
}

} // namespace dedupbackup
