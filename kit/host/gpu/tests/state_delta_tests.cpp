// Remaster A4-lite step 1: property test of the state-table delta encoder/decoder (host/gpu/state_delta.h) against a
// model. Random sequences over several pipelines: render and sampler state writes, draws (normal, rejected with zero
// tables, volume-LOD local copies with no masks and version 0), batch splits, and per-draw flips between delta and
// legacy mode (the A/B switch). Every draw's decoded tables must equal the tables it was queued with.
#include "../state_delta.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Pipe {
    sdelta::Tables t{};
    uint64_t dirty[sdelta::kDirtyWords] = {};
    uint64_t version = 0;
};
struct Queued {
    sdelta::Kind kind;
    size_t at;
    sdelta::Tables expect;
};

static uint64_t g_next = 0;

int main() {
    std::mt19937 rng(12345);
    uint64_t draws = 0, deltas = 0, fulls = 0, reuses = 0, legacies = 0, batches = 0;
    for (int seq = 0; seq < 400; ++seq) {
        Pipe pipes[3];
        std::vector<uint8_t> bytes;
        std::vector<Queued> q;
        sdelta::Encoder enc;
        auto put = [&](const void *p, size_t n) {
            size_t at = bytes.size();
            bytes.insert(bytes.end(), (const uint8_t *)p, (const uint8_t *)p + n);
            return at;
        };
        auto flush = [&] {
            sdelta::Tables scratch{};
            for (size_t i = 0; i < q.size(); ++i) {
                const uint8_t *got = sdelta::decode(scratch, q[i].kind, bytes.data(), q[i].at);
                if (memcmp(got, &q[i].expect, sdelta::kBytes) != 0) {
                    fprintf(stderr, "FAIL seq %d draw %zu kind %d\n", seq, i, (int)q[i].kind);
                    std::exit(1);
                }
            }
            q.clear();
            bytes.clear();
            enc.reset();
            ++batches;
        };
        const int ops = 200 + (int)(rng() % 400);
        for (int op = 0; op < ops; ++op) {
            const uint32_t r = rng() % 100;
            Pipe &p = pipes[rng() % (rng() % 4 == 0 ? 3 : 1)]; // mostly one device, sometimes interleaved
            if (r < 40) { // render state write (value and set flag)
                const uint32_t i = rng() % 256, v = rng() % 4;
                if (p.t.rs[i] != v || !p.t.set[i]) {
                    p.t.rs[i] = v;
                    p.t.set[i] = 1;
                    sdelta::mark_render(p.dirty, i);
                    p.version = ++g_next;
                }
            } else if (r < 60) { // sampler state write
                const uint32_t s = rng() % 16, ty = rng() % 14, v = rng() % 4;
                if (p.t.ss[s * 14 + ty] != v) {
                    p.t.ss[s * 14 + ty] = v;
                    sdelta::mark_sampler(p.dirty, s, ty);
                    p.version = ++g_next;
                }
            } else if (r < 95) { // draw
                const uint32_t pid = (uint32_t)(&p - pipes) + 1;
                const bool use_delta = rng() % 5 != 0; // A/B flips
                const uint32_t kind = rng() % 20;
                size_t at = 0;
                Queued e{};
                if (kind == 0) { // rejected: zero tables, version 0, no masks
                    e.kind = sdelta::encode(enc, put, nullptr, nullptr, nullptr, 0, nullptr, pid, false, &at);
                    e.expect = sdelta::Tables{};
                } else if (kind == 1) { // volume LOD: a local copy, version 0, no masks
                    sdelta::Tables local = p.t;
                    local.ss[(rng() % 16) * 14 + 9] = 7;
                    e.kind = sdelta::encode(enc, put, local.ss, local.rs, local.set, 0, nullptr, pid, use_delta, &at);
                    e.expect = local;
                } else {
                    e.kind = sdelta::encode(enc, put, p.t.ss, p.t.rs, p.t.set, p.version, p.dirty, pid, use_delta,
                                            &at);
                    e.expect = p.t;
                }
                e.at = at;
                q.push_back(e);
                ++draws;
                deltas += e.kind == sdelta::DELTA;
                fulls += e.kind == sdelta::FULL;
                reuses += e.kind == sdelta::REUSE;
                legacies += e.kind == sdelta::LEGACY;
            } else { // batch split (48 commands or 1 MB in the real queue)
                flush();
            }
        }
        flush();
    }
    if (!deltas || !fulls || !reuses || !legacies) {
        fprintf(stderr, "FAIL: a kind was never exercised (delta %llu full %llu reuse %llu legacy %llu)\n",
                (unsigned long long)deltas, (unsigned long long)fulls, (unsigned long long)reuses,
                (unsigned long long)legacies);
        return 1;
    }
    printf("state_delta_tests: %llu draws in %llu batches (delta %llu, full %llu, reuse %llu, legacy %llu): ok\n",
           (unsigned long long)draws, (unsigned long long)batches, (unsigned long long)deltas,
           (unsigned long long)fulls, (unsigned long long)reuses, (unsigned long long)legacies);
    return 0;
}
