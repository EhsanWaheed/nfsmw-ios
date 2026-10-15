/* Denser tyre smoke (run 14; design review/run14/smoke-design.md). Setting 'denser_smoke' (bool, live). Follows Look:
 * Xbox 360 on, PC off, Definitive the menu value (read here, not through k_look_follow).
 *
 * How: a wrap on Emitter::SpawnParticles(float a, float b), thiscall, ret 8 (00509b30; xenon_fx.h wraps it too). The
 * first argument `a` is read once (00509e82) and feeds only the spawn count (design §1), so scaling it scales the
 * number of particles that call spawns and nothing else. Only the emitters in k_smoke_keys are touched; `a` is put
 * back after the call. Off, PC Look, or any other emitter: the hook only calls on (byte-identical).
 *
 * Which emitters (OBS, offline diff of GLOBAL/attributes.bin, 8 Oct, scratchpad smoke/): the tyre-smoke records are
 * emitterdata collections keyed by Attrib::StringHash32 of their names. Rate field +0xc4 (particles per second; +0xc0
 * is 0 in all of them), life +0xbc. The Stuff Pack's "more dense tire smoke" changes no rate: it adds the two unused
 * vanilla emitters emtd_sk_extrasmoke1 (23/s, life 0.75 s) and emtd_sk_asphaltsmokedark1 (5/s, 2 s) to the asphalt
 * skid groups. Live smoke particles (rate x life) in fxtd_sk_asphalt go 48 -> 75.25, x1.57: hence m = 1.6.
 * Run 15 (owner 8/9 Oct: "not very heavy even after build-up"): m 1.6 -> 2.2. denser_smoke owns only this multiplier.
 *
 * Stuff Pack smoke (run 15, owner 9 Oct: "add stuff pack smoke as well and make it a toggle"). Setting
 * 'stuffpack_smoke' (bool, live, same Look rule). It adds, from our code, every emitter the Stuff Pack's data adds to
 * the asphalt groups (offline VLT diff, 8-9 Oct): fxtd_sk_asphalt and _no_leaves + emtd_sk_extrasmoke1 and
 * emtd_sk_asphaltsmokedark1 (unused vanilla records; darker); fxtd_sl_asphalt + emtd_sk_asphaltfastsmk1, emtd_dr_leaves
 * and the same two; fxtd_dr_asphalt_leaves + emtd_dr_asphalt_smk. Nothing else of the Stuff Pack is used.
 * An emittergroup record (class aba86e60) lists its emitters in its layout (Collection+0x18): +0x00 Attrib array
 * header (u16 alloc 6, u16 count, u16 elem size 12), +0x08 RefSpec[alloc] {class b30b18af, key, cached Collection *};
 * +0x50 a second array (alloc 5, elem 8) of per-emitter drive windows, used only when its count equals the emitter
 * count (0050c260: else every emitter gets 0..1e6). The windows are left as they are, as in the Stuff Pack's data: so
 * in fxtd_sl_asphalt and fxtd_dr_asphalt_leaves (1 window = 1 emitter in vanilla) every emitter gets 0..1e6, exactly
 * as there. EmitterGroup::MakeEmitters 0050c260 builds one emitter per RefSpec; EmitterSystem::CreateGroup 0050c7b0
 * counts them first for its 500-emitter budget. A BEFORE hook on both keeps the four groups' arrays in step with the
 * setting: on, the RefSpecs go into the free slots past the count and the count grows; off, the saved bytes go back.
 * Groups made while on keep their added emitters (an emitter's group is at emitter+0x8c); while off (or an A/B "old"
 * period) the spawn wrap gives those a = 0, so nothing spawns from them. Never on = never patched, pass-through.
 * Per skidding wheel in fxtd_sk_asphalt (rate x life): vanilla 48 live, run 14 76.8, now 2.2 x 48 + 27.25 = 133.
 *
 * Pool: every PC emitter shares 1024 particles (004ff0a0; refused at live >= 0x400, 00509b30.c:269). The multiplier
 * tapers with the live count [0x009166c0]: full m below 650, 1.0 at 925 and above (the added emitters go from 1 to 0
 * over the same range), so smoke never starves sparks: 99 slots stay free for them and debris.
 *
 * TEST ONLY: RECOMP_REMASTER_SMOKELOG=1 logs a census of every emitter this function serves (design §5).
 * TEST ONLY in-run A/B: RECOMP_AB_SWITCHES containing "smoke" with RECOMP_AB_PERIOD=<s> (odd periods: no
 * multiplier) or RECOMP_AB_FORCE_OLD=1 (never); the same clock and periods as the kit's recomp_ab (ab_phase.h). */
#ifndef SMOKE_H
#define SMOKE_H

#define SMOKE_SPAWN        0x00509b30u   /* Emitter::SpawnParticles(float, float), thiscall, ret 8 (PROVEN: listing) */
#define SMOKE_LIVE         0x009166c0u   /* EmitterSystem; first word = live particles (00509b30.c:269 tests < 0x400) */
#define SMOKE_UNLIMITED    0x0091606cu   /* nonzero = pools may grow (cut-scene mode, 0050d280) */
#define SMOKE_POOL         0x00916060u   /* ParticleSlotPool *; +0x18 live, +0x1c peak (DERIVED) */
#define SMOKE_SLOT         152           /* g_hooks slot (free: grep, 8 Oct) */
#define SMOKE_M            2.2f          /* run 15: 1.6 -> 2.2 (owner) */
#define SMOKE_TAPER_FULL   650.0f        /* run 15: 600 -> 650 */
#define SMOKE_TAPER_NONE   925.0f        /* run 15: 900 -> 925 */
#define SMOKE_GRP_MAKE     0x0050c260u   /* EmitterGroup::MakeEmitters, thiscall (ecx = group; +0x70 Attrib instance) */
#define SMOKE_GRP_CREATE   0x0050c7b0u   /* EmitterSystem::CreateGroup(Collection *, flags), thiscall, ret 8 */
#define SMOKE_GRP_SLOT_MAKE   150        /* g_hooks slots (free: grep, 9 Oct) */
#define SMOKE_GRP_SLOT_CREATE 151
#define SMOKE_CLASS_EMITTER 0xb30b18afu  /* Attrib::StringHash32("emitterdata") */
/* Run 16 smokeperf (owner 9 Oct: "smoke looks great but GPU ms ~30, 25 fps" at 4K; peak live < 650, so fill, not count):
 * the same cloud from fewer, denser particles. A tyre-smoke emitter spawns 1/K of the particles it had and its draw gets
 * alpha 1 - (1 - a)^K in the kit (PixelVariant alphak; private render state 240 = K x 16, set around the emitter's
 * draw by smoke_light.h): n overlapping layers of alpha a cover exactly as n / K layers of that alpha, so the density
 * and colour of the cloud stay and its fill (overdraw, the 4K cost) drops to 1/K. Table emitters: x2.2 density from
 * x2.2 / K_T particles; Stuff Pack added emitters: their density from 1 / K_X of their particles. Only while the
 * setting that brought the extra smoke is on (vanilla emitters with both off: unchanged, nothing marked).
 * TEST ONLY: NFSMW_TEST_SMOKE_FILL="kt,kx" or "kt:kx" (1:1 = the run-15 behaviour). */
#define SMOKE_FILL_KT      2.0f          /* table emitters with denser_smoke: x2.2 density from x1.1 particles */
#define SMOKE_FILL_KX      2.0f          /* Stuff Pack added emitters: half their particles */
#define SMOKE_RS_ALPHAK    240u          /* private render state: alpha exponent x 16 (kit d3d9_metal.mm) */

/* emitterdata keys (Attrib::StringHash32 of the name; the collection's key at +0x20) */
static const uint32_t k_smoke_keys[] = {
    0xe00557c4u,   /* emtd_sk_asphaltfastsmk1: skid on asphalt (fxtd_sk_asphalt, _no_leaves, fxtd_sk_cobble) */
    0xe586fefbu,   /* emtd_sk_asphaltfastsmk2: skid on asphalt */
    0x81a6b886u,   /* emtd_sl_asphaltfastsmk1: slide on asphalt (fxtd_sl_asphalt) */
    0xe580828bu,   /* emtd_sk_cobblefastsmk1: skid on cobbles (fxtd_sk_cobble) */
    0xdd458eb9u,   /* emtd_dr_asphalt_smk: drive (wheelspin) on asphalt (run 14 lead: all used tyre-smoke emitters) */
    0x583c808au,   /* emtd_dr_cobblefastsmk1: drive on cobbles. Dirt/grass/sand _smk are dust, left alone. */
};
/* run 15: Stuff Pack smoke, the emitters its data adds per group (appended after the group's own; smoke_kinds.h
 * classes 6da6dd9d / 00b4e4ef / e00557c4 / dd458eb9 as tyre smoke for lit smoke) */
/* 15 Oct (owner's iPhone, fresh profile): fxtd_sl_asphalt (7790c105: 1 emitter, +4 here) crashed the game at the
 * countdown of a race (SIGSEGV in guest thread 1, EIP 0081bce0, right after "group 7790c105 on (emitters 1 -> 5)"),
 * every time; it is left as the game has it. The other three groups were in every working run. */
#define SMOKE_GRP_N   3
#define SMOKE_ADD_MAX 4
static const struct { uint32_t group, n, add[SMOKE_ADD_MAX]; } k_smoke_groups[SMOKE_GRP_N] = {
    {0xeaa2e866u, 2, {0x6da6dd9du, 0x00b4e4efu}},               /* fxtd_sk_asphalt + extrasmoke1, asphaltsmokedark1 */
    {0xd2f8d5bau, 2, {0x00b4e4efu, 0x6da6dd9du}},               /* fxtd_sk_asphalt_no_leaves + the same two */
    {0x95cde6cbu, 1, {0xdd458eb9u}},                            /* fxtd_dr_asphalt_leaves + dr_asphalt_smk */
};
typedef struct {
    uint32_t layout;          /* 0 = not seen yet */
    uint32_t hdr;             /* the original array header */
    uint32_t save[3 * SMOKE_ADD_MAX];
    uint8_t ok, applied;      /* ok: room for all and none already present */
} SmokeGroup;
static SmokeGroup g_smoke_grp[SMOKE_GRP_N];
static int g_smoke_grp_ever;  /* set once any group was patched: only then can an added emitter exist */
static int g_sp_on = -1, g_sp_calls_to_refresh;
static uint64_t g_smoke_grp_patches, g_smoke_extra_zeroed;

static int g_smoke_on = -1, g_smoke_calls_to_refresh, g_smoke_ab = -1, g_smoke_ab_phase = -1, g_smoke_log;
static float g_smoke_kt = SMOKE_FILL_KT, g_smoke_kx = SMOKE_FILL_KX;   /* run 16 smokeperf (TEST env may change them) */
static uint64_t g_smoke_marked;
static uint64_t g_smoke_ab_period_ns, g_smoke_scaled, g_smoke_calls, g_smoke_tapered, g_spray_scaled;

/* Pure: 1 below SMOKE_TAPER_FULL live particles, 0 at SMOKE_TAPER_NONE and above, linear between. */
static float smoke_taper_t(uint32_t live) {
    float t = (SMOKE_TAPER_NONE - (float)live) / (SMOKE_TAPER_NONE - SMOKE_TAPER_FULL);
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    return t;
}
/* Pure: the multiplier for `live` particles (0/650/787.5/925/1024 -> m, m, (1+m)/2, 1, 1). */
static float smoke_taper(float m, uint32_t live) { return 1.0f + (m - 1.0f) * smoke_taper_t(live); }
/* Pure: is `key` one of the emitters some group gets added (and so possibly ours)? */
static int smoke_extra_key(uint32_t key) {
    for (unsigned g = 0; g < SMOKE_GRP_N; ++g)
        for (uint32_t i = 0; i < k_smoke_groups[g].n; ++i)
            if (k_smoke_groups[g].add[i] == key) return 1;
    return 0;
}
/* Pure: is `key` added to group `gkey`? */
static int smoke_added_to(uint32_t gkey, uint32_t key) {
    for (unsigned g = 0; g < SMOKE_GRP_N; ++g)
        if (k_smoke_groups[g].group == gkey)
            for (uint32_t i = 0; i < k_smoke_groups[g].n; ++i)
                if (k_smoke_groups[g].add[i] == key) return 1;
    return 0;
}
/* the group key of an emitter (emitter+0x8c = its EmitterGroup; group+0x70 its Attrib instance) */
static uint32_t smoke_emitter_group_key(uint32_t self) {
    const uint32_t grp = get_u32(self + 0x8cu), gi = grp ? get_u32(grp + 0x70u) : 0, gc = gi ? get_u32(gi + 4u) : 0;
    return gc ? get_u32(gc + 0x20u) : 0;
}
static int smoke_key(uint32_t key) {
    for (unsigned i = 0; i < sizeof k_smoke_keys / sizeof k_smoke_keys[0]; ++i)
        if (k_smoke_keys[i] == key) return 1;
    return 0;
}
static uint64_t smoke_now_ns(void) {   /* the kit's os_monotonic_ns clock (uptime on Apple), for the A/B periods */
    struct timespec t;
#ifdef __APPLE__
    clock_gettime(CLOCK_UPTIME_RAW, &t);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* Setting with the Look rule: Xbox 360 on, PC off, Definitive the saved value. Re-read every 64 calls (live). */
static int smoke_enabled(const PopModApi *api) {
    if (g_smoke_on >= 0 && g_smoke_calls_to_refresh-- > 0) return g_smoke_on;
    g_smoke_calls_to_refresh = 64;
    int64_t look = 0, v = 1;
    api->settings_get(api, "look", &look);
    api->settings_get(api, "denser_smoke", &v);
    g_smoke_on = look == 1 ? 1 : look == 2 ? 0 : v != 0;
    return g_smoke_on;
}
/* Run-16 spray census (harness/spraycensus.sh, work/spray/census.log; Rosewood AI drive 200 s, rain vs dry particles):
 * the emitters of effect group 98c0d7ac spawn only in the rain. Left out: 0da41fa7 (group 21a3994d, rain 133 / dry 0,
 * too few to matter) and every emitter that also spawns in the dry. */
#ifndef SPRAY_KEYS
#define SPRAY_KEYS \
    0x942117c8u,   /* rain 5850, dry 0 (group 98c0d7ac) */ \
    0x09123526u,   /* rain 4370, dry 0 (group 98c0d7ac) */ \
    0x1aa64781u    /* rain 2417, dry 0 (group 98c0d7ac) */
#endif
/* Run 16 (D-110, owner's HDR play test: "behind vehicles in the rain the spray tanks performance"; design
 * review/r2close/hdr-defects.md 5.3 F2a; HDRS: particles cost 1.4 ms median / 3.8 ms p90 with the float frame vs 0.85 ms
 * without): rain spray emitters are spawned at 1/K and drawn with the alpha exponent K, the smokeperf trick (same cloud,
 * 1/K of the fill). Hidden setting `spray_fill` = K (default 1 = as the game since 10 Oct, D-117: the HDRX and HDRG in-run
 * A/Bs showed no GPU gain); the PC Look draws the game's own. */
static const uint32_t k_spray_keys[] = {
    SPRAY_KEYS
};
static int smoke_spray_key(uint32_t key) {
    for (unsigned i = 0; i < sizeof k_spray_keys / sizeof k_spray_keys[0]; ++i)
        if (k_spray_keys[i] && k_spray_keys[i] == key) return 1;
    return 0;
}
static float g_spray_k = -1.0f;
static int g_spray_calls_to_refresh;
static float spray_k(const PopModApi *api) {
    if (g_spray_k >= 0.0f && g_spray_calls_to_refresh-- > 0) return g_spray_k;
    g_spray_calls_to_refresh = 64;
    int64_t look = 0, v = 1;
    api->settings_get(api, "look", &look);
    api->settings_get(api, "spray_fill", &v);
    g_spray_k = look == 2 || v < 1 ? 1.0f : v > 4 ? 4.0f : (float)v;
    return g_spray_k;
}
/* stuffpack_smoke, the same Look rule. */
static int smoke_sp_enabled(const PopModApi *api) {
    if (g_sp_on >= 0 && g_sp_calls_to_refresh-- > 0) return g_sp_on;
    g_sp_calls_to_refresh = 64;
    int64_t look = 0, v = 1;
    api->settings_get(api, "look", &look);
    api->settings_get(api, "stuffpack_smoke", &v);
    g_sp_on = look == 1 ? 1 : look == 2 ? 0 : v != 0;
    return g_sp_on;
}
/* TEST ONLY A/B: 1 = this period runs the original (no multiplier). */
static int smoke_ab_old(const PopModApi *api, uint32_t game_live) {
    if (!g_smoke_ab) return 0;
    if (g_smoke_ab < 0) {
        const char *sw = getenv("RECOMP_AB_SWITCHES"), *p = getenv("RECOMP_AB_PERIOD"), *f = getenv("RECOMP_AB_FORCE_OLD");
        g_smoke_ab = sw && strstr(sw, "smoke") ? 1 : 0;
        if (g_smoke_ab && f && !strcmp(f, "1")) g_smoke_ab = 2;
        g_smoke_ab_period_ns = p && atoi(p) > 0 ? (uint64_t)atoi(p) * 1000000000ull : 0;
        if (g_smoke_ab == 1 && !g_smoke_ab_period_ns) g_smoke_ab = 0;
        if (!g_smoke_ab) return 0;
    }
    const int phase = g_smoke_ab == 2 ? 1 : (int)((smoke_now_ns() / g_smoke_ab_period_ns) & 1u);
    if (phase != g_smoke_ab_phase) {
        g_smoke_ab_phase = phase;
        char line[200];
        snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST smoke A/B: denser smoke %s (uptime %llu ns, live %u, scaled calls %llu)",
                 phase ? "off" : "on", (unsigned long long)smoke_now_ns(), game_live, (unsigned long long)g_smoke_scaled);
        api->log(api, line);
    }
    return phase;
}

/* ---- run 15: Stuff Pack smoke, the added emitters (see the header) ---- */
static void smoke_group_sync(const PopModApi *api, uint32_t coll) {
    if (!coll) return;
    const uint32_t key = get_u32(coll + 0x20u);
    unsigned g = 0;
    while (g < SMOKE_GRP_N && k_smoke_groups[g].group != key) ++g;
    if (g == SMOKE_GRP_N) return;
    SmokeGroup *G = &g_smoke_grp[g];
    const uint32_t lay = get_u32(coll + 0x18u), k = k_smoke_groups[g].n;
    if (!lay) return;
    if (G->layout != lay) {   /* first sight (a reloaded database gives a new layout; the old one is gone) */
        memset(G, 0, sizeof *G);
        G->layout = lay;
        G->hdr = get_u32(lay);
        const uint32_t alloc = G->hdr & 0xffffu, n = G->hdr >> 16;
        int ok = alloc <= 6u && n + k <= alloc && (get_u32(lay + 4u) & 0xffffu) == 12u;
        for (uint32_t i = 0; ok && i < n; ++i)
            if (smoke_added_to(key, get_u32(lay + 0x0cu + 12u * i))) ok = 0;   /* already there (modded data): leave it */
        for (uint32_t i = 0; ok && i < 3u * k; ++i) G->save[i] = get_u32(lay + 8u + 12u * n + 4u * i);
        G->ok = (uint8_t)ok;
        if (!ok) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsmw: Stuff Pack smoke: group %08x not extended (array %08x)", key, G->hdr);
            api->log(api, line);
        }
    }
    if (!G->ok) return;
    const int want = smoke_sp_enabled(api);
    if (want == G->applied) return;
    const uint32_t n = G->hdr >> 16, ent = lay + 8u + 12u * n;
    if (want) {
        for (uint32_t j = 0; j < k; ++j) {
            api->guest_write_u32(api, ent + 12u * j, SMOKE_CLASS_EMITTER);
            api->guest_write_u32(api, ent + 12u * j + 4u, k_smoke_groups[g].add[j]);
            api->guest_write_u32(api, ent + 12u * j + 8u, 0u);           /* the game resolves and caches it (004560d0) */
        }
        api->guest_write_u32(api, lay, G->hdr + (k << 16));
        g_smoke_grp_ever = 1;
        ++g_smoke_grp_patches;
    } else {
        api->guest_write_u32(api, lay, G->hdr);
        for (uint32_t i = 0; i < 3u * k; ++i) api->guest_write_u32(api, ent + 4u * i, G->save[i]);
    }
    G->applied = (uint8_t)want;
    char line[160];
    snprintf(line, sizeof line, "core.nfsmw: Stuff Pack smoke: group %08x %s (emitters %u -> %u)",
             key, want ? "on" : "off", want ? n : n + k, want ? n + k : n);
    api->log(api, line);
}
/* BEFORE EmitterSystem::CreateGroup(Collection *, flags): [esp+4] = the group's collection. */
static void smoke_grp_create(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    uint32_t coll = 0;
    if (api->guest_read_u32(api, cpu->esp + 4u, &coll) == POP_OK) smoke_group_sync(api, coll);
}
/* BEFORE EmitterGroup::MakeEmitters (also reached from the rebuild 0050c730): the group's instance at +0x70. */
static void smoke_grp_make(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t inst = cpu->ecx ? get_u32(cpu->ecx + 0x70u) : 0;
    smoke_group_sync(api, inst ? get_u32(inst + 4u) : 0);
}

/* ---- TEST ONLY census (RECOMP_REMASTER_SMOKELOG=1) ---- */
typedef struct {
    uint32_t key, coll, calls, spawned, period_calls, period_spawned;
    float b_min, b_max;
} SmokeCensus;
static SmokeCensus g_smoke_census[128];
static unsigned g_smoke_census_n;
static uint64_t g_smoke_census_next_ns, g_smoke_refused, g_smoke_census_full;
static uint32_t g_smoke_peak_live;
static void smoke_census_first(const PopModApi *api, uint32_t self, uint32_t coll, uint32_t key, float b, uint32_t caller) {
    const uint32_t inst = get_u32(self + 0x7cu), data = inst ? get_u32(inst + 8u) : 0, fx = get_u32(self + 0x8cu);
    char line[512];
    int n = snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST smoke census: new emitter key %08x%s caller %08x b %.3f emitter flags %08x effect flags %08x",
                     key, smoke_key(key) ? " (smoke table)" : smoke_extra_key(key) ? " (Stuff Pack set)" : "", caller, b, get_u32(self + 0x18u), fx ? get_u32(fx + 0x18u) : 0u);
    if (data)
        n += snprintf(line + n, sizeof line - (size_t)n, " +c0 %.3f +c4 %.3f +bc %.3f +e8 %.3f +f0 %.3f",
                      get_float(data + 0xc0u), get_float(data + 0xc4u), get_float(data + 0xbcu), get_float(data + 0xe8u), get_float(data + 0xf0u));
    n += snprintf(line + n, sizeof line - (size_t)n, " group %08x coll", smoke_emitter_group_key(self));
    for (uint32_t o = 0; o < 0x30u && n < (int)sizeof line - 10; o += 4) n += snprintf(line + n, sizeof line - (size_t)n, " %08x", get_u32(coll + o));
    api->log(api, line);
}
static void smoke_census(const PopModApi *api, uint32_t self, uint32_t coll, uint32_t key, float b, uint32_t caller, uint32_t spawned) {
    SmokeCensus *c = NULL;
    for (unsigned i = 0; i < g_smoke_census_n; ++i)
        if (g_smoke_census[i].key == key) { c = &g_smoke_census[i]; break; }
    if (!c) {
        if (g_smoke_census_n >= sizeof g_smoke_census / sizeof g_smoke_census[0]) { ++g_smoke_census_full; return; }
        c = &g_smoke_census[g_smoke_census_n++];
        memset(c, 0, sizeof *c);
        c->key = key; c->coll = coll; c->b_min = c->b_max = b;
        smoke_census_first(api, self, coll, key, b, caller);
    }
    ++c->calls; ++c->period_calls; c->spawned += spawned; c->period_spawned += spawned;
    if (b < c->b_min) c->b_min = b;
    if (b > c->b_max) c->b_max = b;
    const uint64_t now = smoke_now_ns();
    if (!g_smoke_census_next_ns) g_smoke_census_next_ns = now + 10000000000ull;
    if (now < g_smoke_census_next_ns) return;
    g_smoke_census_next_ns = now + 10000000000ull;
    const uint32_t pool = get_u32(SMOKE_POOL);
    char line[256];
    snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST smoke census 10 s: live %u (peak %u), pool live %u peak %u, refusals %llu, calls %llu, scaled %llu (tapered %llu), keys %u",
             get_u32(SMOKE_LIVE), g_smoke_peak_live, pool ? get_u32(pool + 0x18u) : 0u, pool ? get_u32(pool + 0x1cu) : 0u,
             (unsigned long long)g_smoke_refused, (unsigned long long)g_smoke_calls, (unsigned long long)g_smoke_scaled,
             (unsigned long long)g_smoke_tapered, g_smoke_census_n);
    api->log(api, line);
    for (unsigned i = 0; i < g_smoke_census_n; ++i) {
        SmokeCensus *e = &g_smoke_census[i];
        if (!e->period_calls) continue;
        snprintf(line, sizeof line, "core.nfsmw: [remaster] TEST smoke census 10 s:   key %08x%s calls %u spawned %u (%.1f/s) b %.3f..%.3f",
                 e->key, smoke_key(e->key) ? "*" : smoke_extra_key(e->key) ? "+" : "", e->period_calls, e->period_spawned, e->period_spawned / 10.0f, e->b_min, e->b_max);
        api->log(api, line);
        e->period_calls = e->period_spawned = 0;
    }
}

/* Run 16 smokeperf: the alpha exponent x 16 for this emitter's draw (0 = none: draw as the game does). Called by
 * smoke_light.h around the emitter's particle draw (self = the emitter). Same rules as the spawn wrap's scaling. */
static uint32_t smoke_alpha_k16(const PopModApi *api, uint32_t self) {
    const uint32_t inst = get_u32(self + 0x7cu), coll = inst ? get_u32(inst + 4u) : 0, key = coll ? get_u32(coll + 0x20u) : 0;
    if (!key) return 0;
    float k = 0.0f;
    if (smoke_key(key)) {
        if (!smoke_enabled(api)) return 0;
        k = g_smoke_kt;
    } else if (g_smoke_grp_ever && smoke_extra_key(key) && smoke_added_to(smoke_emitter_group_key(self), key)) {
        if (!smoke_sp_enabled(api)) return 0;
        k = g_smoke_kx;
    } else if (smoke_spray_key(key)) {   /* D-110 rain spray */
        k = spray_k(api);
        if (k <= 1.0f) return 0;
    } else {
        return 0;
    }
    if (g_smoke_ab > 0 && g_smoke_ab_phase == 1) return 0;   /* A/B "old" period (phase kept by the spawn wrap) */
    const uint32_t k16 = (uint32_t)(k * 16.0f + 0.5f);
    if (k16 <= 16u) return 0;
    ++g_smoke_marked;
    return k16 > 255u ? 255u : k16;
}
static void smoke_spawn(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t self = cpu->ecx, sp = cpu->esp;
    if (!self || (!g_smoke_log && !g_smoke_grp_ever && !smoke_enabled(api) && spray_k(api) <= 1.0f)) { api->call_next(api, inv, cpu); return; }
    const uint32_t inst = get_u32(self + 0x7cu), coll = inst ? get_u32(inst + 4u) : 0, key = coll ? get_u32(coll + 0x20u) : 0;
    const uint32_t live = get_u32(SMOKE_LIVE);
    uint32_t a_bits = 0, scaled = 0;
    const int table = key && smoke_key(key);
    const int added = key && g_smoke_grp_ever && smoke_extra_key(key) && smoke_added_to(smoke_emitter_group_key(self), key);
    const int spray = key && smoke_spray_key(key);
    if (table || added || spray) {
        const int old = smoke_ab_old(api, live);
        float f = 1.0f;
        if (table && !old && smoke_enabled(api)) {
            const float m = smoke_taper(SMOKE_M, live);
            if (m > 1.0f) { ++g_smoke_scaled; if (m < SMOKE_M) ++g_smoke_tapered; }
            f *= m / g_smoke_kt;   /* run 16: 1/K of the particles, drawn with alpha exponent K (smoke_alpha_k16) */
        }
        if (added) {   /* Stuff Pack smoke: full below 650 live, none at 925+; none while off or in an A/B "old" period */
            const float t = !old && smoke_sp_enabled(api) ? smoke_taper_t(live) : 0.0f;
            if (t < 1.0f) ++g_smoke_extra_zeroed;
            f *= t / g_smoke_kx;
        }
        if (spray && !old) {   /* D-110: rain spray at 1/K, drawn with the alpha exponent K (smoke_alpha_k16) */
            const float k = spray_k(api);
            if (k > 1.0f) {
                f /= k;
                if (++g_spray_scaled == 1) {
                    char sl[160];
                    snprintf(sl, sizeof sl, "core.nfsmw: rain spray fill K %.0f: first spray emitter scaled (key %08x)", k, key);
                    api->log(api, sl);
                }
            }
        }
        if (f != 1.0f) {
            api->guest_read_u32(api, sp + 4u, &a_bits);
            const float a = bits_float(a_bits);
            if (a > 0.0f) {
                api->guest_write_u32(api, sp + 4u, float_bits(a * f));
                scaled = 1;
            }
        }
    }
    if (!g_smoke_log) {
        api->call_next(api, inv, cpu);
        if (scaled) api->guest_write_u32(api, sp + 4u, a_bits);   /* the caller's argument slot as it was */
        return;
    }
    /* census */
    ++g_smoke_calls;
    if (live > g_smoke_peak_live) g_smoke_peak_live = live;
    if (live >= 0x400u && !get_u32(SMOKE_UNLIMITED)) ++g_smoke_refused;
    const uint32_t caller = get_u32(sp), before = get_u32(self + 0x1cu);
    const float b = get_float(sp + 8u);
    api->call_next(api, inv, cpu);
    if (scaled) api->guest_write_u32(api, sp + 4u, a_bits);
    if (key) smoke_census(api, self, coll, key, b, caller, get_u32(self + 0x1cu) - before);
}
/* Installed in every Look: off is a pass-through (the setting is live). */
static void smoke_init(const PopModApi *api) {
    const char *e = getenv("RECOMP_REMASTER_SMOKELOG");
    g_smoke_log = e && !strcmp(e, "1");
    const char *fe = getenv("NFSMW_TEST_SMOKE_FILL");   /* TEST ONLY: "kt,kx" (1,1 = run 15: no fill saving) */
    if (fe) {
        float kt = 0, kx = 0;
        if (sscanf(fe, "%f%*[,:]%f", &kt, &kx) == 2 && kt >= 1.0f && kt <= 8.0f && kx >= 1.0f && kx <= 8.0f) {
            g_smoke_kt = kt;
            g_smoke_kx = kx;
        }
    }
    const int ok = install(SMOKE_SPAWN, smoke_spawn, POP_HOOK_WRAP, SMOKE_SLOT) == POP_OK;
    /* run 15 Stuff Pack smoke: without both hooks the groups are never touched (the multiplier still works) */
    const int grp = ok && install(SMOKE_GRP_CREATE, smoke_grp_create, POP_HOOK_BEFORE, SMOKE_GRP_SLOT_CREATE) == POP_OK &&
                    install(SMOKE_GRP_MAKE, smoke_grp_make, POP_HOOK_BEFORE, SMOKE_GRP_SLOT_MAKE) == POP_OK;
    if (ok && !grp && g_hooks[SMOKE_GRP_SLOT_CREATE]) {
        g_api->hook_remove(g_api, g_hooks[SMOKE_GRP_SLOT_CREATE]);
        g_hooks[SMOKE_GRP_SLOT_CREATE] = 0;
    }
    char line[200];
    snprintf(line, sizeof line, "core.nfsmw: denser tyre smoke: %s (now %s, x%.1f below %d live particles); Stuff Pack smoke %s (now %s); fill K %.2f / %.2f%s",
             ok ? "hook installed" : "hook refused; off", smoke_enabled(api) ? "on" : "off", SMOKE_M, (int)SMOKE_TAPER_FULL,
             grp ? "hooked" : "refused; off", smoke_sp_enabled(api) ? "on" : "off", g_smoke_kt, g_smoke_kx,
             g_smoke_log ? "; TEST census on" : "");
    api->log(api, line);
}
static void smoke_exit(const PopModApi *api) {
    if (!g_smoke_log && !g_smoke_scaled && !g_smoke_grp_patches) return;
    char line[300];
    snprintf(line, sizeof line, "core.nfsmw: denser tyre smoke: %llu scaled calls (%llu tapered), %llu dense-alpha draws, Stuff Pack group patches %llu, added-emitter calls cut %llu, census %u keys (%llu over the table), peak live %u, refusals %llu",
             (unsigned long long)g_smoke_scaled, (unsigned long long)g_smoke_tapered, (unsigned long long)g_smoke_marked,
             (unsigned long long)g_smoke_grp_patches,
             (unsigned long long)g_smoke_extra_zeroed, g_smoke_census_n,
             (unsigned long long)g_smoke_census_full, g_smoke_peak_live, (unsigned long long)g_smoke_refused);
    api->log(api, line);
}
#endif
