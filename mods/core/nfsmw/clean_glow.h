/* clean_glow.h - module H host inputs (R2 run 8, 7 Oct 2026; lighting research proposal 3): lamp glow. The 8
 * clean-pool lamps (clean-pools.bin, the PC game's own lamp placements) nearest the camera, within GLOW_RANGE metres
 * and not behind it, go to the clean chain in view space (x right, y down, z forward: P0); the shader integrates
 * their single scattering along each view ray up to the scene depth and fades it where the frame is already bright.
 * No lamp near (or the setting off, or the clean pools off) -> the module's passes are skipped (no draws). Our own
 * code; PC data only. Included by clean_chain.h after clean_sun.h. */
#ifndef CLEAN_GLOW_H
#define CLEAN_GLOW_H

#define GLOW_LAMPS 8u
#define GLOW_RANGE 60.0f
static uint32_t g_cc_p_glamps, g_cc_p_gdepth, g_cc_p_gparams, g_cc_glow_mem;
static int g_cc_glow_on;
static uint64_t g_cc_glow_frames, g_cc_glow_lamps;

static void cc_h_inputs(const PopModApi *api, unsigned out_w, unsigned out_h) {
    g_cc_glow_on = 0;
    if (!g_cc_p_glamps || !g_cc_p_gdepth || !g_cc_p_gparams || !g_cpl.n || !g_cpl_mode || !out_w || !out_h) return;
    static uint32_t calls;
    static int on;
    static float strength = 1.0f;
    if ((calls++ & 255u) == 0) {
        on = setting("lamp_glow", 1) != 0;
        strength = (float)setting("lamp_glow_strength", 100) / 100.0f;
    }
    if (!on || strength <= 0.0f || get_u32(0x00925e90u) != 6u) return;
    const uint32_t cam = get_u32(0x009195e0u + 0x70u + 0x40u);
    if (!cam) return;
    if (!g_cc_glow_mem && api->guest_alloc(api, 16u * 16u, &g_cc_glow_mem) != POP_OK) { g_cc_glow_mem = 0; return; }
    float m[4][4], eye[3];
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) m[i][j] = get_float(cam + 16u * i + 4u * j);
    for (unsigned i = 0; i < 3; ++i) eye[i] = get_float(cam + 0x40u + 4u * i);
    /* candidates: the lamps whose sphere touches the camera's grid cell or its 8 neighbours */
    uint32_t best[GLOW_LAMPS];
    float bestd[GLOW_LAMPS], bestk[GLOW_LAMPS];   /* d2 for the fade; ranking key (d2 / a^2 with lamp_daylight) */
    unsigned nb = 0;
    const int cx = (int)((eye[0] - g_cpl.x0) / CPL_CELL), cy = (int)((eye[1] - g_cpl.y0) / CPL_CELL);
    for (int y = cy - 1; y <= cy + 1; ++y)
        for (int x = cx - 1; x <= cx + 1; ++x) {
            if (x < 0 || y < 0 || x >= (int)g_cpl.gw || y >= (int)g_cpl.gh) continue;
            const uint32_t c = (uint32_t)y * g_cpl.gw + (uint32_t)x;
            for (uint32_t k = g_cpl.cell_first[c]; k < g_cpl.cell_first[c + 1]; ++k) {
                const uint32_t li = g_cpl.cell_items[k];
                const CplLight *L = &g_cpl.l[li];
                const float dx = L->p[0] - eye[0], dy = L->p[1] - eye[1], dz = L->p[2] - eye[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > GLOW_RANGE * GLOW_RANGE) continue;
                const float la = g_ld_active ? g_ld_kind[L->kind ? 1 : 0] : 1.0f;   /* lamp_daylight.h */
                if (g_ld_active && !(la > 0.0f)) continue;
                float key = d2;
                if (g_ld_active && la < 1.0f) { const float aa = la > 1e-3f ? la : 1e-3f; key /= aa * aa; }
                int dup = 0;
                for (unsigned q = 0; q < nb; ++q) dup |= best[q] == li;
                if (dup) continue;
                if (nb < GLOW_LAMPS) { best[nb] = li; bestd[nb] = d2; bestk[nb++] = key; continue; }
                unsigned far = 0;
                for (unsigned q = 1; q < nb; ++q) if (bestk[q] > bestk[far]) far = q;
                if (key < bestk[far]) { best[far] = li; bestd[far] = d2; bestk[far] = key; }
            }
        }
    if (g_vl_apply)   /* vehicle_lights.h (run 13): police bars join the candidates with the same key (high bit set) */
        for (uint32_t j = 0; j < g_vl_n; ++j) {
            const VlLight *V = &g_vl_l[j];
            if (V->kind != VL_KIND_BAR) continue;
            const float dx = V->p[0] - eye[0], dy = V->p[1] - eye[1], dz = V->p[2] - eye[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > GLOW_RANGE * GLOW_RANGE) continue;
            if (nb < GLOW_LAMPS) { best[nb] = 0x80000000u | j; bestd[nb] = d2; bestk[nb++] = d2; continue; }
            unsigned far = 0;
            for (unsigned q = 1; q < nb; ++q) if (bestk[q] > bestk[far]) far = q;
            if (d2 < bestk[far]) { best[far] = 0x80000000u | j; bestd[far] = d2; bestk[far] = d2; }
        }
    float v[16][4];
    memset(v, 0, sizeof v);
    unsigned used = 0;
    unsigned cars = 0;
    for (unsigned q = 0; q < nb; ++q) {
        const int car = (best[q] & 0x80000000u) != 0;                  /* vehicle_lights.h bar */
        const VlLight *V = car ? &g_vl_l[best[q] & 0xffu] : NULL;
        const CplLight *L = car ? NULL : &g_cpl.l[best[q]];
        const float *lp = car ? V->p : L->p, *lc = car ? V->col : L->col;
        float p[3];
        for (unsigned j = 0; j < 3; ++j) p[j] = lp[0] * m[0][j] + lp[1] * m[1][j] + lp[2] * m[2][j] + m[3][j];
        if (p[2] < -2.0f) continue;                                    /* behind the camera */
        const float dist = sqrtf(bestd[q]);
        const float fade = cc_sat((GLOW_RANGE - dist) / 15.0f);        /* no pop at the range edge */
        float k = 0.06f * strength * fade;                              /* medium density x strength (0.025 barely showed, M4) */
        if (car) k *= vl_activation(VL_KIND_BAR, g_vl_a_dark, 0);
        else if (g_ld_active) k *= g_ld_kind[L->kind ? 1 : 0];          /* lamp_daylight.h */
        v[2 * used][0] = p[0]; v[2 * used][1] = p[1]; v[2 * used][2] = p[2]; v[2 * used][3] = 0.25f;   /* core 0.5 m */
        v[2 * used + 1][0] = lc[0] * k; v[2 * used + 1][1] = lc[1] * k; v[2 * used + 1][2] = lc[2] * k;
        ++used;
        cars += car;
    }
    g_vl_glow += cars;
    if (!used) return;
    pfx_put(g_cc_glow_mem, v, sizeof v);
    {
        const uint32_t a[] = {g_cc_p_glamps, g_cc_glow_mem, sizeof v};
        pfx_call(g_cc_fx, 0x50, a, 3);                                  /* SetValue: GlowLamps[16] */
    }
    const float nf_n = get_float(cam + 0xbcu), nf_f = get_float(cam + 0xc0u);
    const float gd[4] = {nf_n * nf_f, nf_f, nf_f - nf_n, 0.0f};
    const unsigned fov = get_u32(cam + 0xc4u) & 0xffffu;
    const float tv = tanf((float)fov / 65536.0f * 3.14159265f), aspect = (float)out_w / (float)out_h;
    const float gp[4] = {tv * aspect, tv, 1.0f, 2.0f};                /* darkness power 2 */
    cc_set4(g_cc_p_gdepth, gd);
    cc_set4(g_cc_p_gparams, gp);
    g_cc_glow_on = 1;
    ++g_cc_glow_frames;
    g_cc_glow_lamps += used;
}

#endif
