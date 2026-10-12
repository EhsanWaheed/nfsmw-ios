// Test67: the pure part of mods/core/nfsmw/x360_scenery_lights.h (blob parsing, geometry keys, render
// entry tags, register layout). No game data: a synthetic blob. Optional: pass the private
// x360-scenery-lights.bin path to also check the real blob (never run by CTest).
#include "../../mods/core/nfsmw/x360_scenery_lights.h"
#include "../../mods/core/nfsmw/x360_light_pools.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put32(std::vector<uint8_t> &b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static std::vector<uint8_t> key_of(uint8_t seed) {
    std::vector<uint8_t> inst(0x40);
    for (int i = 0; i < 0x40; ++i) inst[i] = (uint8_t)(seed * 7 + i);
    std::vector<uint8_t> k(X3SL_KEY_BYTES);
    x3sl_key_from_instance(inst.data(), k.data());
    return k;
}
static std::vector<uint8_t> blob(int nsets, const std::vector<std::pair<uint8_t, uint32_t>> &keys) {
    std::vector<uint8_t> b = {'X', '3', 'S', 'L'};
    put32(b, 2); put32(b, nsets); put32(b, (uint32_t)keys.size());
    for (int s = 0; s < nsets; ++s) {
        put32(b, 1 + s); put32(b, 100 + s);
        for (int i = 0; i < 128; ++i) { float f = (float)(s * 64 + i); uint32_t u; memcpy(&u, &f, 4); put32(b, u); }
    }
    for (auto &k : keys) {
        auto g = key_of(k.first); b.insert(b.end(), g.begin(), g.end()); b.push_back(0); b.push_back(0); put32(b, k.second);
    }
    return b;
}

// Test76: the light-pool blob and the AuxiliaryGroups block, checked against a reference of the
// WORLD auxiliary vertex program (aux_world.py vs_text: group choice, a0 offset, four USA lights).
static void putf(std::vector<uint8_t> &b, float f) { uint32_t u; memcpy(&u, &f, 4); put32(b, u); }
static void vs_ref(const float blk[X3LR_BLOCK][4], const float P[3], const float N[3], float out[3]) {
    float best = 5e5f, base = 0.f;
    for (unsigned g = 0; g < X3LR_GROUPS; ++g) {
        const float (*r)[4] = blk + g * X3LR_REGS;
        float in = 1.f;
        for (int k = 0; k < 3; ++k) in *= (P[k] >= r[0][k] ? 1.f : 0.f) * (r[1][k] >= P[k] ? 1.f : 0.f);
        float d = std::fmax(std::fabs((P[0] - r[2][0]) * r[2][2]), std::fabs((P[1] - r[2][1]) * r[2][3]));
        d += (1.f - in) * 1e6f;
        const float lt = d < best ? 1.f : 0.f;
        best = std::fmin(best, d);
        base += lt * (r[0][3] - base);
    }
    const int a0 = (int)std::lround(base);
    out[0] = out[1] = out[2] = 0.f;
    if (!(best < 2.5e5f)) return;
    const float tint[3] = {1.f, 0.65f, 0.15f};
    for (int i = 0; i < 4; ++i) {
        const float *lp = blk[a0 + 2 * i], *lc = blk[a0 + 2 * i + 1];
        float L[3] = {lp[0] - P[0], lp[1] - P[1], lp[2] - P[2]};
        float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
        float inv = 1.f / std::sqrt(std::fmax(d2, 1e-12f));
        float nl = std::fmin(1.f, std::fmax(0.f, (N[0] * L[0] + N[1] * L[1] + N[2] * L[2]) * inv));
        float w = nl / (2.f * d2 + lc[3]) * lp[3];
        for (int k = 0; k < 3; ++k) out[k] += lc[k] * tint[k] * w;
    }
}
static void group_regs(std::vector<uint8_t> &b, int lc, float x0, float x1, float lightx, float colour) {
    put32(b, (uint32_t)lc); put32(b, 4);
    float r[X3LR_REGS][4] = {};
    r[0][0] = x0; r[0][1] = 0; r[0][2] = 0; r[1][0] = x1; r[1][1] = 10; r[1][2] = 10;
    r[2][0] = (x0 + x1) / 2; r[2][1] = 5; r[2][2] = 2.f / (x1 - x0); r[2][3] = 0.2f;
    for (int i = 0; i < 4; ++i) r[4 + 2 * i][3] = 1.f;
    r[3][0] = lightx; r[3][1] = 5; r[3][2] = 3; r[3][3] = 1;      // light 0 active
    r[4][0] = colour; r[4][1] = colour; r[4][2] = colour; r[4][3] = 2;
    for (auto &q : r) for (float v : q) putf(b, v);
}
static void light_pool_tests() {
    // two receivers: A has groups 0 (x 0..10) and 1 (x 8..20), B has group 2 (x 100..110)
    std::vector<uint8_t> b = {'X', '3', 'L', 'R'};
    put32(b, 1); put32(b, 2); put32(b, 3);
    auto k1 = key_of(1), k2 = key_of(2);
    b.insert(b.end(), k1.begin(), k1.end()); b.push_back(0); b.push_back(0); put32(b, 0); put32(b, 2); put32(b, 704);
    b.insert(b.end(), k2.begin(), k2.end()); b.push_back(0); b.push_back(0); put32(b, 2); put32(b, 1); put32(b, 709);
    group_regs(b, 29, 0, 10, 5, 1.f); group_regs(b, 31, 8, 20, 14, 2.f); group_regs(b, 40, 100, 110, 105, 3.f);
    static X3lr s;
    CHECK(x3lr_parse(&s, b.data(), b.size()));
    CHECK(s.nrecv == 2 && s.ngroups == 3 && s.recv[1].section == 709 && s.groups[1].lc == 31);
    CHECK(x3lr_lookup(&s, k1.data()) == 0 && x3lr_lookup(&s, k2.data()) == 1 && x3lr_lookup(&s, key_of(3).data()) == UINT32_MAX);
    static float blk[X3LR_BLOCK][4];
    x3lr_block_receiver(&s, 0, blk);
    CHECK(blk[0][3] == 3.f && blk[X3LR_REGS][3] == (float)(X3LR_REGS + 3));     // a0 offsets of groups 0, 1
    CHECK(blk[2 * X3LR_REGS][0] > 1e29f && blk[2 * X3LR_REGS + 1][0] < -1e29f);   // group 2..15 empty
    const float N[3] = {0, 0, 1};
    float o[3], P[3] = {2, 5, 0};
    vs_ref(blk, P, N, o);                       // only in group 0: its light (colour 1)
    const float d2 = 9 + 9, want = 1.f / (2 * d2 + 2) * (3 / std::sqrt(d2));
    CHECK(std::fabs(o[0] - want) < 1e-6f && std::fabs(o[1] - 0.65f * want) < 1e-6f);
    {   // the tie-break: normalised distance |9.5-5|*0.2 = 0.9 (group 0) vs |9.5-14|/6 = 0.75 (group 1)
        float R[3] = {9.5f, 5, 0};
        x3lr_block_receiver(&s, 0, blk); vs_ref(blk, R, N, o);
        float Lx = 14 - 9.5f, dd = Lx * Lx + 9, w = 1.f / (2 * dd + 2) * (3 / std::sqrt(dd)) * 2.f;
        CHECK(std::fabs(o[0] - w) < 1e-6f);
    }
    P[0] = 30; vs_ref(blk, P, N, o);            // in no box: no light
    CHECK(o[0] == 0.f && o[1] == 0.f && o[2] == 0.f);
    x3lr_block_receiver(&s, 1, blk);
    P[0] = 104; vs_ref(blk, P, N, o);
    CHECK(o[0] > 0.f && blk[0][3] == 3.f);
    // a verified instance: one box that holds everything, its first four lights
    X3slSet set{};
    set.count = 2;
    set.rec[0][0] = 50; set.rec[0][1] = 5; set.rec[0][2] = 3; set.rec[0][4] = 1; set.rec[0][7] = 2;
    set.rec[1][0] = 60; set.rec[1][1] = 5; set.rec[1][2] = 3; set.rec[1][4] = 1; set.rec[1][7] = 2;
    x3lr_block_set(&set, blk);
    CHECK(blk[3][0] == 50 && blk[3][3] == 1 && blk[5][0] == 60 && blk[7][3] == 0 && blk[8][3] == 1);
    P[0] = -1e6f; vs_ref(blk, P, N, o); CHECK(o[0] >= 0.f);
    P[0] = 55; vs_ref(blk, P, N, o); CHECK(o[0] > 0.f);
    // tags as x3sl, with the verified-instance form 0x80000000 | set
    uint32_t parts[2] = {0x10, 0x20};
    x3lr_tag(&s, 5, 7, 0x80000003u, parts);
    CHECK(x3lr_entry_recv(&s, 6, 0x20) == 0x80000003u && x3lr_entry_recv(&s, 6, 0x10) == UINT32_MAX);
    x3lr_tag(&s, 0, 1, UINT32_MAX, parts);
    CHECK(x3lr_entry_recv(&s, 6, 0x20) == UINT32_MAX);
    {   // placed models: world -> model space (rotation about z by 90 degrees, translation 10/20/30)
        const float m[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};
        float inv[12], p[3] = {10, 21, 30};       // world point = model (1, 0, 0) * A + T
        CHECK(x3lr_inverse_affine(m, inv));
        x3lr_to_local(inv, p);
        CHECK(std::fabs(p[0] - 1) < 1e-5f && std::fabs(p[1]) < 1e-5f && std::fabs(p[2]) < 1e-5f);
        const float singular[16] = {0};
        CHECK(!x3lr_inverse_affine(singular, inv));
        const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        CHECK(x3lr_is_identity(id) && !x3lr_is_identity(m));
    }
    x3lr_free(&s);
    // rejects: version, size, group run outside the table, more than 16 groups, non-finite register
    auto bad = b; bad[4] = 2; CHECK(!x3lr_parse(&s, bad.data(), bad.size()));
    CHECK(!x3lr_parse(&s, b.data(), b.size() - 4));
    bad = b; bad[16 + 68 + X3SL_KEY_BYTES + 2] = 3; CHECK(!x3lr_parse(&s, bad.data(), bad.size()));   // B's run 3..3 past 3 groups
    bad = b; bad[16 + X3SL_KEY_BYTES + 6] = 17; CHECK(!x3lr_parse(&s, bad.data(), bad.size()));
    bad = b; { float nan = NAN; memcpy(bad.data() + 16 + 2 * 68 + 8, &nan, 4); } CHECK(!x3lr_parse(&s, bad.data(), bad.size()));
    CHECK(s.cap == 0);
}

int main(int argc, char **argv) {
    static X3sl s;
    auto b = blob(2, {{1, 0}, {2, 1}, {3, 1}});
    CHECK(x3sl_parse(&s, b.data(), b.size()));
    CHECK(s.nkeys == 3 && s.nsets == 2 && s.sets[1].number == 101 && s.sets[1].count == 2);
    CHECK(x3sl_lookup(&s, key_of(1).data()) == 0);
    CHECK(x3sl_lookup(&s, key_of(3).data()) == 1);
    CHECK(x3sl_lookup(&s, key_of(9).data()) == UINT32_MAX);
    // the key ignores flags (0x18..0x1b), preculler/context (0x1c..0x1f) and the info number (0x3e)
    {
        std::vector<uint8_t> inst(0x40), k1(X3SL_KEY_BYTES), k2(X3SL_KEY_BYTES);
        for (int i = 0; i < 0x40; ++i) inst[i] = (uint8_t)(7 + i);
        x3sl_key_from_instance(inst.data(), k1.data());
        inst[0x18] ^= 0xff; inst[0x1e] = 0xff; inst[0x1f] = 0xff; inst[0x3e] ^= 1;
        x3sl_key_from_instance(inst.data(), k2.data());
        CHECK(k1 == k2);
        inst[0x20] ^= 1; x3sl_key_from_instance(inst.data(), k2.data());
        CHECK(k1 != k2);
    }
    // tags: set, validate by part pointer, reset on a smaller count
    {
        uint32_t parts[3] = {0x1000, 0x2000, 0x3000};
        x3sl_tag(&s, 10, 13, 1, parts);
        CHECK(x3sl_entry_set(&s, 11, 0x2000) == 1);
        CHECK(x3sl_entry_set(&s, 11, 0x2004) == UINT32_MAX);     // another part at that index
        CHECK(x3sl_entry_set(&s, 13, 0) == UINT32_MAX);
        x3sl_tag(&s, 13, 15, UINT32_MAX, parts);                  // untagged item after it
        CHECK(x3sl_entry_set(&s, 12, 0x3000) == 1);
        CHECK(x3sl_entry_set(&s, 13, 0x1000) == UINT32_MAX);
        x3sl_tag(&s, 2, 3, UINT32_MAX, parts);                    // new list: 2 < 15
        CHECK(x3sl_entry_set(&s, 11, 0x2000) == UINT32_MAX);
        x3sl_tag(&s, 4090, 5000, 0, parts);                       // clamp to the PC array
        CHECK(x3sl_entry_set(&s, 4095, 0) == UINT32_MAX || x3sl_entry_set(&s, 4095, s.tag_part[4095]) == 0);
    }
    x3sl_free(&s);
    // rejects: bad magic/version, size mismatch, set index out of range, duplicate key, empty set
    {
        auto bad = b; bad[0] = 'Y'; CHECK(!x3sl_parse(&s, bad.data(), bad.size()));
        bad = b; bad[4] = 1; CHECK(!x3sl_parse(&s, bad.data(), bad.size()));
        CHECK(!x3sl_parse(&s, b.data(), b.size() - 1));
        auto b2 = blob(2, {{1, 0}, {2, 5}}); CHECK(!x3sl_parse(&s, b2.data(), b2.size()));
        auto b3 = blob(2, {{1, 0}, {1, 1}}); CHECK(!x3sl_parse(&s, b3.data(), b3.size()));
        auto b4 = blob(1, {{1, 0}}); memset(b4.data() + 16, 0, 4); CHECK(!x3sl_parse(&s, b4.data(), b4.size()));
        CHECK(s.cap == 0 && x3sl_lookup(&s, key_of(1).data()) == UINT32_MAX);
    }
    // registers: column-major, row 2 = colour x intensity x 3, row 2 w = intensity (82452f40)
    {
        float rec[16] = {1, 2, 3, 15, 0.5f, 0.25f, 1, 15, 9, 9, 9, 9, 0, 0, 0, 0}, c[4][4];
        x3sl_light_registers(rec, c);
        CHECK(c[0][0] == 1 && c[1][0] == 2 && c[2][0] == 3);            // position in .x of c5..c7
        CHECK(std::fabs(c[0][2] - 22.5f) < 1e-5f && std::fabs(c[1][2] - 11.25f) < 1e-5f && std::fabs(c[2][2] - 45.f) < 1e-5f);
        CHECK(c[3][2] == 15);                                           // c8.z = intensity (attenuation term)
        CHECK(c[0][1] == 0.5f && c[3][1] == 15);                        // row 1 unchanged
    }
    // aux pass registers: USA uploader form (light-0 intensity for every colour; own intensity in w)
    {
        X3slSet set{};
        set.count = 7;                                   // more than the pass's six: first six used
        for (int i = 0; i < 8; ++i) {
            float *r = set.rec[i];
            r[0] = 1.f + i; r[1] = 2.f; r[2] = 3.f; r[3] = 15.f;
            r[4] = 0.5f; r[5] = 0.25f; r[6] = 1.f; r[7] = (i == 0) ? 15.f : 10.f;
        }
        float reg[12][4];
        x3sl_pass_registers(&set, reg);
        CHECK(reg[0][0] == 1.f && reg[0][3] == 1.f && reg[2][0] == 2.f && reg[10][0] == 6.f && reg[10][3] == 1.f);
        CHECK(std::fabs(reg[3][0] - 0.5f * 15.f * 3.f) < 1e-5f && reg[3][3] == 10.f);   // light 1: I0 colour, own I
        CHECK(std::fabs(reg[1][2] - 1.f * 15.f * 3.f) < 1e-5f && reg[1][3] == 15.f);
        X3slSet one{};
        one.count = 1; one.rec[0][7] = 15.f; one.rec[0][4] = 1.f;
        x3sl_pass_registers(&one, reg);
        CHECK(reg[2][3] == 0.f && reg[3][3] == 1.f && reg[3][0] == 0.f);             // unused: inactive, finite
    }
    light_pool_tests();
    if (argc > 2) {   // private light-pool blob (manual)
        FILE *f = std::fopen(argv[2], "rb");
        CHECK(f != nullptr);
        if (f) {
            std::vector<uint8_t> real; uint8_t buf[65536]; size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) real.insert(real.end(), buf, buf + n);
            std::fclose(f);
            static X3lr lr;
            CHECK(x3lr_parse(&lr, real.data(), real.size()));
            uint32_t ok = 0;
            for (uint32_t i = 0; i < lr.nrecv; ++i) ok += x3lr_lookup(&lr, lr.recv[i].key) == i;
            CHECK(ok == lr.nrecv);
            std::printf("real light-pool blob: %u receivers, %u groups, every key finds its receiver: %u\n", lr.nrecv, lr.ngroups, ok);
            x3lr_free(&lr);
        }
    }
    if (argc > 1) {   // private blob check (manual)
        FILE *f = std::fopen(argv[1], "rb");
        CHECK(f != nullptr);
        if (f) {
            std::vector<uint8_t> real;
            uint8_t buf[65536]; size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) real.insert(real.end(), buf, buf + n);
            std::fclose(f);
            CHECK(x3sl_parse(&s, real.data(), real.size()));
            std::printf("real blob: %u keys, %u sets\n", s.nkeys, s.nsets);
            const uint8_t *p = real.data() + 16 + (size_t)s.nsets * (8 + 512);
            uint32_t ok = 0, lights = 0;
            for (uint32_t i = 0; i < s.nkeys; ++i, p += X3SL_KEY_BYTES + 6) {
                uint32_t set; memcpy(&set, p + X3SL_KEY_BYTES + 2, 4);
                ok += x3sl_lookup(&s, p) == set;
            }
            for (uint32_t i = 0; i < s.nsets; ++i) lights += s.sets[i].count;
            CHECK(ok == s.nkeys);
            std::printf("every key finds its set: %u/%u; lights stored %u\n", ok, s.nkeys, lights);
            x3sl_free(&s);
        }
    }
    std::printf("scenery_lights_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
