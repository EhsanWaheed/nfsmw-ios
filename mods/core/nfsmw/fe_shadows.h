/* Front-end shadows (R2 item 6; owner 5 Oct: "as on the Xbox 360: world shadows on, car shadows off in the front
 * end"). Design + challenge: review/2026-10-07-r2-fe-shadows-design.md. Part A, our own code: the car's ground
 * shadow 0x00743ea0 (thiscall, RET 0x18) is called from 0x0074e839 (return PC 0x0074e83e); probe n4tfe: 2814 calls in
 * the front end (flow 0x00925e90 == 3), 13649 in the world (flow 6). A REPLACE hook filtered on that return PC skips
 * the draw only when the setting is on and the flow is 3; otherwise it calls the original unchanged. The mirror's own
 * guest_call of 0x743ea0 (mirror_car_shadows.h) has another return PC and never reaches this hook. Slot 142.
 * Part C (review/2026-10-07-r2-fe-shadows-partC-research.md): the shadow-map pass runs in the front end and the stage
 * receives (view id 1, effect ids 0/1/3/4/5/0x12), but the front-end car (list 0x9b27ec, drawn by 0x00750980 for the
 * main view only) is never drawn into the map. A REPLACE hook on the caster render-list call 0x00723fa0 from
 * 0x006e5186 (return PC 0x006e518b) runs the original, then, in the front end with the setting on, draws the
 * front-end car list into the same shadow view (guest call 0x00750980(view, 0), cdecl); the game's own queue flush
 * right after submits it while the shadow target is bound. Slot 144. */
#ifndef FE_SHADOWS_H
#define FE_SHADOWS_H
static uint64_t g_fes_skipped, g_fes_passed, g_fes_cast, g_fes_floor_marks;
static int g_fes_on, g_fes_floor, g_fes_skip;
/* Run 15 (review/run15/fefloor.md): fe_floor_shadows no longer removes the car's ground shadow (Part A). The run-14
 * floor variant went to the menu art, not the floor, so the car floated; and even on the real floor the cast shadow
 * (K 0.55) leaves the footprint lighter than the old patch (mean 61 vs 14 of 255 at the safe house). The car keeps
 * its ground shadow and the cast shadow is added around it. Only fe_shadows (the Xbox 360 look) removes it. */
/* Run 13 front-end floor shadows (review/run13/fe-shadows-design.md; setting fe_floor_shadows, which also turns on
 * Part C; Part A: run 15 above): the kit gives the garage floor (run 15: its WORLD programs) a receiver variant that samples this shadow map. The kit
 * only does so while private render state 254 is 1 (run 14: was 252, which the D3D9 layer takes as MetalFX's
 * scene-end event and never stores): set here once per shadow pass, 1 in the front end (flow 3) and 0
 * elsewhere, through the device's SetRenderState (vtable 0xe4, as post_fx.h does for 250/251). */
static void fes_floor_mark(void) {
    const uint32_t v = get_u32(0x00925e90u) == 3u ? 1u : 0u, m[] = {254u, v};
    if (pfx_call(get_u32(0x00982bdcu), 0xe4, m, 2) == 0 && v) ++g_fes_floor_marks;
}
static void fes_car_shadow(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    static uint32_t calls;
    if ((calls++ & 255u) == 0) g_fes_skip = setting("fe_shadows", 0) != 0;
    if (g_fes_skip && get_u32(0x00925e90u) == 3u) {
        ++g_fes_skipped;
        api->hook_return(api, cpu, cpu->eax, 0x18);   /* thiscall RET 0x18: no car ground shadow in the front end */
        return;
    }
    ++g_fes_passed;
    api->call_original(api, 0x00743ea0u, cpu);
}
static void fes_casters(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t view = get_u32(cpu->esp + 4u);                 /* cdecl: the shadow view */
    api->call_original(api, 0x00723fa0u, cpu);
    if (g_fes_floor) fes_floor_mark();
    if (g_fes_on && get_u32(0x00925e90u) == 3u && view) {
        const uint32_t args[2] = {view, 0u};
        if (api->guest_call(api, 0x00750980u, 0, args, 2, NULL) == POP_OK) ++g_fes_cast;
    }
}
static void fe_shadows_init(const PopModApi *api) {
    g_fes_floor = setting("fe_floor_shadows", 1) != 0;   /* default on (owner 9 Oct) */
    if (!setting("fe_shadows", 0) && !g_fes_floor) return;   /* off: no hook at all (identical to the game) */
    g_fes_on = 1;
    g_fes_skip = setting("fe_shadows", 0) != 0;
    if (g_fes_floor && !getenv("RECOMP_REMASTER_FEFLOOR")) setenv("RECOMP_REMASTER_FEFLOOR", "1", 0);
    const int ok = install_at(0x00743ea0u, 0x0074e83eu, fes_car_shadow, POP_HOOK_REPLACE, 142) == POP_OK;
    const int ok2 = install_at(0x00723fa0u, 0x006e518bu, fes_casters, POP_HOOK_REPLACE, 144) == POP_OK;
    char line[160];
    snprintf(line, sizeof line, "core.nfsmw: front-end shadows: car ground shadow off %s, car into the shadow map %s, floor receives it %s",
             !g_fes_skip ? "no (kept)" : ok ? "on" : "unavailable", ok2 ? "on" : "unavailable", !g_fes_floor ? "off" : ok2 ? "on" : "unavailable");
    api->log(api, line);
}
static void fe_shadows_exit(const PopModApi *api) {
    if (!g_fes_skipped && !g_fes_passed) return;
    char line[256];
    snprintf(line, sizeof line, "core.nfsmw: front-end shadows: %llu car shadows skipped in the front end, %llu drawn; %llu car casts into the front-end shadow map; %llu front-end floor marks",
             (unsigned long long)g_fes_skipped, (unsigned long long)g_fes_passed, (unsigned long long)g_fes_cast,
             (unsigned long long)g_fes_floor_marks);
    api->log(api, line);
}
#endif
