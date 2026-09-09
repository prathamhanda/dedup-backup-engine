#include "dedupbackup/backup_pipeline.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dedupbackup/bounded_queue.hpp"
#include "dedupbackup/chunker.hpp"
#include "dedupbackup/fixed_chunker.hpp"
#include "dedupbackup/sha256_hasher.hpp"

namespace dedupbackup {

namespace {

namespace fs = std::filesystem;

// Queue capacity, per §3.5. Bounded so a fast walker on a huge tree can't
// buffer an unbounded number of pending file paths.
constexpr size_t kQueueCapacity = 256;

// The walker: the ONE thread that touches std::filesystem's directory
// traversal. Regular files are queued; directories are descended into
// (not queued themselves — the manifest has no directory entries, so an
// empty directory tree is a known, documented gap, not a bug). Anything
// else (symlink, socket, device, fifo...) is out of scope per the
// reduced-scope plan and is skipped with a message to stderr rather than
// silently dropped.
void walker_loop(fs::path root, BoundedQueue<fs::path>& queue) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
        std::fprintf(stderr, "walker: failed to open %s: %s\n", root.string().c_str(),
                     ec.message().c_str());
        queue.close();
        return;
    }

    for (; it != end; it.increment(ec)) {
        if (ec) {
            std::fprintf(stderr, "walker: error advancing directory iterator: %s\n",
                         ec.message().c_str());
            break;
        }
        const fs::directory_entry& entry = *it;

        std::error_code type_ec;
        if (entry.is_regular_file(type_ec) && !type_ec) {
            queue.push(entry.path());
        } else if (entry.is_directory(type_ec) && !type_ec) {
            continue; // descended into automatically by the iterator; not queued itself
        } else {
            std::fprintf(stderr, "walker: skipping unsupported file type: %s\n",
                         entry.path().string().c_str());
        }
    }
    queue.close();
}

// One worker: pop a path, read the whole file, chunk it, hash+dedup each
// chunk through the repository, hash the whole file for restore
// verification, stat() for mode/mtime, and append a manifest entry.
//
// `chunker` and `hasher` are shared (by const&) across every worker
// thread rather than one instance each:
//   - Sha256Hasher::hash() is stateless per call (its EVP_MD_CTX is
//     created and destroyed inside the call, never stored as member
//     state — see sha256_hasher.cpp), so concurrent calls on one shared
//     instance from multiple threads need no synchronization.
//   - FastCDCChunker/FixedChunker's members (config, masks) are set once
//     at construction and never mutated; chunk() is a const method
//     operating only on locals plus a function-local `static const`
//     gear table, whose initialization is thread-safe by the standard
//     ("magic statics") and is read-only afterward.
// Both are safe to share precisely because they hold no mutable state
// after construction — not because of any locking.
void worker_loop(BoundedQueue<fs::path>& queue, const fs::path& root, Repository& repo,
                  const IChunker& chunker, const Sha256Hasher& hasher,
                  std::vector<ManifestFileEntry>& manifest_files, std::mutex& manifest_mutex,
                  PipelineTimings* timings) {
    using clock = std::chrono::steady_clock;
    // Local (non-atomic) accumulators, flushed into `timings` once per
    // file rather than once per chunk -- avoids atomic contention on
    // every single chunk when several worker threads are timing
    // concurrently. Unused (and clock::now() never called) when
    // `timings` is null, so the normal `backup` path pays nothing for
    // this instrumentation.
    uint64_t local_read_ns = 0, local_chunk_ns = 0, local_hash_ns = 0, local_store_ns = 0;

    fs::path path;
    while (queue.pop(path)) {
        const auto t_read0 = timings ? clock::now() : clock::time_point{};
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "worker: failed to open %s, skipping\n", path.string().c_str());
            continue;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        f.close();
        if (timings) local_read_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_read0).count();

        const auto t_chunk0 = timings ? clock::now() : clock::time_point{};
        const std::vector<ChunkSpan> spans = chunker.chunk(data.data(), data.size());
        if (timings) local_chunk_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_chunk0).count();

        std::vector<Digest> chunk_digests;
        chunk_digests.reserve(spans.size());
        for (const ChunkSpan& span : spans) {
            const auto t_hash0 = timings ? clock::now() : clock::time_point{};
            const Digest digest = hasher.hash(data.data() + span.offset, span.length);
            if (timings) local_hash_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_hash0).count();

            const auto t_store0 = timings ? clock::now() : clock::time_point{};
            repo.store_chunk_if_absent(digest, data.data() + span.offset, span.length);
            if (timings) local_store_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_store0).count();

            chunk_digests.push_back(digest);
        }

        // A second, independent pass over the same bytes — deliberately.
        // Per-chunk digests only prove each chunk's own bytes are
        // correct in isolation; they don't prove the chunks were
        // concatenated in the right order during restore. A single
        // whole-file digest is an end-to-end check that catches
        // reassembly/ordering bugs the per-chunk digests alone couldn't.
        // The cost is real (~2x total hashing bytes vs. chunk digests
        // alone) and is a deliberate correctness-over-throughput
        // tradeoff, not an oversight — see README.
        const auto t_fhash0 = timings ? clock::now() : clock::time_point{};
        const Digest file_digest = hasher.hash(data.data(), data.size());
        if (timings) local_hash_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_fhash0).count();

        struct stat st{};
        uint32_t mode = 0;
        int64_t mtime = 0;
        if (::stat(path.c_str(), &st) == 0) {
            mode = st.st_mode;
            mtime = static_cast<int64_t>(st.st_mtime);
        } else {
            std::fprintf(stderr, "worker: stat failed for %s, recording mode=0 mtime=0\n",
                         path.string().c_str());
        }

        ManifestFileEntry entry;
        // lexically_relative: purely textual, doesn't touch the
        // filesystem or resolve symlinks (both paths are already known
        // to be consistent with each other, so there's nothing to
        // resolve). generic_string() guarantees '/' separators per the
        // format spec, regardless of host platform.
        entry.path = path.lexically_relative(root).generic_string();
        entry.mode = mode;
        entry.size = data.size();
        entry.mtime_unix = mtime;
        entry.file_digest = file_digest;
        entry.chunk_digests = std::move(chunk_digests);

        std::lock_guard<std::mutex> lock(manifest_mutex);
        manifest_files.push_back(std::move(entry));
    }

    if (timings) {
        timings->read_ns += local_read_ns;
        timings->chunk_ns += local_chunk_ns;
        timings->hash_ns += local_hash_ns;
        timings->store_ns += local_store_ns;
    }
}

std::string make_snapshot_id(std::time_t now) {
    std::tm tm_utc{};
    gmtime_r(&now, &tm_utc);  // UTC, not local time: reproducible regardless of host timezone,
                              // and snapshot ids sort correctly lexicographically only if
                              // generated on a consistent clock.
    // Buffer sized generously (not just the 16 bytes the well-formed
    // "YYYYMMDD-HHMMSS" + NUL actually needs): GCC's -Wformat-truncation
    // reasons conservatively about %d's worst case (a full int can need
    // up to 11 digits including a sign), so a tight 16-byte buffer is
    // flagged even though tm_year/tm_mon/etc. never realistically reach
    // those extremes. Sizing for the theoretical worst case satisfies
    // the warning honestly rather than suppressing it.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return std::string(buf);
}

} // namespace

std::string run_backup(const std::string& source_dir, Repository& repo,
                        const BackupOptions& options) {
    // Fail fast, before any file is touched: a chunk-config mismatch
    // against repo.meta means this backup would silently stop
    // deduplicating against what's already stored, not that anything is
    // corrupt -- worth refusing outright rather than doing wasted work
    // first.
    repo.check_or_init_chunk_config(options.chunk_config);

    std::unique_ptr<IChunker> chunker;
    if (options.fixed_chunking) {
        chunker = std::make_unique<FixedChunker>(options.chunk_config.avg_size);
    } else {
        chunker = std::make_unique<FastCDCChunker>(options.chunk_config);
    }
    const Sha256Hasher hasher; // shared across all workers -- see worker_loop's comment for why

    BoundedQueue<fs::path> queue(kQueueCapacity);
    std::vector<ManifestFileEntry> manifest_files;
    std::mutex manifest_mutex;

    const unsigned int hw = std::thread::hardware_concurrency();
    const size_t thread_count = options.thread_count != 0 ? options.thread_count
                                                            : std::max(1u, hw);

    const fs::path root(source_dir);
    std::thread walker(walker_loop, root, std::ref(queue));

    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back(worker_loop, std::ref(queue), std::cref(root), std::ref(repo),
                              std::cref(*chunker), std::cref(hasher), std::ref(manifest_files),
                              std::ref(manifest_mutex), options.timings);
    }

    walker.join();
    for (std::thread& w : workers) w.join();

    // Snapshot ids have one-second resolution ("YYYYMMDD-HHMMSS"), so
    // two backups launched within the same second would naively compute
    // the same id -- and since write_manifest() truncates/overwrites,
    // the second write would silently destroy the first snapshot's
    // record. Detected empirically (see handoff §4.10), not just
    // theorized: two real backups a few hundred ms apart against this
    // project's own source tree collided on id during development.
    // Rather than change the on-disk format (already spec'd and shipped
    // in component 6) to add a disambiguator, wait out the collision:
    // exceedingly rare in practice (only matters for backups launched
    // less than a second apart), and failing loudly after a bounded
    // number of retries is far better than ever silently overwriting an
    // existing snapshot.
    std::time_t now = std::time(nullptr);
    std::string snapshot_id = make_snapshot_id(now);
    int collision_retries = 0;
    while (repo.has_snapshot(snapshot_id)) {
        if (++collision_retries > 5) {
            throw std::runtime_error(
                "run_backup: snapshot id '" + snapshot_id +
                "' collided with an existing snapshot five seconds in a row -- refusing to "
                "overwrite it. This should be virtually impossible; something is wrong with "
                "the clock or the repository.");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        now = std::time(nullptr);
        snapshot_id = make_snapshot_id(now);
    }

    Manifest manifest;
    manifest.snapshot_id = snapshot_id;
    manifest.created_unix = static_cast<int64_t>(now);
    manifest.files = std::move(manifest_files);

    repo.write_snapshot_manifest(manifest);
    return manifest.snapshot_id;
}

} // namespace dedupbackup
