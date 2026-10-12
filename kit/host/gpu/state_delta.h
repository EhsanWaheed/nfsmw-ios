// Remaster A4-lite step 1 (design review/2026-10-06-a4lite-step1-design.md, challenge C-A4L1): the render queue sends
// a draw's state tables (sampler 16x14 u32, render state 256 u32, render-state-set 256 B; 2,176 bytes) as a delta of
// the entries changed since the batch's previous put, instead of a full copy each time the state version moves.
// The encoder runs on the game thread per batch; the decoder on the render thread per executed batch, in command
// order. Every batch starts with a full put, so nothing carries across batches. Legacy mode is today's behaviour
// exactly (a full put when the version moves; reuse by offset), for the A/B switch's OLD side, rejected draws and
// tables that are not the pipeline's own (the volume-LOD copy). Header-only so host/gpu/tests can test it alone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sdelta {
constexpr size_t kSampler = 16 * 14, kRender = 256;
constexpr size_t kBytes = kSampler * 4 + kRender * 4 + kRender; // 2,176, the batch layout: ss | rs | set
constexpr uint32_t kMaxDelta = 96;                                // more changed entries: a full put is smaller
constexpr uint32_t kDirtyWords = 8;                               // bits 0-223 sampler [s*14+t], 224-479 render state

struct Tables {
    uint32_t ss[kSampler];
    uint32_t rs[kRender];
    uint8_t set[kRender];
};
static_assert(sizeof(Tables) == kBytes, "Tables must match the batch layout");

enum Kind : uint8_t { LEGACY = 0, FULL = 1, DELTA = 2, REUSE = 3 };

inline void mark_render(uint64_t *dirty, uint32_t i) {
    const uint32_t b = (uint32_t)kSampler + i;
    dirty[b >> 6] |= 1ull << (b & 63);
}
inline void mark_sampler(uint64_t *dirty, uint32_t stage, uint32_t type) {
    const uint32_t b = stage * 14 + type;
    dirty[b >> 6] |= 1ull << (b & 63);
}

// Per batch, on the game thread. Reset with the batch.
struct Encoder {
    bool valid = false;      // the decoder's scratch holds this pipeline's real tables (deltas may follow)
    bool last_delta = false; // the last put was a delta (state_at is then not a full table)
    uint64_t version = 0;
    uint32_t pipeline = 0;
    size_t state_at = 0; // offset of the last full table (legacy reuse) or of the last put
    void reset() {
        *this = Encoder{};
    }
};

// put(p, n) appends n bytes and returns their offset. ss/rs/set null: zero tables (rejected draws). dirty null: the
// tables are not the pipeline's own. Returns the kind; *at is the offset the decoder reads.
template <class Put>
Kind encode(Encoder &e, Put &&put, const uint32_t *ss, const uint32_t *rs, const uint8_t *set, uint64_t version,
            uint64_t *dirty, uint32_t pipeline, bool use_delta, size_t *at) {
    static const uint32_t zero_ss[kSampler] = {}, zero_rs[kRender] = {};
    static const uint8_t zero_set[kRender] = {};
    auto put_full = [&] {
        size_t o = put(ss ? ss : zero_ss, kSampler * 4);
        put(rs ? rs : zero_rs, kRender * 4);
        put(set ? set : zero_set, kRender);
        return o;
    };
    const bool real = ss && rs && set;
    if (!use_delta) { // today's behaviour, exactly
        if (!version || version != e.version || e.last_delta) {
            e.state_at = put_full();
            e.version = version;
            e.last_delta = false;
        }
        e.valid = false; // the decoder's scratch is not maintained in legacy mode
        if (dirty)
            memset(dirty, 0, kDirtyWords * 8);
        *at = e.state_at;
        return LEGACY;
    }
    if (version && version == e.version && e.valid && e.pipeline == pipeline && dirty) {
        *at = e.state_at;
        return REUSE;
    }
    if (real && dirty && e.valid && e.pipeline == pipeline) {
        uint32_t n = 0;
        for (uint32_t w = 0; w < kDirtyWords; ++w)
            n += (uint32_t)__builtin_popcountll(dirty[w]);
        if (n <= kMaxDelta) {
            uint32_t rec[1 + 2 * kMaxDelta];
            uint32_t k = 0;
            rec[k++] = n;
            for (uint32_t w = 0; w < kDirtyWords; ++w)
                for (uint64_t m = dirty[w]; m; m &= m - 1) {
                    const uint32_t b = w * 64 + (uint32_t)__builtin_ctzll(m);
                    if (b < kSampler) {
                        rec[k++] = b;
                        rec[k++] = ss[b];
                    } else {
                        const uint32_t i = b - (uint32_t)kSampler;
                        rec[k++] = b | (set[i] ? 0x10000u : 0u);
                        rec[k++] = rs[i];
                    }
                }
            e.state_at = put(rec, k * 4);
            e.version = version;
            e.last_delta = true;
            memset(dirty, 0, kDirtyWords * 8);
            *at = e.state_at;
            return DELTA;
        }
    }
    e.state_at = put_full();
    e.version = version;
    e.pipeline = pipeline;
    e.valid = real && dirty;
    e.last_delta = false;
    if (dirty)
        memset(dirty, 0, kDirtyWords * 8);
    *at = e.state_at;
    return FULL;
}

// Per executed batch, on the render thread, in command order. Returns the tables the draw uses.
inline const uint8_t *decode(Tables &scratch, Kind k, const uint8_t *base, size_t at) {
    switch (k) {
    case FULL:
        memcpy(&scratch, base + at, kBytes);
        return (const uint8_t *)&scratch;
    case DELTA: {
        uint32_t n;
        memcpy(&n, base + at, 4);
        const uint8_t *p = base + at + 4;
        for (uint32_t i = 0; i < n; ++i, p += 8) {
            uint32_t idx, v;
            memcpy(&idx, p, 4);
            memcpy(&v, p + 4, 4);
            const uint32_t b = idx & 0xffffu;
            if (b < kSampler) {
                scratch.ss[b] = v;
            } else {
                scratch.rs[b - kSampler] = v;
                scratch.set[b - kSampler] = (idx >> 16) & 1u;
            }
        }
        return (const uint8_t *)&scratch;
    }
    case REUSE:
        return (const uint8_t *)&scratch;
    case LEGACY:
    default:
        return base + at;
    }
}
} // namespace sdelta
