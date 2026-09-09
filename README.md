# dedup-backup

A content-addressed, deduplicating backup engine, written from scratch in C++17. Splits files into
content-defined chunks with FastCDC, stores each unique chunk exactly once under its SHA-256
digest, and restores byte-exactly with independent whole-file verification. Crash-safe by
construction: a write-ahead log with a strict fsync ordering guarantees an interrupted backup can
never corrupt the index, only leave a harmless orphaned chunk behind.

```
$ dedup-backup init ~/repo
$ dedup-backup backup ~/some-tree --repo ~/repo        # a duplicate file + an edited one, mixed in
snapshot 20260908-154749 created (4 files) in 4.38s
repository now holds 270 unique chunks
$ dedup-backup backup ~/some-tree --repo ~/repo        # after appending 10 KB to one file
snapshot 20260908-154750 created (4 files) in 1.05s
repository now holds 272 unique chunks                 # +2 chunks, not +the whole edited file
$ dedup-backup stats --repo ~/repo
dedup saved:        72.1% (3.59x)
```

(Real output, single continuous session, small-scale — see [Benchmarks](#benchmarks) for the full
verified numbers and the reproducible procedure for the large-scale kernel-source-tree run this
project is designed for.)

## Why this exists

Copying only changed *files* between backups stays wasteful: edit one metadata tag in a 2 GB video
and a file-level tool re-stores the whole 2 GB to capture a few hundred changed bytes. This project
stops thinking in files and starts thinking in **content-addressed chunks**: split every file into
small pieces (~8 KB), hash each piece, and store a piece only if its hash has never been seen
before. A snapshot becomes a list of hashes — a *recipe* — not a copy of the data.

The interesting engineering problem is *where you cut*. Fixed-size blocks fail catastrophically on
the most common real-world edit: insert one byte at the front of a file, and every subsequent
fixed-size block boundary now covers different content — dedup collapses to zero over a one-byte
edit (measured below: 0/512 blocks survive intact). **Content-defined chunking** (this project uses
[FastCDC](https://www.usenix.org/conference/atc16/technical-sessions/presentation/xia), Xia et al.,
USENIX ATC 2016) makes the cut decision a function of local content instead of absolute position,
so an edit only disturbs the chunk(s) touching it — the rest of the file realigns.

## Architecture

```
                    ┌─────────────┐
  source dir  ──►   │   walker    │  one thread, std::filesystem::recursive_directory_iterator
                    └──────┬──────┘
                           │  file paths
                           ▼
                    ┌─────────────┐
                    │ BoundedQueue│  capacity 256 — backpressure: a fast walker on a huge
                    │  <path>     │  tree can't buffer unbounded pending paths in memory
                    └──────┬──────┘
                           │
        ┌──────────────────┼──────────────────┐
        ▼                  ▼                  ▼
   ┌─────────┐        ┌─────────┐        ┌─────────┐
   │ worker  │        │ worker  │  ...   │ worker  │   N threads (default: hardware_concurrency)
   │ chunk + │        │ chunk + │        │ chunk + │   each: read whole file → FastCDC → SHA-256
   │  hash   │        │  hash   │        │  hash   │   per chunk → whole-file digest
   └────┬────┘        └────┬────┘        └────┬────┘
        │                  │                  │
        └──────────────────┼──────────────────┘
                            ▼
                  ┌───────────────────┐
                  │    Repository     │   single global mutex around:
                  │ (store+index+WAL) │     1. ChunkStore::append()      (fsync)
                  └─────────┬─────────┘     2. Wal::append_chunk_add()   (fsync)
                            │                3. ChunkIndex::insert()     (in-memory)
                            ▼
                    manifest written once, at the end, listing every file's
                    ordered chunk digests + a whole-file SHA-256 for restore verification
```

Restore reverses this: read a manifest, fetch each file's chunks by digest, concatenate, write,
recompute the whole-file SHA-256, and refuse to hand back the result if it doesn't match — a
restore that could silently return wrong bytes is worse than one that fails loudly.

### Key design decisions

**Gear-hash rolling checksum, not Rabin fingerprinting.** `hash = (hash << 1) + GEAR[byte]` — no
explicit window subtraction; a byte's influence decays out of the 64-bit accumulator after ~64
shifts on its own. Cheaper per byte than a true rolling hash, and the FastCDC paper found it
sufficient. The gear table's 256 seed values are generated once, from a **fixed** seed
(`kGearSeed`), because two builds that disagreed on it would chunk identical bytes differently and
share zero chunks between backups — this is why `repo.meta` exists (below).

**Normalized chunking** — two boundary-probability masks instead of one (a stricter mask below
the target average size, a looser one above it) — pulls the chunk-size distribution toward the
target instead of letting a single-mask design spread it out geometrically (many tiny chunks
bloating the index; too many chunks landing at the max-size ceiling, which is really truncation,
not content-defined behavior at all).

**Resync after an edit is a structural, near-certain outcome, not luck.** Once a chunk's scan has
run past its own ~64-byte gear-hash warm-up distance (`min_size` is 2048 — 32× that), the boundary
test is effectively a function of content alone, not of where a chunk's scan started. That's why a
single-byte insertion reliably disturbs only the one chunk it lands in: the very next chunk's scan
converges back onto the same content-derived cut points the original file used. Measured, not just
argued: 50/50 pseudorandom single-byte insertions landed on **exactly 1** differing chunk out of
440 (see [Benchmarks](#benchmarks)). The one caveat named rather than hidden: this breaks down
when a chunk hits the hard `max_size` ceiling without a content boundary firing — that cut is
purely positional, with zero content dependence, so it resets the alignment argument outright. Rare
on high-entropy data (never triggered in this project's own testing); more likely on real source
trees with long low-entropy runs, which is exactly why chunks track whether their own boundary was
content-derived or a positional truncation (`ChunkSpan::content_defined`), diagnosable via
`chunk_identity`'s truncation-ratio report.

**Pack files, not one-file-per-chunk.** A real backup produces tens of thousands of chunks; one
inode and one filesystem block (rounded up, wasting space on small chunks) per chunk, plus an
`open()`/`close()` pair each, adds up fast. Chunks are appended to a growing pack file instead —
one `open()` per backup, sequential writes, no per-chunk filesystem metadata cost.

**Crash safety is a strict write ordering, not a transaction log.** `ChunkStore::append()` writes
chunk bytes and `fsync`s; only then does `Wal::append_chunk_add()` write a record pointing at them
and `fsync`. A crash between the two leaves an orphaned, unreferenced chunk — wasted space, never
corruption, because nothing durable ever claimed to know about it. The reverse order would be
fatal: a durable WAL record could reference bytes that were never actually written. On startup, the
WAL is replayed from byte 0; the first record that's too short for its own length prefix or fails
its CRC32 stops the replay, and the file is **physically truncated** there (not just skipped in
memory), so a later append can't leave stale garbage sitting past it for some future replay to walk
into.

**One global mutex, not one lock per shared structure.** All chunk-store/WAL/index mutation happens
under a single mutex inside `Repository::store_chunk_if_absent()`, which performs
check-index → store-bytes → log-WAL → update-index atomically. The CPU-heavy work (chunking,
hashing) happens *outside* the lock, which is why throughput still scales with threads even though
storage is serialized — the known secondary bottleneck (see `bench`'s phase breakdown) is exactly
this mutex, and the obvious next step is sharding the index by digest prefix.

**Every hash function call is stateless, which is what makes sharing safe.** One `Sha256Hasher` and
one chunker instance are shared (by `const&`) across every worker thread — not because it's
convenient, but because `Sha256Hasher::hash()` builds and destroys its own `EVP_MD_CTX` inside the
call with no member state, and the chunkers never mutate their config after construction. No
per-thread duplication needed; nothing to synchronize.

## On-disk format

```
<repo>/
├── repo.meta                    # magic, version, chunker params + gear seed — the committed
│                                 #   config; a later backup with different parameters is refused
├── packs/
│   └── pack-000001.dat          # raw, unframed, concatenated chunk bytes
├── wal.log                      # append-only index journal, length-prefixed + CRC32 per record
└── snapshots/
    └── 20260908-154749.manifest # one per backup
```

All multi-byte integers little-endian.

| File | Layout |
|---|---|
| `repo.meta` | `magic[8]="DEDUPBK1" \| version:u32 \| min_size:u32 \| avg_size:u32 \| max_size:u32 \| gear_seed:u64` |
| `wal.log` (repeated) | `length:u32 \| type:u8 (1=CHUNK_ADD) \| payload \| crc32:u32`. CRC covers `type+payload`, not the length prefix (which is what tells a reader how many bytes to feed the CRC). `CHUNK_ADD` payload: `digest[32] \| offset:u64 \| length:u32`. |
| `*.manifest` | `magic[8]="DEDUPMF1" \| version:u32 \| snapshot_id[16] \| created_unix:i64 \| file_count:u64`, then per file (sorted by path): `path_len:u16 \| path \| mode:u32 \| size:u64 \| mtime_unix:i64 \| file_digest[32] \| chunk_count:u32 \| chunk_digests[32×count]` |
| pack file | Raw concatenated chunk bytes. A chunk's location — `(offset, length)` — lives entirely in the index/WAL, never inline in the pack file itself. |

## Building

Requires CMake ≥ 3.16, a C++17 compiler (GCC ≥ 7 or Clang ≥ 5), and OpenSSL development headers.

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake libssl-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Produces `build/dedup-backup` plus several standalone verification tools under `build/` (used
during development, kept for regression coverage — see [Testing](#testing)).

> **Windows users:** build under WSL2, not natively — the project uses POSIX file I/O
> (`open`/`pwrite`/`fsync`, etc.) directly and targets Linux. Keep the checkout on the Linux
> filesystem (`~/...`), not a `/mnt/c/...` path — the cross-filesystem translation layer is slow
> enough to invalidate throughput measurements. Separately, and worth knowing before trusting any
> `bench` output: WSL2's own virtualized disk made `fsync()` ~12ms/call during this project's own
> development (vs. typical sub-millisecond bare-metal SSD latency) — see
> [Benchmarks](#benchmarks) for how that was diagnosed and why it means the final throughput number
> needs real hardware, not WSL2.

## CLI

```
dedup-backup init    <repo-path>
dedup-backup backup  <source-dir> --repo <repo> [--threads N] [--avg-chunk-kb K] [--fixed-chunking]
dedup-backup restore <snapshot-id> <dest-dir> --repo <repo>
dedup-backup verify  --repo <repo>
dedup-backup list    --repo <repo>
dedup-backup stats   --repo <repo>
dedup-backup bench   <source-dir> --repo <repo> [--threads N] [--avg-chunk-kb K] [--fixed-chunking]
```

`--fixed-chunking` swaps FastCDC for naive fixed-size blocks — useful for comparing dedup ratios
(see below), not intended as a production mode.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

8 tests, all passing, confirmed idempotent (re-run twice with zero manual cleanup between runs):

| Test | What it proves |
|---|---|
| `roundtrip` | Real `backup` → `restore`, byte-exact, mode bits preserved |
| `insertion_identity` | The core claim: back up a file, insert one byte, back up again — only a handful of new chunks, through the **real** end-to-end pipeline, not the chunker in isolation |
| `repo_meta` | A chunk-config change against an existing repo is refused with a clear error, not silently accepted |
| `crash_recovery` | `SIGKILL` a real backup process mid-write (detected by polling WAL growth, not a fixed sleep — see below); the previous snapshot still restores byte-exactly, `verify` reports no issues, and the repository accepts a fresh backup afterward |
| `wal_torn_write` | A WAL truncated mid-record replays cleanly, discarding exactly the torn tail, both in-memory and physically on disk |
| `store_roundtrip`, `index_dedup`, `manifest_format` | Component-level regression coverage for the pack file, chunk index, and manifest format |

The crash-recovery test is worth a specific note: it kills the backup process the instant it
observes the WAL grow past its pre-test size, rather than sleeping a fixed duration first. A fixed
sleep's "mid-backup" window depends entirely on `fsync` latency, which this project measured
varying by 10–100× between a WSL2 dev environment (~12.4 ms/call) and real hardware — a sleep tuned
for one would be wrong for the other. Polling for actual durable progress makes the test's
correctness independent of the machine running it.

## Benchmarks

### Verified (small-scale, WSL2 Ubuntu-24.04, GCC 13.3.0)

Correctness and the core dedup claim are fully verified — see `tests/` above and
[design decisions](#key-design-decisions). Small-scale numbers, real runs:

```
$ dedup-backup stats --repo ~/repo     # 2 snapshots, real duplicate + a real incremental edit
dedup saved:        72.1% (3.59x)
unique chunks:      272
```

50 pseudorandom single-byte insertions into a 4 MB file, through the standalone chunk-identity
tool (`tools/chunk_identity.cpp`):

```
[FastCDC]     min=1 max=1 mean=1.00   -- every single trial: exactly 1 chunk differs
[Fixed-size]  min=15 max=512 mean=266.30 -- naive fixed blocks: hundreds of chunks differ
```

### What's NOT yet included, and why

`bench`'s phase-breakdown correctly identified `fsync` (inside the WAL/store's single-mutex
critical section) as the dominant cost on the WSL2 development environment — but the *absolute*
throughput number it produced there (0.3 MB/s) is not representative of real hardware. Isolating
`fsync()` in a standalone C program with zero project code involved measured ~12.4 ms/call on that
setup, against a typical bare-metal SSD's sub-millisecond latency — a 10–100× gap entirely
attributable to WSL2's virtualized disk stack, not the engine. **The reproducible benchmark
procedure below is what produces a trustworthy number; run it on real hardware before quoting
one.**

### Reproducible procedure (run on real Linux hardware, not a VM)

```bash
mkdir -p ~/bench && cd ~/bench
for v in 1 2 3 4 5 6 7 8; do
  wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.$v.tar.xz
  mkdir -p tree-$v && tar xf linux-6.6.$v.tar.xz -C tree-$v
done
```

Eight consecutive kernel releases (~9.6 GB extracted) — chosen deliberately: reproducible by
anyone, and consecutive releases have the "mostly unchanged with genuine churn" profile dedup is
built for (ten copies of the *same* tree would give 90%+ and prove nothing about real-world
performance).

```bash
dedup-backup init ~/repo
for v in 1 2 3 4 5 6 7 8; do
  dedup-backup backup ~/bench/tree-$v --repo ~/repo
done
dedup-backup stats --repo ~/repo          # dedup %, dedup factor, unique chunk count
dedup-backup bench ~/bench/tree-1 --repo ~/bench-repo   # separate repo: sustained MB/s + phase breakdown
```

**Chunk-size sweep** (dedup ratio vs. index size — rebuild isn't needed, `--avg-chunk-kb` is a
runtime flag):
```bash
for kb in 4 8 16; do
  rm -rf ~/repo-$kb && dedup-backup init ~/repo-$kb
  for v in 1 2 3 4 5 6 7 8; do
    dedup-backup backup ~/bench/tree-$v --repo ~/repo-$kb --avg-chunk-kb $kb
  done
  echo "=== ${kb}KB ==="; dedup-backup stats --repo ~/repo-$kb
done
```

**CDC vs. fixed-size, and the resync distribution on real source (not synthetic random bytes)**:
```bash
# chunk_identity takes a single file, not a directory -- point it at the extracted tarball
# itself, or concatenate a representative sample of source files into one first:
find ~/bench/tree-1 -name '*.c' -exec cat {} + > /tmp/kernel_sources_concat.bin
build/chunk_identity /tmp/kernel_sources_concat.bin 8
```
The truncation-ratio diagnostic this prints (`content_defined` vs. positional/`max_size`-forced
boundaries) is the number that determines whether the resync guarantee measured on random data
(above, under "Verified") holds on real, low-entropy-rich source code — worth checking rather than
assuming.

## Known limitations

- **No per-file skip-if-unchanged.** Every backup re-reads, re-chunks, and re-hashes every file
  regardless of mtime. Chunk-level dedup still prevents re-storing identical bytes (confirmed: an
  unchanged re-backup adds zero new chunks), so this costs CPU/IO on unchanged files, not
  correctness or disk space.
- **One growing pack file, no rotation.** Simpler; real operational concerns this project doesn't
  exercise (parallel compaction, partial-restore locality) are the reason production systems
  rotate.
- **No garbage collection.** Crash-orphaned chunks (see [crash safety](#key-design-decisions))
  accumulate forever. Would be a mark-and-sweep over every manifest's referenced digests.
  Understanding *why* GC is needed here is the point; implementing it wasn't in scope.
- **`repo.meta` doesn't catch a FastCDC ↔ `--fixed-chunking` switch at the same `avg_chunk_kb`.**
  Sizes would match even though the chunking strategy differs, which also breaks dedup alignment —
  the on-disk format has no field for chunking strategy; extending it was judged out of scope.
- **No index checkpointing.** The WAL *is* the index's persistence — full replay on every open.
  Bounded here by this project's data scale; a production system would checkpoint periodically.
- **Regular files and directories only.** Symlinks, hardlinks, device files, sockets, and xattrs
  are skipped and logged to stderr, not backed up. An otherwise-empty directory (no files anywhere
  under it) isn't recreated on restore, since the manifest only records files.
- **Restore is single-threaded**, deliberately — no resume claim rests on restore throughput, and a
  plain sequential loop removes an entire class of possible concurrency bugs from the one operation
  whose whole job is producing exactly the right bytes.

## What I'd do next

Shard the chunk index by digest prefix to relax the single global mutex (the measured secondary
bottleneck). Garbage-collect orphaned chunks with a mark-and-sweep over manifests. Checkpoint the
index periodically to bound WAL replay time on a long-lived repository. Pack file rotation with
background compaction. Intra-file parallelism for backups dominated by one very large file. A
compression layer on top of dedup (chunks compress well post-dedup, since the redundancy dedup
already removed won't fight the compressor).
