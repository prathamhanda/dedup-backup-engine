#pragma once

#include <cstddef>
#include <string>

#include "dedupbackup/repository.hpp"

namespace dedupbackup {

// Restores `snapshot_id` from `repo` into `dest_dir`. For each file in
// the manifest, in order: fetches its chunks by digest and reassembles
// them, writes the file, recreates its parent directory structure and
// (POSIX permission bits of) its mode, sets its mtime, then recomputes
// the whole-file SHA-256 and compares it against the manifest's
// file_digest.
//
// Fails fast: throws std::runtime_error, naming the offending file, on
// the first missing chunk or digest mismatch, rather than continuing
// past a corruption and producing a partially-wrong restore silently.
// A backup tool that can silently hand back different bytes than what
// was backed up is worse than one that refuses outright.
//
// Deliberately single-threaded, unlike run_backup(): restore's job is
// byte-exact correctness, not throughput (no resume claim rests on
// restore speed), and a plain sequential loop is far easier to reason
// about for zero risk of a threading bug corrupting output — not an
// oversight, a scope decision.
//
// Returns the number of files restored and verified.
size_t run_restore(const std::string& snapshot_id, const std::string& dest_dir, Repository& repo);

} // namespace dedupbackup
