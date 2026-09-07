#!/usr/bin/env bash
# Configures, builds, and exercises everything currently in the repo.
# Run from the repo root on Linux:
#   chmod +x verify_stage1.sh && ./verify_stage1.sh
#
# Requires: cmake >= 3.16, a C++17 compiler (gcc >= 7 or clang >= 5),
# and OpenSSL headers (libssl-dev on Debian/Ubuntu, openssl-devel on
# Fedora/RHEL).
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo
echo "=== ctest (no tests registered until stage 10 — should report 0 tests, not fail) ==="
ctest --test-dir build --output-on-failure

SAMPLE=/tmp/dedup_sample.bin

echo
echo "=== generating 4 MiB random sample via chunk_identity --gen ==="
./build/chunk_identity --gen "$SAMPLE" 4

echo
echo "=== chunk_stats: FastCDC vs fixed-size, avg=8KiB ==="
./build/chunk_stats "$SAMPLE" 8

echo
echo "=== chunk_identity: real SHA-256, resync distribution + multi-byte edits ==="
./build/chunk_identity "$SAMPLE" 8
