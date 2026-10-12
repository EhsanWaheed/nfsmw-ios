/* vehicle_lights.h - run 13 (owner item 7 Oct 19:20; design review/run13/vehicle-lights-design.md, v1): headlight
 * beams and police light bars light the road and walls around the car, as moving lights in the clean pools and the
 * lamp glow. Setting vehicle_lights (off by default, follows Look). v1 has no shader or kit change: the car lights
 * only fill CleanPools slots after the lamps (world_light_pass.h) and join the glow's lamp list (clean_glow.h).
 *  - Capture: a BEFORE hook on the game's flare draw 00505380 at its one call in 00742950 (return 0074323a): main
 *    view 00919650, normal flare (p5 = 0), intensity > 0. Per car (frame-arena matrix): directional flares facing
 *    forward = headlights on; omni flares with a police hash = the bar, with the game's own pulse intensity per
 *    colour class (its flash rhythm, no clock of ours).
 *  - Latch (vehicle_lights_tick, once per frame from game_device_update): the last frame's cars, moved one frame on
 *    (matched to the frame before within 3 m), become at most VL_MAX lights, nearest the camera first: 2 cars x 2
 *    beam points, 2 bars x 1 point. Activation: beams x max(a_dark, tunnel), bars x max(a_dark, VL_COP_DAY).
 *  - Merge: per draw item, the car lights whose sphere meets its box take slots after cpl_select_cap(8 - nd).
 * Setting off: the hook returns at its first test, nd = 0, cap = CPL_MAX (the same selection as before).
 * TEST ONLY: NFSMW_TEST_VL_LOG=1 (probe: flare details every 60 frames, lights not applied), NFSMW_TEST_VL_AB=<s>
 * (in-run A/B: applied / not applied every <s> seconds, a line per phase change), NFSMW_TEST_VL_FWD=<+-1..3> (the car
 * matrix row that is forward, default 1 = row 0), NFSMW_TEST_VL_SIDE=1 (bar colour by the flare's side, left red).
 * Our own code; game data only. The part above VL_PURE_END has no guest access (offline tests include it alone). */
#ifndef VEHICLE_LIGHTS_H
#define VEHICLE_LIGHTS_H
#include <stdint.h>
#include <string.h>
#include <math.h>

#define VL_MAX 6u              /* dynamic lights per frame */
#define VL_ITEM_MAX 4u         /* car lights per draw item (statics keep at least 4 of the 8 slots) */
#define VL_HEAD_CARS 2u
#define VL_COP_CARS 2u
#define VL_CAP_CARS 32u        /* cars captured per frame */
#define VL_COP_DAY 0.25f
#define VL_GAIN 0.45f          /* colour scale: the brightest clean-pools lamp is 0.45 (UNV look; owner judges) */
#define VL_MATCH_M 3.0f
#define VL_HEAD_HEIGHT 0.9f
#define VL_HEAD_NEAR_AHEAD 5.0f
#define VL_HEAD_NEAR_R 7.0f
#define VL_HEAD_NEAR_D0 5.0f
#define VL_HEAD_FAR_AHEAD 13.0f
#define VL_HEAD_FAR_R 11.0f
#define VL_HEAD_FAR_D0 8.0f
#define VL_HEAD_FAR_K 0.6f
#define VL_BAR_UP 0.3f
#define VL_BAR_R 10.0f
#define VL_BAR_D0 4.0f
enum { VL_KIND_HEAD = 0, VL_KIND_BAR = 1 };

typedef struct {
    uint32_t matrix;
    float pos[3], fwd[3];
    uint8_t head_on;
    float head_i, i_red, i_blue;
    float bar[3];              /* sum of the bar flares' world positions */
    uint32_t bar_n;
} VlCar;
typedef struct { float p[3], r, col[3], d0; uint32_t kind; } VlLight;

/* Police flare hashes (00742950): 6a52a241 / e662c161 pulse with local_74, 41489594 / 1e4150b4 with local_78,
 * b4348dba with local_50. Colour classes UNV until the probe job (1 red, 2 blue, 3 both). */
static const uint32_t k_vl_police[5] = {0x6a52a241u, 0xe662c161u, 0x41489594u, 0x1e4150b4u, 0xb4348dbau};
static const uint8_t k_vl_police_class[5] = {1, 1, 2, 2, 3};
static int vl_police_class(uint32_t h) {
    for (unsigned i = 0; i < 5; ++i) if (k_vl_police[i] == h) return k_vl_police_class[i];
    return 0;
}

/* The capture slot of the car drawn with this arena matrix (and within 0.5 m: a reused arena address is another car);
 * NULL when the list is full. fwd_axis: +-(1..3), the matrix row that is forward. */
static VlCar *vl_car_slot(VlCar *cars, uint32_t *n, uint32_t matrix, const float m[16], int fwd_axis) {
    for (uint32_t i = 0; i < *n; ++i) {
        const float dx = cars[i].pos[0] - m[12], dy = cars[i].pos[1] - m[13], dz = cars[i].pos[2] - m[14];
        if (cars[i].matrix == matrix && dx * dx + dy * dy + dz * dz < 0.25f) return &cars[i];
    }
    if (*n >= VL_CAP_CARS) return NULL;
    VlCar *c = &cars[(*n)++];
    memset(c, 0, sizeof *c);
    c->matrix = matrix;
    c->pos[0] = m[12]; c->pos[1] = m[13]; c->pos[2] = m[14];
    const int a = (fwd_axis < 0 ? -fwd_axis : fwd_axis) - 1;
    const float sg = fwd_axis < 0 ? -1.0f : 1.0f;
    const float *r = m + 4 * (a >= 0 && a < 3 ? a : 0);
    const float len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (len > 1e-4f && isfinite(len)) for (int k = 0; k < 3; ++k) c->fwd[k] = sg * r[k] / len;
    return c;
}

/* One flare of a car (local position lp, local direction ld, flags byte +0x2d, game intensity). */
static void vl_add_flare(VlCar *c, const float m[16], uint32_t hash, const float lp[3], const float ld[3], uint8_t flags,
                         float inten, int fwd_axis, int side) {
    const int omni = flags & 1;
    int cls = vl_police_class(hash);
    if (omni && cls) {
        if (side) cls = lp[1] < 0.0f ? 1 : 2;
        for (int k = 0; k < 3; ++k) c->bar[k] += lp[0] * m[k] + lp[1] * m[4 + k] + lp[2] * m[8 + k] + m[12 + k];
        ++c->bar_n;
        const float i = inten < 2.0f ? inten : 2.0f;
        if ((cls & 1) && i > c->i_red) c->i_red = i;
        if ((cls & 2) && i > c->i_blue) c->i_blue = i;
    } else if (!omni) {
        const int a = (fwd_axis < 0 ? -fwd_axis : fwd_axis) - 1;
        const float sg = fwd_axis < 0 ? -1.0f : 1.0f;
        const float len = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
        if (a < 0 || a > 2 || !(len > 1e-4f)) return;
        if (sg * ld[a] / len > 0.5f) {   /* world dir . car forward, with an orthonormal basis */
            c->head_on = 1;
            if (inten > c->head_i) c->head_i = inten < 1.0f ? inten : 1.0f;
        }
    }
}

/* Movement over one frame: from the nearest car of the frame before within VL_MATCH_M (else 0). */
static void vl_velocity(const VlCar *prev, uint32_t np, const float pos[3], float vel[3]) {
    float best = VL_MATCH_M * VL_MATCH_M;
    vel[0] = vel[1] = vel[2] = 0.0f;
    for (uint32_t i = 0; i < np; ++i) {
        const float d[3] = {pos[0] - prev[i].pos[0], pos[1] - prev[i].pos[1], pos[2] - prev[i].pos[2]};
        const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (d2 < best) { best = d2; memcpy(vel, d, sizeof d); }
    }
}

static float vl_d2(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}
static void vl_push(VlLight *out, uint32_t *n, const float p[3], float r, const float col[3], float d0, uint32_t kind) {
    if (*n >= VL_MAX) return;
    VlLight *L = &out[(*n)++];
    memcpy(L->p, p, sizeof L->p); L->r = r; memcpy(L->col, col, sizeof L->col); L->d0 = d0; L->kind = kind;
}
/* The frame's lights from its cars (cur, moved one frame on against prev), nearest to the eye first, with their
 * union box. Colours carry the game intensity and VL_GAIN, not the activation (per draw item). */
static uint32_t vl_build(const VlCar *cur, uint32_t nc, const VlCar *prev, uint32_t np, const float eye[3],
                         VlLight out[VL_MAX], float lo[3], float hi[3], uint32_t *heads, uint32_t *bars) {
    uint32_t n = 0, hsel[VL_HEAD_CARS], csel[VL_COP_CARS], nh = 0, ncop = 0;
    float hd[VL_HEAD_CARS], cd[VL_COP_CARS];
    for (uint32_t i = 0; i < nc; ++i) {   /* nearest cars: insertion into two small sorted lists */
        const float d2 = vl_d2(cur[i].pos, eye);
        if (cur[i].head_on && cur[i].head_i > 0.0f) {
            uint32_t at = nh;
            while (at > 0 && hd[at - 1] > d2) --at;
            if (at < VL_HEAD_CARS) {
                for (uint32_t j = nh < VL_HEAD_CARS ? nh : VL_HEAD_CARS - 1; j > at; --j) { hsel[j] = hsel[j - 1]; hd[j] = hd[j - 1]; }
                hsel[at] = i; hd[at] = d2;
                if (nh < VL_HEAD_CARS) ++nh;
            }
        }
        if (cur[i].bar_n && cur[i].i_red + cur[i].i_blue > 0.0f) {
            uint32_t at = ncop;
            while (at > 0 && cd[at - 1] > d2) --at;
            if (at < VL_COP_CARS) {
                for (uint32_t j = ncop < VL_COP_CARS ? ncop : VL_COP_CARS - 1; j > at; --j) { csel[j] = csel[j - 1]; cd[j] = cd[j - 1]; }
                csel[at] = i; cd[at] = d2;
                if (ncop < VL_COP_CARS) ++ncop;
            }
        }
    }
    for (uint32_t s = 0; s < nh; ++s) {
        const VlCar *c = &cur[hsel[s]];
        float v[3], pn[3], pf[3];
        vl_velocity(prev, np, c->pos, v);
        for (int k = 0; k < 3; ++k) {
            const float base = c->pos[k] + v[k] + (k == 2 ? VL_HEAD_HEIGHT : 0.0f);
            pn[k] = base + c->fwd[k] * VL_HEAD_NEAR_AHEAD;
            pf[k] = base + c->fwd[k] * VL_HEAD_FAR_AHEAD;
        }
        const float g = VL_GAIN * c->head_i;
        const float cn[3] = {1.0f * g, 0.92f * g, 0.80f * g};
        const float cf[3] = {cn[0] * VL_HEAD_FAR_K, cn[1] * VL_HEAD_FAR_K, cn[2] * VL_HEAD_FAR_K};
        vl_push(out, &n, pn, VL_HEAD_NEAR_R, cn, VL_HEAD_NEAR_D0, VL_KIND_HEAD);
        vl_push(out, &n, pf, VL_HEAD_FAR_R, cf, VL_HEAD_FAR_D0, VL_KIND_HEAD);
    }
    for (uint32_t s = 0; s < ncop; ++s) {
        const VlCar *c = &cur[csel[s]];
        float v[3], p[3];
        vl_velocity(prev, np, c->pos, v);
        for (int k = 0; k < 3; ++k) p[k] = c->bar[k] / (float)c->bar_n + v[k] + (k == 2 ? VL_BAR_UP : 0.0f);
        const float col[3] = {VL_GAIN * (c->i_red * 1.0f + c->i_blue * 0.1f), VL_GAIN * (c->i_red * 0.08f + c->i_blue * 0.25f),
                              VL_GAIN * (c->i_red * 0.05f + c->i_blue * 1.0f)};
        vl_push(out, &n, p, VL_BAR_R, col, VL_BAR_D0, VL_KIND_BAR);
    }
    for (uint32_t i = 1; i < n; ++i) {   /* nearest to the eye first */
        VlLight t = out[i];
        const float d = vl_d2(t.p, eye);
        uint32_t j = i;
        while (j > 0 && vl_d2(out[j - 1].p, eye) > d) { out[j] = out[j - 1]; --j; }
        out[j] = t;
    }
    lo[0] = lo[1] = lo[2] = 1e30f; hi[0] = hi[1] = hi[2] = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            if (out[i].p[k] - out[i].r < lo[k]) lo[k] = out[i].p[k] - out[i].r;
            if (out[i].p[k] + out[i].r > hi[k]) hi[k] = out[i].p[k] + out[i].r;
        }
    *heads = nh; *bars = ncop;
    return n;
}

/* The lights (of n) whose sphere meets the box [lo, hi], at most max, in order; 0 when the box misses the union. */
static uint32_t vl_touching_pure(const VlLight *L, uint32_t n, const float ulo[3], const float uhi[3], const float *lo,
                                 const float *hi, uint32_t *out, uint32_t max) {
    if (!n || hi[0] < ulo[0] || lo[0] > uhi[0] || hi[1] < ulo[1] || lo[1] > uhi[1] || hi[2] < ulo[2] || lo[2] > uhi[2])
        return 0;
    uint32_t nd = 0;
    for (uint32_t i = 0; i < n && nd < max; ++i) {
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float v = L[i].p[k] < lo[k] ? lo[k] - L[i].p[k] : L[i].p[k] > hi[k] ? L[i].p[k] - hi[k] : 0.0f;
            d2 += v * v;
        }
        if (d2 < L[i].r * L[i].r) out[nd++] = i;
    }
    return nd;
}
static float vl_activation(uint32_t kind, float a_dark, int tunnel) {
    if (kind == VL_KIND_BAR) return a_dark > VL_COP_DAY ? a_dark : VL_COP_DAY;
    return tunnel ? 1.0f : a_dark;
}
/* ---- VL_PURE_END ---- */

#ifndef VL_PURE_ONLY
#define VL_SLOT 155
#define VL_MAIN_VIEW 0x00919650u
#define VL_GAME_FRAME 0x009885b8u
#define VL_LOG_RECS 24u
static int g_vl_hooked, g_vl_on, g_vl_apply, g_vl_log, g_vl_side, g_vl_fwd = 1;
static double g_vl_ab_s;
static VlCar g_vl_cap[VL_CAP_CARS], g_vl_prev[VL_CAP_CARS];
static uint32_t g_vl_ncap, g_vl_nprev;
static VlLight g_vl_l[VL_MAX];
static uint32_t g_vl_n;
static float g_vl_lo[3], g_vl_hi[3], g_vl_a_dark = 1.0f;
static uint64_t g_vl_frames, g_vl_cars, g_vl_heads, g_vl_bars, g_vl_adds, g_vl_items, g_vl_lit_items, g_vl_new_tagged,
    g_vl_capped, g_vl_glow;
/* TEST ONLY probe records (NFSMW_TEST_VL_LOG=1) */
typedef struct { uint32_t matrix, hash, frame; uint8_t type, flags; float lp[3], ld[3], inten; } VlRec;
static VlRec g_vl_rec[VL_LOG_RECS];
static uint32_t g_vl_nrec, g_vl_log_arm, g_vl_log_lines, g_vl_item_frame, g_vl_tick_frame;

static void vl_flare_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!g_vl_on) return;
    if (get_u32(cpu->esp + 4u) != VL_MAIN_VIEW || get_u32(cpu->esp + 0x14u) != 0u) return;
    const float inten = get_float(cpu->esp + 0x10u);
    const uint32_t flare = get_u32(cpu->esp + 8u), matrix = get_u32(cpu->esp + 0xcu);
    if (!(inten > 0.0f) || !flare || !matrix) return;
    void *mp = NULL, *fp = NULL;
    if (api->guest_ptr(api, matrix, 64, &mp) != POP_OK || !mp || api->guest_ptr(api, flare, 0x30, &fp) != POP_OK || !fp)
        return;
    float m[16], lp[3], ld[3];
    uint32_t hash;
    const uint8_t *f = (const uint8_t *)fp;
    memcpy(m, mp, sizeof m);
    memcpy(&hash, f + 0x08, 4); memcpy(lp, f + 0x10, sizeof lp); memcpy(ld, f + 0x20, sizeof ld);
    if (!isfinite(m[12]) || !isfinite(m[13]) || !isfinite(m[14])) return;
    VlCar *c = vl_car_slot(g_vl_cap, &g_vl_ncap, matrix, m, g_vl_fwd);
    if (c) vl_add_flare(c, m, hash, lp, ld, f[0x2d], inten, g_vl_fwd, g_vl_side);
    if (g_vl_log_arm && g_vl_nrec < VL_LOG_RECS) {
        VlRec *r = &g_vl_rec[g_vl_nrec++];
        r->matrix = matrix; r->hash = hash; r->frame = get_u32(VL_GAME_FRAME); r->type = f[0x2c]; r->flags = f[0x2d];
        memcpy(r->lp, lp, sizeof lp); memcpy(r->ld, ld, sizeof ld); r->inten = inten;
    }
}

/* Exterior darkness: the same sun-z smoothstep as lamp_daylight.h (its window), whether or not that setting is on. */
static float vl_sun_activation(void) {
    const uint32_t tod = get_u32(0x009b392cu);
    if (!tod) return 1.0f;
    float s[3];
    for (unsigned i = 0; i < 3; ++i) s[i] = get_float(tod + 0x180u + 4u * i);
    const float n = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!(n > 1e-4f) || !isfinite(n)) return 1.0f;
    float z = s[2] / n;
    z = z < -1.0f ? -1.0f : z > 1.0f ? 1.0f : z;
    float x = (g_ld_zoff - z) / (g_ld_zoff - g_ld_zon);
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return x * x * (3.0f - 2.0f * x);
}

static void vl_log_probe(const PopModApi *api) {
    if (!g_vl_log) return;
    if (g_vl_log_arm) {   /* the armed window ends: print what the hook recorded */
        g_vl_log_arm = 0;
        for (uint32_t i = 0; i < g_vl_nrec && g_vl_log_lines < 3000u; ++i, ++g_vl_log_lines) {
            const VlRec *r = &g_vl_rec[i];
            char line[320];
            snprintf(line, sizeof line,
                     "core.nfsmw: TEST vehicle lights probe: car %08x hash %08x type %u flags %02x local %.2f/%.2f/%.2f dir "
                     "%.2f/%.2f/%.2f class %d intensity %.3f | game frame at flare %u, at render item %u, at tick %u",
                     r->matrix, r->hash, r->type, r->flags, r->lp[0], r->lp[1], r->lp[2], r->ld[0], r->ld[1], r->ld[2],
                     g_vl_side && (r->flags & 1) && vl_police_class(r->hash) ? (r->lp[1] < 0.0f ? 1 : 2) : vl_police_class(r->hash),
                     r->inten, r->frame, g_vl_item_frame, g_vl_tick_frame);
            api->log(api, line);
        }
    }
    if (g_vl_frames % 60u == 1u) { g_vl_log_arm = 1; g_vl_nrec = 0; g_vl_item_frame = 0; g_vl_tick_frame = get_u32(VL_GAME_FRAME); }
}

/* Once per frame (game_device_update): settings, the A/B phase, then the last frame's cars become this frame's lights. */
static __attribute__((unused)) void vehicle_lights_tick(const PopModApi *api) {
    static uint32_t calls;
    static int env, phase = -1;
    static struct timespec t0;
    if (!env) {
        env = 1;
        const char *e = getenv("NFSMW_TEST_VL_LOG");
        g_vl_log = e && !strcmp(e, "1");
        e = getenv("NFSMW_TEST_VL_SIDE");
        g_vl_side = e && !strcmp(e, "1");
        e = getenv("NFSMW_TEST_VL_FWD");
        if (e) { const int a = atoi(e); if (a != 0 && a >= -3 && a <= 3) g_vl_fwd = a; }
        e = getenv("NFSMW_TEST_VL_AB");
        if (e) g_vl_ab_s = atof(e);
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }
    if ((calls++ & 255u) == 0) g_vl_on = g_vl_hooked && setting("vehicle_lights", 0) != 0;
    if (!g_vl_on || get_u32(0x00925e90u) != 6u) {
        g_vl_n = 0; g_vl_ncap = 0; g_vl_nprev = 0; g_vl_apply = 0;
        return;
    }
    ++g_vl_frames;
    int on = 1;
    if (g_vl_ab_s > 0.0) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        const double s = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) * 1e-9;
        on = ((long long)(s / g_vl_ab_s) & 1) == 0;
        if (on != phase) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsmw: TEST vehicle lights A/B: phase %s at %.1f s (frame %llu, game frame %u)",
                     on ? "on" : "off", s, (unsigned long long)g_vl_frames, get_u32(VL_GAME_FRAME));
            api->log(api, line);
            phase = on;
        }
    }
    g_vl_apply = on && !g_vl_log;
    g_vl_a_dark = vl_sun_activation();
    float eye[3] = {0, 0, 0};
    const uint32_t cam = get_u32(0x009195e0u + 0x70u + 0x40u);
    if (cam) for (unsigned i = 0; i < 3; ++i) eye[i] = get_float(cam + 0x40u + 4u * i);
    uint32_t heads = 0, bars = 0;
    g_vl_n = vl_build(g_vl_cap, g_vl_ncap, g_vl_prev, g_vl_nprev, eye, g_vl_l, g_vl_lo, g_vl_hi, &heads, &bars);
    g_vl_cars += g_vl_ncap; g_vl_heads += heads; g_vl_bars += bars;
    memcpy(g_vl_prev, g_vl_cap, g_vl_ncap * sizeof(VlCar));
    g_vl_nprev = g_vl_ncap;
    g_vl_ncap = 0;
    vl_log_probe(api);
    if (g_vl_frames % 1800u == 1u) {
        char line[360];
        snprintf(line, sizeof line,
                 "core.nfsmw: vehicle lights: frames %llu cars %llu heads %llu bars %llu pool adds %llu | items %llu lit %llu "
                 "new tagged %llu capped %llu glow %llu | now %u lights, dark %.2f, applied %d",
                 (unsigned long long)g_vl_frames, (unsigned long long)g_vl_cars, (unsigned long long)g_vl_heads,
                 (unsigned long long)g_vl_bars, (unsigned long long)g_vl_adds, (unsigned long long)g_vl_items,
                 (unsigned long long)g_vl_lit_items, (unsigned long long)g_vl_new_tagged, (unsigned long long)g_vl_capped,
                 (unsigned long long)g_vl_glow, g_vl_n, g_vl_a_dark, g_vl_apply);
        api->log(api, line);
    }
}

/* Per draw item (world_light_pass.h): the car lights touching its world box; 0 when off or not applied. */
static uint32_t vl_touching(const float *lo, const float *hi, uint32_t *out, uint32_t max) {
    if (g_vl_log_arm && !g_vl_item_frame) g_vl_item_frame = get_u32(VL_GAME_FRAME);
    if (!g_vl_apply || !g_vl_n) return 0;
    ++g_vl_items;
    return vl_touching_pure(g_vl_l, g_vl_n, g_vl_lo, g_vl_hi, lo, hi, out, max);
}

static PopModStatus install_at(uint32_t addr, uint32_t ret, PopHookFn fn, int32_t mode, int slot);   /* nfsmw.c */
static __attribute__((unused)) void vehicle_lights_install(const PopModApi *api) {
    const char *e = getenv("NFSMW_TEST_MIRROR_FLARES");
    if ((e && !strcmp(e, "1")) || g_hooks[99]) {   /* the TEST mirror flare probe wraps 00505380 (slot 99) */
        api->log(api, "core.nfsmw: vehicle lights: not installed (the TEST mirror flare probe holds 00505380)");
        return;
    }
    const PopModStatus s = install_at(0x00505380u, 0x0074323au, vl_flare_hook, POP_HOOK_BEFORE, VL_SLOT);
    g_vl_hooked = s == POP_OK;
    char line[120];
    snprintf(line, sizeof line, "core.nfsmw: vehicle lights: flare hook %s (slot %d, status %d)",
             g_vl_hooked ? "installed" : "FAILED", VL_SLOT, (int)s);
    api->log(api, line);
}
#endif
#endif
