/* Xbox 360 sky. Until run 16 x360_sky 1 loaded the community sky.fx.o (X360Stuff) into guest memory; that path is
 * removed (owner 9 Oct, licence unknown). No effect file or the on-disk executable changes. */
static uint32_t g_x360_sky_path, g_x360_sky_vector, g_x360_sky_brightness_name;
static uint32_t g_x360_sky_fog_names[6], g_x360_sky_fog_vectors;
static int g_x360_sky_ready;
/* Run 13 (owner 8 Oct, "our own version of the 360 shaders"; review/run13/x360-shader-map.md SKY): x360_sky 2, or
 * RECOMP_REMASTER_SKY=1 with x360_sky on, selects our own Xbox 360 sky. The PC sky effect stays (no sky.fx.o is
 * loaded); the kit rewrites its pass 0 combine in the 360 order and gives its in-scatter the 360 horizon distance
 * (d3d9_shadergen.cpp sky_ps_patch / sky_vs_patch). NextGenSky.BIN, the cloud intensities below and the scattering
 * bridge stay as in the legacy path, the bridge bound by the PC effect's semantics (FOG_CONST_1..3). Run 16: ours is
 * the only Xbox 360 sky (the community shader is dropped, see x360_sky_init). */
static int g_x360_sky_ours;

/* Owner X360Stuff StuffSky (10030710) does not submit its sky during the
 * load-complete/front-end transition: in flow 3, GarageMain's screen must
 * exist and its +6c state must be zero. 007a1fa0 is the verified PC wrapper
 * around FindScreen("GarageMain.fng"). Keep this at submission, not in the
 * shared shader: changing atmosphere constants also changes driving views. */
static void x360_sky_submit(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (get_u32(0x00925e90u) == 3) {
        uint32_t garage = 0;
        if (api->guest_call(api, 0x007a1fa0u, 0, NULL, 0, &garage) == POP_OK &&
            (!garage || get_u32(garage + 0x6c) != 0)) {
            api->hook_return(api, cpu, cpu->eax, 0); /* StuffSky is cdecl. */
            return;
        }
    }
    api->call_original(api, cpu->target, cpu);
}

static int x360_sky_init(const PopModApi *api) {
    const int64_t sky_mode = setting("x360_sky", 2);   /* run 14: ours is the default (owner) */
    if (!sky_mode) return 0;
    /* Run 16 (owner 9 Oct, project audit B1): the community sky shader (x360-sky.fx.o, licence unknown) is no longer
     * shipped or loaded. Value 1 is a retired choice (empty label, the menu steps over it) and selects ours. */
    g_x360_sky_ours = 1;
    /* Verified PC sky resource entry: ours relies on the stock effect's programs. */
    if (get_u32(0x009c7598u) != 0x00625090u || get_u32(0x009c759cu) != 14724u) return 0;
    setenv("RECOMP_REMASTER_SKY", "1", 1);
    void *bytes = NULL;
    static const char name[] = "FRONTEND\\PLATFORMS\\NextGenSky.BIN";
    static const char brightness[] = "STANDARD_BRIGHTNESS";
    if (api->guest_alloc(api, sizeof name + 16 + sizeof brightness, &g_x360_sky_path) != POP_OK ||
        api->guest_ptr(api, g_x360_sky_path, sizeof name, &bytes) != POP_OK) return 0;
    memcpy(bytes, name, sizeof name);
    g_x360_sky_vector = g_x360_sky_path + sizeof name;
    g_x360_sky_brightness_name = g_x360_sky_vector + 16;
    if (api->guest_ptr(api, g_x360_sky_brightness_name, sizeof brightness, &bytes) != POP_OK) return 0;
    memcpy(bytes, brightness, sizeof brightness);
    static const char *fog_names[] = {"FOG_BR_PLUS_BM", "FOG_ONE_OVER_BRBM",
        "FOG_CONST1", "FOG_CONST2", "FOG_CONST3", "FOG_DISTANCE_SCALE"};
    static const char *pc_fog_names[] = {"FOG_BR_PLUS_BM", "FOG_ONE_OVER_BRBM",   /* the PC sky effect's semantics */
        "FOG_CONST_1", "FOG_CONST_2", "FOG_CONST_3", "FOG_DISTANCE_SCALE"};
    if (g_x360_sky_ours) memcpy(fog_names, pc_fog_names, sizeof fog_names);
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t len = (uint32_t)strlen(fog_names[i]) + 1;
        if (api->guest_alloc(api, len, &g_x360_sky_fog_names[i]) != POP_OK ||
            api->guest_ptr(api, g_x360_sky_fog_names[i], len, &bytes) != POP_OK) return 0;
        memcpy(bytes, fog_names[i], len);
    }
    if (api->guest_alloc(api, 80, &g_x360_sky_fog_vectors) != POP_OK) return 0;
    g_x360_sky_ready = 1;
    api->log(api, "core.nfsmw: Xbox 360 sky (ours): PC sky effect kept, 360 combine by the kit (RECOMP_REMASTER_SKY=1)");
    return 1;
}

/* Reconstruct the owner's eLightScattering::Update/SetParams (1003bc70 /
 * 1003bba0), using the same live PC time-of-day layout also read by 006d6cf0.
 * The stock PC routine binds these only to its fog effect, whereas the mod
 * binds them to each custom effect, including sky. Keep this bridge confined
 * to the replacement sky rather than altering the accepted world lighting. */
static void x360_sky_scattering(const PopModApi *api, uint32_t effect, uint32_t tod) {
    static uint32_t cached_effect, handles[6];
    if (cached_effect != effect) {
        for (unsigned i = 0; i < 6; ++i) {
            const uint32_t args[] = {effect, 0, g_x360_sky_fog_names[i]};
            handles[i] = 0;
            api->guest_call(api, get_u32(get_u32(effect) + 0x28), 0, args, 3, &handles[i]);
        }
        cached_effect = effect;
    }
    const float rayleigh = (get_float(tod + 0x90) + 1.0f) * 0.0001f;
    const float mie = fmaxf(0.0f, (get_float(tod + 0x94) + 1.0f) * 0.0001f);
    float values[20] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        float br = rayleigh * get_float(tod + 0x70 + i * 4);
        float bm = mie * get_float(tod + 0x80 + i * 4);
        values[i] = br + bm;
        values[4+i] = values[i] != 0.0f ? 1.0f / values[i] : 0.0f;
        values[8+i] = br * 0.05968310311436653f;
        values[12+i] = bm * 0.07957746833562851f;
    }
    float g = get_float(tod + 0x9c);
    values[16] = (1.0f-g) * (1.0f-g);
    values[17] = g*g + 1.0f;
    values[18] = -2.0f*g;
    values[19] = get_float(tod + 0x98);
    for (unsigned i = 0; i < 20; ++i) put_float(g_x360_sky_fog_vectors + i*4, values[i]);
    uint32_t result;
    for (unsigned i = 0; i < 5; ++i) if (handles[i]) {
        const uint32_t args[] = {effect, handles[i], g_x360_sky_fog_vectors + i*16};
        api->guest_call(api, get_u32(get_u32(effect) + 0x88), 0, args, 3, &result);
    }
    if (handles[5]) {
        const uint32_t args[] = {effect, handles[5], get_u32(tod + 0xa0)};
        api->guest_call(api, get_u32(get_u32(effect) + 0x78), 0, args, 3, &result);
    }
    static int logged;
    if (!logged++) {
        char line[256];
        snprintf(line, sizeof line, "core.nfsmw: Xbox atmosphere BrBm=%g/%g/%g scatter=%g phase=%g distance=%g handles=%08x/%08x",
                 values[0], values[1], values[2], values[19], g, get_float(tod + 0xa0), handles[0], handles[5]);
        api->log(api, line);
    }
}

/* X360Stuff SetSkyDiffuseScale at 1002e790 replaces the tail of the PC
 * 006c60f0 routine. Use the same time-of-day cloud value and verified bounds,
 * after the original texture/scroll setup. The fourth vector lane is unused. */
static void x360_sky_params(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    uint32_t object = cpu->ecx;
    api->call_original(api, cpu->target, cpu);
    if (!g_x360_sky_ready || object != get_u32(0x0093dec4u)) return;
    uint32_t tod = get_u32(0x009b392cu), effect = get_u32(object + 0x48);
    uint32_t handle = get_u32(get_u32(object + 0x44) + 0xe84);
    if (!tod || !effect || !handle) return;
    x360_sky_scattering(api, effect, tod);
    static int logged;
    if (!logged++) {
        char line[160];
        snprintf(line, sizeof line, "core.nfsmw: Xbox sky active, nextgen=%u cloud_handle=%08x textures=%08x/%08x/%08x",
                 get_u32(object + 0x40) & 255u, handle, get_u32(0x009829f0),
                 get_u32(0x009829f4), get_u32(0x009829f8));
        api->log(api, line);
    }
    float clouds = fmaxf(get_float(tod + 0x160), 0.12f);
    put_float(g_x360_sky_vector, fminf(clouds * 3.33333325f, 1.0f));
    put_float(g_x360_sky_vector + 4, fminf(clouds * clouds * 2.0f, 0.8f));
    put_float(g_x360_sky_vector + 8, fminf(clouds * clouds, 0.8f));
    put_float(g_x360_sky_vector + 12, 0.0f);
    const uint32_t args[] = {effect, handle, g_x360_sky_vector};
    uint32_t result = 0;
    api->guest_call(api, get_u32(get_u32(effect) + 0x88), 0, args, 3, &result);
    /* X360Stuff's StuffSky explicitly sets STANDARD_BRIGHTNESS to 2 before
     * flushing. PC's original sky effect has no such parameter; its existing
     * setup therefore leaves the replacement effect's default at zero. */
    static uint32_t cached_effect, brightness_handle;
    if (cached_effect != effect) {
        const uint32_t lookup[] = {effect, 0, g_x360_sky_brightness_name};
        if (api->guest_call(api, get_u32(get_u32(effect) + 0x28), 0, lookup, 3, &brightness_handle) != POP_OK)
            brightness_handle = 0;
        cached_effect = effect;
    }
    if (brightness_handle) {
        const uint32_t brightness[] = {effect, brightness_handle, 0x40000000u};
        api->guest_call(api, get_u32(get_u32(effect) + 0x78), 0, brightness, 3, &result);
    }
}
