/* view_hud.h - field of view, front-end and cinematics scale, HUD groups (moved out of nfsmw.c 7 Oct 2026, unchanged). Included once by nfsmw.c at the same place, so the single translation unit is as before. */
#ifndef VIEW_HUD_H
#define VIEW_HUD_H

/* ---- field of view ------------------------------------------------------ */

static void view_projection(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t view = 0, id = 0;
    float h = 1.0f, half = 0.5f, v = 1.0f, aspect = 1.0f;
    (void)inv;
    (void)user;
    if (api->guest_read_u32(api, cpu->esp + 4, &view) != POP_OK || !view ||
        api->guest_read_u32(api, view + 4, &id) != POP_OK)
        return;
    if (id == 1 || id == 4) { /* the player's view and the headlights */
        h = g_hor;
        half = g_half;
        v = g_vert;
    }
    if (id == 3) { /* rear-view mirror */
        aspect = 0.4f;
        const int64_t f = setting("mirror_fov", 100);   /* run 13 mirror parity: 100 = the original, exactly */
        if (f != 100 && f >= 50 && f <= 200) {
            h = (float)f / 100.0f;
            half = 0.5f * h;
            v = h;
        }
    }
    put_float(SLOT_FOV_H, h);
    put_float(SLOT_FOV_HALF, half);
    put_float(SLOT_FOV_V, v);
    put_float(SLOT_FOV_ASPECT, aspect);
}

/* The pinned PC mirror compositor fills four RGBA colours with 255 at
 * 006e7120. Its captured pixel shader f8813735427aff31 multiplies RGB by
 * texture * vertex colour * 2, clipping cloudy sky before presentation.
 * Use the nearest byte to half intensity for this quad only. Alpha stays
 * untouched; the shared front-end shader and the mirror render pass retain
 * their original behavior. Disable mirror_neutral_exposure for an A/B run.
 * The 006d1260 listing verifies the four RGBA records at quad+0x80. */
static void mirror_exposure(const PopModApi *api, pop_cpu_v1 *cpu,
                            PopHookInvocation *inv, void *user) {
    uint32_t quad = 0;
    void *data = NULL;
    (void)inv;
    (void)user;
    if (!setting("mirror_neutral_exposure", 1) ||
        api->guest_read_u32(api, cpu->esp + 4, &quad) != POP_OK || !quad ||
        api->guest_ptr(api, quad, 0x90, &data) != POP_OK)
        return;
    uint8_t *colours = (uint8_t *)data + 0x80;
    for (unsigned vertex = 0; vertex < 4; ++vertex)
        for (unsigned channel = 0; channel < 3; ++channel)
            colours[vertex * 4 + channel] = 128;
}

/* ---- front end and cinematics scale -------------------------------------- */

static void set_transform(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t mat;
    void *src;
    /* A cinematic's quad is sized on its own; the rest of the front end keeps
     * its place, so what sits off screen stays there. */
    float scale = get_u32(MOVIE_PLAYER) ? 1.0f : g_fe_scale;
    (void)inv;
    (void)user;
    if (scale == 1.0f || !g_matrix || api->guest_read_u32(api, cpu->esp + 4, &mat) != POP_OK || !mat)
        return;
    if (api->guest_ptr(api, mat, 64, &src) != POP_OK)
        return;
    void *dst;
    if (api->guest_ptr(api, g_matrix, 64, &dst) != POP_OK)
        return;
    memcpy(dst, src, 64);
    put_float(g_matrix + 0, get_float(mat + 0) * scale);   /* _11 */
    put_float(g_matrix + 20, get_float(mat + 20) * scale); /* _22 */
    api->guest_write_u32(api, cpu->esp + 4, g_matrix);
}

/* ---- HUD groups ---------------------------------------------------------- */

struct Centre {
    uint32_t object;
    float x, y;
};
static struct Centre g_left, g_right;

static void shift_group(uint32_t package, uint32_t hash, struct Centre *cache, float offset) {
    uint32_t object = 0;
    uint32_t args[3] = {package, hash, 0};
    if (g_api->guest_call(g_api, FIND_OBJECT, 0, args, 2, &object) != POP_OK || !object)
        return;
    if (cache->object != object) {
        uint32_t get[3] = {object, g_floats, g_floats + 4};
        if (g_api->guest_call(g_api, GET_CENTER, 0, get, 3, 0) != POP_OK)
            return;
        cache->object = object;
        cache->x = get_float(g_floats);
        cache->y = get_float(g_floats + 4);
    }
    uint32_t set[3] = {object, float_bits(cache->x + offset), float_bits(cache->y)};
    g_api->guest_call(g_api, SET_CENTER, 0, set, 3, 0);
}

static void shift_groups(uint32_t package) {
    if (!package || !g_floats || !g_api->guest_call)
        return;
    shift_group(package, 0x1603009Eu, &g_left, -g_hud_offset);
    shift_group(package, 0x5D0101F1u, &g_right, g_hud_offset);
}

/* The two package messages FEngHud sends when the layout changes. */
static void queue_package_message(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t ret = get_u32(cpu->esp);
    uint32_t package = get_u32(cpu->esp + 8);
    (void)inv;
    (void)user;
    api->call_original(api, cpu->target, cpu);
    if (ret == QPM_RET_A || ret == QPM_RET_B)
        shift_groups(package);
}

/* Minimap: its local position, five children and pivot, moved with the HUD. */
struct MinimapCache {
    uint32_t object;
    float local_x;
    float child_x[5];
};
static struct MinimapCache g_minimap;
static const uint32_t kChildOffsets[5] = {60, 64, 68, 72, 144};

static uint32_t child_data(uint32_t object, int i) {
    uint32_t child = get_u32(object + kChildOffsets[i]);
    return child ? get_u32(child + 44) : 0;
}

static void adjust_minimap(uint32_t object, int wide) {
    const float offset = wide ? -g_hud_offset : g_hud_offset;
    put_float(MINIMAP_PIVOT_X, wide ? -g_hud_offset : 0.0f);
    put_float(MINIMAP_DISP_X, wide ? -0.9375f : 0.9375f);
    if (g_minimap.object != object) {
        g_minimap.object = object;
        g_minimap.local_x = get_float(object + 184);
        for (int i = 0; i < 5; ++i) {
            uint32_t data = child_data(object, i);
            g_minimap.child_x[i] = data ? get_float(data + 28) : 0.0f;
        }
    }
    put_float(object + 184, g_minimap.local_x + offset);
    for (int i = 0; i < 5; ++i) {
        uint32_t data = child_data(object, i);
        if (data)
            put_float(data + 28, g_minimap.child_x[i] + offset);
    }
    uint32_t data = child_data(object, 4);
    if (data)
        g_api->guest_write_u32(g_api, data + 32, get_u32(object + 188));
}

static void minimap(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint8_t wide = 0;
    (void)inv;
    (void)user;
    api->guest_read_u8(api, cpu->esp + 4, &wide);
    if (cpu->ecx)
        adjust_minimap(cpu->ecx, wide != 0);
    api->hook_return(api, cpu, cpu->eax, 4);
}

static void set_widescreen_mode(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t self = cpu->ecx;
    (void)inv;
    (void)user;
    if (g_hud_dirty && self) {
        uint8_t wide = 0;
        uint32_t package = get_u32(self + 36);
        uint32_t args[3] = {0, package, 0};
        api->guest_read_u8(api, self + 816, &wide);
        args[0] = wide ? 0x62ED04ECu : 0x53EC068Cu;
        if (api->guest_call(api, QUEUE_PACKAGE_MESSAGE, get_u32(FENG_INSTANCE), args, 3, 0) == POP_OK)
            shift_groups(package);
        uint32_t map = get_u32(self + 800);
        if (map)
            adjust_minimap(map, wide != 0);
        g_hud_dirty = 0;
    }
    api->call_original(api, cpu->target, cpu);
}

#endif
