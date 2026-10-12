/* Test96 (RC review, approved 2 Oct): Appearance recipes with a truthful Custom state, as the
 * settings plan specifies (Appearance and Quality are separate recipe domains; a single edit is respected; a recipe
 * is applied once when chosen, never re-asserted). Reshaped 2 Oct 2026 (owner): one Definitive version - the best of
 * the Xbox 360, the PC and the community mods - with independent switches instead of separate looks.
 *   0 Definitive (default)    hybrid_exposure 50, hybrid_vignette 0, hybrid_shadows 0
 *   1 (retired)               was Xbox 360 - Original: now Definitive + `xbox_colour_filter` on
 *   2 PC - Original           no adjustable appearance values
 *   3 Definitive - Custom     Definitive with any of its tuning values changed
 *   4 (retired)               was Xbox 360 - Custom: now Definitive + `xbox_colour_filter` on (its values kept)
 * The switches (Xbox 360 colour filter with its highlights/bloom/shadow sliders; motion blur and cinematic depth of
 * field as they arrive) are separate choices, not part of the recipe: turning one on keeps the Definitive label.
 * A profile holding 1 or 4 moves, once, to Definitive (the colour filter left as it is, off by default; Test128), and
 * the log says so. The menu steps over the empty choice label of slot 1. Choosing Definitive writes its recipe
 * values; choosing Definitive - Custom keeps the current values. The label then follows the actual values (twice a
 * second): an edited value turns Definitive into Custom, values equal to the recipe turn Custom back. */
#define APP_RETIRED_XBOX 1
#define APP_HYBRID_CUSTOM 3
#define APP_RETIRED_XBOX_CUSTOM 4
static int64_t g_app_seen = -1;
static uint64_t g_app_tick;
static int app_definitive_original(void) {
    return setting("hybrid_exposure", 50) == 50 && setting("hybrid_vignette", 0) == 0 && setting("hybrid_shadows", 0) == 0;
}
static void app_definitive_recipe(const PopModApi *api) {
    api->settings_set(api, "hybrid_exposure", 50);
    api->settings_set(api, "hybrid_vignette", 0);
    api->settings_set(api, "hybrid_shadows", 0);
}
static void appearance_recipe(const PopModApi *api) {
    static const char *const names[] = {"Definitive", "(retired Xbox 360)", "PC - Original", "Definitive - Custom",
                                        "(retired Xbox 360 - Custom)"};
    int64_t v = LOOK_HYBRID;
    api->settings_get(api, "appearance", &v);
    if (v == APP_RETIRED_XBOX || v == APP_RETIRED_XBOX_CUSTOM || v < 0 || v > APP_RETIRED_XBOX_CUSTOM) {
        /* Test128 (owner's Development 61 session, 2 Oct 2026): the move used to turn the colour filter on at the
         * console's values (highlights 100, bloom 100 = x4), which blew out the front end. Now the filter stays off
         * (its default) and its values are left as they are; turning it on stays the player's choice. */
        const int was_xbox = v == APP_RETIRED_XBOX || v == APP_RETIRED_XBOX_CUSTOM;
        api->settings_set(api, "appearance", LOOK_HYBRID);
        char line[220];
        snprintf(line, sizeof line, "core.nfsmw: appearance %lld moved to Definitive%s (2 Oct 2026: one version, the Xbox 360 "
                 "colour filter is a switch)", (long long)v, was_xbox ? "; the Xbox 360 colour filter stays a separate switch" : "");
        api->log(api, line);
        g_app_seen = LOOK_HYBRID;
        return;
    }
    if (g_app_seen >= 0 && v != g_app_seen) {   /* chosen in the menu (or by a test key) */
        char line[160];
        if (v == LOOK_HYBRID) {
            app_definitive_recipe(api);
            snprintf(line, sizeof line, "core.nfsmw: appearance recipe %s applied (auto exposure 50, vignette 0, shadow detail 0)", names[v]);
        } else {
            snprintf(line, sizeof line, "core.nfsmw: appearance %s chosen (current values kept)", names[v]);
        }
        api->log(api, line);
        g_app_seen = v;
        return;
    }
    if (g_app_seen >= 0 && ++g_app_tick % 30u) return;
    const int64_t want = v == LOOK_PC ? LOOK_PC : app_definitive_original() ? LOOK_HYBRID : APP_HYBRID_CUSTOM;
    if (want != v) {
        api->settings_set(api, "appearance", want);
        char line[120];
        snprintf(line, sizeof line, "core.nfsmw: appearance shows %s (its values %s the recipe)", names[want],
                 want == APP_HYBRID_CUSTOM ? "differ from" : "match");
        api->log(api, line);
    }
    g_app_seen = want;
}
