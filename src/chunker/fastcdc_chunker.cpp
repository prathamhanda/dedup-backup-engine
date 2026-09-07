#include "dedupbackup/fastcdc_chunker.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <random>
#include <stdexcept>

namespace dedupbackup {

namespace {

// GEAR: a 256-entry table of "random" 64-bit constants, one per possible
// byte value. Generated once, with a *fixed* seed, at first use.
//
// The seed must be fixed and never change: two machines (or two runs) that
// disagree on GEAR would compute different chunk boundaries for identical
// bytes, which silently breaks deduplication between backups taken with
// different builds. Determinism here is a correctness requirement, not a
// nicety.
const std::array<uint64_t, 256>& gear_table() {
    static const std::array<uint64_t, 256> table = [] {
        std::array<uint64_t, 256> t{};
        std::mt19937_64 rng(0x8f3f'73b5'cf1c'9adeULL);
        for (auto& v : t) v = rng();
        return t;
    }();
    return table;
}

uint32_t floor_log2(size_t n) {
    uint32_t bits = 0;
    while (n >>= 1) ++bits;
    return bits;
}

// Builds a mask with `ones` low-order bits set, i.e. selects the `ones`
// least-significant bits of the rolling hash for the (hash & mask) == 0
// boundary test.
uint64_t low_bits_mask(uint32_t ones) {
    if (ones == 0) return 0;
    if (ones >= 64) return ~uint64_t{0};
    return (uint64_t{1} << ones) - 1;
}

} // namespace

FastCDCChunker::FastCDCChunker(FastCDCConfig config) : config_(config) {
    if (!(config_.min_size < config_.avg_size && config_.avg_size < config_.max_size)) {
        throw std::invalid_argument("FastCDCConfig requires min_size < avg_size < max_size");
    }

    // FastCDC's "normalized chunking, level 2": bias the boundary
    // probability by +/-2 bits around the natural 1/avg_size rate.
    //   - below avg_size, require 2 *more* zero bits than the naive
    //     mask (stricter -> boundaries are less likely -> fewer chunks
    //     end up smaller than avg_size)
    //   - at/above avg_size, require 2 *fewer* zero bits (looser ->
    //     boundaries become more likely -> the chunker is pushed to cut
    //     soon rather than drift toward max_size)
    // The net effect is a tighter, more symmetric size distribution
    // clustered around avg_size than a single fixed-probability mask
    // would produce.
    const uint32_t bits = floor_log2(config_.avg_size);
    const uint32_t level = 2;
    mask_s_ = low_bits_mask(bits + level);
    mask_l_ = low_bits_mask(bits > level ? bits - level : 1);

    // Deviation from the reference implementation, documented here so it
    // can be defended explicitly: the FastCDC paper's masks (e.g.
    // 0x0000d90003530000 for an 8 KiB average) don't just pick a bit
    // *count*, they pick specific bit *positions* scattered across the
    // 64-bit word — mixing bits set by bytes several steps apart in the
    // gear-hash shift history with bits set only by the most recent
    // couple of bytes. That spread makes the boundary test sensitive to
    // a wider span of the trailing window instead of being dominated by
    // whichever byte happens to occupy the low bits at test time.
    //
    // low_bits_mask() here instead takes a contiguous block of N
    // low-order bits. Because the gear hash shifts left by one bit per
    // byte, the low N bits are still a mix of the last several bytes'
    // GEAR contributions (not just the very last byte), so this keeps
    // the "test depends on multiple recent bytes" property — it's just
    // less deliberately mixed across the full 64-bit history than the
    // paper's hand-picked constants. In exchange it's readable and
    // parameterized by avg_size instead of hardcoded per chunk-size
    // tier. The measured distribution (tools/chunk_stats) is close to
    // the paper's reported shape, so this simplification is treated as
    // acceptable rather than a correctness bug.
}

std::vector<ChunkSpan> FastCDCChunker::chunk(const uint8_t* data, size_t len) const {
    std::vector<ChunkSpan> chunks;
    if (len == 0) return chunks;

    const auto& gear = gear_table();
    size_t start = 0;

    // --- Resync after an edit: the real mechanism ---
    //
    // Each chunk resets `hash = 0` at its own start and doesn't begin
    // testing until `min_size` bytes in. It's tempting to conclude from
    // that reset alone that resync after an edit is a matter of luck —
    // two independently-reset scans happening to land on the same offset.
    // That's NOT what the data shows (see tools/chunk_identity.cpp: 50/50
    // pseudorandom single-byte insertions landed on exactly 1 differing
    // chunk on a random-data sample) — a real coincidence process would
    // produce a spread, not a spike at 1. The actual mechanism is the
    // gear hash's decay property, which the reset argument dismisses too
    // quickly.
    //
    // `hash = (hash << 1) + gear[byte]` shifts every existing bit left by
    // one position per byte processed. A byte's contribution to a 64-bit
    // hash is fully shifted out (and has stopped affecting the low bits
    // the mask tests) after roughly 64 bytes. So once a scan has run for
    // at least ~64 bytes since its last reset, the hash value at that
    // point is — to good approximation — a function of only the trailing
    // ~64 content bytes, not of where the scan itself started. Two
    // different scans, reset at two different offsets, that both happen
    // to be scanning the same underlying ~64-byte content window converge
    // on the same hash value there.
    //
    // `min_size` is 2048 — 32x the ~64-byte warm-up distance — and no
    // testing happens before `min_size` bytes into a chunk. So every
    // position this loop ever actually tests is already fully warmed up.
    // That means the boundary test effectively defines a set of CONTENT
    // positions that satisfy (hash & mask) == 0 — call them candidate cut
    // points — that is a property of the bytes themselves, not of which
    // chunk's scan is currently passing through them.
    //
    // Now trace the edit: the disrupted chunk's scan matches the original
    // up to the inserted byte, then the candidate-point sequence it's
    // walking is locally perturbed near the edit (some candidates
    // vanish, others appear, purely from the ~64 bytes of window
    // overlapping the edit) — it stops at the first candidate reached
    // after that perturbed region, which is either a new candidate born
    // from the edit or the very next pre-existing candidate. Either way,
    // the NEXT chunk starts there, skips min_size (2048, still >> 64), and
    // resumes testing the same content-derived candidate set as
    // everyone else once warmed up. Because inter-candidate spacing
    // averages avg_size (8192 here) and the perturbed region is only
    // about one warm-up window (~64 bytes) wide, there's usually no other
    // candidate squeezed into that gap — so the very next chunk lands on
    // exactly the same candidate the original file's corresponding chunk
    // used, and the sequences are bit-identical from there on. That's why
    // the common case is exactly one differing chunk, not a spread: it's
    // a structural consequence of warm-up distance being tiny relative to
    // candidate spacing, not a coincidence.
    //
    // This is still probabilistic, not guaranteed, for two reasons:
    //   1. Normalized chunking (mask_s_ vs mask_l_) selects its mask by
    //      `i < avg_size`, a CHUNK-RELATIVE position — so the candidate
    //      test isn't purely content-determined after all once two scans
    //      of the same bytes are at different chunk-relative offsets
    //      (e.g. after a multi-byte insertion shifts things). This is the
    //      residual chaining that can still cascade; it's second-order on
    //      random data but not eliminated by the decay argument above.
    //   2. max_size truncation (below) breaks the argument outright — see
    //      that comment for why.
    //
    // Contrast: a pure sliding-window CDC (no reset, no min_size, no
    // position-dependent mask — Rabin-fingerprint schemes like LBFS)
    // makes EVERY tested position content-determined by construction, so
    // resync after one window is a proven guarantee. FastCDC accepts a
    // (usually-but-not-provably tight) probabilistic resync in exchange
    // for the reset, which is what makes normalized chunking and a cheap
    // min_size floor possible, and for never needing to carry hash state
    // across a chunk boundary in the streaming implementation.
    while (start < len) {
        const size_t remaining = len - start;

        // Not enough bytes left to even reach min_size again after this
        // one — take the rest as the final chunk. Avoids producing a
        // tiny trailing sliver on the next iteration.
        if (remaining <= config_.min_size) {
            chunks.push_back({start, remaining, /*content_defined=*/false});
            break;
        }

        const size_t window = std::min(remaining, config_.max_size);
        uint64_t hash = 0;
        size_t cut = window;  // fallback: hit max_size with no boundary found
        bool content_defined = false;

        // Bytes [0, min_size) are never hashed or tested: FastCDC cannot
        // cut before min_size regardless of what the hash says, so there
        // is nothing to gain by evaluating it there. This also keeps the
        // gear hash "fresh" — it reflects only the window that could
        // actually produce a cut.
        for (size_t i = config_.min_size; i < window; ++i) {
            hash = (hash << 1) + gear[data[start + i]];
            const uint64_t mask = (i < config_.avg_size) ? mask_s_ : mask_l_;
            if ((hash & mask) == 0) {
                cut = i + 1;  // boundary is *after* this byte
                content_defined = true;
                break;
            }
        }

        // KNOWN LIMITATION — max_size truncation breaks the resync
        // argument above outright. When no candidate cut point is found
        // before max_size, `cut` falls back to `window`: a boundary
        // placed purely by BYTE COUNT, with zero dependence on content.
        // The whole "same content -> same candidate -> resync" argument
        // relies on every boundary being a content-derived candidate;
        // a positional truncation is not one, so the chunk immediately
        // after a truncation starts at an offset the content-only
        // candidate model says nothing about, and can require another
        // full resync from scratch.
        //
        // This is rare on dense-candidate data (uniform random bytes:
        // stage-1 testing topped out at ~21 KB against a 64 KB ceiling,
        // i.e. never triggered) but becomes likely on data with long
        // low-entropy runs — zero-filled regions, repeated boilerplate,
        // sparse binary padding — where candidates thin out or vanish
        // for stretches longer than max_size. Real source trees (the
        // kernel benchmark this project targets) have exactly that kind
        // of content, so the resync distribution measured on synthetic
        // random data should NOT be assumed to hold there without
        // re-measuring (see tools/chunk_identity.cpp, which reports the
        // truncation ratio precisely so this can be checked rather than
        // assumed).

        chunks.push_back({start, cut, content_defined});
        start += cut;
    }

    return chunks;
}

} // namespace dedupbackup
