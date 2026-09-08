#include "dedupbackup/verify.hpp"

#include <unordered_set>

#include "dedupbackup/chunk_index.hpp"  // for DigestHash
#include "dedupbackup/sha256_hasher.hpp"

namespace dedupbackup {

VerifyReport run_verify(Repository& repo) {
    VerifyReport report;
    const Sha256Hasher hasher;
    std::unordered_set<Digest, DigestHash> checked; // dedup re-verification across references

    for (const std::string& snapshot_id : repo.list_snapshots()) {
        ++report.snapshots_checked;

        Manifest manifest;
        try {
            manifest = repo.read_snapshot_manifest(snapshot_id);
        } catch (const std::exception& e) {
            report.issues.push_back(
                VerifyIssue{snapshot_id, "", std::string("failed to read manifest: ") + e.what()});
            continue;
        }

        for (const ManifestFileEntry& file : manifest.files) {
            ++report.files_checked;

            for (const Digest& digest : file.chunk_digests) {
                ++report.chunk_references_walked;

                if (checked.count(digest)) {
                    continue; // already verified this exact chunk earlier in this run
                }

                if (!repo.has_chunk(digest)) {
                    report.issues.push_back(VerifyIssue{
                        snapshot_id, file.path,
                        "chunk " + to_hex(digest) + " referenced but not present in repository"});
                    continue; // don't mark as checked -- still missing on a future reference
                }

                const ChunkLocation loc = repo.locate_chunk(digest);
                const std::vector<uint8_t> bytes = repo.read_chunk(loc);
                const Digest recomputed = hasher.hash(bytes.data(), bytes.size());
                if (recomputed != digest) {
                    report.issues.push_back(
                        VerifyIssue{snapshot_id, file.path,
                                    "chunk " + to_hex(digest) +
                                        " content does not hash to its own address (recomputed " +
                                        to_hex(recomputed) + ") -- pack file corruption"});
                    continue; // don't mark as checked -- it's corrupt
                }

                checked.insert(digest);
                ++report.unique_chunks_verified;
            }
        }
    }

    return report;
}

} // namespace dedupbackup
