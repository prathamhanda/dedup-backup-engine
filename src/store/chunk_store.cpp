#include "dedupbackup/chunk_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace dedupbackup {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

} // namespace

ChunkStore::ChunkStore(const std::string& pack_path) {
    // O_RDWR, not O_WRONLY: read() and append() share this one fd.
    // Deliberately no O_APPEND — see append()'s comment for why.
    fd_ = ::open(pack_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw_errno("ChunkStore: failed to open " + pack_path);
    }

    // The pack file may already hold data from a previous run against
    // this repo — don't assume it's empty. lseek(SEEK_END) both answers
    // "how big is this file" and is the plain POSIX way to ask, with no
    // std::filesystem dependency (that arrives in component 6).
    const off_t end = ::lseek(fd_, 0, SEEK_END);
    if (end < 0) {
        const int saved_errno = errno;
        ::close(fd_);
        errno = saved_errno;
        throw_errno("ChunkStore: failed to seek to end of pack file");
    }
    write_offset_ = static_cast<uint64_t>(end);
}

ChunkStore::~ChunkStore() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

ChunkLocation ChunkStore::append(const uint8_t* data, uint32_t length) {
    const uint64_t offset = write_offset_;

    // pwrite(), not write()+O_APPEND: write()/read() on a shared fd use
    // one implicit file-offset cursor, so a concurrent read (restore)
    // and write (backup) on the same store would race over where the
    // next operation lands. pwrite()/pread() take an explicit offset and
    // never touch that cursor, which removes the race by construction
    // rather than by convention.
    //
    // Looped rather than a single call: POSIX doesn't guarantee pwrite()
    // transfers the whole buffer in one call (EINTR, or a
    // filesystem-dependent partial write). Chunks are small (<=64 KB)
    // so in practice this is always one iteration, but handling it
    // costs nothing and closes off a rare, latent corruption bug.
    size_t written = 0;
    while (written < length) {
        const ssize_t n = ::pwrite(fd_, data + written, length - written,
                                    static_cast<off_t>(offset + written));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("ChunkStore: pwrite failed");
        }
        written += static_cast<size_t>(n);
    }

    // Durability point: append() does not return until this completes.
    // Everything downstream (the WAL, the index) depends on that.
    if (::fsync(fd_) != 0) {
        throw_errno("ChunkStore: fsync failed");
    }

    write_offset_ += length;
    return ChunkLocation{offset, length};
}

std::vector<uint8_t> ChunkStore::read(const ChunkLocation& loc) const {
    std::vector<uint8_t> buf(loc.length);
    size_t got = 0;
    while (got < loc.length) {
        const ssize_t n = ::pread(fd_, buf.data() + got, loc.length - got,
                                   static_cast<off_t>(loc.offset + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("ChunkStore: pread failed");
        }
        if (n == 0) {
            throw std::runtime_error("ChunkStore: short read (pack file truncated?)");
        }
        got += static_cast<size_t>(n);
    }
    return buf;
}

} // namespace dedupbackup
