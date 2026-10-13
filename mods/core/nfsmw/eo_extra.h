/* eo_extra.h - Remaster workstream EO, batch 1 (owner decisions 5 Oct 2026, EXTRA_OPTIONS_REVIEW.md): Extra Options
 * features rebuilt natively. Addresses from the Extra Options GPL source (ExOptsTeam/NFSMWExOpts 8a094fe) used as a
 * map; every write is a data write (.data/.rdata), verified to fall outside .text. Off = retail.
 *   unlock_all                0x926124 (byte) = 1: unlocks everything, hidden tracks too (Extra Options UnlockAllThings)
 *   remove_neon_barriers      .rdata 0x8B282B/0x8B2838/0x8B2844 = 'J': the player-barrier scenery groups never match
 *                             (Extra Options RemoveNeonBarriers; its whole-map display patches are code and not included)
 *   remove_old_bridge_barrier .rdata 0x8B2817 = 0x20, 0x8B2810 = 0x54 (Extra Options RemoveOldBridgeBarrier)
 *   force_car_lod / force_tire_lod  0x903384 / 0x903388 (int; -1 = the game decides) (Extra Options ForceCarLOD/TireLOD) */
#ifndef EO_EXTRA_H
#define EO_EXTRA_H

static void eo_extra_init(const PopModApi *api) {
    char line[200];
    const int unlock = setting("unlock_all", 0) != 0, neon = setting("remove_neon_barriers", 0) != 0,
              bridge = setting("remove_old_bridge_barrier", 0) != 0;
    const int car_lod = (int)setting("force_car_lod", -1), tire_lod = (int)setting("force_tire_lod", -1);
    if (unlock) api->guest_write_u8(api, 0x00926124u, 1);
    if (neon) {
        api->guest_write_u8(api, 0x008b282bu, 0x4a);
        api->guest_write_u8(api, 0x008b2838u, 0x4a);
        api->guest_write_u8(api, 0x008b2844u, 0x4a);
    }
    if (bridge) {
        api->guest_write_u8(api, 0x008b2817u, 0x20);
        api->guest_write_u8(api, 0x008b2810u, 0x54);
    }
    if (car_lod >= 0) api->guest_write_u32(api, 0x00903384u, (uint32_t)car_lod);
    if (tire_lod >= 0) api->guest_write_u32(api, 0x00903388u, (uint32_t)tire_lod);
    snprintf(line, sizeof line,
             "core.nfsmw: Extra Options (Remaster EO): unlock all %s, neon barriers %s, old bridge barrier %s, car LOD %d, tire LOD %d",
             unlock ? "on" : "off", neon ? "removed" : "kept", bridge ? "removed" : "kept", car_lod, tire_lod);
    api->log(api, line);
}

/* Batch 2 (data writes and keys; Extra Options HotkeyStuff.h/ExtraOptionsStuff.h used as a map):
 *   always_rain           0x9B0A30 (byte) = 1, the game's own force-rain flag (Extra Options AlwaysRain; its .text
 *                         road-reflection patch at 0x758293 is not included)
 *   show_nonpursuit_cops  0x91CF00 (byte) = 1: non-pursuit police on the minimap (ShowNonPursuitCops)
 *   freeze_camera_key     Pause toggles 0x911020 (Camera_StopUpdating) in gameplay (FreezeCamera)
 *   hot_positions         Left Shift + 1-5 saves a position (0x9B0908 = slot), Left Ctrl + 1-5 jumps back to it
 *                         (0x9B090C = slot): the game's own hot-position feature (EnableSaveLoadHotPos)
 * Keys act only in gameplay (game flow 0x925E90 == 6), as in Extra Options. */
static int g_eo_freeze_key, g_eo_hotpos, g_eo_shift, g_eo_ctrl;
static int g_eo_light_keys, g_eo_debug_cam, g_eo_heat_keys, g_eo_heat_on;
static volatile uint32_t g_eo_pending; /* 1 headlights, 2 cop lights, 4 debug camera, 8 next heat level */
static uint32_t g_eo_key_hook;
static int32_t eo_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)dik; (void)user;
    if (vk == 0x10 || vk == 0xa0 || vk == 0xa1) { g_eo_shift = down != 0; return 0; }
    if (vk == 0x11 || vk == 0xa2 || vk == 0xa3) { g_eo_ctrl = down != 0; return 0; }
    if (!down || get_u32(0x00925e90u) != 6u) return 0;
    char line[120];
    if (g_eo_freeze_key && vk == 0x13) {
        const uint32_t v = get_u32(0x00911020u) & 0xffu;
        api->guest_write_u8(api, 0x00911020u, v ? 0 : 1);
        snprintf(line, sizeof line, "core.nfsmw: Extra Options: camera %s", v ? "unfrozen" : "frozen");
        api->log(api, line);
        return 1;
    }
    if (g_eo_hotpos && vk >= 0x31 && vk <= 0x35 && (g_eo_shift || g_eo_ctrl)) {
        const uint32_t slot = (uint32_t)(vk - 0x30);
        api->guest_write_u32(api, g_eo_shift ? 0x009b0908u : 0x009b090cu, slot);
        snprintf(line, sizeof line, "core.nfsmw: Extra Options: hot position %u %s", slot, g_eo_shift ? "saved" : "loaded");
        api->log(api, line);
        return 1;
    }
    /* Batch 3b keys: the work needs guest calls, so it is done in the per-frame hook (eo_frame). */
    if (g_eo_light_keys && (vk == 0x48 || vk == 0x4f)) { g_eo_pending |= vk == 0x48 ? 1u : 2u; return 1; }
    if (g_eo_debug_cam && vk == 0x08) { g_eo_pending |= 4u; return 1; }
    if (g_eo_heat_keys && vk == 0x76) { g_eo_heat_on = !g_eo_heat_on; return 1; }
    if (g_eo_heat_keys && g_eo_heat_on && vk == 0x21) { g_eo_pending |= 8u; return 1; }
    return 0;
}

static void eo_extra_init2(const PopModApi *api) {
    char line[200];
    const int rain = setting("always_rain", 0) != 0, npc = setting("show_nonpursuit_cops", 0) != 0;
    g_eo_freeze_key = setting("freeze_camera_key", 0) != 0;
    g_eo_hotpos = setting("hot_positions", 0) != 0;
    const uint32_t pursuit_retail = get_u32(0x008f3aacu) & 0xffu, npc_retail = get_u32(0x0091cf00u) & 0xffu;
    if (rain) api->guest_write_u8(api, 0x009b0a30u, 1);
    if (npc) api->guest_write_u8(api, 0x0091cf00u, 1);
    if ((g_eo_freeze_key || g_eo_hotpos) && api->on_key) api->on_key(api, eo_key, NULL, &g_eo_key_hook);
    snprintf(line, sizeof line,
             "core.nfsmw: Extra Options (Remaster EO 2): always rain %s, non-pursuit cops on minimap %s (retail %u; pursuit cops retail %u), freeze-camera key %s, hot positions %s",
             rain ? "on" : "off", npc ? "on" : "off", npc_retail, pursuit_retail, g_eo_freeze_key ? "on" : "off",
             g_eo_hotpos ? "on" : "off");
    api->log(api, line);
}

/* Batch 3a (native hooks for Extra Options .text patches; designs in review/2026-10-05-eo-hook-strategies.md):
 *   zero_bounty_fix   heat >= 10 bounty index clamped to 9 (call-site BEFORE 0x454810 from 0x418f76) and a
 *                     combo timer >= 120 s cut to 10 s (AFTER 0x418f30, [this+0xec])  (ZeroBountyFix, on)
 *   dday_fix          GetScoreInfo 0x5dca70 AFTER at 0x57f5ae (resume) and 0x531fb7 (crib): a career with bin < 16
 *                     goes to free roam instead of D-Day again  (DDayFix, on)
 *   busted_nis_fix    ChooseArrestAnimation 0x44d770: the first stage's NIS name wins (retail falls through and the
 *                     last stage always overwrites it)  (BustedNISFix, on)
 *   percent_1lap      GetRaceType 0x5faa20 AFTER at 0x57aa38: 1-lap races show COMPLETED %  (on, as in Extra Options)
 *   hud_on_look_back  0x4741d0 REPLACE returns 0: the HUD stays while looking back  (ShowHUDOnLookBack, off)
 *   subtitles         0x56bb20 AFTER at 0x542db3/0x54ee71/0x54f212 returns 1: subtitles always shown  (ShowSubs, off)
 * Hook slots 100-103, 113-119, 121-127. */
static int g_eo_trace; /* NFSMW_EO_TRACE=1 (test only); set in the 3c init */
static void eo_tr(const PopModApi *api, const char *what, uint32_t a, uint32_t b, uint32_t c) {
    if (!g_eo_trace) return;
    char line[160];
    snprintf(line, sizeof line, "core.nfsmw: EO trace: %s %08x %08x %08x (flow %u)", what, a, b, c, get_u32(0x00925e90u));
    api->log(api, line);
}
static uint32_t g_eo_bounty_this;
static void eo_bounty_index(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (get_u32(cpu->esp + 8u) >= 10u) api->guest_write_u32(api, cpu->esp + 8u, 9u);
}
static void eo_bounty_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    g_eo_bounty_this = cpu->ecx;
}
static void eo_bounty_leave(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    const uint32_t t = g_eo_bounty_this;
    if (t && get_u32(t + 0xecu) >= 0x42f00000u) api->guest_write_u32(api, t + 0xecu, 0x41200000u);
}
static uint32_t eo_career_bin(void) {
    const uint32_t fe = get_u32(0x0091cf90u), p = fe ? get_u32(fe + 0x10u) : 0;
    return p ? (get_u32(p + 0xb0u) & 0xffu) : 0xffu; /* no database: treat as D-Day (change nothing) */
}
static void eo_dday_resume(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    const uint32_t was = cpu->eax & 0xffu, bin = eo_career_bin();
    if (was == 0 && bin < 16u) cpu->eax |= 1u;
    eo_tr(api, "D-Day resume: score-info, career bin, now", was, bin, cpu->eax & 0xffu);
}
static void eo_dday_crib(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    const uint32_t was = cpu->eax & 0xffu, bin = eo_career_bin();
    if (was == 0 && (get_u32(0x00926126u) & 0xffu) == 0 && bin < 16u) cpu->eax |= 1u;
    eo_tr(api, "D-Day crib: score-info, career bin, now", was, bin, cpu->eax & 0xffu);
}
static uint32_t g_eo_nis_name, g_eo_nis_arg3;
static int g_eo_nis_first;
static void eo_nis_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    g_eo_nis_first = 0;
}
static void eo_nis_stage(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    if (!g_eo_nis_first) { g_eo_nis_first = 1; g_eo_nis_name = get_u32(cpu->esp + 8u); g_eo_nis_arg3 = get_u32(cpu->esp + 12u); }
}
static void eo_nis_final(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    eo_tr(api, "Busted NIS: first stage name, last stage name, kept first", g_eo_nis_name, get_u32(cpu->esp + 8u), (uint32_t)g_eo_nis_first);
    if (g_eo_nis_first) {
        api->guest_write_u32(api, cpu->esp + 8u, g_eo_nis_name);
        api->guest_write_u32(api, cpu->esp + 12u, g_eo_nis_arg3);
    }
}
static void eo_percent(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    cpu->eax = get_u32(cpu->esi + 0x3cu) == 1u ? 0u : 1u;
}
static void eo_lookback_hud(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    api->hook_return(api, cpu, 0, 0);
}
static void eo_subs(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    cpu->eax = 1;
}

static void eo_extra_init3(const PopModApi *api) {
    char line[260];
    const int bounty = setting("zero_bounty_fix", 1) != 0, dday = setting("dday_fix", 1) != 0,
              nis = setting("busted_nis_fix", 1) != 0, pct = setting("percent_1lap", 1) != 0,
              hud = setting("hud_on_look_back", 0) != 0, subs = setting("subtitles", 0) != 0;
    int failed = 0;
#define EO_OK(x) do { if ((x) != POP_OK) ++failed; } while (0)
    if (bounty) {
        EO_OK(install_at(0x00454810u, 0x00418f76u, eo_bounty_index, POP_HOOK_BEFORE, 100));
        EO_OK(install(0x00418f30u, eo_bounty_enter, POP_HOOK_BEFORE, 101));
        EO_OK(install(0x00418f30u, eo_bounty_leave, POP_HOOK_AFTER, 102));
    }
    if (dday) {
        EO_OK(install_at(0x005dca70u, 0x0057f5aeu, eo_dday_resume, POP_HOOK_AFTER, 126));
        EO_OK(install_at(0x005dca70u, 0x00531fb7u, eo_dday_crib, POP_HOOK_AFTER, 127));
    }
    if (nis) {
        static const uint32_t stages[6] = {0x0044d8b3u, 0x0044d8ceu, 0x0044d8dfu, 0x0044d8f8u, 0x0044d909u, 0x0044d91au};
        EO_OK(install(0x0044d770u, eo_nis_enter, POP_HOOK_BEFORE, 103));
        for (int i = 0; i < 6; ++i) EO_OK(install_at(0x00460ca0u, stages[i], eo_nis_stage, POP_HOOK_BEFORE, 113 + i));
        EO_OK(install_at(0x00460ca0u, 0x0044d96du, eo_nis_final, POP_HOOK_BEFORE, 119));
    }
    if (pct) EO_OK(install_at(0x005faa20u, 0x0057aa38u, eo_percent, POP_HOOK_AFTER, 125));
    if (hud) EO_OK(install(0x004741d0u, eo_lookback_hud, POP_HOOK_REPLACE, 121));
    if (subs) {
        EO_OK(install_at(0x0056bb20u, 0x00542db3u, eo_subs, POP_HOOK_AFTER, 122));
        EO_OK(install_at(0x0056bb20u, 0x0054ee71u, eo_subs, POP_HOOK_AFTER, 123));
        EO_OK(install_at(0x0056bb20u, 0x0054f212u, eo_subs, POP_HOOK_AFTER, 124));
    }
#undef EO_OK
    snprintf(line, sizeof line,
             "core.nfsmw: Extra Options (Remaster EO 3a): zero-bounty fix %s, D-Day fix %s, Busted NIS fix %s, 1-lap percent %s, HUD on look-back %s, subtitles %s; hooks failed %d",
             bounty ? "on" : "off", dday ? "on" : "off", nis ? "on" : "off", pct ? "on" : "off", hud ? "on" : "off",
             subs ? "on" : "off", failed);
    api->log(api, line);
}

/* Batch 3b (Extra Options hotkeys, owner list B and D): H toggles headlights and O police lights on the player car
 * (PVehicle IsGlareOn/GlareOn/GlareOff 0x6881C0/0x669360/0x669380, ids 7 and 0x7000), Backspace toggles the debug
 * world camera (CameraAI_SetAction 0x479EB0 "CDActionDebug"/"CDActionDrive"), F7 arms and PageUp cycles the heat level
 * within heat_level_min..max (Game_SetWorldHeat 0x612660). Guest calls run in a BEFORE hook on the player's per-frame
 * driver 0x006ee490 (slot 105). Extra Options' extra headlight byte patch (0x6703d6, .text) is not included. */
static uint32_t g_eo_str_debug, g_eo_str_drive;
static int g_eo_cam_debug, g_eo_lights_head, g_eo_lights_cop, g_eo_heat_level;
static uint32_t eo_player_vehicle(const PopModApi *api) {
    const uint32_t player = get_u32(0x0092d888u);
    uint32_t want = 0;
    if (player && get_u32(player)) api->guest_call(api, get_u32(get_u32(player) + 4u), player, NULL, 0, &want);
    const uint32_t n = get_u32(0x0092cd24u);
    for (uint32_t i = 0; i < n && i < 64; ++i) {
        const uint32_t v = get_u32(0x0092cd28u + 4u * i);
        uint32_t sim = 0;
        if (v && get_u32(v)) api->guest_call(api, get_u32(get_u32(v) + 4u), v, NULL, 0, &sim);
        if (want && sim == want) return v;
    }
    return 0;
}
static void eo_glare(const PopModApi *api, uint32_t veh, uint32_t id, int on) {
    uint32_t is = 0;
    api->guest_call(api, 0x006881c0u, veh, &id, 1, &is);
    if (on && !(is & 0xffu)) api->guest_call(api, 0x00669360u, veh, &id, 1, NULL);
    if (!on && (is & 0xffu)) api->guest_call(api, 0x00669380u, veh, &id, 1, NULL);
}
static uint32_t eo_guest_string(const PopModApi *api, const char *text) {
    uint32_t p = 0, n = (uint32_t)strlen(text) + 1;
    if (api->guest_alloc(api, (n + 15u) & ~15u, &p) != POP_OK) return 0;
    for (uint32_t i = 0; i < n; ++i) api->guest_write_u8(api, p + i, (uint8_t)text[i]);
    return p;
}
/* Study S1 probe (TEST ONLY, NFSMW_TOD_PROBE=<0..1>): holds the time-of-day float eTimeOfDayLighting +0x08
 * ([0x009b392c]) at the given value every world frame, and logs +0x00..+0x0c and 0x9b0fa0 now and then (challenge
 * C-S1: the world bound is 0.05..0.95; an override at 0x9b0fa0 may rewrite +0x08 each frame). */
static float g_tod_probe = -1.0f;
static void eo_tod_probe(const PopModApi *api) {
    if (g_tod_probe < 0 || get_u32(0x00925e90u) != 6u) return;
    const uint32_t tod = get_u32(0x009b392cu);
    if (!tod) return;
    static unsigned n;
    if (n++ % 600 == 0) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsmw: S1 probe: tod %08x +0 %.3f +4 %.3f +8 %.3f (hold %.2f) +c %.3f; 9b0fa0 %08x",
                 tod, get_float(tod), get_float(tod + 4), get_float(tod + 8), g_tod_probe, get_float(tod + 12),
                 get_u32(0x009b0fa0u));
        api->log(api, line);
    }
    put_float(tod + 8, g_tod_probe);
}
/* Time of day (owner 6 Oct: the full arc, about one in-game day per hour, a setting with Original; no night). The
 * game's own eTimeOfDayLighting time (+0x08 of [0x009b392c]) drives sun, sky, fog and colour keys; study S1 showed
 * 0.05..0.95 renders a full sunrise-to-sunset arc with the shipped art. Day cycle: start at the game's own value on
 * the first world frame, advance with the game's own frame time step (0 in pause and menus; at most 0.1 s), wrap
 * from evening (0.95) to morning (0.05), both warm low-sun light. Only in the world (flow 6), and never while the
 * game's own override (0x9b0fa0, cut-scene time events; challenge C-S1) is active. NFSMW_TOD_SPEED=<x> (test only)
 * runs the clock x times faster. */
static int g_tod_cycle;
static double g_tod_day_s = 3600.0, g_tod_speed = 1.0, g_tod_t = -1.0, g_tod_last = 0.0;
static double eo_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double eo_sim_step(float *last); /* below, with the pause gating */
static void eo_tod_cycle(const PopModApi *api) {
    if (!g_tod_cycle || get_u32(0x00925e90u) != 6u) { g_tod_last = 0.0; return; }
    {   /* log a hold (pause or menu) of more than 0.5 s of real time, once per hold */
        static double still_since;
        static float seen = -1.0f;
        const float st = get_float(0x009885d8u);
        const double r = eo_now_s();
        if (st != seen) {
            if (still_since > 0.0 && r - still_since > 0.5) {
                char line[96];
                snprintf(line, sizeof line, "core.nfsmw: Time of day: held for %.1f s (pause or menu)", r - still_since);
                api->log(api, line);
            }
            seen = st;
            still_since = r;
        }
    }
    const uint32_t tod = get_u32(0x009b392cu);
    if (!tod || get_u32(0x009b0fa0u)) { g_tod_last = 0.0; return; }
    const double now = eo_now_s();
    if (g_tod_t < 0.0) {
        const float f = get_float(tod + 8);
        g_tod_t = (f >= 0.05f && f <= 0.95f) ? f : 0.5;
        char line[120];
        snprintf(line, sizeof line, "core.nfsmw: Time of day: day cycle from %.3f (a day lasts %.0f min)", g_tod_t, g_tod_day_s / 60.0);
        api->log(api, line);
    } else {
        /* Owner 6 Oct: time moves only while playing: the simulation time step (0 in pause, menus, loading). */
        static float last_sim = -1.0f;
        const double dt = eo_sim_step(&last_sim);
        g_tod_t += dt * g_tod_speed * 0.9 / g_tod_day_s;
        if (g_tod_t > 0.95) g_tod_t = 0.05 + (g_tod_t - 0.95);
    }
    g_tod_last = now;
    /* RECOMP_TOD_STEPS=N (the iOS host: 1200): the game is given the time in N steps a day, not a new value every frame.
     * Each frame's tiny sun move re-aims the shadow map, and at a phone's or tablet's resolution the shadow edges crawl
     * over the cars like a flickering bulb (owner, 13 Oct, races only). 1200 steps: one every ~2.7 s of a 60-minute
     * day, the sun moving ~0.15 degrees, so shadows stand still in between. Our own clock stays continuous. */
    static int tod_steps = -1;
    if (tod_steps < 0) {
        const char *e = getenv("RECOMP_TOD_STEPS");
        const int n = e ? atoi(e) : 0;
        tod_steps = n >= 24 && n <= 100000 ? n : 0;
    }
    put_float(tod + 8, (float)(tod_steps ? floor(g_tod_t * tod_steps) / tod_steps : g_tod_t));
    static unsigned n;
    if (++n % (g_tod_speed != 1.0 ? 300u : 3600u) == 0) { /* every 5 s in a sped-up test */
        char line[80];
        snprintf(line, sizeof line, "core.nfsmw: Time of day: %.3f", g_tod_t);
        api->log(api, line);
    }
}
/* Study S2 probe (TEST ONLY, NFSMW_RAIN_PROBE=dry_overcast|damp): field map review/2026-10-06-s2-rain-fields.md.
 * Player Rain* = [0x009196b8]; intensity +0x28c, CloudIntensity +0x290, RoadDampness +0x36a0. rainOverrideIntensity
 * 0x9b0a48 (< 0 forces dry); 0x9b0a3c cloud floor (DERIVED). dry_overcast: force dry, hold clouds at 1. damp: force
 * dry, hold the road dampness at 1 (does the wet-road view appear, and what does it cost?). Logged every 600
 * frames. Writes every world frame (the per-frame update eases the current values otherwise). */
static int g_rain_probe; /* 0 off, 1 dry_overcast, 2 damp */
static void eo_rain_probe(const PopModApi *api) {
    if (!g_rain_probe || get_u32(0x00925e90u) != 6u) return;
    put_float(0x009b0a48u, -1.0f);
    const uint32_t rain = get_u32(0x009196b8u);
    if (!rain) return;
    if (g_rain_probe == 1) { put_float(0x009b0a3cu, 1.0f); put_float(rain + 0x290u, 1.0f); }
    if (g_rain_probe == 2) put_float(rain + 0x36a0u, 1.0f);
    static unsigned n;
    if (n++ % 600 == 0) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsmw: S2 probe %s: rain %08x intensity %.3f clouds %.3f damp %.3f (desired %.3f %.3f %.3f)",
                 g_rain_probe == 1 ? "dry_overcast" : "damp", rain, get_float(rain + 0x28cu), get_float(rain + 0x290u),
                 get_float(rain + 0x36a0u), get_float(rain + 0x3694u), get_float(rain + 0x3698u), get_float(rain + 0x369cu));
        api->log(api, line);
    }
}
static void eo_tod_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    if (g_tod_probe < 0) eo_tod_cycle(api);
}
/* Pause gating for the day and weather cycles (owner 6 Oct: time moves only while playing). The step is the
 * game's simulation time [0x009885d8] (float seconds, Sim::GetTime 0x6e8de0; review/2026-10-06-pause-signal.md):
 * it advances only while the simulation runs, so it stands still in pause, menus and loading. A decrease is a sim
 * reset (new session). Each caller keeps its own last value. */
static double eo_sim_step(float *last) {
    const float now = get_float(0x009885d8u);
    double dt = (*last >= 0.0f && now >= *last) ? (double)(now - *last) : 0.0;
    *last = now;
    if (dt > 0.1) dt = 0.1;
    return dt;
}
static int eo_world_running(void) {
    return get_u32(0x00925e90u) == 6u;
}

/* Weather cycle (owner 6 Oct; study S2; field map review/2026-10-06-s2-rain-fields.md): a slow natural cycle in
 * free roam and races: clear -> overcast -> rain -> damp road drying -> clear. Per cycle of `weather_cycle_minutes`
 * (20 since run 16; was 40): clear 37.5 %, overcast 20 %, rain 20 %, drying 22.5 %. Data writes only, held every world frame:
 * rainOverrideIntensity 0x9b0a48 (< 0 dry, > 0 that rain level), the player Rain object [0x009196b8] CloudIntensity
 * +0x290 and RoadDampness +0x36a0. Original (0) writes nothing: the game's own weather. The static gameplay check
 * found only render readers (QA_LOG 6 Oct). Starts at a random point of the cycle (run 16; was: clear). */
static int g_weather_cycle;
static double g_weather_cycle_s = 2400.0, g_weather_t = 0.0, g_weather_last = 0.0;
static void eo_weather_cycle(const PopModApi *api) {
    if (!g_weather_cycle || !eo_world_running()) { g_weather_last = 0.0; return; }
    static float last_sim = -1.0f;
    const double dt = eo_sim_step(&last_sim);
    g_weather_t += dt * g_tod_speed / g_weather_cycle_s;
    g_weather_t -= (double)(int)g_weather_t;
    const double ph = g_weather_t;
    float override_rain, clouds = -1.0f, damp = -1.0f;
    const char *name;
    if (ph < 0.375) { override_rain = -1.0f; name = "clear"; }
    else if (ph < 0.575) { override_rain = -1.0f; clouds = (float)((ph - 0.375) / 0.05 < 1.0 ? (ph - 0.375) / 0.05 : 1.0); name = "overcast"; }
    else if (ph < 0.775) { override_rain = (float)(0.15 + 0.45 * ((ph - 0.575) / 0.2 < 0.3 ? (ph - 0.575) / 0.06 : 1.0)); clouds = 1.0f; name = "rain"; }
    else { override_rain = -1.0f; clouds = (float)(1.0 - (ph - 0.775) / 0.225); damp = (float)(1.0 - (ph - 0.775) / 0.225); name = "drying"; }
    if (override_rain > 0.6f) override_rain = 0.6f;
    put_float(0x009b0a48u, override_rain);
    const uint32_t rain = get_u32(0x009196b8u);
    if (rain) {
        if (clouds >= 0.0f) put_float(rain + 0x290u, clouds);
        if (damp >= 0.0f) put_float(rain + 0x36a0u, damp);
    }
    static const char *last;
    if (name != last) {
        last = name;
        char line[96];
        snprintf(line, sizeof line, "core.nfsmw: Weather: %s (cycle %.2f)", name, ph);
        api->log(api, line);
    }
}
static void eo_frame(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    eo_weather_cycle(api);
    eo_rain_probe(api);
    eo_tod_probe(api);
    /* The day cycle runs in an AFTER hook on the game's own time-of-day update (0x769360) instead: it is part of
     * the world update, so it stops with the game in pause and menus (owner 6 Oct), and our write comes after the
     * game's own write each frame. */
    const uint32_t p = g_eo_pending;
    if (!p || get_u32(0x00925e90u) != 6u) return;
    g_eo_pending = 0;
    char line[120];
    if (p & 3u) {
        const uint32_t veh = eo_player_vehicle(api);
        if (veh && (p & 1u)) { g_eo_lights_head = !g_eo_lights_head; eo_glare(api, veh, 7u, g_eo_lights_head); }
        if (veh && (p & 2u)) { g_eo_lights_cop = !g_eo_lights_cop; eo_glare(api, veh, 0x7000u, g_eo_lights_cop); }
        snprintf(line, sizeof line, "core.nfsmw: Extra Options: lights (vehicle %08x) headlights %d police lights %d", veh,
                 g_eo_lights_head, g_eo_lights_cop);
        api->log(api, line);
    }
    if ((p & 4u) && g_eo_str_debug && g_eo_str_drive) {
        g_eo_cam_debug = !g_eo_cam_debug;
        const uint32_t args[2] = {1u, g_eo_cam_debug ? g_eo_str_debug : g_eo_str_drive};
        api->guest_call(api, 0x00479eb0u, 0, args, 2, NULL);
        api->log(api, g_eo_cam_debug ? "core.nfsmw: Extra Options: debug camera on" : "core.nfsmw: Extra Options: debug camera off");
    }
    if (p & 8u) {
        const int lo = (int)setting("heat_level_min", 1), hi = (int)setting("heat_level_max", 10);
        g_eo_heat_level = g_eo_heat_level < lo || g_eo_heat_level >= hi ? lo : g_eo_heat_level + 1;
        const float f = (float)g_eo_heat_level;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        api->guest_call(api, 0x00612660u, 0, &bits, 1, NULL);
        snprintf(line, sizeof line, "core.nfsmw: Extra Options: heat level set to %d", g_eo_heat_level);
        api->log(api, line);
    }
}

static void eo_extra_init3b(const PopModApi *api) {
    char line[200];
    g_eo_light_keys = setting("light_keys", 0) != 0;
    g_eo_debug_cam = setting("debug_camera", 0) != 0;
    g_eo_heat_keys = setting("heat_keys", 0) != 0;
    int failed = 0;
    { const char *e = getenv("NFSMW_TOD_PROBE"); if (e && *e) g_tod_probe = (float)atof(e); }
    { const char *e = getenv("NFSMW_RAIN_PROBE");
      g_rain_probe = !e ? 0 : !strcmp(e, "dry_overcast") ? 1 : !strcmp(e, "damp") ? 2 : 0; }
    g_tod_cycle = setting("time_of_day", 1) == 1;
    g_tod_day_s = 60.0 * (double)setting("tod_day_minutes", 60);
    if (g_tod_day_s < 600.0) g_tod_day_s = 600.0;
    { const char *e = getenv("NFSMW_TOD_SPEED"); if (e && *e && atof(e) > 0) g_tod_speed = atof(e); }
    if (g_tod_cycle && install(0x00769360u, eo_tod_after, POP_HOOK_AFTER, 139) != POP_OK) ++failed;
    g_weather_cycle = setting("weather", 1) == 1;
    g_weather_cycle_s = 60.0 * (double)setting("weather_cycle_minutes", 20);
    if (g_weather_cycle_s < 600.0) g_weather_cycle_s = 600.0;
    /* Run 16 (owner 9 Oct, "random start + shorter"): each session starts at a random point of the cycle, so rain can
     * come at any time (the 40-minute cycle that always started clear only rained after ~23 minutes of play); the
     * default cycle is 20 minutes. TEST: NFSMW_TEST_WEATHER_START=<0..1> fixes the start (harness runs). */
    if (g_weather_cycle) {
        const char *e = getenv("NFSMW_TEST_WEATHER_START");
        g_weather_t = e && *e ? fmod(fabs(atof(e)), 1.0) : (double)arc4random_uniform(1000000u) / 1e6;
        snprintf(line, sizeof line, "core.nfsmw: Weather cycle: %.0f min, this session starts at cycle %.2f%s",
                 g_weather_cycle_s / 60.0, g_weather_t, e && *e ? " (TEST)" : "");
        api->log(api, line);
    }
    if (g_eo_light_keys || g_eo_debug_cam || g_eo_heat_keys || g_tod_probe >= 0 || g_rain_probe || g_weather_cycle) {
        if (install(0x006ee490u, eo_frame, POP_HOOK_BEFORE, 105) != POP_OK) ++failed;
        if (!g_eo_key_hook && api->on_key) api->on_key(api, eo_key, NULL, &g_eo_key_hook);
        if (g_eo_debug_cam) { g_eo_str_debug = eo_guest_string(api, "CDActionDebug"); g_eo_str_drive = eo_guest_string(api, "CDActionDrive"); }
    }
    snprintf(line, sizeof line, "core.nfsmw: Extra Options (Remaster EO 3b): light keys %s, debug camera %s, heat keys %s; hooks failed %d",
             g_eo_light_keys ? "on" : "off", g_eo_debug_cam ? "on" : "off", g_eo_heat_keys ? "on" : "off", failed);
    api->log(api, line);
}

/* Batch 3c (review/2026-10-05-eo-hook-strategies.md; slots 128-137):
 *   world_map_anywhere  the pause-menu World Map (case 0x2f of 0x57c660) also opens in races and pursuits: the
 *                       race-status value [[0x91e000]+0x1960] that blocks it reads 0 only for that check (saved in an
 *                       AFTER on 0x633dc0 at 0x57c742, restored before the call at 0x57c941 and on exit)
 *   skip_track_anywhere the T key skips the music track in game too: game flow 0x925e90 reads 3 only for the
 *                       front-end check in 0x517070 (set after 0x5b5b70/0x591d90 when they return 0, restored at
 *                       the call at 0x5170dc and on exit)
 *   no_catch_up         ICheater::GetCatchupCheat 0x409390 returns 0.0 (ST0) and the quick-race catch-up byte is
 *                       forced off (call sites 0x7ac20b, 0x7ac240)  (owner-kept gameplay departure, off by default)
 * Not included: Extra Options' AlwaysRain road-reflection patch (marked "??" in its source) and the Time of Day
 * menu slider (moved to study S1). */
static uint32_t g_eo_map_saved, g_eo_map_pending, g_eo_flow_saved, g_eo_flow_pending;
static void eo_map_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t rs = get_u32(0x0091e000u), selp = get_u32(cpu->esp + 0x14u);
    if (!rs || !selp || get_u32(selp) != 0x2fu) return;
    const uint32_t v = get_u32(rs + 0x1960u);
    if (!v) return;
    g_eo_map_saved = v; g_eo_map_pending = rs;
    api->guest_write_u32(api, rs + 0x1960u, 0);
    eo_tr(api, "world map in race/pursuit: race status cleared for the check", v, rs, 0);
}
static void eo_map_restore(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    if (g_eo_map_pending) { api->guest_write_u32(api, g_eo_map_pending + 0x1960u, g_eo_map_saved); g_eo_map_pending = 0; }
}
static void eo_flow_fake(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (g_eo_trace) {
        static unsigned n;
        if (++n <= 16) {
            char line[140];
            snprintf(line, sizeof line, "core.nfsmw: EO trace: skip-track helper returned %u (flow %u)", cpu->eax,
                     get_u32(0x00925e90u));
            api->log(api, line);
        }
    }
    if (cpu->eax != 0 || g_eo_flow_pending) return;
    const uint32_t f = get_u32(0x00925e90u);
    if (f == 3u) return;
    g_eo_flow_saved = f; g_eo_flow_pending = 1;
    api->guest_write_u32(api, 0x00925e90u, 3u);
    static unsigned fired;
    if (++fired <= 8) {
        char line[120];
        snprintf(line, sizeof line, "core.nfsmw: Extra Options: skip track in game (flow %u read as 3 for the music check)", f);
        api->log(api, line);
    }
}
static void eo_flow_restore(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    if (g_eo_flow_pending) { api->guest_write_u32(api, 0x00925e90u, g_eo_flow_saved); g_eo_flow_pending = 0; }
}
static void eo_skip_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    static unsigned n;
    if (++n <= 16) {
        char line[120];
        snprintf(line, sizeof line, "core.nfsmw: EO trace: skip-track function 0x517070 entered (flow %u)", get_u32(0x00925e90u));
        api->log(api, line);
    }
}
static uint64_t g_eo_catchup_calls;
static void eo_catchup_zero(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    cpu->st[cpu->fpu_top & 7u] = 0.0; /* ST0: st[] is physical, ST(i) = st[(top + i) & 7] (kit hooks.cpp) */
    ++g_eo_catchup_calls;
}
static void eo_catchup_bl(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    cpu->ebx &= ~0xffu;
}
static void eo_catchup_byte(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (cpu->eax) api->guest_write_u8(api, cpu->eax + 0xbu, 0);
}

static void eo_extra_init3c(const PopModApi *api) {
    char line[220];
    const int map = setting("world_map_anywhere", 1) != 0, skip = setting("skip_track_anywhere", 1) != 0,
              nocatch = setting("no_catch_up", 0) != 0;
    int failed = 0;
#define EO_OK(x) do { if ((x) != POP_OK) ++failed; } while (0)
    if (map) {
        EO_OK(install_at(0x00633dc0u, 0x0057c742u, eo_map_check, POP_HOOK_AFTER, 128));
        EO_OK(install_at(0x00627400u, 0x0057c941u, eo_map_restore, POP_HOOK_BEFORE, 129));
        EO_OK(install(0x0057c660u, eo_map_restore, POP_HOOK_AFTER, 130));
    }
    { const char *tr = getenv("NFSMW_EO_TRACE"); g_eo_trace = tr && *tr == '1'; }
    if (g_eo_trace) EO_OK(install(0x00517070u, eo_skip_enter, POP_HOOK_BEFORE, 138));
    if (skip) {
        EO_OK(install_at(0x005b5b70u, 0x005170bfu, eo_flow_fake, POP_HOOK_AFTER, 131));
        EO_OK(install_at(0x00591d90u, 0x005170abu, eo_flow_fake, POP_HOOK_AFTER, 132));
        EO_OK(install_at(0x005cc240u, 0x005170dcu, eo_flow_restore, POP_HOOK_BEFORE, 133));
        EO_OK(install(0x00517070u, eo_flow_restore, POP_HOOK_AFTER, 134));
    }
    if (nocatch) {
        EO_OK(install(0x00409390u, eo_catchup_zero, POP_HOOK_AFTER, 135));
        EO_OK(install_at(0x0056dbe0u, 0x007ac20bu, eo_catchup_bl, POP_HOOK_AFTER, 136));
        EO_OK(install_at(0x0056dbe0u, 0x007ac240u, eo_catchup_byte, POP_HOOK_AFTER, 137));
    }
#undef EO_OK
    snprintf(line, sizeof line, "core.nfsmw: Extra Options (Remaster EO 3c): world map anywhere %s, skip track anywhere %s, no catch-up %s; hooks failed %d",
             map ? "on" : "off", skip ? "on" : "off", nocatch ? "on" : "off", failed);
    api->log(api, line);
}

#endif
