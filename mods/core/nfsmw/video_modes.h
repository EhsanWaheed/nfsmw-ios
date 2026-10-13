/* video_modes.h - video settings log and the resolution hooks (moved out of nfsmw.c 7 Oct 2026, unchanged). Included once by nfsmw.c at the same place, so the single translation unit is as before. */
#ifndef VIDEO_MODES_H
#define VIDEO_MODES_H

static void video_settings_log(const PopModApi *api, const char *reason) {
    char line[280];
    uint32_t w = 0, h = 0;
    choose_resolution(&w, &h);
    snprintf(line, sizeof line,
        "core.nfsmw: video %s %ux%u level=%u car-reflection=%u road=%u world=%u shadow=%u filter=%u VT=%u bloom=%u blur=%u guest-vsync=%u fsaa=%u",
        reason, w, h, get_u32(0x00982c00u), get_u32(0x009017b8u),
        get_u32(0x009017d4u), get_u32(0x009017f4u), get_u32(0x00901830u),
        get_u32(0x00901818u), get_u32(0x00901828u), get_u32(0x009017fcu),
        get_u32(0x009017dcu), get_u32(0x00901824u), get_u32(0x00901808u));
    api->log(api, line);
}

/* Verified in 006c1990/006c1780 and the six-face table at008f9028:
 * the legacy top preset selects car-reflection1 (one face each frame),
 * road2/world2/shadow1/filter1. The accepted HQ preset uses 3/3/3/2/2.
 * Upgrade only a newly selected top preset; custom individual reductions
 * and saved settings remain intact. Never exceed the game's actual caps.
 * Preserve VSync: changing detail should not silently turn it off. */
static void video_preset(const PopModApi *api, pop_cpu_v1 *cpu,
                         PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    uint32_t vsync = get_u32(0x00901824u);
    api->call_original(api, cpu->target, cpu);
    if (setting("mod_hq_preset", 1) && get_u32(0x00982c00u) >= 4) {
        static const struct { uint32_t value, cap, high; } quality[] = {
            {0x009017b8u, 0x00901840u, 3}, /* all six cubemap faces each frame */
            {0x009017bcu, 0x00901844u, 1},
            {0x009017d4u, 0x0090185cu, 3},
            {0x009017f4u, 0x0090187cu, 3},
            {0x009017f8u, 0x00901880u, 1},
            {0x00901830u, 0x009018b8u, 2},
            {0x00901818u, 0x009018a0u, 2},
        };
        for (unsigned i = 0; i < sizeof quality / sizeof quality[0]; ++i) {
            uint32_t cap = get_u32(quality[i].cap);
            api->guest_write_u32(api, quality[i].value,
                                 cap < quality[i].high ? cap : quality[i].high);
        }
    }
    api->guest_write_u32(api, 0x00901824u, vsync);
    video_mod_lighting(api);
    video_settings_log(api, "preset");
}

/* The registry keeps the size, not its place in the list. */
static void save_settings(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    const ResEntry *e = &g_res[resolution_index()];
    uint32_t packed = e->w ? 0x80000000u | (uint32_t)e->w << 16 | e->h : 0;
    (void)inv;
    (void)user;
    (void)cpu;
    api->guest_write_u32(api, SLOT_RES_SAVED, packed);
    video_mod_lighting(api);
    video_settings_log(api, "save");
}

static void load_settings(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    /* No value, or one the original game saved (a place in its own list of
     * six), leaves "Auto". */
    api->guest_write_u32(api, SLOT_RES_SAVED, 0);
    api->guest_write_u32(api, RESOLUTION_INDEX, 0);
    api->call_original(api, cpu->target, cpu);
    uint32_t packed = get_u32(SLOT_RES_SAVED);
    int index = 0;
    if (packed & 0x80000000u) {
        uint32_t w = (packed >> 16) & 0x7fff, h = packed & 0xffff;
        for (int i = 1; i < g_res_count; ++i)
            if (g_res[i].w == w && g_res[i].h == h)
                index = i;
    }
    api->guest_write_u32(api, RESOLUTION_INDEX, (uint32_t)index);
    video_mod_lighting(api);
    g_qp_applied = -2;
    quality_apply(api, 0);
    g_msaa_applied = -2;
    msaa_apply(api, 0);
    video_settings_log(api, "load");
}

/* The option's labels: the game has strings for its own six sizes only. */
static void localized_wide(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t out = 0, size = 0, hash = 0;
    (void)inv;
    (void)user;
    api->guest_read_u32(api, cpu->esp + 4, &out);
    api->guest_read_u32(api, cpu->esp + 8, &size);
    api->guest_read_u32(api, cpu->esp + 12, &hash);
    for (int i = 0; i < g_res_count && out && size; ++i) {
        if (g_res[i].hash != hash)
            continue;
        const char *s = g_res[i].label;
        size_t n = strlen(s);
        if (n + 1 > size)
            n = size - 1;
        void *p;
        if (api->guest_ptr(api, out, (uint32_t)(2 * (n + 1)), &p) != POP_OK)
            break;
        uint8_t *b = (uint8_t *)p;
        for (size_t k = 0; k < n; ++k) {
            b[2 * k] = (uint8_t)s[k];
            b[2 * k + 1] = 0;
        }
        b[2 * n] = b[2 * n + 1] = 0;
        api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

static void apply_resolution(uint32_t w, uint32_t h) {
    int64_t scaling = setting("fov_scaling", 1);
    float fe = (float)setting("fe_scale", 100) / 100.0f;
    g_aspect = (float)w / (float)h;
    g_hor = (4.0f / 3.0f) / g_aspect;
    g_vert = 1.215f;
    g_half = 0.43434f;
    if (scaling) {
        g_hor /= 1.047485948f;
        g_vert = scaling == 2 ? 1.27f : 1.21f;
    }
    float hud_scale = (1.0f / (float)w * ((float)h / 480.0f)) * 2.0f;
    float centre = 1.0f / hud_scale; /* 320 at 4:3 */
    put_float(HUD_SCALE_X, hud_scale);
    put_float(HUD_CENTRE_X, centre);
    put_float(AUTOSCULPT_SCALE, 480.0f * g_aspect);
    put_float(ARREST_BLUR, (1.0f / 640.0f) * ((4.0f / 3.0f) / g_aspect));
    put_float(SLOT_MIRROR_A, (centre - 320.0f) + 450.0f);
    put_float(SLOT_MIRROR_B, (centre - 320.0f) + 190.0f);
    put_float(SLOT_SHADOW_H, g_hor);
    g_hud_offset = 240.0f * g_aspect - 320.0f;
    g_fe_scale = fe * g_aspect / (4.0f / 3.0f);
    if (g_fe_scale > fe)
        g_fe_scale = fe;
    /* RECOMP_FE_FIT_WIDE=1 (the iOS host): the forced widescreen layout puts some front-end objects near the 16:9
     * frame's edges (the TRAX song box, page titles, about 56 units in from an 854-unit width). On a screen
     * narrower than that, a 4:3 iPad, they fell off the sides; the front end is scaled down just enough that a
     * 742-unit-wide (854 - 2 x 56) band fits: 0.86 at 4:3, nothing at 16:10 or wider. */
    {
        const char *fit = getenv("RECOMP_FE_FIT_WIDE");
        const float need = fe * (480.0f * g_aspect) / 742.0f;
        if (fit && *fit == '1' && need < g_fe_scale)
            g_fe_scale = need;
    }
    {
        /* 0.5 is the 4:3 frame's half size; the screen is aspect / (4/3) of
         * that wide and the picture 1 / FMV_PICTURE_HEIGHT of it tall. */
        float s = g_aspect / (4.0f / 3.0f);
        if (s > 1.0f / FMV_PICTURE_HEIGHT)
            s = 1.0f / FMV_PICTURE_HEIGHT;
        if (s < 1.0f)
            s = 1.0f;
        put_float(SLOT_FMV_BOTTOM, 0.5f * s);
        put_float(SLOT_FMV_RIGHT, 0.5f * s);
        put_float(SLOT_FMV_TOP, -0.5f * s);
        put_float(SLOT_FMV_LEFT, -0.5f * s);
    }
    g_hud_dirty = 1;
}

/* The verified movie-only callsite (0059a03f ->00591460) supplies a
 * normalized quad transformed by the FE asset's matrix, plus UV bounds
 * width/nextPowerOfTwo(width), height/nextPowerOfTwo(height). The actual PC
 * texture creator006ddd90 requests exact dimensions, as our Metal capture
 * confirms (1280x720). Sampling only0.625x0.703125 crops the decoded image.
 * Fit full widescreen movies inside the current480-high FE viewport using
 * the actual asset matrix (the shipped movie asset scales640x512). Preserve
 * embedded-letterbox sizing for old4:3 movies and preserve vertical UV flip.
 * No global texture, decoder, subtitle or unrelated FE transform changes. */
static void movie_fit(const PopModApi *api, pop_cpu_v1 *cpu,
                      PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!setting("movie_fit", 1)) return;
    uint32_t player = get_u32(MOVIE_PLAYER);
    if (!player || get_u32(player + 0x11cu) != 5) return;
    uint32_t dimensions = get_u32(0x0091cb6cu);
    uint32_t w = dimensions & 0xffffu, h = dimensions >> 16;
    if (!w || !h || w > 8192 || h > 8192) return;
    /*006ddd90 allocates exactly TextureInfo width x height, not POT padding. */
    put_float(cpu->esp + 24, 0.0f);
    put_float(cpu->esp + 32, 1.0f);
    put_float(cpu->esp + 28, get_float(cpu->esp + 28) > 0.0f ? 1.0f : 0.0f);
    put_float(cpu->esp + 36, get_float(cpu->esp + 36) > 0.0f ? 1.0f : 0.0f);
    float aspect = (float)w / (float)h;
    if (g_widescreen && aspect > 1.5f) {
        float mx = fabsf(get_float(cpu->ecx + 0x30));
        float my = fabsf(get_float(cpu->ecx + 0x44));
        if (mx > 0.0f && my > 0.0f) {
            float height = 480.0f * fminf(1.0f, g_aspect / aspect);
            float x = height * aspect / (2.0f * mx);
            float y = height / (2.0f * my);
            put_float(cpu->esp + 4, -x);
            put_float(cpu->esp + 8, -y);
            put_float(cpu->esp + 12, x);
            put_float(cpu->esp + 16, y);
        }
    }
    static uint32_t last_dimensions;
    static float last_aspect;
    if (dimensions != last_dimensions || g_aspect != last_aspect) {
        char line[144];
        snprintf(line, sizeof line, "core.nfsmw: movie %ux%u full-texture fit; screen aspect %.4f",
                 w, h, g_aspect);
        api->log(api, line);
        last_dimensions = dimensions; last_aspect = g_aspect;
    }
}

static void resolution(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t w, h, pw, ph;
    (void)inv;
    (void)user;
    choose_resolution(&w, &h);
    if (api->guest_read_u32(api, cpu->esp + 4, &pw) == POP_OK && pw)
        api->guest_write_u32(api, pw, w);
    if (api->guest_read_u32(api, cpu->esp + 8, &ph) == POP_OK && ph)
        api->guest_write_u32(api, ph, h);
    if (g_widescreen)
        apply_resolution(w, h);
    api->hook_return(api, cpu, cpu->eax, 8);
}

#endif
