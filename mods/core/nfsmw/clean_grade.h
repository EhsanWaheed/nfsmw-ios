/* clean_grade.h - G2 golden colour filter and the mirror grade + glass, host side (run 13, 8 Oct 2026;
 * review/run13/golden-filter-plan.md sections 3-4, review/run13/mirror-parity-design.md M1/M2). Our own code; the game's
 * data only (the visual treatment object [0x00982af0], read every frame, never cached). No Xbox code, table or name.
 * Two routes (owner 8 Oct: the filter matches the Xbox 360 visual treatment's stage order and formulas):
 *  vt (default): the 360 order, our own programs. clean_exposure -> GradeInput (exposed, no bloom); clean_grade_down ->
 *    GradeLuma (frame/4 local luminance s); clean_grade_vt -> FinishInput: curve(s) = cubic in s (x black bloom, yzw
 *    colour bloom), out = desat(c) * curve.x + curve.yzw * c, x brightness (folded into the cubic), then + bloom
 *    (BloomParams.z * BloomV, ungraded, as on the 360); clean_exposure_bloom is skipped. The effect vignette (+) and the
 *    vignette darkening are module D's (FinishParams.z / .x), after the grade: no double vignette, no double bloom.
 *    The cubic comes from the object's curves: power form of the Bezier y (t = s) or, TEST curve=x, a least-squares
 *    cubic of y at x(t) = s; x = BBI Bb, yzw = CBI tint Bc; brightness b = 1 / grey gain at s = pivot (0.5), i.e. the
 *    grade keeps our exposure at the pivot (TEST bright=<b> fixed; 1 = the raw formula). saturation = 1 - Desaturation.
 *  pc (TEST NFSMW_TEST_GRADE_MODE=pc): the PC-data G2 below (LUT route; alt=GradeInput).
 *
 * G2: when golden_on() (setting golden_filter after the Look: Auto = on in Xbox 360, off in Definitive; the PC look
 * never runs the clean chain, it uses the game's own treatment), in game (0x00925e90 == 6), with the object present
 * and the PC treatment flags clear: the effective grade E (object +0x10..+0xaf, the game's own blend, so Speedbreaker
 * and the cop camera follow) or, if E is invalid, the Midday layout [+0x18c], becomes a 256x1 LUT (rebuilt only when
 * an input changes; tools/effects/grade_lut.py is its Python twin). clean_exposure_bloom then writes its alt= target
 * GradeInput and clean_grade writes FinishInput. Off: clean_grade skipped and alt= ignored before any device call,
 * GradeInput not made (the frame is today's).
 * Mirror (M1/M2): after the main passes, when the mirror was drawn this frame and mirror_grade (only with
 * mirror_neutral_exposure on: risk R1) or mirror_glass is on: the mirror surface is copied to the first `mirror`-size
 * target and the mirror=1 passes draw on the mirror texture. Both off: nothing, before any device call.
 * TEST ONLY: NFSMW_TEST_GRADE=force|off; NFSMW_TEST_GRADE_MODE=vt|pc (route); NFSMW_TEST_GRADE_CURVE=t|x (vt cubic);
 * NFSMW_TEST_GRADE_BRIGHT=<b> (vt fixed brightness); NFSMW_TEST_GRADE_PIVOT=<s> (vt, default 0.5);
 * NFSMW_TEST_GRADE_W=<w> strength (both); NFSMW_TEST_GRADE_TINT=add (pc);
 * NFSMW_TEST_GRADE_R=<r> (pc); NFSMW_TEST_GRADE_SCALE=<cbi>,<bbi> (both); NFSMW_TEST_GRADE_AB=<s> grade off in odd periods of <s>
 * seconds (logged per change); NFSMW_TEST_GRADE_LOG=1 object probe every 120 grade frames on change;
 * NFSMW_TEST_MIRROR_PERIOD=<s> mirror grade/glass off in odd periods. Periodic (every 600 chain frames) and exit lines
 * "core.nfsmw: grade: frames N lut builds L source S" and "core.nfsmw: mirror grade: frames N".
 * Included by clean_chain.h after clean_ao.h. */
#ifndef CLEAN_GRADE_H
#define CLEAN_GRADE_H

typedef struct { float bb[4][2], cb[4][2], bbi, cbi, desat, tint[3]; } CcGradeIn;
typedef struct { CcGradeIn in; float r, scale[2]; int src, tint_add; } CcGradeKey;

static uint32_t g_cc_p_grade, g_cc_p_mirror, g_cc_p_gc[4];   /* GradeC0..3: the vt route's cubic (c92-c95) */
static int g_cc_grade_route = 1, g_cc_grade_pc_ready, g_cc_grade_vt_ready, g_cc_grade_mode_pc, g_cc_grade_curve_x;
static float g_cc_grade_bright = -1.0f, g_cc_grade_pivot = 0.5f, g_cc_grade_b = 1.0f, g_cc_grade_c[4][4];
static uint32_t g_cc_grade_lut;              /* 256x1 A8R8G8B8, managed; released with the targets */
static int g_cc_grade_ready, g_cc_grade_on, g_cc_grade_key_ok, g_cc_grade_src = -1, g_cc_grade_want;
static CcGradeKey g_cc_grade_key;
static int g_cc_grade_env = -1, g_cc_grade_tint_add, g_cc_grade_ab_s, g_cc_grade_ab_phase = -1, g_cc_grade_log;
static float g_cc_grade_w = 1.0f, g_cc_grade_r = 1.0f, g_cc_grade_scale[2] = {1.0f, 1.0f};
static uint64_t g_cc_grade_frames, g_cc_grade_builds, g_cc_grade_midday, g_cc_grade_chain_frames, g_cc_grade_ab_off;
static int g_cc_grade_fail_logged;
static int g_cc_mirror_fresh;                /* g_mirror_fresh latched (and cleared) by pfx_frame_body */
static unsigned g_cc_mirror_np, g_cc_mirror_w, g_cc_mirror_h;
static uint32_t g_cc_mirror_tex, g_cc_mirror_surf;
static int g_cc_mirror_ab_s = -1, g_cc_mirror_ab_phase = -1, g_cc_mirror_fail_logged;
static uint64_t g_cc_mirror_frames, g_cc_mirror_stale, g_cc_mirror_ab_off;

static const char *cc_grade_route_name(void) { return g_cc_grade_route == 2 ? "vt" : "pc"; }
static const char *cc_grade_src_name(void) { return g_cc_grade_src == 0 ? "E" : g_cc_grade_src == 1 ? "midday" : "none"; }

static void cc_grade_release(void) {
    pfx_release(&g_cc_grade_lut);
    g_cc_grade_key_ok = 0;                   /* a recreated LUT is always filled */
}

/* After the manifest is parsed: parameters, lazy targets, checks. 0 = a manifest error (g_cc_fail says which). */
static int cc_grade_resolve(void) {
    g_cc_p_grade = cc_param("GradeParams");
    g_cc_p_mirror = cc_param("MirrorParams");
    int grade_pass = 0, alt = 0, vt_pass = 0, gc = 1;
    for (unsigned c = 0; c < 4; ++c) {
        char n[16];
        snprintf(n, sizeof n, "GradeC%u", c);
        gc &= (g_cc_p_gc[c] = cc_param(n)) != 0;
    }
    g_cc_mirror_np = 0;
    for (unsigned i = 0; i < g_cc_np; ++i) {
        const CcPass *p = &g_cc_p[i];
        if (p->recopy) continue;
        grade_pass |= p->grade && (p->gmask & 2);
        vt_pass |= p->grade && (p->gmask & 4) && !strcmp(p->tech, "clean_grade_vt");
        alt |= p->alt >= 0;
        if (p->mirror) ++g_cc_mirror_np;
        int bad = p->mirror ? p->dst != CC_MIRROR : p->dst == CC_MIRROR;
        for (unsigned s = 0; s < CC_MAXS; ++s) {
            const int t = p->src[s];
            if (t == CC_MIRROR && p->mirror) bad = 1;   /* the mirror pass reads its copy, never its destination */
            if (t >= 0 && g_cc_t[t].mirror && !p->mirror) bad = 1;
        }
        if (p->dst >= 0 && g_cc_t[p->dst].mirror && !p->mirror) bad = 1;
        if (bad) { snprintf(g_cc_fail, sizeof g_cc_fail, "pass %s: `mirror` targets belong to mirror=1 passes only", p->tech); return 0; }
    }
    g_cc_grade_pc_ready = g_cc_p_grade && grade_pass && alt;
    g_cc_grade_vt_ready = g_cc_p_grade && gc && vt_pass;
    g_cc_grade_ready = g_cc_grade_pc_ready || g_cc_grade_vt_ready;
    for (unsigned t = 0; t < g_cc_nt; ++t) {   /* made only while a grade route using it is on (bit 1 pc, bit 2 vt) */
        int by_grade = 0, by_other = 0;
        for (unsigned i = 0; i < g_cc_np; ++i) {
            const CcPass *p = &g_cc_p[i];
            if (p->recopy) continue;
            int uses = p->dst == (int)t;
            for (unsigned s = 0; s < CC_MAXS; ++s) uses |= p->src[s] == (int)t;
            if (p->alt == (int)t) by_grade |= 2;
            if (uses && p->grade) by_grade |= p->gmask & 6;
            else if (uses) by_other = 1;
        }
        g_cc_t[t].grade = by_other ? 0 : by_grade;
    }
    if (g_cc_mirror_np && !g_cc_p_mirror) g_cc_mirror_np = 0;
    return 1;
}

/* grade-math begin (pure: tools/effects/grade_lut.py is the twin; the offline C check compiles these regions) */
static float cc_bez(const float p[4][2], int axis, float t) {
    const float u = 1.0f - t;
    return u * u * u * p[0][axis] + 3.0f * u * u * t * p[1][axis] + 3.0f * u * t * t * p[2][axis] + t * t * t * p[3][axis];
}
static float cc_curve(const float p[4][2], float x) {   /* y at x(t) = x: 24 bisection steps on the raw curve */
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float m = 0.5f * (lo + hi);
        if (cc_bez(p, 0, m) < x) lo = m; else hi = m;
    }
    return cc_bez(p, 1, 0.5f * (lo + hi));
}
static float cc_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* vt route: the cubic a0 + a1 s + a2 s^2 + a3 s^3 of one curve. curve_x 0: power form of the Bezier y (t = s, the form
 * the 360's cubic evaluates); 1: least-squares cubic through y at x(t) = s, s = 0, 1/32 .. 1 (normal equations). */
static void cc_vt_cubic(const float p[4][2], int curve_x, double a[4]) {
    if (!curve_x) {
        a[0] = p[0][1];
        a[1] = 3.0 * ((double)p[1][1] - p[0][1]);
        a[2] = 3.0 * ((double)p[0][1] - 2.0 * p[1][1] + p[2][1]);
        a[3] = (double)p[3][1] - 3.0 * p[2][1] + 3.0 * p[1][1] - p[0][1];
        return;
    }
    double m[4][5] = {{0}};
    for (int j = 0; j <= 32; ++j) {
        const double x = j / 32.0, y = cc_curve(p, (float)x);
        double pw[4] = {1.0, x, x * x, x * x * x};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) m[r][c] += pw[r] * pw[c];
            m[r][4] += pw[r] * y;
        }
    }
    for (int c = 0; c < 4; ++c) {                 /* Gauss-Jordan, partial pivot */
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(m[r][c]) > fabs(m[piv][c])) piv = r;
        for (int k = 0; k < 5; ++k) { const double t = m[c][k]; m[c][k] = m[piv][k]; m[piv][k] = t; }
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c] / m[c][c];
            for (int k = c; k < 5; ++k) m[r][k] -= f * m[c][k];
        }
    }
    for (int c = 0; c < 4; ++c) a[c] = m[c][4] / m[c][c];
}

/* vt route: GradeC0..3 (c[k] = (x black bloom, yzw colour bloom) of s^k) with the brightness folded in; returns b.
 * x = BBI Bb(s), yzw = CBI tint Bc(s); b = bright when >= 0, else 1 / (x + Y.yzw) at s = pivot, clamped 0.25..4. */
static float cc_vt_coeffs(const CcGradeIn *g, int curve_x, float bright, float pivot, const float scale[2], float c[4][4]) {
    static const double Y[3] = {0.2126, 0.7152, 0.0722};
    double ab[4], ac[4];
    cc_vt_cubic(g->bb, curve_x, ab);
    cc_vt_cubic(g->cb, curve_x, ac);
    const double bbi = (double)g->bbi * scale[1], cbi = (double)g->cbi * scale[0];
    double k[4][4];
    for (int i = 0; i < 4; ++i) {
        k[i][0] = bbi * ab[i];
        for (int ch = 0; ch < 3; ++ch) k[i][1 + ch] = cbi * g->tint[ch] * ac[i];
    }
    double b = bright;
    if (b < 0.0) {
        const double s = pivot;
        double gain = 0.0, pw = 1.0;
        for (int i = 0; i < 4; ++i, pw *= s) gain += pw * (k[i][0] + Y[0] * k[i][1] + Y[1] * k[i][2] + Y[2] * k[i][3]);
        b = gain > 1e-3 ? 1.0 / gain : 1.0;
        b = b < 0.25 ? 0.25 : b > 4.0 ? 4.0 : b;
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) c[i][j] = (float)(b * k[i][j]);
    return (float)b;
}
/* grade-math end */

/* E (the object's effective grade) or a layout, into one form; 0 when any value is out of range. */
static int cc_grade_read(uint32_t base, int layout, CcGradeIn *g) {
    const uint32_t bb = base + (layout ? 0x40u : 0x10u), cb = base + (layout ? 0x80u : 0x50u);
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned a = 0; a < 2; ++a) {
            g->bb[i][a] = get_float(bb + 16u * i + 4u * a);
            g->cb[i][a] = get_float(cb + 16u * i + 4u * a);
        }
    g->bbi = get_float(base + (layout ? 0xdcu : 0x90u));
    g->cbi = get_float(base + (layout ? 0xd0u : 0x94u));
    g->desat = get_float(base + (layout ? 0xd4u : 0x98u));
    for (unsigned c = 0; c < 3; ++c) g->tint[c] = get_float(base + (layout ? 0xc0u : 0xa0u) + 4u * c);
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned a = 0; a < 2; ++a)
            if (!isfinite(g->bb[i][a]) || !isfinite(g->cb[i][a]) || fabsf(g->bb[i][a]) >= 4.0f || fabsf(g->cb[i][a]) >= 4.0f) return 0;
    const float s[3] = {g->bbi, g->cbi, g->desat};
    for (unsigned i = 0; i < 3; ++i) if (!isfinite(s[i]) || s[i] < 0.0f || s[i] > 4.0f) return 0;
    for (unsigned c = 0; c < 3; ++c) if (!isfinite(g->tint[c]) || g->tint[c] < 0.0f || g->tint[c] > 2.0f) return 0;
    return 1;
}

/* The LUT (grade_lut.py is the twin): texel i = (B, G, R, A) bytes of T(L) and k * 128, L = i / 255. */
static void cc_grade_build(const CcGradeKey *k, uint8_t out[256][4]) {
    static const float Y[3] = {0.2126f, 0.7152f, 0.0722f};
    const CcGradeIn *g = &k->in;
    const float cbi = g->cbi * k->scale[0], bbi = g->bbi * k->scale[1];
    float ty = g->tint[0] * Y[0] + g->tint[1] * Y[1] + g->tint[2] * Y[2];
    if (ty < 1e-3f) ty = 1e-3f;
    for (unsigned i = 0; i < 256; ++i) {
        const float L = (float)i / 255.0f;
        const float L1 = L + bbi * (cc_curve(g->bb, L) - L);
        const float kc = cbi * cc_clampf(cc_curve(g->cb, L), 0.0f, 1.0f);
        float T[3];
        for (unsigned c = 0; c < 3; ++c)
            T[c] = k->tint_add ? L1 + kc * g->tint[c] : L1 * (1.0f + kc * (g->tint[c] / ty - 1.0f));
        const float ratio = i ? L1 / (L > 1.0f / 255.0f ? L : 1.0f / 255.0f) : 1.0f;
        const float kk = cc_clampf((1.0f - cc_clampf(g->desat, 0.0f, 1.0f)) * (1.0f + k->r * (ratio - 1.0f)), 0.0f, 1.99f);
        out[i][0] = (uint8_t)lroundf(255.0f * cc_clampf(T[2], 0.0f, 1.0f));
        out[i][1] = (uint8_t)lroundf(255.0f * cc_clampf(T[1], 0.0f, 1.0f));
        out[i][2] = (uint8_t)lroundf(255.0f * cc_clampf(T[0], 0.0f, 1.0f));
        out[i][3] = (uint8_t)lroundf(128.0f * kk);
    }
}

static void cc_grade_log(const PopModApi *api) {
    char line[240];
    if (g_cc_grade_ready) {
        snprintf(line, sizeof line, "core.nfsmw: grade: frames %llu lut builds %llu source %s route %s b %.3f (midday fallbacks %llu, A/B off %llu)",
                 (unsigned long long)g_cc_grade_frames, (unsigned long long)g_cc_grade_builds, cc_grade_src_name(),
                 cc_grade_route_name(), (double)g_cc_grade_b,
                 (unsigned long long)g_cc_grade_midday, (unsigned long long)g_cc_grade_ab_off);
        api->log(api, line);
    }
    if (g_cc_mirror_np) {
        snprintf(line, sizeof line, "core.nfsmw: mirror grade: frames %llu (mirror not drawn %llu, A/B off %llu, last %ux%u)",
                 (unsigned long long)g_cc_mirror_frames, (unsigned long long)g_cc_mirror_stale,
                 (unsigned long long)g_cc_mirror_ab_off, g_cc_mirror_w, g_cc_mirror_h);
        api->log(api, line);
    }
}

static void cc_grade_env(const PopModApi *api) {
    const char *e = getenv("NFSMW_TEST_GRADE"), *w = getenv("NFSMW_TEST_GRADE_W"), *t = getenv("NFSMW_TEST_GRADE_TINT"),
               *r = getenv("NFSMW_TEST_GRADE_R"), *sc = getenv("NFSMW_TEST_GRADE_SCALE"), *ab = getenv("NFSMW_TEST_GRADE_AB"),
               *lg = getenv("NFSMW_TEST_GRADE_LOG"), *md = getenv("NFSMW_TEST_GRADE_MODE"), *cv = getenv("NFSMW_TEST_GRADE_CURVE"),
               *br = getenv("NFSMW_TEST_GRADE_BRIGHT"), *pv = getenv("NFSMW_TEST_GRADE_PIVOT");
    g_cc_grade_mode_pc = md && !strcmp(md, "pc");
    g_cc_grade_curve_x = cv && !strcmp(cv, "x");
    if (br && *br) g_cc_grade_bright = cc_clampf((float)atof(br), 0.0f, 4.0f);
    if (pv && *pv) g_cc_grade_pivot = cc_clampf((float)atof(pv), 0.0f, 1.0f);
    g_cc_grade_env = e && !strcmp(e, "force") ? 1 : e && !strcmp(e, "off") ? 2 : 0;
    if (w && *w) g_cc_grade_w = cc_clampf((float)atof(w), 0.0f, 1.0f);
    g_cc_grade_tint_add = t && !strcmp(t, "add");
    if (r && *r) g_cc_grade_r = (float)atof(r);
    if (sc && *sc) { float a = 1.0f, b = 1.0f; if (sscanf(sc, "%f,%f", &a, &b) >= 1) { g_cc_grade_scale[0] = a; g_cc_grade_scale[1] = b; } }
    g_cc_grade_ab_s = ab && *ab ? atoi(ab) : 0;
    if (g_cc_grade_ab_s < 0) g_cc_grade_ab_s = 0;
    g_cc_grade_log = lg && *lg == '1';
    if (g_cc_grade_env || (w && *w) || g_cc_grade_tint_add || (r && *r) || (sc && *sc) || g_cc_grade_ab_s || g_cc_grade_log ||
        (md && *md) || (cv && *cv) || (br && *br) || (pv && *pv)) {
        char line[320];
        snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST grade: mode %d, route %s, curve %s, bright %.3f, pivot %.3f, w %.3f, tint %s, r %.3f, "
                 "scale cbi %.3f bbi %.3f, A/B %d s, probe %d",
                 g_cc_grade_env, g_cc_grade_mode_pc ? "pc" : "vt", g_cc_grade_curve_x ? "x" : "t", (double)g_cc_grade_bright,
                 (double)g_cc_grade_pivot, (double)g_cc_grade_w, g_cc_grade_tint_add ? "add" : "mul", (double)g_cc_grade_r,
                 (double)g_cc_grade_scale[0], (double)g_cc_grade_scale[1], g_cc_grade_ab_s, g_cc_grade_log);
        api->log(api, line);
    }
}

/* TEST probe: the mode word, E vs Midday, the event amounts, the cop-cam layout tint, the PC flags. */
static void cc_grade_probe(const PopModApi *api, uint32_t vt, const CcGradeIn *e, int e_ok) {
    static uint64_t last;
    static uint32_t last_sig;
    const uint32_t mid = get_u32(vt + 0x18cu), cop = get_u32(vt + 0x1b4u);
    CcGradeIn m;
    const int m_ok = mid && cc_grade_read(mid, 1, &m);
    const int same = e_ok && m_ok && !memcmp(e, &m, sizeof m);
    const uint32_t sig = get_u32(vt) * 31u ^ (uint32_t)same * 7u ^ (uint32_t)(e_ok ? e->tint[2] * 1000.0f : 0.0f);
    if (last && (sig == last_sig || g_cc_grade_frames - last < 120u)) return;   /* on change, at most every 120 frames */
    last = g_cc_grade_frames; last_sig = sig;
    char line[480];
    snprintf(line, sizeof line, "core.nfsmw: TEST grade probe: mode %u, E valid %d, E==midday %d, E tint %.3f %.3f %.3f cbi %.3f desat %.3f bbi %.3f, "
             "+174 %.3f +178 %.3f +17c %.3f +180 %.3f, cop layout %08x tint %.3f %.3f %.3f, flags %u %u, builds %llu, route %s b %.3f",
             get_u32(vt), e_ok, same, e_ok ? (double)e->tint[0] : 0.0, e_ok ? (double)e->tint[1] : 0.0, e_ok ? (double)e->tint[2] : 0.0,
             e_ok ? (double)e->cbi : 0.0, e_ok ? (double)e->desat : 0.0, e_ok ? (double)e->bbi : 0.0,
             (double)get_float(vt + 0x174u), (double)get_float(vt + 0x178u), (double)get_float(vt + 0x17cu), (double)get_float(vt + 0x180u),
             cop, cop ? (double)get_float(cop + 0xc0u) : 0.0, cop ? (double)get_float(cop + 0xc4u) : 0.0, cop ? (double)get_float(cop + 0xc8u) : 0.0,
             get_u32(0x00901828u), get_u32(0x009017fcu), (unsigned long long)g_cc_grade_builds, cc_grade_route_name(), (double)g_cc_grade_b);
    api->log(api, line);
}

/* Every clean-chain frame, before the passes: decides g_cc_grade_on and sets the grade inputs. */
static void cc_grade_inputs(const PopModApi *api, uint32_t dev, unsigned out_w, unsigned out_h, uint32_t game_frame) {
    g_cc_grade_on = 0;
    if (!g_cc_grade_ready && !g_cc_mirror_np) return;
    if (++g_cc_grade_chain_frames % 600u == 0) cc_grade_log(api);   /* periodic: the combined run gates on it */
    if (!g_cc_grade_ready) return;
    if (g_cc_grade_env < 0) cc_grade_env(api);
    static uint32_t calls;
    if ((calls++ & 7u) == 0) g_cc_grade_want = golden_on();          /* live; the Look applies (nfsmw.c) */
    const int want = g_cc_grade_env == 1 ? 1 : g_cc_grade_env == 2 ? 0 : g_cc_grade_want;
    if (!want) {   /* off: the grade route's targets go (nothing to release when never made: no device call) */
        for (unsigned i = 0; i < g_cc_nt; ++i)
            if (g_cc_t[i].grade && g_cc_t[i].surf[0]) {
                pfx_release(&g_cc_t[i].surf[0]);
                pfx_release(&g_cc_t[i].tex[0]);
                g_cc_t[i].w[0] = g_cc_t[i].h[0] = 0;
            }
        return;
    }
    const int route = g_cc_grade_vt_ready && !g_cc_grade_mode_pc ? 2 : 1;   /* vt (360 order) unless TEST pc or no vt passes */
    if (route == 1 && !g_cc_grade_pc_ready) return;
    {
        static int logged;
        if (!logged++) {
            char line[200];
            snprintf(line, sizeof line, "core.nfsmw: grade: route %s (%s)", route == 2 ? "vt" : "pc",
                     route == 2 ? "360 order: curve + desaturation, brightness, bloom; vignettes in finish" : "PC-data LUT");
            api->log(api, line);
        }
    }
    const uint32_t vt = get_u32(0x00982af0u);
    if (get_u32(0x00925e90u) != 6u || !vt || get_u32(0x00901828u) || get_u32(0x009017fcu)) return;
    if (g_cc_grade_ab_s > 0) {   /* TEST ONLY in-run A/B: odd periods off */
        const int phase = (int)((pfx_now_ns() / ((uint64_t)g_cc_grade_ab_s * 1000000000ull)) & 1u);
        if (phase != g_cc_grade_ab_phase) {
            g_cc_grade_ab_phase = phase;
            char line[220];
            snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST grade A/B: grade %s (game frame %u, monotonic %llu ns, grade frames %llu, off frames %llu)",
                     phase ? "off" : "on", game_frame, (unsigned long long)pfx_now_ns(), (unsigned long long)g_cc_grade_frames,
                     (unsigned long long)g_cc_grade_ab_off);
            api->log(api, line);
        }
        if (phase) { ++g_cc_grade_ab_off; return; }
    }
    CcGradeKey k;
    memset(&k, 0, sizeof k);
    const int e_ok = cc_grade_read(vt, 0, &k.in);
    if (g_cc_grade_log) cc_grade_probe(api, vt, &k.in, e_ok);
    if (e_ok) k.src = 0;
    else {
        const uint32_t mid = get_u32(vt + 0x18cu);
        if (!mid || !cc_grade_read(mid, 1, &k.in)) {
            if (!g_cc_grade_fail_logged++) api->log(api, "core.nfsmw: grade: no valid colour data (object and Midday layout); grade off");
            return;
        }
        k.src = 1;
        if (!g_cc_grade_midday++) api->log(api, "core.nfsmw: grade: the object's grade is out of range; the Midday layout is used");
    }
    k.r = g_cc_grade_r;
    k.scale[0] = g_cc_grade_scale[0];
    k.scale[1] = g_cc_grade_scale[1];
    k.tint_add = g_cc_grade_tint_add;
    for (unsigned i = 0; i < g_cc_nt; ++i) {   /* the route's targets at their size (lazy); the other route's go */
        CcTarget *t = &g_cc_t[i];
        if (!t->grade) continue;
        if (!(t->grade >> route & 1)) {
            if (t->surf[0]) { pfx_release(&t->surf[0]); pfx_release(&t->tex[0]); t->w[0] = t->h[0] = 0; }
            continue;
        }
        const unsigned w = t->div ? (out_w + t->div - 1) / t->div : t->aw, h = t->div ? (out_h + t->div - 1) / t->div : t->ah;
        if (!pfx_target(dev, &t->tex[0], &t->surf[0], &t->w[0], &t->h[0], w ? w : 1, h ? h : 1, t->fmt)) {
            if (!g_cc_grade_fail_logged++) api->log(api, "core.nfsmw: grade: target could not be made; grade off");
            return;
        }
    }
    if (route == 2) {   /* vt: the cubic and the saturation are constants (no LUT, no device call but SetValue) */
        g_cc_grade_b = cc_vt_coeffs(&k.in, g_cc_grade_curve_x, g_cc_grade_bright, g_cc_grade_pivot, g_cc_grade_scale, g_cc_grade_c);
        for (unsigned c = 0; c < 4; ++c) cc_set4(g_cc_p_gc[c], g_cc_grade_c[c]);
        const float gp[4] = {g_cc_grade_w, 1.0f - cc_clampf(k.in.desat, 0.0f, 1.0f), 0.0f, 0.0f};
        cc_set4(g_cc_p_grade, gp);
        g_cc_grade_src = k.src;
        g_cc_grade_route = 2;
        g_cc_grade_on = 1;
        ++g_cc_grade_frames;
        return;
    }
    if (!g_cc_grade_lut) {
        const uint32_t a[] = {256, 1, 1, 0, 21, 1 /* MANAGED */, CC_OUT, 0};
        if (pfx_call(dev, 0x5c, a, 8) != 0 || !(g_cc_grade_lut = get_u32(CC_OUT))) {      /* CreateTexture */
            g_cc_grade_lut = 0;
            if (!g_cc_grade_fail_logged++) api->log(api, "core.nfsmw: grade: LUT texture could not be made; grade off");
            return;
        }
        g_cc_grade_key_ok = 0;
    }
    if (!g_cc_grade_key_ok || memcmp(&k, &g_cc_grade_key, sizeof k)) {
        uint8_t texels[256][4];
        cc_grade_build(&k, texels);
        const uint32_t lk[] = {0, CC_OUT + 0x20u, 0, 0};
        if (pfx_call(g_cc_grade_lut, 0x4c, lk, 4) != 0 || !get_u32(CC_OUT + 0x24u)) {     /* LockRect */
            if (!g_cc_grade_fail_logged++) api->log(api, "core.nfsmw: grade: LUT lock failed; grade off");
            return;
        }
        pfx_put(get_u32(CC_OUT + 0x24u), texels, sizeof texels);
        const uint32_t ul[] = {0};
        pfx_call(g_cc_grade_lut, 0x50, ul, 1);                                              /* UnlockRect */
        g_cc_grade_key = k;
        g_cc_grade_key_ok = 1;
        ++g_cc_grade_builds;
    }
    g_cc_grade_src = k.src;
    const float gp[4] = {g_cc_grade_w, 0.0f, 0.0f, 0.0f};
    cc_set4(g_cc_p_grade, gp);
    g_cc_grade_route = 1;
    g_cc_grade_on = 1;
    ++g_cc_grade_frames;
}

/* After the main passes: 1 = draw the mirror=1 passes now (the copy is made, MirrorParams set); cc_mirror_done after. */
static int cc_mirror_prepare(const PopModApi *api, uint32_t dev) {
    const int fresh = g_cc_mirror_fresh;
    g_cc_mirror_fresh = 0;
    if (!g_cc_mirror_np) return 0;
    static uint32_t calls;
    static int64_t grade, glass, neutral = 1;
    if ((calls++ & 7u) == 0) {
        grade = setting("mirror_grade", 0);
        glass = setting("mirror_glass", 0);
        neutral = setting("mirror_neutral_exposure", 1);
    }
    const int g = grade && neutral;
    const int64_t gl = glass < 0 ? 0 : glass > 100 ? 100 : glass;
    if (!g && !gl) {   /* off: the mirror targets go (none made: no device call) */
        for (unsigned i = 0; i < g_cc_nt; ++i)
            if (g_cc_t[i].mirror && g_cc_t[i].surf[0]) {
                pfx_release(&g_cc_t[i].surf[0]);
                pfx_release(&g_cc_t[i].tex[0]);
                g_cc_t[i].w[0] = g_cc_t[i].h[0] = 0;
            }
        return 0;
    }
    if (!fresh) { ++g_cc_mirror_stale; return 0; }   /* not drawn this frame: never grade a mirror twice */
    if (g_cc_mirror_ab_s < 0) {
        const char *e = getenv("NFSMW_TEST_MIRROR_PERIOD");
        g_cc_mirror_ab_s = e && *e ? atoi(e) : 0;
        if (g_cc_mirror_ab_s < 0) g_cc_mirror_ab_s = 0;
    }
    if (g_cc_mirror_ab_s > 0) {   /* TEST ONLY in-run A/B: odd periods off */
        const int phase = (int)((pfx_now_ns() / ((uint64_t)g_cc_mirror_ab_s * 1000000000ull)) & 1u);
        if (phase != g_cc_mirror_ab_phase) {
            g_cc_mirror_ab_phase = phase;
            char line[200];
            snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST mirror A/B: mirror grade/glass %s (monotonic %llu ns, mirror frames %llu)",
                     phase ? "off" : "on", (unsigned long long)pfx_now_ns(), (unsigned long long)g_cc_mirror_frames);
            api->log(api, line);
        }
        if (phase) { ++g_cc_mirror_ab_off; return 0; }
    }
    const uint32_t tex = get_u32(0x00982a7cu);
    if (!tex) return 0;
    const uint32_t s[] = {0, CC_OUT};
    if (pfx_call(tex, 0x48, s, 2) != 0 || !get_u32(CC_OUT)) return 0;                     /* GetSurfaceLevel */
    g_cc_mirror_surf = get_u32(CC_OUT);
    const uint32_t d[] = {CC_OUT + 8u};
    if (pfx_call(g_cc_mirror_surf, 0x30, d, 1) != 0) { pfx_release(&g_cc_mirror_surf); return 0; }   /* GetDesc */
    g_cc_mirror_w = get_u32(CC_OUT + 8u + 24u);
    g_cc_mirror_h = get_u32(CC_OUT + 8u + 28u);
    int copy = -1;
    for (unsigned i = 0; i < g_cc_nt; ++i) {
        CcTarget *t = &g_cc_t[i];
        if (!t->mirror) continue;
        if (copy < 0) copy = (int)i;
        if (!g_cc_mirror_w || !g_cc_mirror_h ||
            !pfx_target(dev, &t->tex[0], &t->surf[0], &t->w[0], &t->h[0], g_cc_mirror_w, g_cc_mirror_h, t->fmt)) {
            if (!g_cc_mirror_fail_logged++) api->log(api, "core.nfsmw: mirror grade: mirror copy could not be made; off");
            pfx_release(&g_cc_mirror_surf);
            return 0;
        }
    }
    if (copy < 0) { pfx_release(&g_cc_mirror_surf); return 0; }
    const uint32_t sr[] = {g_cc_mirror_surf, 0, g_cc_t[copy].surf[0], 0, 0 /* NONE */};
    if (pfx_call(dev, 0x88, sr, 5) != 0) { ++g_cc_pass_errors; pfx_release(&g_cc_mirror_surf); return 0; }   /* StretchRect */
    g_cc_mirror_tex = tex;
    const float mp[4] = {g ? 1.0f : 0.0f, (float)gl / 100.0f, (float)g_cc_mirror_w, (float)g_cc_mirror_h};
    cc_set4(g_cc_p_mirror, mp);
    {
        static int logged;
        if (!logged++) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsmw: mirror grade: first mirror frame %ux%u (grade %d, glass %d %%)",
                     g_cc_mirror_w, g_cc_mirror_h, g, (int)gl);
            api->log(api, line);
        }
    }
    return 1;
}
static void cc_mirror_done(void) {
    pfx_release(&g_cc_mirror_surf);
    g_cc_mirror_tex = 0;
    ++g_cc_mirror_frames;
}

static void cc_grade_exit(const PopModApi *api) {
    cc_grade_log(api);
}
#endif
