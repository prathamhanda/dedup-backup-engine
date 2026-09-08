#include "dedupbackup/wal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "dedupbackup/byte_io.hpp"

namespace dedupbackup {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

// CHUNK_ADD payload: digest[32] | offset u64 | length u32 = 44 bytes.
// Plus the 1-byte type, the (type+payload) blob the CRC covers is 45
// bytes.
constexpr size_t kChunkAddPayloadSize = 32 + 8 + 4;
constexpr size_t kChunkAddRecordBodySize = 1 + kChunkAddPayloadSize; // type + payload

const std::array<uint32_t, 256>& crc32_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// Reads exactly `len` bytes at `offset`, looping on short reads/EINTR.
// Returns false (rather than throwing) on a genuine short read at EOF —
// that's the expected, unexceptional signal for "this record is
// truncated", not an I/O error.
bool pread_exact(int fd, uint8_t* buf, size_t len, uint64_t offset) {
    size_t got = 0;
    while (got < len) {
        const ssize_t n = ::pread(fd, buf + got, len - got, static_cast<off_t>(offset + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("Wal: pread failed");
        }
        if (n == 0) return false; // short read: hit EOF before filling the buffer
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
    const auto& table = crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

Wal::Wal(const std::string& wal_path) {
    fd_ = ::open(wal_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw_errno("Wal: failed to open " + wal_path);
    }

    const off_t end = ::lseek(fd_, 0, SEEK_END);
    if (end < 0) {
        const int saved_errno = errno;
        ::close(fd_);
        errno = saved_errno;
        throw_errno("Wal: failed to seek to end of WAL file");
    }
    write_offset_ = static_cast<uint64_t>(end);
}

Wal::~Wal() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void Wal::append_chunk_add(const Digest& digest, const ChunkLocation& loc) {
    // Build (type + payload) first — this is exactly what the CRC covers.
    std::vector<uint8_t> body(kChunkAddRecordBodySize);
    body[0] = static_cast<uint8_t>(WalRecordType::ChunkAdd);
    std::memcpy(&body[1], digest.data(), digest.size());
    write_u64_le(&body[1 + 32], loc.offset);
    write_u32_le(&body[1 + 32 + 8], loc.length);

    const uint32_t crc = crc32(body.data(), body.size());

    // Full on-disk record: length prefix + body + crc.
    std::vector<uint8_t> record(4 + body.size() + 4);
    write_u32_le(&record[0], static_cast<uint32_t>(body.size()));
    std::memcpy(&record[4], body.data(), body.size());
    write_u32_le(&record[4 + body.size()], crc);

    size_t written = 0;
    while (written < record.size()) {
        const ssize_t n = ::pwrite(fd_, record.data() + written, record.size() - written,
                                    static_cast<off_t>(write_offset_ + written));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("Wal: pwrite failed");
        }
        written += static_cast<size_t>(n);
    }

    // Durability point — see the class comment for why this ordering
    // (chunk bytes fsync'd first, this record fsync'd second) is
    // load-bearing for crash safety.
    if (::fsync(fd_) != 0) {
        throw_errno("Wal: fsync failed");
    }

    write_offset_ += record.size();
}

size_t Wal::replay(ChunkIndex& index) {
    const off_t file_size_signed = ::lseek(fd_, 0, SEEK_END);
    if (file_size_signed < 0) {
        throw_errno("Wal: failed to seek to end of WAL file during replay");
    }
    const uint64_t file_size = static_cast<uint64_t>(file_size_signed);

    uint64_t pos = 0;
    uint64_t good_offset = 0;
    size_t valid_count = 0;
    std::vector<uint8_t> body_and_crc; // scratch buffer, reused per record

    while (pos < file_size) {
        // Need at least the 4-byte length prefix to even know how big
        // this record claims to be.
        if (file_size - pos < 4) {
            break; // torn: a crash mid-write of the length prefix itself
        }
        uint8_t len_bytes[4];
        if (!pread_exact(fd_, len_bytes, 4, pos)) {
            break;
        }
        const uint32_t body_len = read_u32_le(len_bytes);

        // 4 (length prefix) + body_len (type+payload) + 4 (crc)
        const uint64_t record_total = 4 + static_cast<uint64_t>(body_len) + 4;
        if (pos + record_total > file_size) {
            break; // torn: the length prefix promises more than remains
        }

        body_and_crc.resize(body_len + 4);
        if (!pread_exact(fd_, body_and_crc.data(), body_and_crc.size(), pos + 4)) {
            break; // shouldn't happen given the size check above, but handle defensively
        }

        const uint32_t stored_crc = read_u32_le(&body_and_crc[body_len]);
        const uint32_t computed_crc = crc32(body_and_crc.data(), body_len);
        if (computed_crc != stored_crc) {
            break; // torn/corrupt: bytes present but don't match their own checksum
        }

        const uint8_t type = body_and_crc[0];
        if (type == static_cast<uint8_t>(WalRecordType::ChunkAdd)) {
            if (body_len != kChunkAddRecordBodySize) {
                // A CHUNK_ADD record with the wrong body length can't be
                // parsed safely even though its CRC checked out (e.g. a
                // future format change wrote it) — treat as the end of
                // what this binary can trust, same as any other corrupt
                // tail, rather than guessing at a different layout.
                break;
            }
            Digest digest;
            std::memcpy(digest.data(), &body_and_crc[1], digest.size());
            const uint64_t offset = read_u64_le(&body_and_crc[1 + 32]);
            const uint32_t length = read_u32_le(&body_and_crc[1 + 32 + 8]);
            index.insert(digest, ChunkLocation{offset, length});
        } else {
            // No other record types are defined yet. An unrecognized
            // type this binary doesn't understand is treated the same
            // as corruption: stop here rather than silently skip bytes
            // whose meaning we can't verify.
            break;
        }

        pos += record_total;
        good_offset = pos;
        ++valid_count;
    }

    if (good_offset != file_size) {
        // Torn tail found — physically discard it rather than just
        // stopping the in-memory scan short. See the header comment for
        // why this matters (a future append must not leave stale
        // unreachable-but-present garbage sitting after it on disk).
        if (::ftruncate(fd_, static_cast<off_t>(good_offset)) != 0) {
            throw_errno("Wal: failed to truncate torn tail");
        }
    }
    write_offset_ = good_offset;

    return valid_count;
}

} // namespace dedupbackup
