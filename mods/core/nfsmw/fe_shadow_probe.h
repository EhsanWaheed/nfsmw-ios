/* TEST ONLY (R2 item 6 probe, NFSMW_FE_SHADOW_PROBE=1; review/2026-10-07-r2-fe-shadows-design.md + challenge):
 * which shadow paths run in which game flow (0x00925e90: 3 front end INFERRED, 6 world)? Counts, per flow, the car
 * ground-shadow call at 0x0074e839 (-> 0x00743ea0) and DrawWorldShadows 0x006e54e0 entries, and logs every 120
 * device updates (and on a flow change) with the shadow detail 0x00901830 and the sun height 0x008fae68 against
 * its minimum 0x008fae50. Observes only (BEFORE hooks, no state written). Slots 140, 141. */
#ifndef FE_SHADOW_PROBE_H
#define FE_SHADOW_PROBE_H
static int g_fsp_on;
static uint32_t g_fsp_car[8], g_fsp_world[8], g_fsp_n, g_fsp_flow = 0xffffffffu;
/* Part C probe: the receiver shadow bind 0x006c5b80 (binds the shadow map only when detail > 0 and the current view
 * id [[0x00982a20]+4] == 1, 0x6c5bc2-0x6c5bd9): calls per flow, and per flow the view ids seen (0-7, 8 = other). */
static uint32_t g_fsp_recv[8][9];
static unsigned fsp_flow(void) { const uint32_t f = get_u32(0x00925e90u); return f < 7u ? f : 7u; }
static void fsp_car(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user; ++g_fsp_car[fsp_flow()];
}
static void fsp_world(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user; ++g_fsp_world[fsp_flow()];
}
/* Part C probe 2: every render entry (x3sl_draw_entry): per flow, the current view id [[0x00982a20]+4] (0-7, 8 other)
 * and, in flow 3, the entry's effect type (eeffect +4: 0 WORLD, 1 WORLDREFLECT, 3 WORLDNORMALMAP, ... 15 other). */
static uint32_t g_fsp_ent[8][9], g_fsp_fx3[16];
static void fsp_entry(uint32_t eeffect) {
    if (!g_fsp_on) return;
    const uint32_t list = get_u32(0x00982a20u), id = list ? get_u32(list + 4u) : 0xffffffffu, f = fsp_flow();
    ++g_fsp_ent[f][id < 8u ? id : 8u];
    if (f == 3u && eeffect) { const uint32_t t = get_u32(eeffect + 4u); ++g_fsp_fx3[t < 15u ? t : 15u]; }
}
static void fsp_recv(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    const uint32_t list = get_u32(0x00982a20u), id = list ? get_u32(list + 4u) : 0xffffffffu;
    ++g_fsp_recv[fsp_flow()][id < 8u ? id : 8u];
}
static void fe_shadow_probe_tick(const PopModApi *api) {
    if (!g_fsp_on) return;
    const uint32_t flow = get_u32(0x00925e90u);
    if (++g_fsp_n % 120u && flow == g_fsp_flow) return;
    g_fsp_flow = flow;
    char line[300];
    snprintf(line, sizeof line, "core.nfsmw: TEST fe shadow probe: flow %u detail %u sunH %.3f (min %.3f) | car shadow calls per flow 0-7: "
             "%u %u %u %u %u %u %u %u | world shadow passes: %u %u %u %u %u %u %u %u", flow, get_u32(0x00901830u),
             (double)get_float(0x008fae68u), (double)get_float(0x008fae50u), g_fsp_car[0], g_fsp_car[1], g_fsp_car[2],
             g_fsp_car[3], g_fsp_car[4], g_fsp_car[5], g_fsp_car[6], g_fsp_car[7], g_fsp_world[0], g_fsp_world[1],
             g_fsp_world[2], g_fsp_world[3], g_fsp_world[4], g_fsp_world[5], g_fsp_world[6], g_fsp_world[7]);
    api->log(api, line);
    for (unsigned f = 3; f <= 6; f += 3) {
        const uint32_t *e = g_fsp_ent[f];
        snprintf(line, sizeof line, "core.nfsmw: TEST fe shadow probe: render entries in flow %u by view id 0-7,other: %u %u %u %u %u %u %u %u %u",
                 f, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]);
        api->log(api, line);
    }
    snprintf(line, sizeof line, "core.nfsmw: TEST fe shadow probe: flow 3 entries by effect type 0-15: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
             g_fsp_fx3[0], g_fsp_fx3[1], g_fsp_fx3[2], g_fsp_fx3[3], g_fsp_fx3[4], g_fsp_fx3[5], g_fsp_fx3[6], g_fsp_fx3[7],
             g_fsp_fx3[8], g_fsp_fx3[9], g_fsp_fx3[10], g_fsp_fx3[11], g_fsp_fx3[12], g_fsp_fx3[13], g_fsp_fx3[14], g_fsp_fx3[15]);
    api->log(api, line);
    for (unsigned f = 3; f <= 6; f += 3) {
        const uint32_t *h = g_fsp_recv[f];
        snprintf(line, sizeof line, "core.nfsmw: TEST fe shadow probe: receiver binds in flow %u by view id 0-7,other: %u %u %u %u %u %u %u %u %u",
                 f, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]);
        api->log(api, line);
    }
}
static void fe_shadow_probe_init(const PopModApi *api) {
    const char *e = getenv("NFSMW_FE_SHADOW_PROBE");
    if (!e || strcmp(e, "1")) return;
    int ok = install_at(0x00743ea0u, 0x0074e83eu, fsp_car, POP_HOOK_BEFORE, 140) == POP_OK;   /* callee + return PC of the 0x74e839 call */
    ok += install(0x006e54e0u, fsp_world, POP_HOOK_BEFORE, 141) == POP_OK;
    ok += install(0x006c5b80u, fsp_recv, POP_HOOK_BEFORE, 143) == POP_OK;
    g_fsp_on = ok > 0;
    char line[96];
    snprintf(line, sizeof line, "core.nfsmw: TEST fe shadow probe on: %d of 3 hooks", ok);
    api->log(api, line);
}
#endif
