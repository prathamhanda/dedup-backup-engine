#!/usr/bin/env bash
# CTest test 3/4 (per the original spec): kill -9 the process mid-backup,
# restart, and assert the previous snapshot still restores correctly.
#
# Usage: crash_recovery_test.sh <path-to-dedup-backup-binary>
#
# Design note on timing: rather than a fixed sleep (fragile -- "mid-way"
# on a fast machine with sub-millisecond fsync is a very different
# duration than on this project's own WSL2 dev environment, which
# measured ~12ms/fsync -- see handoff §4.12), this polls the WAL file
# size and kills the moment it observes ANY growth beyond its
# pre-backup-B size. That's proof of real, durable progress (at least one
# chunk was fully stored+logged+fsync'd) while still being as early as
# possible relative to a backup of >1 file/chunk -- so the process is
# reliably killed partway through, regardless of how fast or slow the
# underlying disk is.
set -uo pipefail  # deliberately NOT -e: a SIGKILLed backup "failing" is expected, not an error

if [ $# -lt 1 ]; then
    echo "usage: crash_recovery_test.sh <path-to-dedup-backup-binary>" >&2
    exit 1
fi
DEDUP_BACKUP_BIN="$1"

BASE=$(mktemp -d)
trap 'rm -rf "$BASE"' EXIT

REPO="$BASE/repo"
SRC_A="$BASE/srcA"
SRC_B="$BASE/srcB"
mkdir -p "$SRC_A" "$SRC_B"

fail() {
    echo "CRASH RECOVERY TEST FAILED: $1" >&2
    exit 1
}

# --- snapshot A: small, completes fully -- this is what must survive the
#     later crash ---
echo "hello world" > "$SRC_A/file1.txt"
head -c 100000 /dev/urandom > "$SRC_A/file2.bin"

"$DEDUP_BACKUP_BIN" init "$REPO" || fail "init failed"
SNAP_A_OUTPUT=$("$DEDUP_BACKUP_BIN" backup "$SRC_A" --repo "$REPO") || fail "backup of A failed"
echo "$SNAP_A_OUTPUT"
SNAP_A=$(echo "$SNAP_A_OUTPUT" | head -1 | awk '{print $2}')
[ -n "$SNAP_A" ] || fail "could not determine snapshot A's id"

# Baseline sanity: A restores correctly before we even touch the crash.
"$DEDUP_BACKUP_BIN" restore "$SNAP_A" "$BASE/restoreA1" --repo "$REPO" \
    || fail "baseline restore of A failed before the crash test began"
diff -r "$SRC_A" "$BASE/restoreA1" > /dev/null || fail "baseline restore of A not byte-exact"

# --- snapshot B: several files, so killing at the first sign of progress
#     lands clearly mid-backup, not at the very end ---
for i in $(seq 1 20); do
    head -c 200000 /dev/urandom > "$SRC_B/file_$i.bin"
done

WAL_PATH="$REPO/wal.log"
WAL_SIZE_BEFORE=0
[ -f "$WAL_PATH" ] && WAL_SIZE_BEFORE=$(stat -c%s "$WAL_PATH")

"$DEDUP_BACKUP_BIN" backup "$SRC_B" --repo "$REPO" > "$BASE/backup_b.log" 2>&1 &
BACKUP_PID=$!

DEADLINE=$((SECONDS + 15))
while true; do
    if [ -f "$WAL_PATH" ]; then
        CUR_SIZE=$(stat -c%s "$WAL_PATH" 2>/dev/null || echo 0)
        [ "$CUR_SIZE" -gt "$WAL_SIZE_BEFORE" ] && break
    fi
    if ! kill -0 "$BACKUP_PID" 2>/dev/null; then
        fail "backup of B exited before any observable WAL progress -- test setup problem"
    fi
    if [ "$SECONDS" -ge "$DEADLINE" ]; then
        kill -9 "$BACKUP_PID" 2>/dev/null
        fail "timed out waiting for WAL progress"
    fi
    sleep 0.01
done

# The actual crash: SIGKILL, unblockable, no destructors, no cleanup --
# simulates power loss / OOM kill / kill -9, not a graceful shutdown.
kill -9 "$BACKUP_PID"
wait "$BACKUP_PID" 2>/dev/null  # reap it; its exit status (killed) is expected, not checked

echo "killed backup of B mid-write (WAL grew past ${WAL_SIZE_BEFORE} bytes before the kill)"

# --- the actual assertions ---
"$DEDUP_BACKUP_BIN" restore "$SNAP_A" "$BASE/restoreA2" --repo "$REPO" \
    || fail "snapshot A no longer restores after the crash"
diff -r "$SRC_A" "$BASE/restoreA2" > /dev/null \
    || fail "snapshot A restored but is not byte-exact after the crash"
echo "snapshot A survives the crash and still restores byte-exactly"

"$DEDUP_BACKUP_BIN" verify --repo "$REPO" || fail "verify reports issues after the crash"
echo "verify reports no issues after the crash"

# Bonus, beyond the minimum spec: the repository must still be USABLE,
# not just "not obviously broken" -- a fresh, uninterrupted backup must
# complete, proving WAL replay-on-reopen actually recovered a working
# repository rather than merely a non-corrupt one.
"$DEDUP_BACKUP_BIN" backup "$SRC_B" --repo "$REPO" > /dev/null \
    || fail "repository unusable for a new backup after crash recovery"
echo "repository accepts a fresh backup after crash recovery"

echo "CRASH RECOVERY TEST PASSED"
exit 0
