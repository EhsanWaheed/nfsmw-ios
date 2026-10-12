/* Test67: USA Xbox 360 scenery auxiliary lights on the native draw path - data and draw identity.
 *
 * USA (retail 360): the scenery-instance renderer 824c34a8 reads LightingContextNumber (instance
 * +0x1e), fetches the context from the table the 0x80034115 loader fills and passes it to
 * AddRenderEntry (entry +0x10); per draw, 82452f40 uploads up to four lights of a type-1 context to the
 * WORLD vertex program b14e04aa (Lights c5-c20, ActiveAuxiliaryLight0..3 c21-c24).
 * PC 1.3 (runs natively here): the same loader exists but no PC track ships the chunk, no instance
 * carries a context, and the draw-item consumer 00724080 always passes a NULL light context to
 * 006da9b0 (push 0 at 007241f1). This file is the first missing boundary: the private
 * x360-scenery-lights.bin (built by verification/test67-scenery-light-instance/build_scenery_lights.py
 * from the owner's USA ISO) maps each PC instance whose identity was verified against a USA
 * instance with a context - same section, SceneryInfo name and byte-identical geometry, geometry
 * unique among all PC instances - to that context's lights (up to the eight the USA uploader takes). At run time the instance the
 * PC draw path hands over is identified by the same 54 geometry bytes, and the render entries 006da9b0
 * adds for it are tagged. The context is kept on the native side; PC code never sees a type-1 context
 * (its own light-context consumer 006c7420 expects the PC types).
 * The pure part below (no guest access) is shared with native/tests/scenery_lights_tests.cpp. */
#ifndef X360_SCENERY_LIGHTS_H
#define X360_SCENERY_LIGHTS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define X3SL_KEY_BYTES 54u
#define X3SL_MAX_ENTRIES 4096u /* PC render-entry array 0093e878, 0x44 bytes, max 0x1000 */

typedef struct {
    uint32_t count;      /* lights in the USA context (1-12; WORLD VS uses 4, the normal-map/reflect PS 6) */
    int32_t number;      /* USA LightingContextNumber */
    float rec[8][16];    /* raw 64-byte USA light records: the first eight (82452f40 clamps to eight) */
} X3slSet;

typedef struct {
    uint8_t key[X3SL_KEY_BYTES];
    uint32_t set;        /* index into sets; UINT32_MAX = empty slot */
} X3slSlot;

typedef struct {
    X3slSet *sets;
    uint32_t nsets;
    X3slSlot *table;
    uint32_t cap;        /* power of two */
    uint32_t nkeys;
    /* render-entry tags: set+1 (0 none), validated against the entry's part pointer at draw time */
    uint32_t tag_set[X3SL_MAX_ENTRIES];
    uint32_t tag_part[X3SL_MAX_ENTRIES];
    uint32_t last_after;
} X3sl;

static uint64_t x3sl_hash(const uint8_t *k) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < X3SL_KEY_BYTES; ++i)
        h = (h ^ k[i]) * 1099511628211ull;
    return h;
}

/* instance record (0x40 bytes) -> the 54 geometry bytes: +0x00..+0x17 (bbox) and +0x20..+0x3d
 * (position, Q13 rotation); flags, preculler, context number and info number are excluded. */
static void x3sl_key_from_instance(const uint8_t *inst, uint8_t *key) {
    memcpy(key, inst, 0x18);
    memcpy(key + 0x18, inst + 0x20, 0x1e);
}

static void x3sl_free(X3sl *s) {
    free(s->sets);
    free(s->table);
    memset(s, 0, sizeof *s);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Returns 1 and fills s from a complete, consistent blob; 0 (s left empty) otherwise. */
static int x3sl_parse(X3sl *s, const uint8_t *b, size_t n) {
    memset(s, 0, sizeof *s);
    if (n < 16 || memcmp(b, "X3SL", 4) != 0 || rd32(b + 4) != 2)
        return 0;
    const uint32_t nsets = rd32(b + 8), nkeys = rd32(b + 12);
    const size_t set_bytes = 8 + 8 * 64, key_bytes = X3SL_KEY_BYTES + 2 + 4;
    if (nsets == 0 || nsets > 65536 || nkeys == 0 || nkeys > 262144 ||
        n != 16 + (size_t)nsets * set_bytes + (size_t)nkeys * key_bytes)
        return 0;
    uint32_t cap = 1;
    while (cap < nkeys * 2) cap <<= 1;
    s->sets = (X3slSet *)calloc(nsets, sizeof *s->sets);
    s->table = (X3slSlot *)malloc(cap * sizeof *s->table);
    if (!s->sets || !s->table) { x3sl_free(s); return 0; }
    for (uint32_t i = 0; i < cap; ++i) s->table[i].set = UINT32_MAX;
    s->cap = cap;
    s->nsets = nsets;
    const uint8_t *p = b + 16;
    for (uint32_t i = 0; i < nsets; ++i, p += set_bytes) {
        s->sets[i].count = rd32(p);
        memcpy(&s->sets[i].number, p + 4, 4);
        memcpy(s->sets[i].rec, p + 8, 8 * 64);
        if (s->sets[i].count == 0) { x3sl_free(s); return 0; }
    }
    for (uint32_t i = 0; i < nkeys; ++i, p += key_bytes) {
        const uint32_t set = rd32(p + X3SL_KEY_BYTES + 2);
        if (set >= nsets) { x3sl_free(s); return 0; }
        uint32_t h = (uint32_t)x3sl_hash(p) & (cap - 1);
        while (s->table[h].set != UINT32_MAX) {
            if (!memcmp(s->table[h].key, p, X3SL_KEY_BYTES)) { x3sl_free(s); return 0; } /* duplicate key */
            h = (h + 1) & (cap - 1);
        }
        memcpy(s->table[h].key, p, X3SL_KEY_BYTES);
        s->table[h].set = set;
        ++s->nkeys;
    }
    return 1;
}

/* set index for an instance's geometry key, or UINT32_MAX */
static uint32_t x3sl_lookup(const X3sl *s, const uint8_t *key) {
    if (!s->cap) return UINT32_MAX;
    uint32_t h = (uint32_t)x3sl_hash(key) & (s->cap - 1);
    while (s->table[h].set != UINT32_MAX) {
        if (!memcmp(s->table[h].key, key, X3SL_KEY_BYTES)) return s->table[h].set;
        h = (h + 1) & (s->cap - 1);
    }
    return UINT32_MAX;
}

/* Around one 006da9b0 call: render entries [before, after) were added for an instance with set
 * `set` (UINT32_MAX: none). A count below the last seen one means the list was reset: all tags go. */
static void x3sl_tag(X3sl *s, uint32_t before, uint32_t after, uint32_t set,
                     const uint32_t *parts /* part pointer of each new entry, after-before of them */) {
    if (before < s->last_after) {
        memset(s->tag_set, 0, sizeof s->tag_set);
        memset(s->tag_part, 0, sizeof s->tag_part);
    }
    s->last_after = after;
    if (after > X3SL_MAX_ENTRIES) after = X3SL_MAX_ENTRIES;
    for (uint32_t i = before; i < after; ++i) {
        s->tag_set[i] = set == UINT32_MAX ? 0 : set + 1;
        s->tag_part[i] = set == UINT32_MAX ? 0 : parts[i - before];
    }
}

/* At draw: the set for render entry `index` whose part pointer is `part`, or UINT32_MAX. */
static uint32_t x3sl_entry_set(const X3sl *s, uint32_t index, uint32_t part) {
    if (index >= X3SL_MAX_ENTRIES || !s->tag_set[index] || s->tag_part[index] != part)
        return UINT32_MAX;
    return s->tag_set[index] - 1;
}

/* The pass registers of the auxiliary-light pass (Test67 aux_pass.py): for light i < 6, [2i] = position
 * xyz + active 1, [2i+1] = colour (row 1) x intensity of LIGHT 0 x 3 (82452f40 reads +0x1c from the
 * first record for every light) + the light's own intensity; unused lights are zero with w = 1 in the
 * second register (a finite attenuation denominator; their contribution is multiplied by active 0). */
static void x3sl_pass_registers(const X3slSet *set, float out[12][4]) {
    memset(out, 0, 12 * 4 * sizeof(float));
    const uint32_t n = set->count < 6 ? set->count : 6;
    const float i0 = set->rec[0][7];
    for (uint32_t i = 0; i < 6; ++i) {
        out[2 * i + 1][3] = 1.0f;
        if (i >= n) continue;
        const float *r = set->rec[i];
        out[2 * i][0] = r[0]; out[2 * i][1] = r[1]; out[2 * i][2] = r[2]; out[2 * i][3] = 1.0f;
        out[2 * i + 1][0] = r[4] * i0 * 3.0f; out[2 * i + 1][1] = r[5] * i0 * 3.0f;
        out[2 * i + 1][2] = r[6] * i0 * 3.0f; out[2 * i + 1][3] = r[7];
    }
}

/* The four WORLD registers of one light exactly as 82452f40 + b14e04aa see them (column-major
 * float4x4 of the uploaded record): before upload the record's row 2 becomes colour (row 1 xyz) x
 * intensity (row 1 w) x 3.0 and row 2 w = intensity. Returns the 4x4 registers c[k].xyzw. */
static inline void x3sl_light_registers(const float rec[16], float c[4][4]) {
    float r[16];
    memcpy(r, rec, sizeof r);
    r[8] = rec[4] * rec[7] * 3.0f;
    r[9] = rec[5] * rec[7] * 3.0f;
    r[10] = rec[6] * rec[7] * 3.0f;
    r[11] = rec[7];
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 4; ++j)
            c[k][j] = r[j * 4 + k];
}
#endif
