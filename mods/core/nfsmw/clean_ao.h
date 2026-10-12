/* clean_ao.h - SSAO v1 host inputs (run 13, 8 Oct 2026; review/run13/ssao-ssr-design.md sections 2, 4, 7):
 * horizon-based ambient occlusion from the main view's depth at frame/2, blurred and applied before every other pass
 * of the clean chain (programs: tools/effects/gen_ssao.py). The host gives the projection (camera +0xbc/+0xc0 near and
 * far, +0xc4 vertical FOV, the frame's aspect), the strength (setting ssao_strength) and the TEST views; the manifest
 * gives the shape constants (AoParams, AoFade, AoBlur).
 * Off (setting ssao 0, the default; the Xbox 360 and PC looks force it through the Look-follow table): every clean_ao*
 * pass, the `recopy ao` line and the Ao constants are skipped before any device or effect call, and the AoA/AoB targets
 * are not made, so the frame is today's. An effect or manifest without the Ao parts = AO never on.
 * TEST ONLY: NFSMW_TEST_SSAO_VIEW=1 shows the blurred AO as grey, =2 the depth normals (blur skipped);
 * NFSMW_TEST_SSAO_AB=<s> forces AO off in odd periods of <s> seconds (in-run A/B, logged per change).
 * Run 15 (owner play test 8 Oct, notes 13/16/22: downtown "lots of flicker on buildings", AO adds pop-in in motion):
 * the raw AO is accumulated over frames (clean_ao_temporal into the history AoH) with camera reprojection: the host
 * gives AoReproj0-2 (this AO view -> last frame's AO view, from the camera's world->view matrix at +0x00, row vectors,
 * translation in row 3; AO view = game view with y flipped) and AoTemporal.y (1 = AoH.prev holds last frame's AO:
 * AO drawn on the previous chain frame, no chain reset, no camera cut). AoH is made only while ssao is on.
 * Our own code. Included by clean_chain.h after clean_glow.h. */
#ifndef CLEAN_AO_H
#define CLEAN_AO_H

static uint32_t g_cc_p_aoproj, g_cc_p_aodepth, g_cc_p_aoparams, g_cc_p_aofade, g_cc_p_aoblur;
static uint32_t g_cc_p_aotemp, g_cc_p_aorep[3];   /* run 15 temporal AO (manifest pass clean_ao_temporal) */
static int g_cc_ao_temporal;                      /* the manifest has the temporal pass */
static uint64_t g_cc_ao_hist_frames, g_cc_ao_hist_resets;
static int g_cc_ao_hist_made;      /* run 15: AoH (re)made since the last AO frame */
static uint32_t g_cc_ao_meta_frame; /* run 16 TEST ssao meta: the game frame of this AO frame */
static int g_cc_ao_ready;           /* effect has the Ao parameters and the manifest has ao passes */
static int g_cc_ao_want;            /* setting ssao (read through setting(): the Look applies) */
static int g_cc_ao_on;              /* this frame draws the ao passes */
static int g_cc_ao_targets_ok;      /* AoA/AoB made for this frame size */
static int g_cc_have_depth;         /* cc_e_inputs: the depth copy is valid this frame */
static int64_t g_cc_ao_strength = 100;
static int g_cc_ao_view = -1, g_cc_ao_ab_s = -1, g_cc_ao_ab_phase = -1, g_cc_ao_fail_logged;
static uint64_t g_cc_ao_frames, g_cc_ao_skipped, g_cc_ao_recopies, g_cc_ao_ab_off, g_cc_ao_chain_frames;

/* After the manifest is parsed: resolve the parameters, mark the targets only ao passes use (made lazily) and the
 * Ao manifest constants (set only on frames that draw AO). */
static void cc_ao_resolve(const PopModApi *api) {
    g_cc_p_aoproj = cc_param("AoProj");
    g_cc_p_aodepth = cc_param("AoDepth");
    g_cc_p_aoparams = cc_param("AoParams");
    g_cc_p_aofade = cc_param("AoFade");
    g_cc_p_aoblur = cc_param("AoBlur");
    g_cc_p_aotemp = cc_param("AoTemporal");
    g_cc_p_aorep[0] = cc_param("AoReproj0"); g_cc_p_aorep[1] = cc_param("AoReproj1"); g_cc_p_aorep[2] = cc_param("AoReproj2");
    int passes = 0;
    g_cc_ao_temporal = 0;
    for (unsigned i = 0; i < g_cc_np; ++i) passes += g_cc_p[i].ao && !g_cc_p[i].recopy;
    for (unsigned i = 0; i < g_cc_np; ++i) g_cc_ao_temporal |= g_cc_p[i].ao && g_cc_p[i].dst >= 0 && g_cc_t[g_cc_p[i].dst].history;
    for (unsigned t = 0; t < g_cc_nt; ++t) {
        int by_ao = 0, by_other = 0;
        for (unsigned i = 0; i < g_cc_np; ++i) {
            const CcPass *p = &g_cc_p[i];
            if (p->recopy) continue;
            int uses = p->dst == (int)t;
            for (unsigned s = 0; s < CC_MAXS; ++s) uses |= p->src[s] == (int)t;
            if (uses) { if (p->ao) by_ao = 1; else by_other = 1; }
        }
        g_cc_t[t].ao = by_ao && !by_other;   /* run 15: the AO history AoH too (made with both halves by cc_ao_targets) */
    }
    for (unsigned i = 0; i < g_cc_ng; ++i)
        g_cc_gao[i] = g_cc_gparam[i] && (g_cc_gparam[i] == g_cc_p_aoparams || g_cc_gparam[i] == g_cc_p_aofade || g_cc_gparam[i] == g_cc_p_aoblur ||
                                         (g_cc_p_aotemp && g_cc_gparam[i] == g_cc_p_aotemp));
    g_cc_ao_ready = passes > 0 && g_cc_p_aoproj && g_cc_p_aodepth && g_cc_p_aoparams && g_cc_p_aofade && g_cc_p_aoblur &&
                    (!g_cc_ao_temporal || (g_cc_p_aotemp && g_cc_p_aorep[0] && g_cc_p_aorep[1] && g_cc_p_aorep[2]));
    if (passes) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsmw: ssao: %d passes in the manifest; %s", passes,
                 g_cc_ao_ready ? "ready (setting ssao)" : "the effect lacks the Ao constants: off");
        api->log(api, line);
    }
}

/* Before the targets are made: the setting (live, read every 8 chain frames). */
static void cc_ao_want_update(void) {
    static uint32_t calls;
    if (!g_cc_ao_ready) { g_cc_ao_want = 0; return; }
    if ((calls++ & 7u) == 0) {
        g_cc_ao_want = setting("ssao", 1) != 0;
        g_cc_ao_strength = setting("ssao_strength", 100);
    }
}

/* AoA/AoB: made only while ssao is wanted (no history reset: nothing else reads them), released when it goes off. */
static void cc_ao_targets(const PopModApi *api, uint32_t dev, unsigned out_w, unsigned out_h) {
    g_cc_ao_targets_ok = 0;
    if (!g_cc_ao_ready) return;
    for (unsigned i = 0; i < g_cc_nt; ++i) {
        CcTarget *t = &g_cc_t[i];
        if (!t->ao) continue;
        const unsigned halves = t->history ? 2u : 1u;   /* run 15: AoH is a history pair */
        if (!g_cc_ao_want) {
            for (unsigned k = 0; k < halves; ++k)
                if (t->surf[k]) { pfx_release(&t->surf[k]); pfx_release(&t->tex[k]); t->w[k] = t->h[k] = 0; }
            continue;
        }
        const unsigned w = t->div ? (out_w + t->div - 1) / t->div : t->aw, h = t->div ? (out_h + t->div - 1) / t->div : t->ah;
        for (unsigned k = 0; k < halves; ++k) {
        if (t->surf[k] && t->w[k] == w && t->h[k] == h) continue;
        if (t->history) g_cc_ao_hist_made = 1;   /* a new history holds nothing yet */
        if (!pfx_target(dev, &t->tex[k], &t->surf[k], &t->w[k], &t->h[k], w ? w : 1, h ? h : 1, t->fmt)) {
            if (!g_cc_ao_fail_logged) {
                g_cc_ao_fail_logged = 1;
                char line[160];
                snprintf(line, sizeof line, "core.nfsmw: ssao: target %s (%ux%u) could not be made; AO skipped", t->name, w, h);
                api->log(api, line);
            }
            return;
        }
        char line[160];
        snprintf(line, sizeof line, "core.nfsmw: ssao: target %s%s made (%ux%u)", t->name, t->history ? (k ? ".1" : ".0") : "", w, h);
        api->log(api, line);
        }
    }
    g_cc_ao_targets_ok = g_cc_ao_want;
}

static void cc_ao_log(const PopModApi *api) {
    char line[200];
    snprintf(line, sizeof line, "core.nfsmw: ssao: frames %llu skipped %llu recopies %llu temporal %d (history frames %llu, resets %llu)",
             (unsigned long long)g_cc_ao_frames, (unsigned long long)g_cc_ao_skipped, (unsigned long long)g_cc_ao_recopies,
             g_cc_ao_temporal, (unsigned long long)g_cc_ao_hist_frames, (unsigned long long)g_cc_ao_hist_resets);
    api->log(api, line);
}

static float cc_ao_gval(uint32_t param, unsigned k, float fallback) {   /* a manifest constant's component */
    for (unsigned i = 0; i < g_cc_ng; ++i)
        if (g_cc_gparam[i] == param) return g_cc_gval[i][k];
    return fallback;
}

/* Run 15: AoReproj0-2 (this AO view -> last frame's AO view) and AoTemporal (y = history valid) for this AO frame.
 * Camera +0x00: world -> view, row vectors (v = w R + t, t = row 3), the matrix clean_glow.h projects lamps with. AO view
 * = (v.x, -v.y, v.z). P_last = S (S P - t_c) R_c^-1 R_l + S t_l = P (S A S) + S (t_l - t_c A), A = R_c^-1 R_l. The history
 * is valid when the previous chain frame drew AO, no chain reset or new AoH came between, and the camera moved < 25 m. */
static void cc_ao_reproject(uint32_t cam) {
    static double last[4][4];
    static uint64_t last_chain = 0, last_resets = ~0ull;
    double m[4][4];
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) m[i][j] = get_float(cam + 16u * i + 4u * j);
    int valid = last_chain && last_chain + 1u == g_cc_ao_chain_frames && last_resets == g_cc_resets && !g_cc_ao_hist_made;
    double (*R)[4] = m;
    const double det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                       R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
    float rows[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    if (valid && fabs(det) > 1e-9) {
        double inv[3][3];
        inv[0][0] = (R[1][1] * R[2][2] - R[1][2] * R[2][1]) / det; inv[0][1] = (R[0][2] * R[2][1] - R[0][1] * R[2][2]) / det;
        inv[0][2] = (R[0][1] * R[1][2] - R[0][2] * R[1][1]) / det; inv[1][0] = (R[1][2] * R[2][0] - R[1][0] * R[2][2]) / det;
        inv[1][1] = (R[0][0] * R[2][2] - R[0][2] * R[2][0]) / det; inv[1][2] = (R[0][2] * R[1][0] - R[0][0] * R[1][2]) / det;
        inv[2][0] = (R[1][0] * R[2][1] - R[1][1] * R[2][0]) / det; inv[2][1] = (R[0][1] * R[2][0] - R[0][0] * R[2][1]) / det;
        inv[2][2] = (R[0][0] * R[1][1] - R[0][1] * R[1][0]) / det;
        double A[3][3], b[3];
        static const double sg[3] = {1.0, -1.0, 1.0};
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j) A[i][j] = inv[i][0] * last[0][j] + inv[i][1] * last[1][j] + inv[i][2] * last[2][j];
        for (unsigned j = 0; j < 3; ++j) b[j] = sg[j] * (last[3][j] - (m[3][0] * A[0][j] + m[3][1] * A[1][j] + m[3][2] * A[2][j]));
        if (b[0] * b[0] + b[1] * b[1] + b[2] * b[2] > 25.0 * 25.0) valid = 0;   /* a camera cut or a teleport */
        for (unsigned j = 0; j < 3; ++j) {
            for (unsigned i = 0; i < 3; ++i) rows[j][i] = (float)(sg[i] * A[i][j] * sg[j]);
            rows[j][3] = (float)b[j];
        }
    } else {
        valid = 0;
    }
    {   /* TEST ONLY (run 15 CONF15): NFSMW_TEST_SSAO_TEMPORAL_AB=<s> - odd periods drop the history (no accumulation), the
         * same drive giving with / without temporal AO frames; logged at each phase change. */
        static int tab = -1, tphase = -1;
        if (tab < 0) { const char *e = getenv("NFSMW_TEST_SSAO_TEMPORAL_AB"); tab = e && atoi(e) > 0 ? atoi(e) : 0; }
        if (tab > 0) {
            struct timespec ur;   /* CLOCK_UPTIME_RAW, as the other in-run A/B switches (harness abpairs.py classes dumps by it) */
            clock_gettime(CLOCK_UPTIME_RAW, &ur);
            const uint64_t up = (uint64_t)ur.tv_sec * 1000000000ull + (uint64_t)ur.tv_nsec;
            const int ph = (int)((up / ((uint64_t)tab * 1000000000ull)) & 1u);
            if (ph != tphase) {
                tphase = ph;
                char line[160];
                snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST ssao temporal A/B: history %s (uptime raw %llu ns)",
                         ph ? "off" : "on", (unsigned long long)up);
                g_api->log(g_api, line);
            }
            if (ph) valid = 0;
        }
    }
    {   /* TEST ONLY (run 16): NFSMW_TEST_SSAO_META=1 (also on with NFSMW_TEST_SSAO_TEMPORAL_AB) - one line per AO frame
         * with the state this frame RENDERS with, so a gate classes dumps by it (harness/tools/conf15_gates.py aometa):
         * game frame, AO chain frame, wall (CLOCK_REALTIME, = dump file times) and uptime-raw ns, history used 1/0,
         * history age (consecutive history frames), reset reason (first / gap = AO not drawn on the previous chain frame /
         * chain = chain reset / target = AoH remade / det / cut = camera moved > 25 m / ab = TEST A/B off phase / none),
         * camera move (m) and rotation (deg) since the previous AO frame (rotation part taken as orthonormal; -1 = none). */
        static int meta = -1;
        static uint64_t age, lwall;
        if (meta < 0) {
            const char *e = getenv("NFSMW_TEST_SSAO_META"), *t = getenv("NFSMW_TEST_SSAO_TEMPORAL_AB");
            meta = (e && atoi(e) > 0) || (t && atoi(t) > 0);
        }
        if (meta) {
            double mv = -1.0, rot = -1.0;
            if (last_chain) {
                double c0[3], c1[3], tr = 0.0;
                for (unsigned k = 0; k < 3; ++k) {
                    c0[k] = c1[k] = 0.0;
                    for (unsigned j = 0; j < 3; ++j) { c0[k] -= last[3][j] * last[k][j]; c1[k] -= m[3][j] * m[k][j]; tr += m[k][j] * last[k][j]; }
                }
                mv = sqrt((c1[0] - c0[0]) * (c1[0] - c0[0]) + (c1[1] - c0[1]) * (c1[1] - c0[1]) + (c1[2] - c0[2]) * (c1[2] - c0[2]));
                const double cs = (tr - 1.0) * 0.5;
                rot = acos(cs > 1.0 ? 1.0 : cs < -1.0 ? -1.0 : cs) * 57.29577951308232;
            }
            const char *why = valid ? "none" : !last_chain ? "first" : last_chain + 1u != g_cc_ao_chain_frames ? "gap"
                            : last_resets != g_cc_resets ? "chain" : g_cc_ao_hist_made ? "target" : !(fabs(det) > 1e-9) ? "det"
                            : mv > 25.0 ? "cut" : "ab";
            age = valid ? age + 1u : 0u;
            struct timespec wt, ur;
            clock_gettime(CLOCK_REALTIME, &wt);
            clock_gettime(CLOCK_UPTIME_RAW, &ur);
            const uint64_t wall = (uint64_t)wt.tv_sec * 1000000000ull + (uint64_t)wt.tv_nsec;
            char line[300];
            snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST ssao meta: game %u chain %llu wall %llu up %llu hist %d age %llu "
                     "reset %s move %.3f rot %.3f dt %.2f", g_cc_ao_meta_frame, (unsigned long long)g_cc_ao_chain_frames,
                     (unsigned long long)wall, (unsigned long long)ur.tv_sec * 1000000000ull + (unsigned long long)ur.tv_nsec,
                     valid, (unsigned long long)age, why, mv, rot, lwall ? (double)(wall - lwall) / 1e6 : -1.0);
            g_api->log(g_api, line);
            lwall = wall;
        }
    }
    if (!valid) { for (unsigned j = 0; j < 3; ++j) for (unsigned i = 0; i < 4; ++i) rows[j][i] = i == j ? 1.0f : 0.0f; ++g_cc_ao_hist_resets; }
    else ++g_cc_ao_hist_frames;
    for (unsigned j = 0; j < 3; ++j) cc_set4(g_cc_p_aorep[j], rows[j]);
    const float tp[4] = {cc_ao_gval(g_cc_p_aotemp, 0, 0.2f), valid ? 1.0f : 0.0f, cc_ao_gval(g_cc_p_aotemp, 2, 0.05f), cc_ao_gval(g_cc_p_aotemp, 3, 0.02f)};
    cc_set4(g_cc_p_aotemp, tp);
    memcpy(last, m, sizeof last);
    last_chain = g_cc_ao_chain_frames;
    last_resets = g_cc_resets;
    g_cc_ao_hist_made = 0;
}

/* Every chain frame, after cc_e_inputs (depth validity) and the other modules' inputs. */
static void cc_ao_inputs(const PopModApi *api, unsigned out_w, unsigned out_h, uint32_t game_frame) {
    g_cc_ao_on = 0;
    if (!g_cc_ao_ready) return;
    if (++g_cc_ao_chain_frames % 1800u == 0) cc_ao_log(api);   /* periodic: a test run gates on it */
    if (!g_cc_ao_want || g_cc_ao_strength <= 0) return;
    if (g_cc_ao_view < 0) {   /* TEST ONLY switches, read once */
        const char *v = getenv("NFSMW_TEST_SSAO_VIEW"), *ab = getenv("NFSMW_TEST_SSAO_AB");
        g_cc_ao_view = v && (*v == '1' || *v == '2') ? *v - '0' : 0;
        g_cc_ao_ab_s = ab && *ab ? atoi(ab) : 0;
        if (g_cc_ao_ab_s < 0) g_cc_ao_ab_s = 0;
        if (g_cc_ao_view || g_cc_ao_ab_s) {
            char line[240];
            struct timespec ur;
            clock_gettime(CLOCK_UPTIME_RAW, &ur);
            snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST ssao: view %d, A/B period %d s (clocks: monotonic %llu ns, uptime raw %llu ns)",
                     g_cc_ao_view, g_cc_ao_ab_s, (unsigned long long)pfx_now_ns(),
                     (unsigned long long)ur.tv_sec * 1000000000ull + (unsigned long long)ur.tv_nsec);
            api->log(api, line);
        }
    }
    if (g_cc_ao_ab_s > 0) {   /* TEST ONLY in-run A/B: odd periods off */
        const int phase = (int)((pfx_now_ns() / ((uint64_t)g_cc_ao_ab_s * 1000000000ull)) & 1u);
        if (phase != g_cc_ao_ab_phase) {
            g_cc_ao_ab_phase = phase;
            char line[200];
            snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST ssao A/B: ssao %s (game frame %u, monotonic %llu ns, ao frames %llu, off frames %llu)",
                     phase ? "off" : "on", game_frame, (unsigned long long)pfx_now_ns(), (unsigned long long)g_cc_ao_frames,
                     (unsigned long long)g_cc_ao_ab_off);
            api->log(api, line);
        }
        if (phase) { ++g_cc_ao_ab_off; return; }
    }
    const uint32_t cam = get_u32(0x009195e0u + 0x70u + 0x40u);
    g_cc_ao_meta_frame = game_frame;
    float n = 0.0f, f = 0.0f, tv = 0.0f;
    if (cam) {
        n = get_float(cam + 0xbcu);
        f = get_float(cam + 0xc0u);
        const unsigned fov = get_u32(cam + 0xc4u) & 0xffffu;   /* vertical FOV, 65536 = full turn */
        tv = fov && fov < 32768u ? tanf((float)fov / 65536.0f * 3.14159265f) : 0.0f;
    }
    if (!g_cc_ao_targets_ok || !g_cc_have_depth || !cam || !(n > 0.0f) || !(f > n) || !(tv > 0.0f) || !out_w || !out_h) {
        ++g_cc_ao_skipped;
        return;
    }
    const float aspect = (float)out_w / (float)out_h;
    /* "sky" = beyond the fade end (the sky dome is not at depth 1): no AO there and the apply pass leaves it alone */
    const float zend = cc_ao_gval(g_cc_p_aofade, 1, 100.0f);
    float dcut = zend > n ? (f - n * f / zend) / (f - n) : 0.99999f;
    if (dcut > 0.99999f) dcut = 0.99999f;
    const float proj[4] = {tv * aspect, tv, n * f, f}, dep[4] = {f - n, dcut, 1.0f / f, 0.0f};
    const float par[4] = {cc_ao_gval(g_cc_p_aoparams, 0, 1.0f), cc_ao_gval(g_cc_p_aoparams, 1, 0.03f),
                          (float)g_cc_ao_strength / 100.0f, cc_ao_gval(g_cc_p_aoparams, 3, 0.35f)};
    const float fad[4] = {cc_ao_gval(g_cc_p_aofade, 0, 60.0f), zend, cc_ao_gval(g_cc_p_aofade, 2, 0.08f), cc_ao_gval(g_cc_p_aofade, 3, 0.55f)};
    const float blr[4] = {cc_ao_gval(g_cc_p_aoblur, 0, 0.05f), cc_ao_gval(g_cc_p_aoblur, 1, 0.9f), (float)g_cc_ao_view,
                          g_cc_ao_view == 2 ? 1.0f : 0.0f};
    cc_set4(g_cc_p_aoproj, proj);
    cc_set4(g_cc_p_aodepth, dep);
    cc_set4(g_cc_p_aoparams, par);
    cc_set4(g_cc_p_aofade, fad);
    cc_set4(g_cc_p_aoblur, blr);
    if (g_cc_ao_temporal) cc_ao_reproject(cam);
    g_cc_ao_on = 1;
    ++g_cc_ao_frames;
}

static void cc_ao_exit(const PopModApi *api) {
    if (!g_cc_ao_ready) return;
    cc_ao_log(api);
    if (g_cc_ao_ab_s > 0) {
        char line[160];
        snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST ssao A/B: %llu frames on, %llu frames off",
                 (unsigned long long)g_cc_ao_frames, (unsigned long long)g_cc_ao_ab_off);
        api->log(api, line);
    }
}

#endif
