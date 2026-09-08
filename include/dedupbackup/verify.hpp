#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dedupbackup/repository.hpp"

namespace dedupbackup {

struct VerifyIssue {
    std::string snapshot_id;
    std::string file_path;    // empty if the issue isn't specific to one file
    std::string message;
};

struct VerifyReport {
    size_t snapshots_checked = 0;
    size_t files_checked = 0;
    size_t chunk_references_walked = 0;  // every (file, chunk) reference visited
    size_t unique_chunks_verified = 0;   // distinct chunks actually re-hashed (see run_verify)
    std::vector<VerifyIssue> issues;

    bool ok() const { return issues.empty(); }
};

// Walks every snapshot in `repo` and confirms every chunk any manifest
// references is both present and hashes to its own address — i.e.
// independently re-derives each digest from the chunk's actual on-disk
// bytes rather than trusting the index, catching pack-file corruption
// (bit rot, truncation, whatever) that the index alone can't detect.
//
// A popular chunk shared by many files/snapshots is only re-hashed ONCE
// per run_verify() call, not once per reference — a chunk's bytes don't
// change between references, so re-checking it again would just cost
// time without checking anything new. On a real repository with heavy
// dedup this is the difference between a verify that's practical to run
// and one that re-reads the same chunk thousands of times.
//
// Never throws for a data-integrity problem — those are collected into
// the returned report's `issues` so a single missing chunk doesn't abort
// checking everything else. May still throw for a genuinely
// unrecoverable I/O error (e.g. the repository directory itself can't be
// read at all).
VerifyReport run_verify(Repository& repo);

} // namespace dedupbackup
