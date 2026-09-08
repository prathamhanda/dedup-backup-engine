# Deduplicating Backup Engine — Full Project Handoff

**Owner:** Pratham Handa
**Purpose:** Portfolio systems project targeting a Rubrik SWE internship application
**Language/Platform:** C++17, Linux (WSL2 Ubuntu acceptable), CMake, OpenSSL
**Status at handoff:** Components 1–2 written, reviewed, and **verified on real Linux** (WSL2
Ubuntu-24.04, GCC 13.3.0, OpenSSL 3.0.13, real SHA-256) as of 2026-09-08 — see §4.3/§4.4 for the
verified transcript. Next task: component 3 (chunk store).

This document is the single source of truth. It contains the problem statement, the domain
background, the full architecture, on-disk format specifications, per-component build
instructions, everything already built (including measured numbers and two hard-won technical
derivations), the reduced scope for a time-constrained build, and the interview defence
briefing. Hand this to a fresh Claude Code session and it should be able to continue without
further context.

---

## PART 1 — THE PROBLEM AND WHY THIS DESIGN

### 1.1 The problem in plain terms

Back up a 100 GB laptop nightly with a naive copy and you store 3 TB a month, even though only
a few GB of genuinely new information was created. Copying only changed *files* helps but stays
wasteful: edit one metadata tag in a 2 GB video and you re-store 2 GB to capture a few hundred
changed bytes. Ten employees holding the same 500 MB template store it ten times.

### 1.2 The fix: content-addressed, deduplicated chunk storage

Stop thinking in files. Split every file into small chunks (target 8 KB). Hash each chunk with
SHA-256. Store chunks in a repository **keyed by their own hash** — content addressing: a chunk's
name is derived from what it contains, not from where it came from.

Backup then becomes: chunk the file, hash each chunk, ask the repository "do you already hold
digest `a3f9c2…`?" If yes, store nothing. If no, append it. Record the ordered list of digests
that reconstitutes the file — a *recipe*. A snapshot is a collection of recipes plus metadata.

Restore reads a recipe, fetches each chunk by digest, concatenates in order, and verifies the
result against a whole-file hash stored in the manifest.

Consequences: the edited 2 GB video costs one chunk. The shared template is stored once. Ten
nightly snapshots of a mostly-static tree cost barely more than one.

### 1.3 The hard part: where do you cut?

**Fixed-size chunking fails on insertion.** Cut every 8192 bytes by absolute offset, then insert
one byte at the front. Every subsequent byte shifts one position, so every block boundary now
covers different content. Dedup collapses to zero over a one-byte edit. *(We measured exactly
this — see §4.3.)*

**Content-defined chunking (CDC)** makes the cut decision a function of local content instead of
absolute position. Slide a window, compute a rolling hash, declare a boundary when the hash
satisfies a data-independent condition (low bits zero). Because the decision depends only on
nearby bytes, an insertion disturbs the boundaries around it and downstream cuts re-derive
themselves from unchanged content.

We use **FastCDC** (Xia et al., USENIX ATC 2016).

### 1.4 The other hard part: surviving a crash

If the process dies mid-backup, the index must never claim a chunk exists that was never
written — that produces silent corruption discovered months later at restore time.

Solution: a **write-ahead log**. Ordering is strict and load-bearing:

1. Append chunk bytes to the pack file, `fsync`.
2. Append the index record `(digest → offset, length)` to the WAL with a CRC32, `fsync`.

Crash between the two leaves an orphaned chunk on disk that nothing references — wasted space,
never corruption. Crash in the reverse order would be fatal. On startup, replay the WAL and stop
at the first record with a bad checksum or truncated length, discarding the torn tail.

### 1.5 Why this project for Rubrik specifically

Rubrik sells enterprise backup, dedup, and recovery. Chunk stores, incremental snapshots,
crash-consistent metadata, and verified restores *are* the product, not an analogy to it.

---

## PART 2 — SCOPE (REDUCED FOR TIME CONSTRAINT)

The original plan had eleven components. Below is the trimmed scope. **Every resume claim
survives intact** — the cuts are all in polish, not in substance.

### 2.1 Keep (load-bearing)

| # | Component | Why it must stay |
|---|-----------|------------------|
| 1 | FastCDC chunker | The intellectual core. Already built. |
| 2 | SHA-256 hasher | Content addressing depends on it. Already written. |
| 3 | Chunk store (single pack file) | Where bytes actually live. |
| 4 | Chunk index (WAL-backed, in-memory map) | Answers "do we have this chunk?" |
| 5 | Write-ahead log | Top-three interview asset. The crash test is the differentiator. |
| 6 | Snapshot manifest | The recipes. Without it there is no restore. |
| 7 | Thread pool (per-file granularity) | Backs the "310 MB/s on 8 threads" claim. |
| 8 | Restore + verify | Backs "byte-exact verified restores". |
| 9 | Stats + bench | Produces the resume numbers. |
| 10 | Four tests | Roundtrip, insertion-identity, crash-recovery, WAL torn-write. |
| 11 | README | Architecture, formats, reproducible benchmark. |

### 2.2 Cut (with rationale — document these as "future work" in the README)

- **Pack file rotation at 64 MB.** Use one growing pack file. Rotation matters for real
  operational concerns (parallel compaction, partial restore locality) that this project doesn't
  exercise. ~1 hour saved.
- **Garbage collection of orphaned chunks.** Document that crash-orphans accumulate and that GC
  would be a mark-and-sweep over all manifests. Knowing *why* it's needed is the interview
  currency; implementing it is not. ~3 hours saved.
- **Streaming chunker across read-buffer boundaries.** *(This is the biggest saving.)* Originally
  the pipeline was to stream fixed-size read buffers through a queue, forcing the chunker to
  carry rolling state across buffer boundaries. Instead: **each worker reads one whole file and
  chunks it in one pass.** Parallelism is per-file rather than intra-file. On a kernel tree
  (~80,000 files) this saturates 8 threads trivially. Known limitation: a single enormous file
  won't parallelise, and files must fit in memory. Both are defensible and worth stating aloud.
  ~5 hours saved and removes an entire class of bugs.
- **Chunk-size sweep as a CLI feature.** Run it manually once by rebuilding with different
  `--avg-chunk-kb` values and record the numbers by hand.
- **Index checkpointing.** The WAL *is* the index persistence — replay it fully on open. Note in
  the README that a production system would checkpoint periodically to bound replay time.
- **Symlinks, hardlinks, xattrs, ownership.** Regular files and directories only; preserve mode
  bits and mtime. Skip anything else and log it.
- **`--fixed-chunking` as a production flag.** It already exists inside `chunk_identity` for
  comparison purposes; that's where it's needed.

### 2.3 Realistic schedule

Roughly **four to five focused evenings**. Components 3–6 are one evening each if the formats
below are followed exactly; component 7 is half an evening; 8–11 together are one evening. The
WAL crash test is the piece most likely to overrun — budget for it.

---

## PART 3 — ARCHITECTURE

### 3.1 Repository layout on disk

```
<repo>/
├── repo.meta                    # magic, version, chunker params (see §3.3)
├── packs/
│   └── pack-000001.dat          # append-only chunk bytes, single file
├── wal.log                      # append-only index journal, CRC32 per record
└── snapshots/
    └── 20260907-143022.manifest # one per backup
```

**`repo.meta` stores the chunker parameters and the gear-table seed.** This is not decoration: if
two builds disagreed on `min/avg/max` or the gear table, identical bytes would chunk differently
and two snapshots of the same data would share zero chunks. On open, compare the binary's
compiled-in parameters against `repo.meta` and **refuse to proceed on mismatch** with a clear
error. This is a genuinely good detail to be able to point at.

### 3.2 Source tree layout

```
dedup-backup/
├── CMakeLists.txt
├── verify_stage1.sh
├── include/dedupbackup/
│   ├── chunker.hpp              # IChunker, ChunkSpan            [DONE]
│   ├── fastcdc_chunker.hpp      #                                [DONE]
│   ├── fixed_chunker.hpp        #                                [DONE]
│   ├── hasher.hpp               # IHasher, Digest, to_hex        [DONE]
│   ├── sha256_hasher.hpp        #                                [DONE, VERIFIED]
│   ├── chunk_store.hpp          # ChunkLocation, ChunkStore       [DONE, VERIFIED]
│   ├── chunk_index.hpp          # DigestHash, ChunkIndex          [DONE, VERIFIED]
│   ├── wal.hpp                  # Wal, WalRecordType, crc32       [DONE, VERIFIED]
│   ├── byte_io.hpp              # LE read/write helpers           [DONE, VERIFIED]
│   ├── manifest.hpp             # ManifestFileEntry, Manifest     [DONE, VERIFIED]
│   ├── bounded_queue.hpp        # BoundedQueue<T>                 [DONE, VERIFIED]
│   ├── backup_pipeline.hpp      # BackupOptions, run_backup       [DONE, VERIFIED]
│   └── repository.hpp           # ties store+index+wal together  [DONE, VERIFIED — built as
│                                 #   part of c7, not c3; see §4.10]
├── src/
│   ├── chunker/{fastcdc_chunker,fixed_chunker}.cpp               [DONE]
│   ├── hasher/sha256_hasher.cpp                                  [DONE, VERIFIED]
│   ├── store/chunk_store.cpp                                     [DONE, VERIFIED]
│   ├── index/chunk_index.cpp                                     [DONE, VERIFIED]
│   ├── wal/wal.cpp                                                [DONE, VERIFIED]
│   ├── manifest/manifest.cpp                                     [DONE, VERIFIED]
│   └── pipeline/repository.cpp, backup_pipeline.cpp              [DONE, VERIFIED]
│   └── main.cpp                 # CLI: init, backup implemented; list/restore/verify/stats/bench
│                                 #      remain [TODO c8/c9]
├── tools/
│   ├── chunk_stats.cpp          # chunk-size distribution        [DONE]
│   ├── chunk_identity.cpp       # dedup identity + resync dist.  [DONE, VERIFIED]
│   ├── store_check.cpp          # pack-file round-trip + reopen  [DONE, VERIFIED]
│   ├── index_check.cpp          # store+index+hasher dedup demo  [DONE, VERIFIED]
│   ├── wal_check.cpp            # CRC32 self-test + torn-write   [DONE, VERIFIED]
│   └── manifest_check.cpp       # format round-trip + corruption [DONE, VERIFIED]
└── tests/
    └── CMakeLists.txt           # wired up in c10
```

### 3.3 On-disk format specifications

All integers little-endian. All digests 32 raw bytes.

**`repo.meta`**
```
magic        char[8]   "DEDUPBK1"
version      u32       = 1
min_size     u32       = 2048
avg_size     u32       = 8192
max_size     u32       = 65536
gear_seed    u64       = 20240907
```

**`packs/pack-000001.dat`** — raw concatenated chunk bytes, no framing. Location is
`(offset u64, length u32)` held in the index. Framing lives in the WAL, not the pack.

**`wal.log`** — sequence of records:
```
length       u32       # byte count of type+payload
type         u8        # 1 = CHUNK_ADD
payload      bytes     # for CHUNK_ADD: digest[32] | offset u64 | length u32
crc32        u32       # over type+payload
```
Replay from offset 0. Stop at the first record where fewer than `length` bytes remain or the
CRC fails. Truncate the file to the last good record.

**`snapshots/<id>.manifest`**
```
magic        char[8]   "DEDUPMF1"
version      u32       = 1
snapshot_id  char[16]  # "YYYYMMDD-HHMMSS"
created_unix i64
file_count   u64
--- repeated file_count times, sorted by path for determinism ---
path_len     u16
path         bytes     # relative to backup root, '/' separated
mode         u32
size         u64
mtime_unix   i64
file_digest  byte[32]  # SHA-256 of whole file, for restore verification
chunk_count  u32
chunk_digests byte[32 * chunk_count]
```

### 3.4 CLI surface (reduced)

```
dedup-backup init    <repo>
dedup-backup backup  <source-dir> --repo <repo> [--threads N]
dedup-backup list    --repo <repo>
dedup-backup restore <snapshot-id> <dest-dir> --repo <repo>
dedup-backup verify  --repo <repo>
dedup-backup stats   --repo <repo>
dedup-backup bench   <source-dir> --repo <repo> [--threads N]
```

### 3.5 Concurrency model (simplified, per-file)

- **One reader/walker thread** traverses the source tree with `std::filesystem::recursive_directory_iterator`, pushing file paths into a **bounded blocking queue** (capacity ~256).
- **N worker threads** (default `std::thread::hardware_concurrency()`) each pop a path, read the
  whole file, chunk it, hash each chunk, compute the whole-file digest, then take a **single
  global mutex** to insert new chunks into the pack, append WAL records, and update the index.
- Per-file manifest entries accumulate in a vector under the same mutex; sort by path before
  writing so manifests are deterministic.
- The queue must be **bounded** so a fast walker cannot exhaust memory on a large tree. That is
  the backpressure design, and it is worth being able to explain.
- **Known bottleneck to state honestly:** one global mutex serialises all store mutation.
  Hashing and chunking — the CPU-heavy part — happen outside it, which is why throughput still
  scales. Sharding the index by digest prefix is the obvious next step.

---

## PART 4 — WORK ALREADY COMPLETE

### 4.1 Component 1 — FastCDC chunker `[DONE]`

**Gear hash:** `hash = (hash << 1) + GEAR[byte]`, where `GEAR` is 256 pseudorandom `uint64_t`
constants generated once from a hardcoded `std::mt19937_64` seed (`20240907`).

Two properties matter:

- **No window subtraction.** A Rabin-style rolling hash must subtract the byte leaving the
  window. Gear skips this: the left shift pushes old contributions toward the high bits until
  they overflow out of the 64-bit word, so influence decays naturally after ~64 bytes. An
  approximation of a fixed window, but the FastCDC paper found it sufficient, and it saves a
  subtraction and a lookup per byte.
- **The seed must never change.** Different gear tables chunk identical bytes differently, so two
  snapshots taken by different binaries would share zero chunks. Hence `gear_seed` in `repo.meta`.

**Normalized chunking.** A single mask of N low bits gives a geometric chunk-size distribution —
long tail of oversized chunks, excess of tiny ones. Tiny chunks bloat the index (one entry each);
chunks near `max_size` aren't really content-defined, they're truncated. FastCDC uses two masks:

- Below `avg_size`: **stricter** mask (`bits + 2` ones) — boundaries less likely, discourages
  premature small chunks.
- At/above `avg_size`: **looser** mask (`bits - 2` ones) — boundaries much more likely, pulls the
  cut toward `avg_size` rather than drifting to `max_size`.

With `bits = floor(log2(avg_size)) = 13`, that's a 15-bit and an 11-bit mask, reproducing the
paper's pattern for an 8 KB average.

**Core loop:**
```cpp
for (size_t i = config_.min_size; i < window; ++i) {
    hash = (hash << 1) + gear[data[start + i]];
    const uint64_t mask = (i < config_.avg_size) ? mask_s_ : mask_l_;
    if ((hash & mask) == 0) { cut = i + 1; break; }
}
```
Bytes before `min_size` are never hashed — no boundary test runs there, so hashing them is
wasted work. If no boundary fires by `max_size`, cut there anyway (hard ceiling; without it a
long run of identical bytes could produce an unbounded chunk).

The hash is **reset to 0 at the start of every chunk**.

**Known deviation from the paper:** we use contiguous low-bit masks; the paper uses masks with
1-bits deliberately spread across the word (e.g. `0x0000d90003530000`) so the boundary decision
samples a wider span of the rolling window. Ours works — the left shift still mixes several
recent bytes into the low bits — but it is less deliberately spread. **This is documented in a
comment above the mask computation and is a likely interview question.**

**Instrumentation added:** `ChunkSpan` carries a `content_defined` flag, false only for a
`max_size` truncation or the final short remainder. `FixedChunker` always reports false, since
every one of its cuts is positional by construction.

### 4.2 Component 2 — SHA-256 hasher `[WRITTEN, NEVER COMPILED]`

`include/dedupbackup/hasher.hpp` defines `IHasher`, `Digest` (fixed 32 bytes), and `to_hex`.
`sha256_hasher.cpp` uses OpenSSL's modern `EVP_Digest*` API, **not** the legacy `SHA256_*`
functions deprecated in OpenSSL 3.x.

**Why cryptographic, not xxHash/Murmur/CRC:** in a content-addressed store the hash *is* the
identity check. A collision makes the store believe two different chunks are the same, store one,
and silently corrupt every file referencing the other. Non-cryptographic hashes optimise for
speed and distribution, not collision resistance; at 32/64 bits, accidental collisions become
reachable at store scale via the birthday bound, and an adversary can construct them
deliberately. SHA-256's 256-bit space is what licenses treating "same digest" as "same bytes"
without ever comparing the bytes — which is the entire efficiency premise of content addressing.

**Swappability:** `IHasher` is a one-method interface (`hash(data, len) -> Digest`) and all
consumers depend on it, never on `Sha256Hasher`. `Digest` is fixed at 32 bytes rather than
variable-length, trading the ability to drop in a different output size for zero heap allocation
per chunk on the hottest path. Defensible because every plausible replacement (BLAKE2s, BLAKE3,
SHA3-256) is also 32 bytes.

### 4.3 Measurements so far

**Verified on real Linux 2026-09-08:** WSL2 Ubuntu-24.04, GCC 13.3.0, CMake 3.28.3, OpenSSL
3.0.13, `-std=c++17 -Wall -Wextra` — **zero warnings.** Real `chunk_identity` binary, real SHA-256
via OpenSSL EVP, not a stand-in. This supersedes an earlier MinGW/`std::hash` pre-check that
produced structurally identical but numerically different results (documented below for
context — the discrepancy is expected, not a bug: see note on exact counts).

Test input: 4 MB from the OS CSPRNG (fresh random bytes each run — see note below on why exact
chunk counts aren't reproducible run-to-run even though the *structure* of the results is).
Config `min=2048 avg=8192 max=65536`.

Chunk-size distribution:
```
FastCDC:     440 chunks, mean 9533, median 9433, min 2060, max 20345
Fixed 8 KB:  512 chunks, all exactly 8192
```
(440 × 9533 ≈ 4,194,520 ≈ 4 MB — internally consistent. Max of 20 KB sits well under the 64 KB
ceiling, i.e. `mask_l` is working. Note `min 2060 ≠ min_size 2048`; nothing guarantees a chunk of
exactly `min_size` occurs.)

Boundary origin: **439/440 content-defined, 1/440 (0.23%) positional** — that one is the EOF
remainder, not a real `max_size` hit. Truncation ratio ≈ 0, as predicted for high-entropy input.

Chunk **identity** after inserting one byte (real SHA-256):
```
FastCDC,    insert at offset 0:     440/440 chunks, 439 shared, 1 differing
FastCDC,    insert at midpoint:     440/440 chunks, 439 shared, 1 differing
Fixed-size, insert at offset 0:     512 vs 513,       0 shared   (total reflow)
Fixed-size, insert at midpoint:     512 vs 513,     256 shared   (only the prefix survives)
```
This is the headline result and the proof the whole project rests on. Fixed-size dedup is purely
a function of how much of the file precedes the edit; CDC's is not.

A 50-offset pseudorandom sweep (seed `20240907`) showed **all 50 trials differing by exactly one
chunk** — `min=1 max=1 mean=1.00`, entire histogram mass at "1 chunk differs: 50 trials" — against
a 15–512 spread (`mean=266.30`) for fixed-size. This is the real, non-stand-in confirmation of the
decay-based derivation in §4.4: not just plausible, empirically exact on this dataset.

Multi-byte edit shapes (real SHA-256): 100-byte insert and 100-byte delete both cost FastCDC
exactly 1 chunk, same as the single-byte case. A 10 KiB insert cost 1 chunk on the original side
but 2 on the modified side (`440→441` total) — expected: 10 KiB of new content is itself larger
than one average chunk, so the edit doesn't just displace a boundary, it also introduces roughly
one whole new chunk's worth of genuinely new material.

**Note on exact counts not being reproducible run-to-run:** `chunk_identity --gen` draws from
`std::random_device` (the OS CSPRNG) fresh every invocation, so the *exact* chunk count varies
between runs (457 in the original MinGW/`std::hash` pre-check vs. 440 here) even on an identical
4 MB size and config — different random bytes produce different candidate cut points. What stays
invariant across runs is the *structure*: mean near `avg_size`, max well under `max_size`,
truncation ratio ≈ 0, and — the actual claim — the resync distribution spiking at exactly 1. Don't
be alarmed if a future run shows yet another chunk count; that alone is not a regression signal.

**Debugging anecdote worth keeping:** the first attempt generated "random" test bytes with a
hand-rolled LCG and the chunker produced degenerate output — every chunk hit `max_size`. Not a
chunker bug: an LCG's low bits have short cycles, and the gear hash's boundary test reads exactly
those low bits. Switching to the OS CSPRNG fixed it immediately. Good story, and a real lesson
about test-data quality.

### 4.4 The resync derivation — CORRECTED, READ THIS CAREFULLY

This went through three wrong explanations before landing. The final version is the one to use.

**Why the distribution spikes at exactly 1:**

The gear hash forgets its history after ~64 bytes. `min_size` is 2048 — **32× the warm-up
distance** — so by the time the loop tests any position, the hash has entirely forgotten where
its scan began. Therefore the set of file positions satisfying the cut condition (call them
**candidate cut points**) is a property of the *content alone*, not of where chunking started.

Trace an insertion: the chunk containing it has its boundary displaced to an essentially random
new position. The next chunk starts there, skips `min_size` (far past warm-up), then cuts at the
**first candidate it encounters**. If no candidate lies between the old scan-start and the new
one, it lands on *exactly the same candidate the original chunk used* — alignment restored in a
single chunk. That is the common case, and it explains a sharp spike at 1 rather than a spread.

**Three explanations that are wrong, and why** (each may come up in interview):

1. *"The hash reset at each chunk start makes each chunk's boundary a pure function of its own
   bytes, so resync is architecturally immediate."* No. The reset plus the `min_size` skip means a
   boundary's position depends on where its chunk started, so boundaries form a **chain**. The
   reset argues *against* guaranteed resync, not for it.
2. *"It cascades unboundedly."* Overstated. The decay property makes candidates position-
   independent, which is what damps the chain.
3. *"It realigns by coincidence."* Coincidence would produce a spread, not 50/50 at exactly 1.

**Residual second-order effect:** normalized chunking selects its mask by distance from the chunk
start, so the candidate set is not *perfectly* position-independent. Small on random data.

**The honest framing:** high-probability for a specific structural reason, not guaranteed. A pure
sliding-window CDC with no reset and no minimum size *would* have a provable one-window resync
bound. FastCDC trades that guarantee away for speed and a bounded size distribution. **That
trade is the interesting answer**, and it's better than any of the three wrong ones.

### 4.5 The `max_size` limitation — documented

When a chunk hits `max_size` without a boundary firing, the cut is placed at a fixed offset from
the chunk start. That cut is **purely positional, with zero content dependency**, so it breaks
the alignment argument outright — the following chunk starts at an arbitrary position with no
content-derived reason to realign.

On uniform random bytes candidates are dense and `max_size` essentially never fires (our sample
topped out at 21 KB). **Real source trees are different**: long zero runs, repeated boilerplate,
low-entropy regions where candidates are sparse and truncation *will* fire. That is the concrete
mechanism by which the resync distribution could widen on kernel data.

This is why `content_defined` instrumentation exists. **The truncation ratio on the kernel tree
versus ≈0 on random data is the diagnostic**, and both numbers belong in the README.

### 4.6 Component 3 — chunk store `[DONE, VERIFIED]`

`ChunkStore` owns a single append-only pack file. No digest/dedup awareness at all — that's the
index's job (component 4). It answers exactly two questions: "store these bytes, tell me where,"
and "give me the bytes at this (offset, length)."

**Why pack files, not one-file-per-chunk.** A real backup produces tens of thousands of chunks.
One file per chunk means one inode per chunk (filesystem metadata overhead), a full block
allocated per chunk regardless of chunk size (a 2 KB chunk still burns a full 4 KB block on most
filesystems), one `open()`/`close()` pair per chunk, and a directory with tens of thousands of
entries that's slow to list or `rsync`. Packing amortizes all of it: one `open()` per backup,
sequential writes, no per-chunk filesystem metadata operation.

**Why `pwrite`/`pread` at explicit offsets, not `write`/`read` with `O_APPEND`.** `write()` and
`read()` on one fd share an implicit file-offset cursor — a concurrent reader (restore) and writer
(backup) on the same store would race over where the next operation lands. `pwrite()`/`pread()`
take the offset explicitly and never touch that cursor, removing the race by construction. (There's
also a Linux-specific wrinkle where `O_APPEND` affects `pwrite()` in a way that contradicts POSIX —
one more reason to avoid the combination.)

**Durability contract:** `append()` does not return until `fsync()` completes. This is the
half of the WAL ordering guarantee (§1.4) that lives in this component — the WAL (component 5)
depends on being able to write a record pointing at a chunk immediately after `append()` returns,
trusting the bytes are already durable.

**Verified on Linux** (`tools/store_check.cpp`, WSL2 Ubuntu-24.04, GCC 13.3.0, 2026-09-08): 500
random chunks (1 byte–64 KB) appended to a fresh pack file, all 500 read back byte-identical by
their returned `(offset, length)`, then the store was destructed and reopened against the same
file — the reopened instance correctly resumed appending at the exact byte offset the previous
instance left off at, and the newly appended chunk round-tripped correctly too. Real transcript:
```
appended 500 chunks, pack file now 16158857 bytes
round-trip OK: all 500 chunks read back byte-identical
reopen OK: appended after existing 16158857 bytes, round-tripped correctly
```
Build: zero warnings at `-Wall -Wextra` / `-std=c++17` / GCC 13.3.0.

**Known limitation, by design (see §2.2):** one growing pack file, no rotation at a size
threshold. Also: this component provides no crash-safety on its own — a `kill -9` between the
`pwrite` loop and the `fsync` could in principle leave a torn write in the pack file. In practice
this is why the WAL never trusts a pack file offset it hasn't itself durably recorded: an orphaned
or torn chunk at the tail of the pack file, from a chunk whose WAL record never got written, is
simply never referenced by anything and is harmless. Verifying that end-to-end is component 5's
crash-recovery test, not this component's.

### 4.7 Component 4 — chunk index `[DONE, VERIFIED]`

`ChunkIndex` is a thin wrapper over `std::unordered_map<Digest, ChunkLocation, DigestHash>` — the
whole answer to "do we already have this chunk?" Two decisions worth being able to defend:

**Hash function truncates the digest instead of re-hashing it.** `Digest` is already a SHA-256
output — uniformly-distributed, cryptographically strong bits by construction. `DigestHash` just
takes the first 8 bytes as a `size_t`. Re-hashing with `std::hash`/FNV on top would throw away
hash quality for no benefit. The key distinction: a hash-*table* collision (two digests landing
in the same bucket) is harmless, resolved by chaining — a totally different concern from a
*cryptographic* collision (two different chunks producing the same digest), which would actually
corrupt the store. Truncating is safe for the former even though it would be catastrophic for the
latter. Same pattern used by restic, Borg, ZFS dedup.

**No persistence of its own, no internal locking — both deliberate.** Persistence is the WAL's
job (component 5): on startup, replay every valid WAL record and call `insert()` for each — the
index is a pure derived structure, never itself the source of truth, so there's no separate index
file that could get out of sync with the WAL. Locking is the caller's job (§3.5's single global
mutex around the whole "check index, store bytes, write WAL, update index" sequence) — adding a
second lock inside `ChunkIndex` would be redundant at best.

**Verified on Linux** (`tools/index_check.cpp`, WSL2 Ubuntu-24.04, GCC 13.3.0, 2026-09-08): the
first end-to-end rehearsal of the actual dedup decision (`hash → contains? → skip : store+insert`)
across all three components built so far (hasher, store, index). 300 synthetic logical chunks (100
unique + 200 exact re-uses of earlier ones) processed; verified the index landed on exactly 100
unique entries, exercised full round-trip recovery (index.find + store.read) for all 300 —
including the 200 that were dedup hits and never separately stored — and confirmed every one
produced byte-identical content back. Real transcript:
```
processed 300 logical chunks: 200 dedup hits, 100 newly stored
index size (unique chunks): 100
logical bytes: 9627507, physical bytes stored: 3150137, saved: 67.3%
round-trip OK: all 300 logical chunks resolve to correct bytes via the index
```
Build: zero warnings at `-Wall -Wextra` / `-std=c++17` / GCC 13.3.0.

### 4.8 Component 5 — write-ahead log `[DONE, VERIFIED]`

`Wal` is the index's persistence (§4.7): on startup, `replay()` walks the file from byte 0 and
calls `ChunkIndex::insert()` for every valid record. There is no separate index file — the WAL
*is* the source of truth, exactly as scoped in §2.2.

**Format, exactly per §3.3:** `length u32 | type u8 | payload | crc32 u32`, all little-endian.
For `CHUNK_ADD` (the only defined type): `payload = digest[32] | offset u64 | length u32` (44
bytes), so the full on-disk record is `4 + 1 + 44 + 4 = 53` bytes. The CRC covers `type + payload`
only (45 bytes) — not the length prefix, since the length prefix is what tells a reader how many
bytes to feed the CRC in the first place, so it can't be self-referential.

**CRC32 implemented from scratch** (standard IEEE 802.3 / zlib polynomial `0xEDB88320`, table
generated once via static init — the same "build a lookup table on first use" pattern as the gear
table in the chunker, for consistency). Verified against the standard CRC-32/ISO-HDLC test vector:
`crc32("123456789") == 0xCBF43926`. Not the WAL's own invention — a recognizable, standard
algorithm, which matters for defensibility ("did you implement CRC32 correctly" is answerable with
a known test vector, not just "trust me").

**Little-endian I/O factored into `byte_io.hpp`** (`write_u32_le`/`read_u32_le`/`write_u64_le`/
`read_u64_le`) rather than duplicated in the WAL and, later, the manifest (component 6, which
needs identical LE serialization per §3.3). Deliberately explicit byte-at-a-time shift/mask
instead of a struct memcpy — struct padding and host endianness are both compiler/platform
decisions the on-disk format can't depend on; explicit serialization is portable regardless.

**`pwrite`/`pread` at tracked offsets, same pattern as `ChunkStore`** (§4.6) — for the same reason:
no shared file-offset cursor to race over between a hypothetical concurrent reader and the single
writer.

**Torn-tail recovery is a physical truncation, not just an in-memory stop.** `replay()` stops
scanning at the first record that's too short for its own length prefix, or whose CRC doesn't
match, then calls `ftruncate()` to physically cut the file at the last good record boundary. This
matters: without it, a later append would write new valid bytes starting right after the last good
record, but any leftover garbage past the end of *that* new record would still physically be on
disk, waiting for some future replay to walk into it. Truncating removes that failure class
outright rather than relying on probability (garbage bytes never coincidentally passing a length+
CRC check).

**Durability ordering — the actual crash-safety guarantee (§1.4):** `ChunkStore::append()` fsyncs
chunk bytes; only then does `Wal::append_chunk_add()` write and fsync the record that references
them. A crash between the two leaves an orphaned, unreferenced chunk in the pack file — wasted
space, never corruption, because nothing durable ever claimed to know about it. The reverse order
would be fatal: a durably-written WAL record could reference chunk bytes that were never actually
written, and replay would hand the index a location pointing at garbage or past-EOF.

**Verified on Linux** (`tools/wal_check.cpp`, WSL2 Ubuntu-24.04, GCC 13.3.0, 2026-09-08) — this is
the real foundation for the component-10 WAL-torn-write CTest test, run here as a standalone
binary first:
```
CRC32 self-test OK (0xcbf43926 matches standard test vector)
clean replay OK: 50/50 records recovered correctly after reopen
torn-write replay OK: recovered exactly 55 good records, discarded the torn tail
physical truncation OK: file shrank from 2925 to 2915 bytes on disk
post-recovery append OK: 56 total records replay cleanly
```
The 2925→2915 shrink is exact, not approximate: 55 good records × 53 bytes/record = 2915 bytes;
the 10 hand-crafted "crash mid-write" garbage bytes were physically removed, not merely skipped
over. Build: zero warnings at `-Wall -Wextra` / `-std=c++17` / GCC 13.3.0.

### 4.9 Component 6 — snapshot manifest `[DONE, VERIFIED]`

`write_manifest`/`read_manifest` serialize/deserialize a `Manifest` (snapshot id, creation time,
a list of `ManifestFileEntry`) to the exact binary format in §3.3. This component is deliberately
**opaque to the filesystem** — a `ManifestFileEntry`'s mode/size/mtime/digests arrive already
populated; nothing here calls `stat()`, walks a directory, or touches `std::filesystem`. See the
correction in §5.2: that dependency was originally predicted here but actually belongs to
component 7, which is where a real directory gets turned into `ManifestFileEntry` values.

**Sorting is enforced internally, not trusted to callers.** `write_manifest` sorts a local copy of
`files` by path before serializing, regardless of what order they arrive in. §3.2 requires
"sorted by path for determinism" — making that the caller's responsibility would mean every future
call site has to remember it, and a single forgotten sort would silently make manifests
non-deterministic. Enforcing it once, in the one place that writes the format, closes off that bug
class entirely.

**Fixed-width `snapshot_id` field (16 bytes: 15-char `"YYYYMMDD-HHMMSS"` + 1 NUL pad byte), unlike
the length-prefixed `path` field.** Possible because the format guarantees the snapshot id has a
canonical shape, and it means a reader can validate the entire fixed-size header — magic, version,
snapshot_id, created_unix, file_count — in one pass before it has to start walking variable-length
file entries one at a time.

**Single buffered write, one `fsync`, unlike the WAL/pack file's per-operation durability.** A
manifest has one entry per *file*, not per byte of backed-up data, so even a large backup produces
a manifest small enough to build entirely in memory and write once at the very end of a successful
backup run. The WAL's incremental-durability design exists specifically because a backup can run
for a long time and a mid-run crash must not lose already-completed work; a manifest that's only
ever written after a backup fully completes has no such requirement.

**Verified on Linux** (`tools/manifest_check.cpp`, WSL2 Ubuntu-24.04, GCC 13.3.0, 2026-09-08) — a
synthetic manifest (5 files inserted deliberately out of path order, including one zero-chunk
empty file), the `snapshot_id` 15/16-character boundary, and two corruption-detection cases (bad
magic, truncated file):
```
wrote manifest: /tmp/manifest_test/snapshots/20260907-143022.manifest (5 files, inserted out of path order)
sort-by-path OK: entries stored in ascending order regardless of insertion order
round-trip OK: all 5 file entries (including the zero-chunk empty file) match exactly
snapshot_id boundary OK: 15-character id accepted
snapshot_id boundary OK: 16-character id correctly rejected
corruption detection OK: bad magic correctly rejected
corruption detection OK: truncated manifest correctly rejected
```
Build: zero warnings at `-Wall -Wextra` / `-std=c++17` / GCC 13.3.0.

### 4.10 Component 7 — thread pool, directory walk, and the first real `backup` `[DONE, VERIFIED]`

Ties every prior component together into an actual working `dedup-backup init` / `dedup-backup
backup`. This is where `std::filesystem` genuinely enters the codebase (the correction from §5.2)
via `std::filesystem::recursive_directory_iterator` in the walker thread — the manifest format
itself (component 6) never needed it.

**`BoundedQueue<T>`** (`bounded_queue.hpp`, header-only template): `push()` blocks while full,
`pop()` blocks while empty, plain `std::mutex` + two `std::condition_variable`s, no lock-free
structure per the project's constraints. This is the entire backpressure design: one walker thread
pushes file paths (capacity 256, per §3.5), N worker threads pop them; if workers fall behind, the
queue fills and the walker blocks, so a huge source tree can never cause the walker to buffer an
unbounded number of pending paths.

**`Repository`** (`repository.hpp`/`.cpp`) ties `ChunkStore` + `ChunkIndex` + `Wal` together and
owns the single global mutex from §3.5. Its one synchronized entry point,
`store_chunk_if_absent()`, performs check-index / store-bytes-with-fsync / log-WAL-with-fsync /
update-index atomically — worker threads never touch a mutex directly, they just call this method.
Originally mis-scoped as component 3 in the early plan (before the index/WAL it ties together
existed); built here, where it actually belongs.

**Sharing one `Sha256Hasher` and one chunker instance across all worker threads, not one per
thread** — deliberate, and safe for a specific, checkable reason rather than assumption:
`Sha256Hasher::hash()` creates and destroys its own `EVP_MD_CTX` inside the call (verified by
re-reading `sha256_hasher.cpp` — no member state at all), and `FastCDCChunker`/`FixedChunker` set
their members once at construction and never mutate them afterward, with `chunk()` a `const`
method touching only locals plus a function-local `static const` gear table (thread-safe init via
"magic statics", read-only after). Both are safe to share concurrently because they hold no
mutable state post-construction — not because of any added locking.

**Whole-file digest costs a genuine second pass over the data — not an oversight.** Per-chunk
digests only prove each chunk's own bytes are intact in isolation; they don't prove the chunks
were concatenated in the right order during restore. An independent whole-file SHA-256 is an
end-to-end check that would catch a reassembly/ordering bug the per-chunk digests alone couldn't.
The cost is real — total hashing work is ~2x what chunk digests alone would need, since every byte
of the file gets hashed once as part of its chunk and once again as part of the whole-file digest
— and is named explicitly here as a deliberate correctness-over-throughput tradeoff, not
discovered later as a performance surprise.

**`mode`/`mtime` come from POSIX `stat()`, not `std::filesystem::last_write_time()`**, even though
`std::filesystem` is already in use for the walk. `last_write_time()` returns a
`std::filesystem::file_time_type`, and converting that to a plain Unix-epoch `time_t` has no
portable, well-defined path before C++20's `clock_cast` — `stat()`'s `st_mtime` is already exactly
the `time_t` the manifest format wants, with no conversion question at all. `std::filesystem` earns
its place here specifically for the recursive walk, which it's genuinely good at; it isn't used
for jobs a simpler, more direct POSIX call already does better.

**A real bug, found empirically, not theorized — and fixed:** snapshot ids have one-second
resolution (`"YYYYMMDD-HHMMSS"`). The very first real two-backups-in-a-row test against this
project's own source tree produced two identical snapshot ids (both `20260908-101219`), and the
second `write_manifest()` call — which truncates-and-overwrites — silently destroyed the first
snapshot's manifest. Content happened to be unchanged between those two test runs, so nothing was
actually lost that time, but had the source changed in between, the first snapshot would have
become permanently unrecoverable with no error of any kind. Fixed by having `run_backup()` check
`Repository::has_snapshot()` before writing and retry (sleeping up to 5 seconds) rather than ever
silently overwrite an existing manifest; failing loudly after 5 straight collisions rather than
looping forever. Deliberately didn't change the on-disk snapshot-id format (already spec'd and
shipped in component 6) to add a disambiguator — the collision window is sub-second and this
project's realistic usage pattern (manual or scripted, not sub-second-repeated) makes the retry
cost negligible in practice. **This is exactly the kind of thing worth volunteering in an
interview**, per the working agreement's spirit: a real correctness bug this project's own testing
caught before it caused real data loss, not a bug someone else found.

**Verified on Linux** (WSL2 Ubuntu-24.04, GCC 13.3.0, 2026-09-08) — the first real end-to-end runs
of the actual `dedup-backup` binary:

*Same-content re-backup* (against this project's own `src/`, twice, back to back):
```
snapshot 20260908-101354.manifest created (10 files) — 15 unique chunks
snapshot 20260908-101355.manifest created (10 files) — 15 unique chunks   (distinct id, both
                                                                            manifests intact —
                                                                            the collision fix
                                                                            working correctly)
```

*Real dedup demonstration* (4 files: a 2 MB random file, an exact byte-for-byte duplicate of it in
a subdirectory, a small text file, a 500 KB random file; then a realistic edit — 10 KB appended to
the 2 MB file, simulating a growing log — before a second backup):
```
=== backup 1 ===
snapshot 20260908-101412 created (4 files) — 266 unique chunks
=== backup 2 (one file appended to) ===
snapshot 20260908-101413 created (4 files) — 267 unique chunks
```
The exact-duplicate file contributed **zero** new chunks in backup 1 (fully deduped against its
twin). Appending 10 KB to a 2 MB file grew the unique chunk count by exactly **1** — the same
single-chunk resync behavior proven analytically in §4.4 and empirically in §4.3, now observed
through the real end-to-end pipeline rather than the standalone `chunk_identity` tool. Both
snapshots' manifests remain distinct and intact on disk.

Build: zero warnings at `-Wall -Wextra` / `-std=c++17` / GCC 13.3.0 (one real warning surfaced and
was fixed during this component — `-Wformat-truncation` on the snapshot-id `snprintf`, GCC
reasoning conservatively about `%d`'s theoretical worst case; fixed by sizing the buffer for that
worst case rather than suppressing the warning).

**Known limitation, honestly scoped, not yet built:** no per-file skip-if-unchanged optimization —
every backup re-reads, re-chunks, and re-hashes every file it walks, regardless of mtime. The
chunk-level dedup in `Repository::store_chunk_if_absent()` still prevents re-storing identical
bytes (confirmed above: an unchanged re-backup added zero new chunks), so correctness and disk
usage are unaffected — this only costs CPU/IO on files that didn't change. Not in the reduced
scope (§2.2); worth naming as a "what would you do next" answer.

---

## PART 5 — IMMEDIATE NEXT STEPS

### 5.1 Environment (blocking everything)

WSL2 on Windows 11 is a real Linux kernel — real `fsync`, real ext4, real `std::filesystem` —
and is fully adequate, including for benchmark numbers.

PowerShell as Administrator:
```powershell
wsl --install -d Ubuntu-24.04
```
Reboot, set username/password, then inside Ubuntu:
```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev git
```

**Critical:** do **not** keep the repo under `/mnt/c/`. The Windows↔Linux translation layer is
slow enough to make throughput numbers meaningless. Work in `~/`. Push from Windows, clone
inside WSL. In VS Code install the **WSL extension** and use *WSL: Reopen Folder in WSL* so the
editor and any Claude Code session both execute in Linux.

Free space needed: ~30 GB.

### 5.2 First task — verify stages 1 and 2 `[DONE 2026-09-08]`

Completed on WSL2 Ubuntu-24.04. `cmake --build` produced zero warnings at `-Wall -Wextra` under
GCC 13.3.0. `chunk_identity` reproduced §4.3 with real SHA-256 — see that section for the verified
numbers. This gate is closed; component 3 is next.

Only C++17 dependency in the tree today: a structured binding in `chunk_identity.cpp`.
`std::filesystem` arrives in component 7, not 6 — **correction from the original plan**: component
6 (the manifest) turned out to need no filesystem access at all, since it only serializes/
deserializes a `ManifestFileEntry` struct that's opaque to how its fields (mode/size/mtime/
digests) were obtained. Populating those from a real directory is `std::filesystem::
recursive_directory_iterator`'s job, and that's genuinely component 7 (the tree walk +
thread pool), not the manifest format itself. Cleaner separation of concerns than originally
scoped — see §4.9.

### 5.3 Then build, in order

Components 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11, per Part 2 and the formats in §3.3.

---

## PART 6 — BENCHMARK PROCEDURE

```bash
mkdir -p ~/bench && cd ~/bench
for v in 1 2 3 4 5 6 7 8; do
  wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.$v.tar.xz
  mkdir -p tree-$v && tar xf linux-6.6.$v.tar.xz -C tree-$v
done
```

Eight consecutive kernel releases, ~9.6 GB extracted. This dataset is chosen deliberately: it is
reproducible by anyone, and consecutive releases have the "mostly unchanged with genuine churn"
profile dedup is built for. Ten copies of the *same* tree would give 90%+ and prove nothing.

```bash
dedup-backup init ~/repo
for v in 1 2 3 4 5 6 7 8; do
  dedup-backup backup ~/bench/tree-$v --repo ~/repo
done
dedup-backup stats --repo ~/repo
dedup-backup bench ~/bench/tree-1 --repo ~/repo-bench
```

Also run: the 50-offset resync sweep against a kernel tree (not just synthetic), and the
`content_defined` truncation ratio for both datasets.

### Resume claims that must come from this run

The following are currently **targets**, not measurements. Replace with real numbers before
submitting anything.

| Claim on resume | Target | Notes |
|---|---|---|
| Dedup reduction | **76% (4.2×)** | Conservative for 8 kernel trees. 71% or 81% is fine — quote the real one. |
| Dataset size | **9.6 GB** | Whatever your extraction actually totals. |
| Throughput | **310 MB/s on 8 threads** | Deliberately conservative. SHA-256 without SHA-NI runs ~250–400 MB/s per core; you will likely beat this. |
| Crash-consistent WAL | — | Backed by the crash-recovery test. |
| Byte-exact verified restores | — | Backed by manifest file digests. |

---

## PART 7 — TESTS (component 10, four only)

1. **Roundtrip.** Back up a generated tree, restore it, assert byte-exact equality and matching
   mode/mtime.
2. **Insertion identity.** Back up a file, insert one byte at the front, back up again, assert
   the second snapshot added only a small number of new chunks. *This is the test that proves
   CDC works.*
3. **Crash recovery.** `kill -9` mid-backup, restart, assert the previous snapshot still restores
   byte-exactly and the WAL replayed cleanly.
4. **WAL torn write.** Truncate `wal.log` mid-record, assert clean recovery to the last good
   record with no crash and no bogus index entries.

---

## PART 8 — INTERVIEW DEFENCE BRIEFING

**Why CDC over fixed-size blocks?** The boundary-shift problem. Quote the measured numbers:
456/457 shared versus 0/512 after a front insertion.

**How does FastCDC pick a boundary?** Rolling gear hash, cut when the masked low bits are zero.
Expected chunk size 2^N. Min/max enforced because the geometric distribution otherwise produces
pathological chunks at both ends. Normalized chunking uses two masks to tighten it.

**Why 8 KB average?** Dedup granularity versus index overhead — smaller chunks find more
redundancy but multiply index entries and per-chunk metadata. Quote your own 4/8/16 KB sweep.

**Why those specific mask constants in the paper?** Scattered 1-bits sample the rolling window
more evenly across its span; our contiguous masks are a documented simplification.

**Hash collisions?** Birthday bound ~2^-128, orders of magnitude below undetected disk error
rates. Same argument every production dedup system makes.

**What happens on a crash mid-backup?** Chunk bytes then WAL record, each fsynced. Crash between
leaves an unreferenced orphan — wasted space, never corruption. Replay stops at the first bad
CRC. Reverse ordering would be fatal. Proven by test 3.

**Where's the bottleneck?** SHA-256, not I/O — confirm with `perf`. Next steps: SHA-NI intrinsics
or BLAKE3. Secondary: the single global store mutex; shard the index by digest prefix.

**How do you know restores are correct?** Per-file SHA-256 in the manifest, recomputed after
restore and compared.

**How fast does chunking resync after an edit?** Use §4.4. Lead with the decay/candidate-cut-point
derivation, note it's high-probability rather than guaranteed, and mention the FastCDC-versus-
pure-CDC trade. Then raise the `max_size` truncation limitation from §4.5 yourself — volunteering
the failure mode of your own design is the strongest move available.

**What would you do next?** Garbage collection of orphans, index checkpointing to bound WAL
replay, pack rotation with compaction, intra-file parallelism for large files, compression on
top of dedup.

---

## PART 9 — WORKING AGREEMENT FOR THE NEXT SESSION

1. Build one component at a time. Show code, explain decisions, stop, wait.
2. **Never present a number without stating exactly which binary produced it and what command was
   run.** This has already caused one round of confusion.
3. A scratch file that isn't in the repo must not be the source of a headline number.
4. Compile-tested is not tested. Every component gets run on Linux before the next one starts.
5. When a derivation is challenged, re-derive from mechanism rather than defending the previous
   answer. Two of the three wrong resync explanations came from defending rather than re-deriving.
