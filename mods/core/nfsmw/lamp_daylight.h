/* lamp_daylight.h - R2 run 9 item B (owner 7 Oct; design review/2026-10-07-r2-daynight-lamps-design.md, challenged):
 * outdoor street lamps fade out in daylight, tunnel lamps stay on. Setting lamp_daylight (off by default).
 * Once per frame (game_device_update) the exterior-lamp activation a_ext is latched from the game's own sun direction
 * ([0x009b392c] +0x180, unit vector to the sun, z up): 1 while the sun is low (z <= Z_ON), 0 when high (z >= Z_OFF),
 * smoothstep between. a_ext = 1 (today's output) unless the setting is on, the game is in the world (flow 6), the
 * time-of-day object exists and the sun vector is finite and non-zero. The clean pools (cpl_select / cpl_block) and the
 * lamp glow (clean_glow.h) read the same latched value: lamps at 0 are not candidates (no pool pass: a CPU saving),
 * dim lamps rank by d2 / a^2 so they give up the 8 slots gradually, and their colour is scaled by a. Setting off: the
 * original calls, unchanged. TEST: NFSMW_TEST_DAYLIGHT_Z=<on>:<off> overrides the window. Our own code; game data only. */
#ifndef LAMP_DAYLIGHT_H
#define LAMP_DAYLIGHT_H

static int g_ld_active;           /* lamp_daylight on this frame */
static float g_ld_a = 1.0f;       /* exterior lamp activation this frame */
static float g_ld_kind[2] = {1.0f, 1.0f};   /* activation by lamp kind (0 exterior, 1 tunnel), for cpl_select/cpl_block */
static float g_ld_zon = 0.10f, g_ld_zoff = 0.26f;
static float g_ld_zmin = 2.0f, g_ld_zmax = -2.0f;
static uint64_t g_ld_frames, g_ld_zero, g_ld_partial;

static void lamp_daylight_tick(const PopModApi *api) {
    static uint32_t calls;
    static int on, env;
    if ((calls++ & 255u) == 0)
        on = setting("lamp_daylight", 0) != 0;
    if (!env) {
        env = 1;
        const char *e = getenv("NFSMW_TEST_DAYLIGHT_Z");
        float zon, zoff;
        if (e && sscanf(e, "%f:%f", &zon, &zoff) == 2 && zoff > zon) { g_ld_zon = zon; g_ld_zoff = zoff; }
    }
    g_ld_active = on;
    g_ld_a = 1.0f;
    g_ld_kind[0] = g_ld_kind[1] = 1.0f;
    if (!on || get_u32(0x00925e90u) != 6u)
        return;
    const uint32_t tod = get_u32(0x009b392cu);
    if (!tod)
        return;
    float s[3];
    for (unsigned i = 0; i < 3; ++i) s[i] = get_float(tod + 0x180u + 4u * i);
    const float n = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!(n > 1e-4f) || !isfinite(n))
        return;
    float z = s[2] / n;
    z = z < -1.0f ? -1.0f : z > 1.0f ? 1.0f : z;
    float x = (g_ld_zoff - z) / (g_ld_zoff - g_ld_zon);
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    g_ld_a = x * x * (3.0f - 2.0f * x);
    g_ld_kind[0] = g_ld_a;
    if (z < g_ld_zmin) g_ld_zmin = z;
    if (z > g_ld_zmax) g_ld_zmax = z;
    ++g_ld_frames;
    g_ld_zero += g_ld_a <= 0.0f;
    g_ld_partial += g_ld_a > 0.0f && g_ld_a < 1.0f;
    if (g_ld_frames % 1800u == 1u) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsmw: lamp daylight: time %.3f sun z %.3f exterior lamps %.3f (window %.2f-%.2f; z seen %.3f..%.3f)",
                 get_float(tod + 8u), z, g_ld_a, g_ld_zon, g_ld_zoff, g_ld_zmin, g_ld_zmax);
        api->log(api, line);
    }
}
static void lamp_daylight_exit(const PopModApi *api) {
    if (!g_ld_frames) return;
    char line[200];
    snprintf(line, sizeof line, "core.nfsmw: lamp daylight: %llu world frames, exterior lamps off on %llu, partly on %llu; sun z %.3f..%.3f",
             (unsigned long long)g_ld_frames, (unsigned long long)g_ld_zero, (unsigned long long)g_ld_partial, g_ld_zmin, g_ld_zmax);
    api->log(api, line);
}
#endif
