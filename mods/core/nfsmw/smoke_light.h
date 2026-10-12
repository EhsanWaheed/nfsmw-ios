/* Lit smoke, stage 1 (run 14; design review/run14/smoke-realism-design.md 4 stage 1 and 7). Setting 'lit_smoke' (bool,
 * live; default off until the owner's eye check; Look Xbox 360 and PC: off through k_look_follow).
 *
 * How the kind reaches the draw. Every EmitterSystem particle (tyre smoke, flare smoke, stacks, fog, steam) is drawn by
 * the one IDI_PARTICLES_FX pixel shader with the shared FX_SMK sprites, so the kind must come from the emitter. The
 * render loop 00503d00 draws one batch per emitter: 006ceee0 begins it (locks the quad buffer), 006c1160 writes each
 * particle's quad (world space), and 006da2f0 ends it - its 006c1010 issues the DrawIndexedPrimitive. At the call from
 * 00503d00 (return PC 00503f71) EDI holds the emitter (00503d00.asm: EDI = the list node whose +0x7c instance and
 * +0x80 particle list the loop reads). A WRAP on 006da2f0 filtered on that return PC maps the emitter's key (instance
 * +0x7c -> collection +4 -> key +0x20, as smoke.h) to a kind and sets private render states around the draw (through
 * the device's SetRenderState, vtable 0xe4, as post_fx.h does for 250/251):
 *   248 = kind (1 tyre, 2 red flare, 3 stack steam, 4 stack soot, 5 fog/mist, 6 vent steam, 7 green flare);
 *   245..247 = the direction toward the sun x its strength (float bits, world z up: time-of-day +0x180 as clean_sun.h,
 *              strength = sun height fade x clear sky (Rain +0x290)); sent only when it changes (kept between draws);
 *   242..244 = the emitter's world position, flares only (sml_source: the matrix translation +0x50, or the lowest
 *              particle when that translation is local - attached emitters, COMB14B).
 * 248 goes back to 0 right after the draw, so nothing reaches the rain or any other draw. The kit (host/gpu/metal/
 * smoke_light.h) turns the kind into the lit-smoke shader variant. Off (setting, Look, or an unknown emitter): the hook
 * only calls on and sets nothing (byte-identical draws).
 *
 * TEST ONLY: NFSMW_TEST_LITSMOKE_LOG=1 logs the first 20 marked draws and each key's first 3 (key, kind, emitter
 * position, sun; at most 400 lines) and a 10 s summary of marked draws per kind. */
#ifndef SMOKE_LIGHT_H
#define SMOKE_LIGHT_H

#define SML_END        0x006da2f0u   /* per-emitter end of the particle render loop: the batch's draw (006c1010) */
#define SML_RET        0x00503f71u   /* return PC of its call from 00503d00 (call at 00503f6c) */
#define SML_SLOT       153           /* g_hooks slot (free: grep, 8 Oct) */
#define SML_RS_KIND    248u
#define SML_RS_SUN     245u
#define SML_RS_POS     242u
#define SML_RS_SOFT    241u   /* run 15 tunnel2: soft-kind marker (fog / vent steam) while lit smoke is off */

#include "smoke_kinds.h"

static int g_sml_on = -1, g_sml_log;
static uint32_t g_sml_calls_to_refresh, g_sml_sun_bits[3], g_sml_sun_age;
static uint64_t g_sml_marked, g_sml_calls, g_sml_by_kind[SMK_KIND_MAX + 1], g_sml_logged, g_sml_next_ns;

/* Run 15 tunnel2: with lit smoke off but soft particles on, fog (5) and vent steam (6) still tell the kit their kind
 * through private render state 241 (0 after the draw), so the plain soft fade gets the lit path's near-camera fade and
 * fog's wide fade (no hard sprite edges on the road in tunnels). Soft particles off (setting or Look): nothing set. */
static int g_sml_soft = -1;
static uint32_t g_sml_soft_refresh;
static int sml_soft_enabled(void) {
    if (g_sml_soft >= 0 && g_sml_soft_refresh-- > 0) return g_sml_soft;
    g_sml_soft_refresh = 256;
    g_sml_soft = setting("soft_particles", 1) != 0;   /* Look Xbox 360 / PC read 0 (k_look_follow) */
    return g_sml_soft;
}
static int sml_enabled(void) {
    if (g_sml_on >= 0 && g_sml_calls_to_refresh-- > 0) return g_sml_on;
    g_sml_calls_to_refresh = 256;
    g_sml_on = setting("lit_smoke", 0) != 0;   /* Look Xbox 360 / PC read 0 (k_look_follow) */
    return g_sml_on;
}
static void sml_rs(uint32_t state, uint32_t value) {
    const uint32_t a[] = {state, value};
    pfx_call(get_u32(0x00982bdcu), 0xe4, a, 2);
}
/* The direction toward the sun x strength (0 at night or under full cloud), world space; 0 vector if unknown. */
static void sml_sun(float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    const uint32_t tod = get_u32(0x009b392cu);
    if (!tod) return;
    float s[3];
    for (unsigned i = 0; i < 3; ++i) s[i] = get_float(tod + 0x180u + 4u * i);
    const float n = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!(n > 1e-4f) || !isfinite(n)) return;
    float h = (s[2] / n) * 6.0f + 0.3f;   /* fades as the sun sets below about -3 degrees (clean_sun.h) */
    h = h < 0.0f ? 0.0f : h > 1.0f ? 1.0f : h;
    const uint32_t rain = get_u32(0x009196b8u);
    float clouds = rain ? get_float(rain + 0x290u) : 0.0f;
    if (!(clouds >= 0.0f)) clouds = 0.0f;
    if (clouds > 1.0f) clouds = 1.0f;
    const float k = h * (1.0f - 0.8f * clouds) / n;
    for (unsigned i = 0; i < 3; ++i) out[i] = s[i] * k;
}
/* The emitter's source in world space. Its matrix translation (+0x50) is world for placed emitters, but local for
 * emitters attached to a car or a parent (COMB14B: steam at (1.5, 7.3, 0)). The particles' own positions (+0x50 of
 * each, the list at emitter +0x80, next at +0) are world (006c1160 builds the quads from them). So: the translation if
 * a particle lies within 20 m of it, else the lowest particle (smoke rises from its source). 1 = from the particles. */
static int sml_source(uint32_t self, float out[3]) {
    for (unsigned i = 0; i < 3; ++i) out[i] = get_float(self + 0x50u + 4u * i);
    const uint32_t head = self + 0x80u;
    float low[3] = {0, 0, 0}, best = 1e30f;
    int n = 0;
    for (uint32_t p = get_u32(head); p && p != head && n < 64; p = get_u32(p), ++n) {
        float q[3], d = 0.0f;
        for (unsigned i = 0; i < 3; ++i) { q[i] = get_float(p + 0x50u + 4u * i); d += (q[i] - out[i]) * (q[i] - out[i]); }
        if (!isfinite(d)) continue;
        if (d < best) best = d;
        if (n == 0 || q[2] < low[2]) memcpy(low, q, sizeof low);
    }
    if (!n || best <= 400.0f) return 0;
    memcpy(out, low, sizeof low);
    return 1;
}
static void sml_end_inner(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t ret = get_u32(cpu->esp), self = cpu->edi;
    if (ret == SML_RET && self && !sml_enabled() && sml_soft_enabled()) {
        const uint32_t i7 = get_u32(self + 0x7cu), c4 = i7 ? get_u32(i7 + 4u) : 0, k20 = c4 ? get_u32(c4 + 0x20u) : 0;
        const uint32_t sk = k20 ? smk_kind(k20) : 0;
        if (sk == SMK_FOG || sk == SMK_STEAM) {
            sml_rs(SML_RS_SOFT, sk);
            api->call_next(api, inv, cpu);
            sml_rs(SML_RS_SOFT, 0);
            return;
        }
    }
    if (ret != SML_RET || !self || !sml_enabled()) { api->call_next(api, inv, cpu); return; }
    ++g_sml_calls;
    const uint32_t inst = get_u32(self + 0x7cu), coll = inst ? get_u32(inst + 4u) : 0, key = coll ? get_u32(coll + 0x20u) : 0;
    const uint32_t kind = key ? smk_kind(key) : 0;
    if (!kind) { api->call_next(api, inv, cpu); return; }
    float sun[3];
    sml_sun(sun);
    uint32_t sb[3];
    memcpy(sb, sun, sizeof sb);
    if (memcmp(sb, g_sml_sun_bits, sizeof sb) || ++g_sml_sun_age >= 600u) {   /* resent now and then (a device Reset) */
        for (unsigned i = 0; i < 3; ++i) sml_rs(SML_RS_SUN + i, sb[i]);
        memcpy(g_sml_sun_bits, sb, sizeof sb);
        g_sml_sun_age = 0;
    }
    float pos[3] = {0, 0, 0};
    int from_particles = 0;
    if (kind == SMK_FLARE_RED || kind == SMK_FLARE_GREEN) {
        from_particles = sml_source(self, pos);
        for (unsigned i = 0; i < 3; ++i) sml_rs(SML_RS_POS + i, float_bits(pos[i]));
    }
    sml_rs(SML_RS_KIND, kind);
    ++g_sml_marked;
    ++g_sml_by_kind[kind];
    int fresh = 0;   /* TEST: a key's first 3 draws (new emitters: where the stacks and fog banks are) */
    if (g_sml_log) {
        static uint32_t seen[64], seen_n[64], nseen;
        unsigned i = 0;
        while (i < nseen && seen[i] != key) ++i;
        if (i == nseen && nseen < 64u) seen[nseen++] = key;
        fresh = i < 64u && seen_n[i]++ < 3u;
    }
    if (g_sml_log && (fresh || g_sml_logged < 20u) && g_sml_logged < 400u) {
        ++g_sml_logged;
        float tr[3];
        for (unsigned i = 0; i < 3; ++i) tr[i] = get_float(self + 0x50u + 4u * i);
        if (kind != SMK_FLARE_RED && kind != SMK_FLARE_GREEN) from_particles = sml_source(self, pos);
        char line[300];
        snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST lit smoke: key %08x kind %u particles %u pos %.1f %.1f %.1f (%s; +0x50 %.1f %.1f %.1f) sun %.3f %.3f %.3f",
                 key, kind, get_u32(self + 0x1cu) & 0xffffu, pos[0], pos[1], pos[2], from_particles ? "lowest particle" : "translation",
                 tr[0], tr[1], tr[2], sun[0], sun[1], sun[2]);
        api->log(api, line);
    }
    api->call_next(api, inv, cpu);
    sml_rs(SML_RS_KIND, 0);
    if (g_sml_log) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        const uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
        if (!g_sml_next_ns) g_sml_next_ns = now + 10000000000ull;
        if (now >= g_sml_next_ns) {
            g_sml_next_ns = now + 10000000000ull;
            char line[256];
            snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST lit smoke 10 s: marked %llu of %llu emitter draws (tyre %llu flare %llu stack %llu soot %llu fog %llu steam %llu green %llu)",
                     (unsigned long long)g_sml_marked, (unsigned long long)g_sml_calls, (unsigned long long)g_sml_by_kind[1],
                     (unsigned long long)g_sml_by_kind[2], (unsigned long long)g_sml_by_kind[3], (unsigned long long)g_sml_by_kind[4],
                     (unsigned long long)g_sml_by_kind[5], (unsigned long long)g_sml_by_kind[6], (unsigned long long)g_sml_by_kind[7]);
            api->log(api, line);
        }
    }
}
/* Run 16 smokeperf: tyre smoke spawned at 1/K (smoke.h) is drawn with the alpha exponent K (private render state 240 =
 * K x 16 around the emitter's draw, 0 after it), lit or plain. Nothing set for any other emitter or with the settings off. */
static void sml_end(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    const uint32_t self = cpu->edi;
    const uint32_t k16 = self && get_u32(cpu->esp) == SML_RET ? smoke_alpha_k16(api, self) : 0u;
    if (k16) sml_rs(SMOKE_RS_ALPHAK, k16);
    sml_end_inner(api, cpu, inv, user);
    if (k16) sml_rs(SMOKE_RS_ALPHAK, 0);
}
/* Installed in every Look: off is a pass-through (the setting is live). */
static void smoke_light_init(const PopModApi *api) {
    const char *e = getenv("NFSMW_TEST_LITSMOKE_LOG");
    g_sml_log = e && !strcmp(e, "1");
    const int ok = install(SML_END, sml_end, POP_HOOK_WRAP, SML_SLOT) == POP_OK;
    char line[160];
    snprintf(line, sizeof line, "core.nfsmw: lit smoke: %s (now %s, %u emitter keys)%s", ok ? "hook installed" : "hook refused; off",
             sml_enabled() ? "on" : "off", (unsigned)(sizeof k_smk_keys / sizeof k_smk_keys[0]), g_sml_log ? "; TEST log on" : "");
    api->log(api, line);
}
static void smoke_light_exit(const PopModApi *api) {
    if (!g_sml_marked) return;
    char line[200];
    snprintf(line, sizeof line, "core.nfsmw: lit smoke: %llu marked emitter draws of %llu", (unsigned long long)g_sml_marked,
             (unsigned long long)g_sml_calls);
    api->log(api, line);
}
#endif
