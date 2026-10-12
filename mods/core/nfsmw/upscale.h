/* MetalFX upscaling, v1 spatial (Remaster run 13; review/run13/metalfx-design.md). Included by nfsmw.c after
 * post_fx.h (it uses get_u32, setting, g_look, install_at and g_api from there).
 *
 * The setting `upscale` (0 Off, 1 Quality 1.5x, 2 Performance 2x; a player setting, it does not follow Look) is read
 * once at launch and handed to the kit as RECOMP_REMASTER_UPSCALE; `upscale_temporal` as
 * RECOMP_REMASTER_UPSCALE_TEMPORAL (the kit says when temporal is not built yet). With Off nothing here installs.
 *
 * The scene markers, two private render states the kit turns into renderer events (kit dx/d3d9.cpp):
 *  - 253 (scene begin): before the main view's colour pass, the call of 006de210 at 006ded1c, in the world only
 *    (00925e90 == 6) and never in the PC look (Look PC, or OverBright 009017fc / visual treatment 00901828 on): the
 *    hook runs inside an active view, so a frame whose view did not draw sends nothing (A3 challenge H1);
 *  - 252 (scene end): before the frame's HUD/front-end call 006c3870 at 006e75a1, only after a begin. Installed
 *    before the post chain's hook at the same call (pfx_frame), so the chain, the HDR mark, the HUD and the mirror
 *    quad all draw at the native size.
 * The kit checks that the back buffer is render target 0 at the begin, and Present closes a scene left open.
 *
 * Remaster run 14 foundation F1/F2 (review/run14/foundation-design.md 1-2; TEST ONLY until a consumer passes its
 * gates): with RECOMP_REMASTER_FOUNDATION=1 the same two markers are sent also when `upscale` is Off (the kit then opens
 * the scene window without a smaller backing), and at each begin the camera goes to the kit as the private
 * SetTransform states 0x200 (eView 1 camera +0x00..+0x3c, world -> view, row vectors, y down) and 0x201 (FOV, near,
 * far, cut flag, mover type, game frame; kit dx/host_d9.h HOST_D9_TS_SCENE_*). Unset: nothing here changes (no
 * markers without upscaling, no SetTransform). The PC look sends no markers: the foundation is off there too. */

#define UPSCALE_SLOT_BEGIN 156
#define UPSCALE_SLOT_END 157

static int g_up_mode;            /* the `upscale` setting at launch */
static int g_up_fnd;             /* run 14 foundation F1/F2: RECOMP_REMASTER_FOUNDATION=1 (TEST ONLY) */
static uint32_t g_up_cam_mem;    /* guest memory for the two SetTransform matrices (0x80 bytes) */
static uint64_t g_up_cams;
static int g_up_open;            /* a begin went out this frame */
static int g_up_late_end;        /* RECOMP_UPSCALE_EFFECTS_INTERNAL=1: the end marker goes after the post chain */
static int g_up_logged = -1;     /* the last state logged: 1 the world is upscaled, 2 the PC look stands aside */
static uint64_t g_up_begins, g_up_ends, g_up_aside;

/* IDirect3DDevice9::SetRenderState(state, 1) (vtable 0xe4). */
static void up_marker(uint32_t dev, uint32_t state) {
    uint32_t a[3] = {dev, state, 1}, r = 0;
    const uint32_t fn = get_u32(get_u32(dev) + 0xe4u);
    if (fn) g_api->guest_call(g_api, fn, 0, a, 3, &r);
}
static void up_log_state(const PopModApi *api, int state) {
    if (state == g_up_logged) return;
    g_up_logged = state;
    if (!g_up_mode) {   /* run 14 foundation only */
        api->log(api, state == 1 ? "core.nfsmw: foundation: scene window markers and camera on (TEST)"
                                 : "core.nfsmw: foundation: the PC look is on; no scene window");
        return;
    }
    api->log(api, state == 1 ? "core.nfsmw: upscaling: the 3D scene is drawn smaller and scaled up with MetalFX"
                             : "core.nfsmw: upscaling: the PC look is on; the scene stays at the native size");
}
/* Run 14 foundation F1 (G): IDirect3DDevice9::SetTransform (vtable 0xb0) with the private states 0x200 (view) and
 * 0x201 (info), the camera of eView 1 (0x009195e0 + 0x70, camera +0x40) as clean_sun.h / clean_chain.h read it. */
static void up_send_camera(uint32_t dev) {
    const uint32_t view = 0x009195e0u + 0x70u, cam = get_u32(view + 0x40u);
    const uint32_t fn = get_u32(get_u32(dev) + 0xb0u);
    if (!cam || !fn) return;
    if (!g_up_cam_mem && g_api->guest_alloc(g_api, 0x80u, &g_up_cam_mem) != POP_OK) { g_up_cam_mem = 0; return; }
    for (uint32_t i = 0; i < 16; ++i) g_api->guest_write_u32(g_api, g_up_cam_mem + 4u * i, get_u32(cam + 4u * i));
    const unsigned fov = get_u32(cam + 0xc4u) & 0xffffu;   /* full vertical FOV, 65536 = 2 pi (clean_sun.h P0) */
    uint32_t mover_type = 0xffffffffu;
    if (get_u32(view + 0x48u) != view + 0x44u) {           /* the first camera mover (clean_chain.h E inputs) */
        const uint32_t node = get_u32(view + 0x48u), mover = node ? node - 4u : 0u;
        if (mover) mover_type = get_u32(mover + 0x0cu);
    }
    /* flags: 1 an in-engine cut-scene (the NIS instance 0x009885c8 nonzero; xenon_fx.h XFX_NIS_INSTANCE, the contrail
     * gate), 2 the world, 4 the 008f9b28 low byte is 0 (info). CONF14B: the design's "008f9b28 low byte != 0" was true in
     * every frame of a parked and an AI drive (cut 2595 / 2595), so that byte is nonzero in play; clean_chain.h reads
     * its cut flag as byte == 0 (DER, no cut-scene in the test drives). */
    const uint32_t flags = (get_u32(0x009885c8u) ? 1u : 0u) | 2u | ((get_u32(0x008f9b28u) & 0xffu) == 0 ? 4u : 0u);
    const uint32_t info[16] = {float_bits((float)fov / 65536.0f * 6.28318531f), get_u32(cam + 0xbcu), get_u32(cam + 0xc0u),
                               flags, mover_type, get_u32(0x009885b8u), fov};
    for (uint32_t i = 0; i < 16; ++i) g_api->guest_write_u32(g_api, g_up_cam_mem + 0x40u + 4u * i, info[i]);
    uint32_t a[3] = {dev, 0x200u, g_up_cam_mem}, r = 0;
    g_api->guest_call(g_api, fn, 0, a, 3, &r);
    a[1] = 0x201u;
    a[2] = g_up_cam_mem + 0x40u;
    g_api->guest_call(g_api, fn, 0, a, 3, &r);
    ++g_up_cams;
}

static void up_scene_begin(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    if (get_u32(0x00925e90u) != 6) return;                                        /* the world only */
    if (g_look == LOOK_PC || get_u32(0x009017fcu) || get_u32(0x00901828u)) {      /* the PC look: native */
        ++g_up_aside;
        up_log_state(api, 2);
        return;
    }
    const uint32_t dev = get_u32(0x00982bdcu);
    if (!dev || g_up_open) return;
    up_marker(dev, 253u);
    if (g_up_fnd) up_send_camera(dev);   /* run 14 foundation F1: after the begin, inside the window */
    g_up_open = 1;
    ++g_up_begins;
    up_log_state(api, 1);
}
static void up_scene_end(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (!g_up_open) return;
    g_up_open = 0;
    const uint32_t dev = get_u32(0x00982bdcu);
    if (!dev) return;
    up_marker(dev, 252u);
    ++g_up_ends;
}

/* At launch, before the post chain's hook on 006c3870 is installed (so this one runs first at 006e75a1). */
static void upscale_init(const PopModApi *api) {
    { const char *e = getenv("RECOMP_REMASTER_FOUNDATION"); g_up_fnd = e && *e == '1'; }   /* run 14 F1/F2 (TEST) */
    g_up_mode = (int)setting("upscale", 0);
    if (g_up_mode < 1 || g_up_mode > 2) g_up_mode = 0;
    /* A host that drives the scene size itself (iOS: host_upscale_set_rows) asks for the markers by setting
     * RECOMP_REMASTER_UPSCALE before the mods load; the setting being Off does not withhold them then. */
    if (!g_up_mode && getenv("RECOMP_REMASTER_UPSCALE") && atof(getenv("RECOMP_REMASTER_UPSCALE")) >= 1.0)
        g_up_mode = 2;
    if (!g_up_mode && !g_up_fnd) return;
    if (g_up_mode && !getenv("RECOMP_REMASTER_UPSCALE")) setenv("RECOMP_REMASTER_UPSCALE", g_up_mode == 1 ? "1.5" : "2", 0);
    if (g_up_mode && setting("upscale_temporal", 0) && !getenv("RECOMP_REMASTER_UPSCALE_TEMPORAL"))
        setenv("RECOMP_REMASTER_UPSCALE_TEMPORAL", "1", 0);
    /* RECOMP_UPSCALE_EFFECTS_INTERNAL=1 (TEST, iOS menu; off by default): the scene ends after the post chain, so glow,
     * depth of field and motion blur draw into the smaller scene backing before the upscale. The end marker is then
     * installed by upscale_install_end_late, after pfx_frame on the same call (same-address hooks run in install order). */
    g_up_late_end = getenv("RECOMP_UPSCALE_EFFECTS_INTERNAL") && *getenv("RECOMP_UPSCALE_EFFECTS_INTERNAL") == '1';
    const int ok = install_at(0x006de210u, 0x006ded21u, up_scene_begin, POP_HOOK_BEFORE, UPSCALE_SLOT_BEGIN) == POP_OK &&
                   (g_up_late_end ||
                    install_at(0x006c3870u, 0x006e75a6u, up_scene_end, POP_HOOK_BEFORE, UPSCALE_SLOT_END) == POP_OK);
    char line[200];
    if (g_up_mode)
        snprintf(line, sizeof line, "core.nfsmw: upscaling %s at launch (%s)%s%s", ok ? "on" : "UNAVAILABLE (hook)",
                 g_up_mode == 1 ? "Quality, 1.5x" : "Performance, 2x",
                 setting("upscale_temporal", 0) ? "; temporal asked for" : "", g_up_fnd ? "; foundation markers (TEST)" : "");
    else
        snprintf(line, sizeof line, "core.nfsmw: foundation scene window %s at launch (TEST, RECOMP_REMASTER_FOUNDATION=1)",
                 ok ? "on" : "UNAVAILABLE (hook)");
    api->log(api, line);
    if (!ok) {   /* without both markers the scene stays native: take back whichever went in */
        for (int s = UPSCALE_SLOT_BEGIN; s <= UPSCALE_SLOT_END; ++s)
            if (g_hooks[s]) { api->hook_remove(api, g_hooks[s]); g_hooks[s] = 0; }
        g_up_mode = 0;
        g_up_fnd = 0;
        g_up_late_end = 0;
    }
}
/* After the post chain's hook (nfsmw.c): the end marker for RECOMP_UPSCALE_EFFECTS_INTERNAL=1. */
static void upscale_install_end_late(const PopModApi *api) {
    if (!g_up_late_end || (!g_up_mode && !g_up_fnd)) return;
    if (install_at(0x006c3870u, 0x006e75a6u, up_scene_end, POP_HOOK_BEFORE, UPSCALE_SLOT_END) == POP_OK) {
        api->log(api, "core.nfsmw: upscaling: effects draw at the scene size, before the upscale (TEST)");
        return;
    }
    api->log(api, "core.nfsmw: upscaling UNAVAILABLE (late end hook); the scene stays native");
    if (g_hooks[UPSCALE_SLOT_BEGIN]) { api->hook_remove(api, g_hooks[UPSCALE_SLOT_BEGIN]); g_hooks[UPSCALE_SLOT_BEGIN] = 0; }
    g_up_mode = g_up_fnd = g_up_late_end = 0;
}
/* TEST ONLY evidence at exit: begins and ends must match (one each per world frame); aside counts PC-look frames. */
static void upscale_exit(const PopModApi *api) {
    if (!g_up_mode && !g_up_fnd) return;
    char line[200];
    snprintf(line, sizeof line, "core.nfsmw: upscaling: %llu scene begins, %llu ends, %llu PC-look frames aside; foundation cameras %llu",
             (unsigned long long)g_up_begins, (unsigned long long)g_up_ends, (unsigned long long)g_up_aside,
             (unsigned long long)g_up_cams);
    api->log(api, line);
}
