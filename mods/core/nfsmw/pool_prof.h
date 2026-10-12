/* pool_prof.h - TEST ONLY (Remaster P1, 7 Oct 2026; review/2026-10-07-props-measurement-design.md, challenged):
 * per-frame counts and times of the clean light-pool passes, to attribute the prop-pools cost (QA_LOG C9-C11).
 * NFSMW_TEST_POOL_PROF=<csv path>: one row per game frame (device update), stamped with CLOCK_UPTIME_RAW (the clock
 * of frames.csv sealed_s and of the kit's prof CSV): pool entries scheduled per pass (WORLD / WORLDREFLECT /
 * WORLDNORMALMAP) and view (main / other), explicit pass-0 restores, cpl_world_entry inclusive ns and each SetValue's
 * ns, SetValues whose bytes repeat the last ones for that pass, lamps per entry, and a measure-only
 * zero-contribution class (beyond the shader's camera-distance fade end + 1 m with the main eye and world box; or
 * strength <= 0; or all selected lamps black; other views and non-world entries: unknown).
 * NFSMW_TEST_PROPS_PERIOD=<s>: in odd periods of that clock (latched once per frame) WORLDNORMALMAP entries are not
 * scheduled (props off; effect copies stay loaded). Neither changes anything unless set. Included by
 * world_light_pass.h. */
#ifndef POOL_PROF_H
#define POOL_PROF_H
#include <time.h>

typedef struct {
    uint32_t sched[3][2];          /* [pass k][0 main view, 1 other] scheduled pool entries */
    uint32_t props_skipped;        /* WORLDNORMALMAP entries left unscheduled by the props toggle */
    uint32_t restores_in_list, restores_list_end;
    uint64_t entry_ns, sv_ns[3];
    uint32_t sv_calls, sv_repeat[3];
    uint32_t lamps[9];
    uint32_t zero_fade, zero_strength, zero_colour, zero_unknown, maybe_lit;
} PoolProf;
static PoolProf g_pp;
static int g_pp_on = -1;
static FILE *g_pp_csv;
static uint64_t g_pp_period_ns;
static int g_pp_props_old;
static uint8_t g_pp_zclass[X3SL_MAX_ENTRIES];   /* per entry: 0 may be lit, 1 beyond the fade, 2 unknown */

/* Run 10 (TEST ONLY): NFSMW_TEST_FUSION_PERIOD=<s>: in odd periods of the same clock the pool fusion is off (two-pass),
 * so one run measures fused (NEW, even) against two-pass (OLD, odd). */
static uint64_t g_pp_fusion_period_ns;
static int g_pp_fusion_old;
static uint64_t pp_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int pp_on(void) {
    if (g_pp_on < 0) {
        const char *p = getenv("NFSMW_TEST_POOL_PROF");
        g_pp_csv = p && *p ? fopen(p, "w") : NULL;
        g_pp_on = g_pp_csv != NULL;
        if (g_pp_csv)
            fprintf(g_pp_csv, "t_s,world_main,world_other,reflect_main,reflect_other,props_main,props_other,props_skipped,"
                              "restores_in_list,restores_list_end,entry_ms,sv0_ms,sv1_ms,sv2_ms,sv_calls,sv0_repeat,"
                              "sv1_repeat,sv2_repeat,l1,l2,l3,l4,l5,l6,l7,l8,zero_fade,zero_strength,zero_colour,"
                              "zero_unknown,maybe_lit,props_old\n");
        const char *t = getenv("NFSMW_TEST_PROPS_PERIOD");
        const long s = t ? strtol(t, NULL, 10) : 0;
        g_pp_period_ns = s > 0 ? (uint64_t)s * 1000000000ull : 0;
        const char *f = getenv("NFSMW_TEST_FUSION_PERIOD");   /* run 10: odd periods two-pass instead of fused */
        const long fs = f ? strtol(f, NULL, 10) : 0;
        g_pp_fusion_period_ns = fs > 0 ? (uint64_t)fs * 1000000000ull : 0;
    }
    return g_pp_on;
}
/* Once per game frame (game_device_update). */
static void pool_prof_tick(const PopModApi *api) {
    (void)api;
    pp_on();
    const uint64_t now = pp_now();
    if (g_pp_csv) {
        static uint32_t rows;
        const PoolProf *p = &g_pp;
        fprintf(g_pp_csv, "%.6f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%u,%u,%u,%u", now / 1e9, p->sched[0][0],
                p->sched[0][1], p->sched[1][0], p->sched[1][1], p->sched[2][0], p->sched[2][1], p->props_skipped,
                p->restores_in_list, p->restores_list_end, p->entry_ns / 1e6, p->sv_ns[0] / 1e6, p->sv_ns[1] / 1e6,
                p->sv_ns[2] / 1e6, p->sv_calls, p->sv_repeat[0], p->sv_repeat[1], p->sv_repeat[2]);
        for (unsigned i = 1; i <= 8; ++i)
            fprintf(g_pp_csv, ",%u", p->lamps[i]);
        fprintf(g_pp_csv, ",%u,%u,%u,%u,%u,%d\n", p->zero_fade, p->zero_strength, p->zero_colour, p->zero_unknown,
                p->maybe_lit, g_pp_props_old);
        if (++rows % 600 == 0)
            fflush(g_pp_csv);
        memset(&g_pp, 0, sizeof g_pp);
    }
    g_pp_props_old = g_pp_period_ns ? (int)((now / g_pp_period_ns) & 1u) : 0;
    g_pp_fusion_old = g_pp_fusion_period_ns ? (int)((now / g_pp_fusion_period_ns) & 1u) : 0;
}

#endif
