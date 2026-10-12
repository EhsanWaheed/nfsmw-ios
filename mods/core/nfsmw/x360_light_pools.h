/* Test76: Xbox 360 light pools on PC terrain - the render-only partition data and the WORLD
 * auxiliary-light pass registers.
 *
 * USA (retail 360): lit terrain is chopped into *_AUX*_CHOP* pieces, one per light group; the WORLD
 * vertex program b14e04aa lights each piece's vertices with the first four lights of that piece's
 * context (Test67 README sec. 2). PC 1.3 ships the same ground as larger unchopped pieces without the
 * lights. The private x360-light-receivers.bin (verification/test76-light-pools/build_receivers.py, from
 * the owner's USA ISO and PC data) lists, per PC terrain piece ("receiver", keyed by the same 54 instance
 * geometry bytes as x360_scenery_lights.h), up to 16 GROUPS: one USA chop's box and four lights, already
 * in register form. The WORLD effect's added pass (x360-world.fx, aux_world.py) places each vertex in
 * the group whose box contains it and lights it the USA way; no box, no light.
 * The same pass serves Test67's verified instances drawn with the WORLD effect: one group whose box is
 * everything, with that instance's context lights.
 * The pure part below (no guest access) is shared with native/tests/scenery_lights_tests.cpp. */
#ifndef X360_LIGHT_POOLS_H
#define X360_LIGHT_POOLS_H
#include "x360_scenery_lights.h"
#include <math.h>

#define X3LR_GROUPS 16u
#define X3LR_REGS 11u
#define X3LR_BLOCK (X3LR_GROUPS * X3LR_REGS) /* float4 registers of AuxiliaryGroups */

typedef struct {
    int32_t lc;          /* USA context number (diagnostics) */
    uint32_t lights;     /* lights in the context, as stored */
    float regs[X3LR_REGS][4];
} X3lrGroup;

typedef struct {
    uint8_t key[X3SL_KEY_BYTES];
    uint32_t first, count;
    int32_t section;
} X3lrRecv;

typedef struct {
    X3lrRecv *recv;
    uint32_t nrecv;
    X3lrGroup *groups;
    uint32_t ngroups;
    uint32_t *table;     /* receiver index or UINT32_MAX; open addressing */
    uint32_t cap;
    uint32_t tag[X3SL_MAX_ENTRIES];       /* receiver + 1 (0 none) */
    uint32_t tag_part[X3SL_MAX_ENTRIES];
    uint32_t last_after;
} X3lr;

static void x3lr_free(X3lr *s) {
    free(s->recv);
    free(s->groups);
    free(s->table);
    memset(s, 0, sizeof *s);
}

/* Returns 1 and fills s from a complete, consistent blob; 0 (s left empty) otherwise. */
static int x3lr_parse(X3lr *s, const uint8_t *b, size_t n) {
    memset(s, 0, sizeof *s);
    if (n < 16 || memcmp(b, "X3LR", 4) != 0 || rd32(b + 4) != 1)
        return 0;
    const uint32_t nrecv = rd32(b + 8), ngroups = rd32(b + 12);
    const size_t recv_bytes = X3SL_KEY_BYTES + 2 + 12, group_bytes = 8 + X3LR_REGS * 16;
    if (nrecv == 0 || nrecv > 65536 || ngroups == 0 || ngroups > 1u << 20 ||
        n != 16 + (size_t)nrecv * recv_bytes + (size_t)ngroups * group_bytes)
        return 0;
    uint32_t cap = 1;
    while (cap < nrecv * 2) cap <<= 1;
    s->recv = (X3lrRecv *)calloc(nrecv, sizeof *s->recv);
    s->groups = (X3lrGroup *)calloc(ngroups, sizeof *s->groups);
    s->table = (uint32_t *)malloc(cap * sizeof *s->table);
    if (!s->recv || !s->groups || !s->table) { x3lr_free(s); return 0; }
    for (uint32_t i = 0; i < cap; ++i) s->table[i] = UINT32_MAX;
    s->cap = cap;
    const uint8_t *p = b + 16;
    for (uint32_t i = 0; i < nrecv; ++i, p += recv_bytes) {
        X3lrRecv *r = &s->recv[i];
        memcpy(r->key, p, X3SL_KEY_BYTES);
        r->first = rd32(p + X3SL_KEY_BYTES + 2);
        r->count = rd32(p + X3SL_KEY_BYTES + 6);
        memcpy(&r->section, p + X3SL_KEY_BYTES + 10, 4);
        if (r->count == 0 || r->count > X3LR_GROUPS || r->first > ngroups || ngroups - r->first < r->count) {
            x3lr_free(s);
            return 0;
        }
        uint32_t h = (uint32_t)x3sl_hash(r->key) & (cap - 1);
        while (s->table[h] != UINT32_MAX) {
            if (!memcmp(s->recv[s->table[h]].key, r->key, X3SL_KEY_BYTES)) { x3lr_free(s); return 0; }
            h = (h + 1) & (cap - 1);
        }
        s->table[h] = i;
        ++s->nrecv;
    }
    for (uint32_t i = 0; i < ngroups; ++i, p += group_bytes) {
        memcpy(&s->groups[i].lc, p, 4);
        s->groups[i].lights = rd32(p + 4);
        memcpy(s->groups[i].regs, p + 8, sizeof s->groups[i].regs);
        for (unsigned k = 0; k < X3LR_REGS * 4; ++k)
            if (!isfinite((&s->groups[i].regs[0][0])[k])) { x3lr_free(s); return 0; }
    }
    s->ngroups = ngroups;
    return 1;
}

static uint32_t x3lr_lookup(const X3lr *s, const uint8_t *key) {
    if (!s->cap) return UINT32_MAX;
    uint32_t h = (uint32_t)x3sl_hash(key) & (s->cap - 1);
    while (s->table[h] != UINT32_MAX) {
        if (!memcmp(s->recv[s->table[h]].key, key, X3SL_KEY_BYTES)) return s->table[h];
        h = (h + 1) & (s->cap - 1);
    }
    return UINT32_MAX;
}

/* As x3sl_tag: entries [before, after) belong to receiver `recv` (UINT32_MAX: none). */
static void x3lr_tag(X3lr *s, uint32_t before, uint32_t after, uint32_t recv, const uint32_t *parts) {
    if (before < s->last_after) {
        memset(s->tag, 0, sizeof s->tag);
        memset(s->tag_part, 0, sizeof s->tag_part);
    }
    s->last_after = after;
    if (after > X3SL_MAX_ENTRIES) after = X3SL_MAX_ENTRIES;
    for (uint32_t i = before; i < after; ++i) {
        s->tag[i] = recv == UINT32_MAX ? 0 : recv + 1;
        s->tag_part[i] = recv == UINT32_MAX ? 0 : parts[i - before];
    }
}

static uint32_t x3lr_entry_recv(const X3lr *s, uint32_t index, uint32_t part) {
    if (index >= X3SL_MAX_ENTRIES || !s->tag[index] || s->tag_part[index] != part)
        return UINT32_MAX;
    return s->tag[index] - 1;
}

/* An empty block: no group contains any vertex (min above max), lights inactive with a finite
 * attenuation denominator. Group g's [0].w is its register offset within the block (+3: its lights),
 * which the vertex program moves to a0. */
static void x3lr_block_empty(float out[X3LR_BLOCK][4]) {
    memset(out, 0, X3LR_BLOCK * 4 * sizeof(float));
    for (uint32_t g = 0; g < X3LR_GROUPS; ++g) {
        float (*r)[4] = out + g * X3LR_REGS;
        r[0][0] = r[0][1] = r[0][2] = 1e30f;
        r[1][0] = r[1][1] = r[1][2] = -1e30f;
        r[0][3] = (float)(g * X3LR_REGS + 3);
        for (unsigned i = 0; i < 4; ++i) r[4 + 2 * i][3] = 1.0f;
    }
}

/* The AuxiliaryGroups block for receiver `recv`. */
static void x3lr_block_receiver(const X3lr *s, uint32_t recv, float out[X3LR_BLOCK][4]) {
    x3lr_block_empty(out);
    if (recv >= s->nrecv) return;
    const X3lrRecv *r = &s->recv[recv];
    for (uint32_t g = 0; g < r->count; ++g) {
        memcpy(out + g * X3LR_REGS, s->groups[r->first + g].regs, sizeof s->groups[0].regs);
        out[g * X3LR_REGS][3] = (float)(g * X3LR_REGS + 3);
    }
}

/* The block for a Test67 verified instance: one group containing every vertex, its context's first four
 * lights in the uploader's form (x3sl_pass_registers' first eight registers). */
static void x3lr_block_set(const X3slSet *set, float out[X3LR_BLOCK][4]) {
    x3lr_block_empty(out);
    out[0][0] = out[0][1] = out[0][2] = -1e30f;
    out[1][0] = out[1][1] = out[1][2] = 1e30f;
    out[2][0] = out[2][1] = out[2][2] = out[2][3] = 0.0f; /* centre distance 0 */
    float lights[12][4];
    x3sl_pass_registers(set, lights);
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(out[3 + 2 * i], lights[2 * i], sizeof lights[0]);
        memcpy(out[4 + 2 * i], lights[2 * i + 1], sizeof lights[0]);
        if (i >= set->count) out[3 + 2 * i][3] = 0.0f;
    }
}
/* Placed models: the draw item's matrix M maps model to world, world = local * M (D3D row vectors: rows
 * 0-2 the basis A, row 3 the translation T), and the effects' vertex positions are model-local (their
 * WorldViewProj and LocalEyePos are per model). A light's world position goes to model space as
 * p * A^-1 - T * A^-1; out[0..8] = A^-1 (row-major), out[9..11] = -T * A^-1. Returns 0 when A is singular
 * or not finite. Distances are preserved when A is a rotation (the scenery placements: Q13 rotations). */
static int x3lr_inverse_affine(const float m[16], float out[12]) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(fabs(det) > 1e-12) || !isfinite(det)) return 0;
    const double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                           (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                           (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int k = 0; k < 9; ++k) out[k] = (float)inv[k];
    for (int col = 0; col < 3; ++col)
        out[9 + col] = (float)-(m[12] * inv[col] + m[13] * inv[3 + col] + m[14] * inv[6 + col]);
    return 1;
}
static void x3lr_to_local(const float inv[12], float p[3]) {
    const float x = p[0], y = p[1], z = p[2];
    for (int col = 0; col < 3; ++col)
        p[col] = x * inv[col] + y * inv[3 + col] + z * inv[6 + col] + inv[9 + col];
}
static int x3lr_is_identity(const float m[16]) {
    for (int k = 0; k < 16; ++k)
        if (fabsf(m[k] - ((k % 5) == 0 ? 1.0f : 0.0f)) > 1e-5f) return 0;
    return 1;
}
#endif
