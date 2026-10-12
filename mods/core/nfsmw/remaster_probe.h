/* remaster_probe.h - TEST ONLY (NFSMW_PROFILE_PROBE=1; Remaster, owner request 6 Oct "R3 = skip track unless Manual").
 * Finds the Transmission (Automatic/Manual) setting: every 20 device updates (front end and world) it snapshots the
 * front-end database object [0x0091cf90] (0x400 bytes) and the block its +0x10 points to (most likely the user
 * profile; review/2026-10-06-r3-skip-track-research.md), and logs each byte that changed since the last snapshot.
 * Offsets that keep changing (counters, timers) are logged at most 6 times so a toggle stands out. Reads only. */
#define RPP_A 0x400u
#define RPP_B 0x800u
static int g_rpp_on = -1;
static uint32_t g_rpp_n, g_rpp_db, g_rpp_prof;
static uint8_t g_rpp_a[RPP_A], g_rpp_b[RPP_B], g_rpp_ca[RPP_A], g_rpp_cb[RPP_B];

static void rpp_region(const PopModApi *api, const char *name, uint32_t base, uint8_t *prev, uint8_t *count,
                       uint32_t size, int fresh) {
    uint8_t cur[RPP_B];
    for (uint32_t i = 0; i < size; i += 4) {
        const uint32_t v = get_u32(base + i);
        memcpy(cur + i, &v, 4);
    }
    if (!fresh) {
        int logged = 0;
        for (uint32_t i = 0; i < size && logged < 12; ++i) {
            if (cur[i] == prev[i] || count[i] > 6) continue;
            ++count[i];
            char line[160];
            snprintf(line, sizeof line, "core.nfsmw: profile probe: %s %08x +0x%03x: %02x -> %02x (flow %u)", name, base, i,
                     prev[i], cur[i], get_u32(0x00925e90u));
            api->log(api, line);
            ++logged;
        }
    }
    memcpy(prev, cur, size);
}

static void profile_probe(const PopModApi *api) {
    if (g_rpp_on < 0) {
        const char *e = getenv("NFSMW_PROFILE_PROBE");
        g_rpp_on = e && *e == '1';
    }
    if (!g_rpp_on || (++g_rpp_n % 20u)) return;
    const uint32_t db = get_u32(0x0091cf90u);
    if (!db) return;
    const uint32_t prof = get_u32(db + 0x10u);
    const int fresh_db = db != g_rpp_db, fresh_prof = prof != g_rpp_prof;
    if (fresh_db || fresh_prof) {
        char line[128];
        snprintf(line, sizeof line, "core.nfsmw: profile probe: database %08x, profile block %08x", db, prof);
        api->log(api, line);
        memset(g_rpp_ca, 0, sizeof g_rpp_ca);
        memset(g_rpp_cb, 0, sizeof g_rpp_cb);
    }
    g_rpp_db = db;
    g_rpp_prof = prof;
    rpp_region(api, "database", db, g_rpp_a, g_rpp_ca, RPP_A, fresh_db);
    if (prof) rpp_region(api, "profile", prof, g_rpp_b, g_rpp_cb, RPP_B, fresh_prof);
}

/* TEST ONLY (NFSMW_SUN_PROBE=1; R2 N4 P0, review/2026-10-07-r2-lighting-research.md section 4): where is the sun?
 * Every 30 device updates in the world it logs the time-of-day object [0x009b392c] (+0x08 time, +0x0c/+0x10 angles,
 * +0x170 sun vector, +0x180 normalised), the player camera (eView 1 +0x40: matrix +0x00..+0x3c, FOV u16 +0xc4,
 * near/far +0xbc/+0xc0) and the sun's projected screen position under the hypothesis view = world x matrix (row
 * vectors, D3D x right / y up / z forward), FOV = u16 / 65536 turns as the horizontal angle. Reads only. */
static int g_rsp_on = -1;
static uint32_t g_rsp_n;
static void sun_probe(const PopModApi *api) {
    if (g_rsp_on < 0) { const char *e = getenv("NFSMW_SUN_PROBE"); g_rsp_on = e && *e == '1'; }
    if (!g_rsp_on || (++g_rsp_n % 30u)) return;
    const uint32_t tod = get_u32(0x009b392cu), cam = get_u32(0x009195e0u + 0x70u + 0x40u);
    if (!tod || !cam) return;
    float s[3], m[4][4];
    for (unsigned i = 0; i < 3; ++i) s[i] = get_float(tod + 0x180u + 4u * i);
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) m[i][j] = get_float(cam + 16u * i + 4u * j);
    float v[3];
    for (unsigned j = 0; j < 3; ++j) v[j] = s[0] * m[0][j] + s[1] * m[1][j] + s[2] * m[2][j];
    const unsigned fov = get_u32(cam + 0xc4u) & 0xffffu;
    const float th = tanf((float)fov / 65536.0f * 3.14159265f), aspect = 16.0f / 9.0f;
    const float u = v[2] > 0.0f ? 0.5f + 0.5f * v[0] / (v[2] * th) : -9.0f;
    const float w = v[2] > 0.0f ? 0.5f - 0.5f * v[1] * aspect / (v[2] * th) : -9.0f;
    char line[512];
    snprintf(line, sizeof line, "core.nfsmw: sun probe: t %.4f ang %.4f %.4f sun170 %.0f %.0f %.0f sun %.4f %.4f %.4f | "
             "cam r0 %.3f %.3f %.3f %.3f r1 %.3f %.3f %.3f %.3f r2 %.3f %.3f %.3f %.3f r3 %.1f %.1f %.1f | fov %u (%.1f deg) nf %.2f %.0f | "
             "view %.3f %.3f %.3f uv %.3f %.3f | slots %.4f %.4f %.4f %.4f",
             (double)get_float(tod + 0x08u), (double)get_float(tod + 0x0cu), (double)get_float(tod + 0x10u),
             (double)get_float(tod + 0x170u), (double)get_float(tod + 0x174u), (double)get_float(tod + 0x178u),
             (double)s[0], (double)s[1], (double)s[2],
             (double)m[0][0], (double)m[0][1], (double)m[0][2], (double)m[0][3], (double)m[1][0], (double)m[1][1], (double)m[1][2],
             (double)m[1][3], (double)m[2][0], (double)m[2][1], (double)m[2][2], (double)m[2][3], (double)m[3][0], (double)m[3][1],
             (double)m[3][2], fov, (double)fov / 65536.0 * 360.0, (double)get_float(cam + 0xbcu), (double)get_float(cam + 0xc0u),
             (double)v[0], (double)v[1], (double)v[2], (double)u, (double)w,
             (double)get_float(0x00a37804u), (double)get_float(0x00a37808u), (double)get_float(0x00a3780cu), (double)get_float(0x00a37810u));
    api->log(api, line);
}

/* TEST ONLY (NFSMW_GRADE_PROBE=1; R2 N3-B P0, review/2026-10-07-r2-golden-filter-spec.md): the PC visual-treatment
 * object [0x00982af0]. Every 300 device updates in the world: the words +0x170..+0x1fc (floats and hex), and for the
 * three layout pointers +0x18c / +0x1a0 / +0x1ac the floats at +0x40..+0xdc of each. Reads only. */
static int g_rgp_on = -1;
static uint32_t g_rgp_n;
static void grade_probe(const PopModApi *api) {
    if (g_rgp_on < 0) { const char *e = getenv("NFSMW_GRADE_PROBE"); g_rgp_on = e && *e == '1'; }
    if (!g_rgp_on || (++g_rgp_n % 300u) || get_u32(0x00925e90u) != 6u) return;
    const uint32_t vt = get_u32(0x00982af0u);
    if (!vt) return;
    char line[1600];
    int n = snprintf(line, sizeof line, "core.nfsmw: grade probe: obj %08x +0x00 %08x |", vt, get_u32(vt));
    for (uint32_t o = 0x170u; o < 0x200u && n < (int)sizeof line - 40; o += 4u)
        n += snprintf(line + n, sizeof line - (size_t)n, " %x:%08x(%.3g)", o, get_u32(vt + o), (double)get_float(vt + o));
    api->log(api, line);
    static const uint32_t lay[3] = {0x18cu, 0x1a0u, 0x1acu};
    for (unsigned k = 0; k < 3; ++k) {
        const uint32_t p = get_u32(vt + lay[k]);
        if (!p || p < 0x00400000u) continue;
        n = snprintf(line, sizeof line, "core.nfsmw: grade probe: layout +0x%x = %08x:", lay[k], p);
        for (uint32_t o = 0x40u; o < 0xe0u && n < (int)sizeof line - 24; o += 4u)
            n += snprintf(line + n, sizeof line - (size_t)n, " %x:%.4g", o, (double)get_float(p + o));
        api->log(api, line);
    }
}
