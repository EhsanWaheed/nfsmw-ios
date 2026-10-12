/* Test84 (MENU3 section 5): Quality presets Low / Medium / High / Ultra / Custom on the Graphics page. A preset is a
 * compact recipe over the game's OWN video values (the ones its Video menu edits; 009017b0.., caps at +0x88,
 * clamped by 006c1780). It never touches anti-aliasing (owner 8 Oct: Smooth edges, `antialiasing`, is the player's
 * own Off / 2x / 4x choice, so a preset neither sets it nor reads it for its label), appearance, resolution, the
 * frame cap, audio, controls or gameplay. Choosing one writes its values once and asks for the renderer reset the game's
 * own Video menu uses (00982c39 = 1, handled by 006e7220 at a safe point); it does not keep re-asserting itself: any
 * later individual change (the game's Video menu) turns the label to Custom, and an exact match of
 * a recipe shows that recipe's name. The label is derived from the game's actual values about once a second.
 * At video load a chosen preset's values are written before the device exists (as Anti-aliasing is).
 * The tiers are INITIAL recipes; Test84 measures and calibrates them. Ultra is the accepted HQ set (Development
 * 57+: car reflection 3, road 3, world 3, shadows 2, filtering 2). Until 8 Oct High and Ultra also set 4x
 * anti-aliasing and Low/Medium off; that column is gone. */
#define QP_FIELDS 7
#define QP_CUSTOM 4
static const uint32_t k_qp_addr[QP_FIELDS] = {0x009017b8u, 0x009017bcu, 0x009017d4u, 0x009017f4u,
                                              0x009017f8u, 0x00901830u, 0x00901818u};
static const char *const k_qp_tier[5] = {"Low", "Medium", "High", "Ultra", "Custom"};
/* car reflection update, car reflection detail, road reflection, world detail, 009017f8, shadows, texture
 * filtering (0 bilinear, 1 trilinear, 2 anisotropic) */
static const uint8_t k_qp_recipe[4][QP_FIELDS] = {
    {0, 0, 0, 1, 0, 2, 1}, /* Low (shadows held at 2: see qp_shadow_floor) */
    {1, 0, 1, 2, 0, 2, 2}, /* Medium */
    {2, 1, 2, 3, 1, 2, 2}, /* High */
    {3, 1, 3, 3, 1, 2, 2}, /* Ultra */
};
static int64_t g_qp_applied = -2;
static uint64_t g_qp_tick;

static int qp_caps_ready(void) { return get_u32(0x0090187cu) != 0; /* world detail cap, set at hardware detection */ }
static uint32_t qp_want(int tier, int i) {
    const uint32_t v = k_qp_recipe[tier][i];
    if (!qp_caps_ready()) return v;
    const uint32_t cap = get_u32(k_qp_addr[i] + 0x88u);
    return v < cap ? v : cap;
}
/* the recipe the game's actual values match, else Custom */
static int qp_match(void) {
    for (int t = 3; t >= 0; --t) {
        int ok = 1;
        for (int i = 0; i < QP_FIELDS && ok; ++i)
            if (get_u32(k_qp_addr[i]) != qp_want(t, i)) ok = 0;
        if (ok) return t;
    }
    return QP_CUSTOM;
}
/* Test94: the game's shadow detail below 2 misdraws on this renderer, in every look (Ultra with shadows 1: hard
 * black tree shadows over the road; 0: the road near the car black, the car body white; the PC look the same), so
 * the value is held at 2 (or the hardware cap) wherever it comes from: a preset, the game's Video menu, a saved
 * profile. A lower value is raised before the renderer reset the game asked for, or with one of its own. To be
 * fixed in the renderer (optimization step); then this floor goes. */
#define QP_SHADOWS 0x00901830u
static int g_qp_shadow_logged;
static void qp_shadow_floor(const PopModApi *api, int reset) {
    if (getenv("NFSMW_TEST_QP_FIELD")) return;   /* a test that sets fields keeps them */
    const uint32_t v = get_u32(QP_SHADOWS);
    uint32_t floor = 2;
    if (qp_caps_ready()) {
        const uint32_t cap = get_u32(QP_SHADOWS + 0x88u);
        if (cap < floor) floor = cap;
    }
    if (v >= floor) return;
    uint8_t pending = 0;
    api->guest_read_u8(api, GAME_VIDEO_RESET_REQUEST, &pending);
    api->guest_write_u32(api, QP_SHADOWS, floor);
    if (reset && !pending) api->guest_write_u8(api, GAME_VIDEO_RESET_REQUEST, 1);
    if (g_qp_shadow_logged++ < 8) {
        char line[160];
        snprintf(line, sizeof line, "core.nfsmw: shadow detail %u raised to %u (lower values misdraw on this renderer)%s",
                 v, floor, reset ? (pending ? "; with the game's own renderer reset" : "; renderer reset requested") : "");
        api->log(api, line);
    }
}
/* Before msaa_apply (which owns anti-aliasing; a preset leaves it as the player set it). */
static void quality_apply(const PopModApi *api, int reset) {
    qp_shadow_floor(api, reset);
    const int64_t want = setting("quality", QP_CUSTOM);
    if (want != g_qp_applied) {
        g_qp_applied = want;
        if (want < 0 || want >= QP_CUSTOM) return;
        const int t = (int)want;
        char line[200];
        int n = snprintf(line, sizeof line, "core.nfsmw: Quality %s %s:", k_qp_tier[t], reset ? "applied" : "at video load");
        /* TEST-ONLY (Test94): NFSMW_TEST_QP_FIELD="i=v,i=v" overrides recipe fields, to find which one misdraws */
        int test_field[QP_FIELDS];
        for (int i = 0; i < QP_FIELDS; ++i) test_field[i] = -1;
        const char *test_env = getenv("NFSMW_TEST_QP_FIELD");
        if (test_env)
            for (const char *c = test_env; *c;) {
                int fi = -1, fv = -1;
                if (sscanf(c, "%d=%d", &fi, &fv) == 2 && fi >= 0 && fi < QP_FIELDS) test_field[fi] = fv;
                const char *nx = strchr(c, ',');
                if (!nx) break;
                c = nx + 1;
            }
        for (int i = 0; i < QP_FIELDS; ++i) {
            const uint32_t v = test_field[i] >= 0 ? (uint32_t)test_field[i] : qp_want(t, i);
            api->guest_write_u32(api, k_qp_addr[i], v);
            n += snprintf(line + n, sizeof line - (size_t)n, " %u", v);
        }
        if (reset) api->guest_write_u8(api, GAME_VIDEO_RESET_REQUEST, 1);
        snprintf(line + n, sizeof line - (size_t)n, "%s", reset ? " (renderer reset requested)" : "");
        api->log(api, line);
        return;
    }
    /* the label follows the actual values, about once a second, never while a reset is pending */
    if (!reset || ++g_qp_tick % 60u) return;
    uint8_t pending = 0;
    api->guest_read_u8(api, GAME_VIDEO_RESET_REQUEST, &pending);
    if (pending || aa_effective() != g_msaa_applied) return;
    const int m = qp_match();
    if (m != want) {
        g_qp_applied = m;
        api->settings_set(api, "quality", m);
        char line[120];
        snprintf(line, sizeof line, "core.nfsmw: Quality shows %s (the game's video values %s)", k_qp_tier[m],
                 m == QP_CUSTOM ? "match no preset" : "match it");
        api->log(api, line);
    }
}
