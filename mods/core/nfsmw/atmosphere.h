/* atmosphere.h - Remaster run 14 B1 height fog and haze (review/run14/smoke-realism-design.md 8 (B) B1). Our own
 * code; PC data only. Included by nfsmw.c after upscale.h (it uses get_u32, get_float, setting, install_at, g_api).
 *
 * The setting `atmosphere` (off until the owner's eye check; Xbox 360 and PC looks off via k_look_follow) is read at
 * launch and handed to the kit as RECOMP_REMASTER_ATMOS=1; `atmosphere_strength` (%, live) scales the density. Off:
 * nothing here installs and the kit runs no pass.
 *
 * Per world frame two private SetTransform states (kit dx/host_d9.h HOST_D9_TS_ATMOS_A/B; layout in kit
 * host/gpu/metal/atmosphere.h Params):
 *  - mode 1 at the main view's colour pass (006de210 called at 006ded1c, as upscale.h / post_fx.h): the game's own
 *    scattering from the live time-of-day object [0x009b392c] (Rayleigh colour +0x70, Mie colour +0x80, multipliers
 *    +0x90/+0x94 as x360_sky.h makes Br/Bm, HG g +0x9c, sun direction +0x180) and the art values below;
 *    then the camera (0x200/0x201 as upscale.h sends it) so the kit can pick the scene's view-projection;
 *  - mode 2 before the frame's HUD call (006c3870 at 006e75a6; installed before upscale.h's end marker and the post
 *    chain, so it lands while the scene pass is still open): the kit's fallback point for a frame without particles.
 * Art values (owner tunes; v1 by sun height only, no district table yet): a thin haze everywhere (aerial perspective,
 * base 0 m, scale height 600 m) and a dense low layer near sea level (base 25 m, scale 15 m: the bay mist) at dawn and
 * dusk that is gone at midday. TEST ONLY: the kit's RECOMP_REMASTER_ATMOS_HAZE / _FOG / _DEBUG override them. */
#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#define ATMOS_SLOT_BEGIN 158
#define ATMOS_SLOT_END 159

static int g_atm_on, g_atm_open;
static uint32_t g_atm_mem;   /* guest memory for the two packets (0x80 bytes) */
static uint64_t g_atm_begins, g_atm_ends;
static float g_atm_strength = 1.0f;

static float atm_sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float atm_mixf(float a, float b, float t) { return a + (b - a) * t; }

/* The packets: a = sun dir, g | Br, - | Bm, sun intensity | sun colour, -;  b = haze rho, base, H | fog rho, base, H |
 * start, far, max opacity | ambient rgb | strength, Mie cap, -, mode. */
static void atm_build(float a[16], float b[16], float mode) {
    memset(a, 0, 16 * sizeof(float));
    memset(b, 0, 16 * sizeof(float));
    b[15] = mode;
    if (mode != 1.0f) return;
    const uint32_t tod = get_u32(0x009b392cu);
    if (!tod) return;   /* no time of day: strength 0, no pass */
    float s[3];
    for (unsigned i = 0; i < 3; ++i) s[i] = get_float(tod + 0x180u + 4u * i);
    const float n = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!(n > 1e-4f) || !isfinite(n)) return;
    for (unsigned i = 0; i < 3; ++i) s[i] /= n;
    const float rayleigh = (get_float(tod + 0x90u) + 1.0f) * 0.0001f;
    const float mie = fmaxf(0.0f, (get_float(tod + 0x94u) + 1.0f) * 0.0001f);
    for (unsigned i = 0; i < 3; ++i) {
        a[i] = s[i];
        a[4 + i] = fmaxf(0.0f, rayleigh * get_float(tod + 0x70u + 4u * i));
        a[8 + i] = fmaxf(0.0f, mie * get_float(tod + 0x80u + 4u * i));
    }
    const float g = get_float(tod + 0x9cu);
    a[3] = isfinite(g) ? g : 0.0f;
    /* Sun: fades below the horizon (as clean_sun.h), warmer as it sinks; less through cloud (Rain +0x290). */
    const uint32_t rain = get_u32(0x009196b8u);
    float clouds = rain ? get_float(rain + 0x290u) : 0.0f;
    if (!(clouds >= 0.0f)) clouds = 0.0f;
    const float up = atm_sat(s[2] * 6.0f + 0.3f), low = 1.0f - atm_sat(s[2] / 0.35f);
    a[11] = 0.6f * up * (1.0f - 0.6f * atm_sat(clouds));
    a[12] = 1.0f;
    a[13] = atm_mixf(0.90f, 0.62f, low);
    a[14] = atm_mixf(0.74f, 0.36f, low);
    /* Ambient: the game's scattering colour (Br + Bm, normalised to its largest channel) toward grey, by daylight. */
    float c[3], cm = 0.0f;
    for (unsigned i = 0; i < 3; ++i) { c[i] = a[4 + i] + a[8 + i]; cm = fmaxf(cm, c[i]); }
    const float day = atm_sat(s[2] * 4.0f + 0.4f);
    for (unsigned i = 0; i < 3; ++i) {
        const float ci = cm > 0.0f ? c[i] / cm : 1.0f;
        b[9 + i] = (0.04f + 0.26f * day) * atm_mixf(ci, 1.0f, 0.5f);
    }
    /* Haze (aerial perspective): denser when the sun is low. Fog: a low layer at dawn and dusk, none at midday. */
    b[0] = atm_mixf(1.5e-4f, 3.5e-4f, low);
    b[1] = 0.0f;
    b[2] = 600.0f;
    b[3] = 4.0e-3f * low * low;
    b[4] = 25.0f;
    b[5] = 15.0f;
    b[6] = 20.0f;     /* start distance: the car and the road just ahead stay clear */
    b[7] = 2500.0f;   /* far / sky distance */
    b[8] = 0.95f;     /* max opacity */
    b[12] = g_atm_strength;
    b[13] = 4.0f;     /* Mie (glow) cap */
}
/* IDirect3DDevice9::SetTransform (vtable 0xb0) with the private states 0x202 then 0x203. */
static void atm_send(uint32_t dev, float mode) {
    const uint32_t fn = get_u32(get_u32(dev) + 0xb0u);
    if (!fn) return;
    if (!g_atm_mem && g_api->guest_alloc(g_api, 0x80u, &g_atm_mem) != POP_OK) { g_atm_mem = 0; return; }
    float a[16], b[16];
    atm_build(a, b, mode);
    for (uint32_t i = 0; i < 16; ++i) {
        g_api->guest_write_u32(g_api, g_atm_mem + 4u * i, float_bits(a[i]));
        g_api->guest_write_u32(g_api, g_atm_mem + 0x40u + 4u * i, float_bits(b[i]));
    }
    uint32_t args[3] = {dev, 0x202u, g_atm_mem}, r = 0;
    g_api->guest_call(g_api, fn, 0, args, 3, &r);
    args[1] = 0x203u;
    args[2] = g_atm_mem + 0x40u;
    g_api->guest_call(g_api, fn, 0, args, 3, &r);
}
static void atm_begin(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (g_atm_open || get_u32(0x00925e90u) != 6) return;                          /* the world only, once a frame */
    if (g_look == LOOK_PC || get_u32(0x009017fcu) || get_u32(0x00901828u)) return; /* the PC look: none */
    const uint32_t dev = get_u32(0x00982bdcu);
    if (!dev) return;
    static uint32_t calls;
    if ((calls++ & 255u) == 0) g_atm_strength = (float)setting("atmosphere_strength", 100) / 100.0f;
    atm_send(dev, 1.0f);
    /* COMB14B fix: the camera (SetTransform 0x200/0x201, upscale.h) for the kit's view-projection pick; with the
     * foundation on, upscale.h's own begin already sends it in this window. */
    if (!g_up_fnd) up_send_camera(dev);
    g_atm_open = 1;
    ++g_atm_begins;
}
static void atm_end(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (!g_atm_open) return;
    g_atm_open = 0;
    const uint32_t dev = get_u32(0x00982bdcu);
    if (!dev) return;
    atm_send(dev, 2.0f);
    ++g_atm_ends;
}
/* At launch, before upscale_init (same-address hooks run in install order: the end packet goes before the 252 marker
 * and the post chain). */
static void atmosphere_init(const PopModApi *api) {
    if (!setting("atmosphere", 0)) return;
    if (!getenv("RECOMP_REMASTER_ATMOS")) setenv("RECOMP_REMASTER_ATMOS", "1", 0);
    g_atm_strength = (float)setting("atmosphere_strength", 100) / 100.0f;
    const int ok = install_at(0x006de210u, 0x006ded21u, atm_begin, POP_HOOK_BEFORE, ATMOS_SLOT_BEGIN) == POP_OK &&
                   install_at(0x006c3870u, 0x006e75a6u, atm_end, POP_HOOK_BEFORE, ATMOS_SLOT_END) == POP_OK;
    if (!ok)
        for (int s = ATMOS_SLOT_BEGIN; s <= ATMOS_SLOT_END; ++s)
            if (g_hooks[s]) { api->hook_remove(api, g_hooks[s]); g_hooks[s] = 0; }
    g_atm_on = ok;
    api->log(api, ok ? "core.nfsmw: atmosphere (height fog and haze) on at launch" : "core.nfsmw: atmosphere UNAVAILABLE (hook)");
}
static void atmosphere_exit(const PopModApi *api) {
    if (!g_atm_on) return;
    char line[160];
    snprintf(line, sizeof line, "core.nfsmw: atmosphere: %llu frame packets, %llu scene ends", (unsigned long long)g_atm_begins,
             (unsigned long long)g_atm_ends);
    api->log(api, line);
}

#endif
