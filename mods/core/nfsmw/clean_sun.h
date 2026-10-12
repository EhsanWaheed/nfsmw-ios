/* clean_sun.h - module G host inputs (R2 N4, 7 Oct 2026; review/2026-10-07-r2-lighting-research.md proposal 1):
 * screen-space sun rays + sun glare in the clean chain. Our own code; PC data only. Included by clean_chain.h.
 * Per frame: the sun direction (time-of-day object [0x009b392c] +0x180, normalised, world space), the player camera
 * (eView 1 +0x40: view matrix +0x00, row vectors; FOV u16 +0xc4), projected to the screen with the frame's aspect.
 * Intensity = owner strength x on-screen fade (margin 0.25 outside the frame) x (1 - cloud intensity of the player
 * Rain object [0x009196b8] +0x290) x sun height fade. Intensity 0 -> the host skips the module's passes (no draws),
 * so off is the frame without module G. */
#ifndef CLEAN_SUN_H
#define CLEAN_SUN_H

static uint32_t g_cc_p_sun, g_cc_p_suncol;
static int g_cc_sun_on, g_cc_sun_setting = -1;
static uint64_t g_cc_sun_frames, g_cc_sun_skipped;
static float g_cc_sun_last[4];

static float cc_sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static void cc_g_inputs(const PopModApi *api, unsigned out_w, unsigned out_h, uint32_t game_frame) {
    g_cc_sun_on = 0;
    if (!g_cc_p_sun || !g_cc_p_suncol || !out_w || !out_h) return;
    static uint32_t calls;
    static float strength = 1.0f;
    if ((calls++ & 255u) == 0) {
        g_cc_sun_setting = setting("sun_rays", 1) != 0;
        strength = (float)setting("sun_rays_strength", 100) / 100.0f;
    }
    if (!g_cc_sun_setting || strength <= 0.0f || get_u32(0x00925e90u) != 6u) { ++g_cc_sun_skipped; return; }
    const uint32_t tod = get_u32(0x009b392cu), cam = get_u32(0x009195e0u + 0x70u + 0x40u);
    if (!tod || !cam) { ++g_cc_sun_skipped; return; }
    float s[3], v[3] = {0, 0, 0};
    for (unsigned i = 0; i < 3; ++i) s[i] = get_float(tod + 0x180u + 4u * i);
    for (unsigned j = 0; j < 3; ++j)
        for (unsigned i = 0; i < 3; ++i) v[j] += s[i] * get_float(cam + 16u * i + 4u * j);
    const unsigned fov = get_u32(cam + 0xc4u) & 0xffffu;
    const float aspect = (float)out_w / (float)out_h;
    /* P0 / N4W (7 Oct, QA_LOG): the camera's view space is x right, y DOWN, z forward; the u16 angle is the vertical
     * FOV (mode 0: the rays converge on the game's own sun glare; mode 1, the 4:3 horizontal reading, misses it).
     * TEST ONLY: NFSMW_TEST_SUN_FOV=1 selects the rejected reading. */
    static int fovmode = -1;
    if (fovmode < 0) { const char *e = getenv("NFSMW_TEST_SUN_FOV"); fovmode = e && *e == '1' ? 1 : 0; }
    const float tv = tanf((float)fov / 65536.0f * 3.14159265f) / (fovmode ? 4.0f / 3.0f : 1.0f);   /* tan(vertical FOV / 2) */
    if (!(v[2] > 0.02f) || !(tv > 0.01f) || !(tv < 20.0f)) { ++g_cc_sun_skipped; return; }
    const float u = 0.5f + 0.5f * v[0] / (v[2] * tv * aspect), w = 0.5f + 0.5f * v[1] / (v[2] * tv);
    const float margin = 0.25f;
    const float ex = u < 0.0f ? -u : (u > 1.0f ? u - 1.0f : 0.0f), ey = w < 0.0f ? -w : (w > 1.0f ? w - 1.0f : 0.0f);
    const float onscreen = cc_sat(1.0f - (ex > ey ? ex : ey) / margin);
    const uint32_t rain = get_u32(0x009196b8u);
    float clouds = rain ? get_float(rain + 0x290u) : 0.0f;
    if (!(clouds >= 0.0f)) clouds = 0.0f;
    const float clear = 1.0f - cc_sat(clouds);
    const float height = cc_sat(s[2] * 6.0f + 0.3f);   /* fades as the sun sets below about -3 degrees */
    static float gain = -1.0f, shape[4];
    static int have_shape = -1;
    if (gain < 0.0f) {   /* TEST ONLY: NFSMW_TEST_SUN_GAIN (x intensity, <= 8) and NFSMW_TEST_SUN_SHAPE="a,b,c,d" (SunShape) */
        const char *e = getenv("NFSMW_TEST_SUN_GAIN");
        gain = e ? (float)atof(e) : 1.0f;
        if (!(gain > 0.0f) || gain > 8.0f) gain = 1.0f;
        const char *sh = getenv("NFSMW_TEST_SUN_SHAPE");
        have_shape = sh && sscanf(sh, "%f%*[,:]%f%*[,:]%f%*[,:]%f", &shape[0], &shape[1], &shape[2], &shape[3]) == 4;   /* , or : (T71_ENV splits on ,) */
    }
    const float k = 0.5f * strength * onscreen * clear * height * gain;   /* 0.5: N4W split frames (gain 1 washed out) */
    if (!(k > 0.002f)) { ++g_cc_sun_skipped; return; }
    const float sp[4] = {u, w, k, aspect};
    const float sc[4] = {1.0f, 0.90f, 0.74f, 6.0f};   /* warm sun white; mask radius 0.41 (1 / 6; 0.6 washed out a low sun) */
    cc_set4(g_cc_p_sun, sp);
    cc_set4(g_cc_p_suncol, sc);
    if (have_shape) { static uint32_t hs; if (!hs) hs = cc_param("SunShape"); if (hs) cc_set4(hs, shape); }
    static int split = -1;   /* TEST ONLY: NFSMW_TEST_SUN_SPLIT=1 -> rays on the right half only (same-frame A/B) */
    if (split < 0) {
        split = getenv("NFSMW_TEST_SUN_SPLIT") != NULL;
        for (unsigned i = 0; split && i < g_cc_np; ++i)
            if (!strcmp(g_cc_p[i].tech, "clean_sun_add"))
                for (unsigned k = 0; k < g_cc_p[i].nc; ++k) g_cc_p[i].cval[k][3] = 0.5f;
    }
    g_cc_sun_on = 1;
    ++g_cc_sun_frames;
    memcpy(g_cc_sun_last, sp, sizeof sp);
    static int glog = -1;   /* TEST ONLY: NFSMW_TEST_SUN_LOG=1 logs the module G inputs every 120 frames */
    if (glog < 0) glog = getenv("NFSMW_TEST_SUN_LOG") != NULL;
    if (glog && g_cc_sun_frames % 120u == 1u) {
        char line[240];
        snprintf(line, sizeof line, "core.nfsmw: TEST clean sun (game frame %u): uv %.3f %.3f intensity %.3f (onscreen %.2f clear %.2f height %.2f) view %.3f %.3f %.3f fov %u",
                 game_frame, (double)u, (double)w, (double)k, (double)onscreen, (double)clear, (double)height,
                 (double)v[0], (double)v[1], (double)v[2], fov);
        api->log(api, line);
    }
}

#endif
