/* world_light_pass.h - the world auxiliary-light passes (Test67/Test76; the clean pools draw through them; the Xbox data paths are retired) (moved out of nfsmw.c 7 Oct 2026, unchanged). Included once by nfsmw.c at the same place, so the single translation unit is as before. */
#ifndef WORLD_LIGHT_PASS_H
#define WORLD_LIGHT_PASS_H
#include "vehicle_lights.h"   /* run 13: car lights in the clean pools (setting vehicle_lights; off: nd = 0) */

/* ---- Test67: Xbox 360 scenery auxiliary lights, first boundary (data + draw identity) ---- */
static X3sl g_x3sl;
static int g_x3sl_ready;
static uint64_t g_x3sl_items, g_x3sl_matched_items, g_x3sl_tagged_entries, g_x3sl_tagged_draws, g_x3sl_aux_draws;
static uint64_t g_x3sl_type_draws[16]; /* tagged draws per PC effect index (0 WORLD, 1 WORLDREFLECT, 2 WORLDBONE, 3 WORLDNORMALMAP, ...) */
/* Test76: light pools - the WORLD effect's auxiliary-light pass for receivers (PC terrain pieces covering
 * USA AUX chops) and for Test67's verified instances drawn with WORLD. */
static X3lr g_x3lr;
#include "pool_prof.h"   /* TEST ONLY (Remaster P1): pool-pass counts/times, props toggle */
static int g_x3lr_pass_ready;
static uint32_t g_x3lr_values;
static uint64_t g_x3lr_recv_items, g_x3lr_matrix_items, g_x3lr_recv_draws, g_x3lr_set_draws, g_x3lr_single_pass, g_x3lr_other_view;
/* 00982b25 (set per frame by 006e7220; read only by the two pass-count checks 006e0200/006d7660) is set for
 * one entry so the engine draws the WORLD effect's second pass, and put back at the next entry / after the list. */
static int g_x3lr_b25_set;
static uint32_t g_x3lr_b25_old;
static void x3lr_set_b25(uint32_t v) {
    const uint32_t w = get_u32(0x00982b24u);
    g_api->guest_write_u32(g_api, 0x00982b24u, (w & ~0xff00u) | ((v & 0xffu) << 8));
}
static void x3lr_restore_flag(void) {
    if (g_x3lr_b25_set) x3lr_set_b25(g_x3lr_b25_old);
    g_x3lr_b25_set = 0;
}
/* Per render entry: the inverse placement of its draw item (world -> model space), so the auxiliary passes
 * can give lights in the model's own space. State 0: world-space model (no matrix, or identity); 1: placed
 * (g_entry_inv valid); 2: matrix unreadable or singular (no auxiliary light). */
static uint8_t g_entry_space[X3SL_MAX_ENTRIES];
static float g_entry_inv[X3SL_MAX_ENTRIES][12];
static uint64_t g_x3_placed_lit;
static uint64_t g_x3lr_type_draws[16]; /* receiver / WORLD-tagged entries drawn, by PC effect index */
/* Test113 module F (clean_pools): the same added-pass route with the clean side's pass and PC lamp data. Per render
 * entry: the selected lights (CleanPools block, already in the entry's space) and their count. */
static Cpl g_cpl;
static int g_cpl_mode;
static uint8_t g_cpl_count[X3SL_MAX_ENTRIES];
static float g_cpl_regs[X3SL_MAX_ENTRIES][2 * CPL_MAX][4];
static uint32_t g_cpl_values;              /* guest: CleanPools[16], CleanPoolCount, CleanPoolParams, then the names */
static uint32_t g_cpl_handles[3][3];       /* per pass: CleanPools, CleanPoolCount, CleanPoolParams */
static uint64_t g_cpl_items, g_cpl_lit_items, g_cpl_lights_sum, g_cpl_draws;
static int g_x3_old_prop_space; /* TEST ONLY (NFSMW_TEST_X3_OLD_PROP_SPACE): world-space lights for placed models, as before Test76 */
static uint8_t x3_item_space(const PopModApi *api, uint32_t matrix, float inv[12]) {
    if (!matrix) return 0;
    void *m = NULL;
    if (api->guest_ptr(api, matrix, 64, &m) != POP_OK || !m) return 2;
    if (x3lr_is_identity((const float *)m)) return 0;
    return x3lr_inverse_affine((const float *)m, inv) ? 1 : 2;
}
/* Light position registers (xyz of every second register from `first`, `n` lights) into entry space. */
static int x3_lights_to_entry(uint32_t index, float (*regs)[4], unsigned first, unsigned n) {
    if (g_x3_old_prop_space) return 1;
    if (index >= X3SL_MAX_ENTRIES || g_entry_space[index] == 2) return 0;
    if (g_entry_space[index] == 1)
        for (unsigned i = 0; i < n; ++i) x3lr_to_local(g_entry_inv[index], regs[first + 2 * i]);
    return 1;
}

static int x3sl_init(const PopModApi *api) {
    if (api) return 0;   /* Retired 7 Oct 2026 (owner): the Xbox 360-derived light pools / scenery lights; clean pools off = no pools. */
    char path[2048];
    snprintf(path, sizeof path, "%s/x360-scenery-lights.bin", api->mod_dir(api));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *b = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < 8 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
        (b = (uint8_t *)malloc((size_t)n)) && fread(b, 1, (size_t)n, f) == (size_t)n) {
        g_x3sl_ready = x3sl_parse(&g_x3sl, b, (size_t)n);
    }
    free(b);
    fclose(f);
    char line[160];
    snprintf(line, sizeof line, g_x3sl_ready ? "core.nfsmw: Xbox scenery lights: %u verified instances, %u USA contexts"
                                             : "core.nfsmw: Xbox scenery lights: data rejected (%u/%u)",
             g_x3sl.nkeys, g_x3sl.nsets);
    api->log(api, line);
    return g_x3sl_ready;
}

/* Call site 007241fe (00724080's call of 006da9b0 for one draw item; edi = item {model, matrix,
 * instance}). The original runs unchanged, still with a NULL light context. */
#include "sign_trace.h"     /* Test145: narrow draw trace for the Rosewood sign (scenery_trace) */
static void x3sl_render_item(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    uint32_t set = UINT32_MAX;
    const uint32_t instance = get_u32(cpu->edi + 8);
    const uint32_t matrix = get_u32(cpu->edi + 4); /* 0: the model is in world space */
    void *p = NULL;
    uint8_t key[X3SL_KEY_BYTES];
    ++g_x3sl_items;
    if (instance && api->guest_ptr(api, instance, 0x40, &p) == POP_OK && p) {
        x3sl_key_from_instance((const uint8_t *)p, key);
        set = x3sl_lookup(&g_x3sl, key);
    }
    static float near_x, near_y;
    if (p) {
        const float *inst = (const float *)p;
        if (inst[8] != 0.0f || inst[9] != 0.0f) { near_x = inst[8]; near_y = inst[9]; }
    }
    if (g_x3sl_items == 1 || g_x3sl_items == 100000 || g_x3sl_items % 1000000 == 0) {
        char line[240];
        snprintf(line, sizeof line, "core.nfsmw: Xbox scenery lights: %llu draw items so far (near %.0f/%.0f), %llu verified instances drawn, %llu tagged entries, %llu tagged draws, %llu with the aux pass",
                 (unsigned long long)g_x3sl_items, near_x, near_y, (unsigned long long)g_x3sl_matched_items,
                 (unsigned long long)g_x3sl_tagged_entries, (unsigned long long)g_x3sl_tagged_draws,
                 (unsigned long long)g_x3sl_aux_draws);
        api->log(api, line);
    }
    const uint32_t before = get_u32(0x00982cdcu);
    api->call_original(api, cpu->target, cpu);
    uint32_t after = get_u32(0x00982cdcu);
    if (after < before || after > X3SL_MAX_ENTRIES) after = before;
    uint32_t parts[64];
    if (after - before > 64) after = before + 64; /* one model: a handful of parts */
    for (uint32_t i = before; i < after; ++i) parts[i - before] = get_u32(0x0093e878u + i * 0x44u);
    sign_trace_item(api, instance, before, after);   /* Test145 */
    if (set != UINT32_MAX || g_x3lr_pass_ready) {
        float inv[12];
        const uint8_t space = x3_item_space(api, matrix, inv);
        for (uint32_t i = before; i < after && i < X3SL_MAX_ENTRIES; ++i) {
            g_entry_space[i] = space;
            if (space == 1) memcpy(g_entry_inv[i], inv, sizeof inv);
        }
        if (set != UINT32_MAX && space == 1) ++g_x3_placed_lit;
    }
    x3sl_tag(&g_x3sl, before, after, set, parts);
    if (g_x3lr_pass_ready && g_cpl_mode) {
        /* Test113 module F: lights whose sphere meets the instance's world box (instance +0x00 min, +0x0c max),
         * nearest to the camera first, moved into each entry's space. */
        uint32_t m = 0, idx[CPL_MAX], nd = 0, didx[VL_ITEM_MAX];
        float eye_world[3] = {0, 0, 0};
        int have_eye_world = 0;
        ++g_cpl_items;
        if (p && before < X3SL_MAX_ENTRIES && g_entry_space[before] != 2) {
            const float *inst = (const float *)p;
            float eye[3] = {0, 0, 0}, viewm[16], vinv[12];
            const uint32_t cam = get_u32(0x009195e0u + 0x70u + 0x40u);
            int have_eye = 0;
            if (cam) {
                for (unsigned k = 0; k < 16; ++k) viewm[k] = get_float(cam + 4u * k);
                if (x3lr_inverse_affine(viewm, vinv)) { x3lr_to_local(vinv, eye); have_eye = 1; }
            }
            nd = vl_touching(inst, inst + 3, didx, VL_ITEM_MAX);   /* vehicle_lights.h: 0 when off -> cap CPL_MAX */
            m = cpl_select_cap(&g_cpl, inst, inst + 3, have_eye ? eye : NULL, idx, g_ld_active ? g_ld_kind : NULL, CPL_MAX - nd);
            if (have_eye) { memcpy(eye_world, eye, sizeof eye); have_eye_world = 1; }
        }
        const uint32_t mt = m + nd;   /* lamps, then car lights */
        if (mt && g_pp_csv) {   /* TEST ONLY (P1): measure-only zero-contribution class of these entries */
            uint8_t zc = 2;
            if (p && before < X3SL_MAX_ENTRIES && g_entry_space[before] == 0 && have_eye_world) {
                const float *inst = (const float *)p;
                const float fe = g_cpl.params[1] + 1.0f;
                zc = cpl_box_d2(eye_world, inst, inst + 3) >= fe * fe ? 1 : 0;
            }
            for (uint32_t i = before; i < after && i < X3SL_MAX_ENTRIES; ++i) g_pp_zclass[i] = zc;
        }
        if (mt) {
            if (m) { ++g_cpl_lit_items; g_cpl_lights_sum += m; }
            float block[2 * CPL_MAX][4];
            cpl_block(&g_cpl, idx, m, block, g_ld_active ? g_ld_kind : NULL);
            if (nd) {   /* vehicle_lights.h: tunnel = a tunnel lamp in this item's selection */
                int tunnel = 0;
                for (uint32_t j = 0; j < m; ++j) tunnel |= g_cpl.l[idx[j]].kind == 1;
                for (uint32_t j = 0; j < nd; ++j) {
                    const VlLight *L = &g_vl_l[didx[j]];
                    cpl_block_append(block, m + j, L->p, L->r, L->col, L->d0, vl_activation(L->kind, g_vl_a_dark, tunnel));
                }
                ++g_vl_lit_items; g_vl_adds += nd;
                g_vl_new_tagged += m == 0;
                g_vl_capped += m == CPL_MAX - nd;
            }
            for (uint32_t i = before; i < after && i < X3SL_MAX_ENTRIES; ++i) {
                memcpy(g_cpl_regs[i], block, sizeof block);
                if (g_entry_space[i] == 1)
                    for (uint32_t j = 0; j < mt; ++j) x3lr_to_local(g_entry_inv[i], g_cpl_regs[i][2 * j]);
                g_cpl_count[i] = (uint8_t)mt;
            }
        }
        x3lr_tag(&g_x3lr, before, after, mt ? 0u : UINT32_MAX, parts);
        if (g_cpl_items == 1 || g_cpl_items % 2000000 == 0) {
            char line[200];
            snprintf(line, sizeof line, "core.nfsmw: clean pools: %llu draw items, %llu with lights (mean %.2f lights), %llu lit draws",
                     (unsigned long long)g_cpl_items, (unsigned long long)g_cpl_lit_items,
                     g_cpl_lit_items ? (double)g_cpl_lights_sum / (double)g_cpl_lit_items : 0.0, (unsigned long long)g_cpl_draws);
            api->log(api, line);
        }
    } else if (g_x3lr_pass_ready) {
        /* Receiver boxes are world boxes: receivers must be world-space models (they all are: the terrain). */
        uint32_t world = UINT32_MAX;
        const uint32_t recv = p && set == UINT32_MAX ? x3lr_lookup(&g_x3lr, key) : UINT32_MAX;
        if (recv != UINT32_MAX) ++g_x3lr_recv_items;
        if (recv != UINT32_MAX && before < X3SL_MAX_ENTRIES && g_entry_space[before] != 0) {
            ++g_x3lr_matrix_items;
            static int logged;
            void *m = NULL;
            if (logged < 4 && p && api->guest_ptr(api, matrix, 64, &m) == POP_OK && m) {
                ++logged;
                const float *f = (const float *)m, *inst = (const float *)p;
                char line[240];
                snprintf(line, sizeof line, "core.nfsmw: Xbox light pools: terrain piece at %.1f/%.1f/%.1f is placed by a matrix (diag %.3f %.3f %.3f %.3f, translation %.1f/%.1f/%.1f); no pool light",
                         inst[8], inst[9], inst[10], f[0], f[5], f[10], f[15], f[12], f[13], f[14]);
                api->log(api, line);
            }
        }
        else if (recv != UINT32_MAX) world = recv;
        else if (set != UINT32_MAX) world = 0x80000000u | set;
        x3lr_tag(&g_x3lr, before, after, world, parts);
    }
    if (set != UINT32_MAX) {
        ++g_x3sl_matched_items;
        g_x3sl_tagged_entries += after - before;
        static int logged;
        if (logged < 3) {
            ++logged;
            const float *inst = (const float *)p;
            char line[200];
            snprintf(line, sizeof line, "core.nfsmw: Xbox scenery lights: instance at %.1f/%.1f/%.1f has USA context %d (%u lights), %u render entries",
                     inst[8], inst[9], inst[10], g_x3sl.sets[set].number, g_x3sl.sets[set].count, after - before);
            api->log(api, line);
        }
    }
}

/* ---- the auxiliary-light pass (Test67): a second pass in the PC WORLDNORMALMAP effect, as the USA
 * effect has one, drawn only for tagged entries. The engine's pass loop (006e05b0) draws
 * [eEffect + 0x14] passes but only begins pass 0 when the effect changes, so after an entry that used
 * the aux pass the next entry of the same effect is put back on pass 0 here, and after the list
 * (006e0200 AFTER) pass 0's states are re-applied once. */
static int g_x3sl_pass_ready;
static uint32_t g_x3sl_values, g_x3sl_scratch, g_x3sl_prev_effect, g_x3sl_prev_aux;
static uint32_t x3sl_fx(uint32_t id3dx, uint32_t slot) { return get_u32(get_u32(id3dx) + slot); }
static void x3sl_restore_pass0(const PopModApi *api, uint32_t id3dx, int begun) {
    uint32_t r = 0;
    if (begun) ++g_pp.restores_in_list; else ++g_pp.restores_list_end;   /* TEST ONLY (P1) */
    if (begun) {
        const uint32_t a1[] = {id3dx};
        api->guest_call(api, x3sl_fx(id3dx, 0x108), 0, a1, 1, &r);            /* EndPass */
        const uint32_t a2[] = {id3dx, 0};
        api->guest_call(api, x3sl_fx(id3dx, 0x100), 0, a2, 2, &r);            /* BeginPass(0) */
    } else {
        const uint32_t a1[] = {id3dx, g_x3sl_scratch, 0};
        api->guest_call(api, x3sl_fx(id3dx, 0xfc), 0, a1, 3, &r);             /* Begin */
        const uint32_t a2[] = {id3dx, 0};
        api->guest_call(api, x3sl_fx(id3dx, 0x100), 0, a2, 2, &r);            /* BeginPass(0) */
        api->guest_call(api, x3sl_fx(id3dx, 0x108), 0, a2, 1, &r);            /* EndPass */
        api->guest_call(api, x3sl_fx(id3dx, 0x10c), 0, a2, 1, &r);            /* End */
    }
}
static void x3sl_list_done(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    x3lr_restore_flag();
    if (g_x3sl_prev_aux && g_x3sl_prev_effect && get_u32(g_x3sl_prev_effect + 0x48))
        x3sl_restore_pass0(api, get_u32(g_x3sl_prev_effect + 0x48), 0);
    g_x3sl_prev_aux = 0;
    g_x3sl_prev_effect = 0;
}
/* TEST ONLY (NFSMW_TEST_AUX_PASS_KEY set in the environment): F4 switches the aux pass off/on at run
 * time, so one stationary scene can be captured both ways (F4; the key is consumed). Not installed otherwise. */
static int g_x3sl_pass_live = 1;
static uint32_t g_x3sl_key_hook;
static int32_t x3sl_test_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)vk; (void)user;
    if (dik != 0x3e) return 0; /* DIK_F4 */
    if (down) {
        g_x3sl_pass_live = !g_x3sl_pass_live;
        api->log(api, g_x3sl_pass_live ? "core.nfsmw: Xbox scenery lights: TEST aux pass on" : "core.nfsmw: Xbox scenery lights: TEST aux pass off");
    }
    return 1;
}
static int x3sl_pass_init(const PopModApi *api) {
    if (api) return 0;   /* Retired 7 Oct 2026 (owner): the Xbox 360-derived light pools / scenery lights; clean pools off = no pools. */
    char path[2048];
    snprintf(path, sizeof path, "%s/x360-worldnormalmap.fx", api->mod_dir(api));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    /* The verified IDI_WORLDNORMALMAP_FX resource data entry of the pinned PC executable. */
    long size = -1;
    if (get_u32(0x009c7638u) != 0x005ecba0u || get_u32(0x009c763cu) != 55128u || fseek(f, 0, SEEK_END) != 0 ||
        (size = ftell(f)) <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    uint32_t address = 0;
    void *bytes = NULL;
    if (api->guest_alloc(api, (uint32_t)size, &address) != POP_OK ||
        api->guest_ptr(api, address, (uint32_t)size, &bytes) != POP_OK ||
        fread(bytes, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 0; }
    fclose(f);
    static const char name[] = "AuxiliaryLights";
    if (api->guest_alloc(api, 12 * 16 + 16 + sizeof name, &g_x3sl_values) != POP_OK ||
        api->guest_ptr(api, g_x3sl_values + 12 * 16 + 16, sizeof name, &bytes) != POP_OK) return 0;
    memcpy(bytes, name, sizeof name);
    g_x3sl_scratch = g_x3sl_values + 12 * 16;
    api->guest_write_u32(api, 0x009c7638u, address - 0x00400000u);
    api->guest_write_u32(api, 0x009c763cu, (uint32_t)size);
    api->log(api, "core.nfsmw: Xbox scenery lights: auxiliary-light pass installed in the WORLDNORMALMAP effect");
    if (getenv("NFSMW_TEST_AUX_PASS_KEY") && api->on_key)
        api->on_key(api, x3sl_test_key, NULL, &g_x3sl_key_hook);
    return 1;
}

/* ---- Test76: the WORLD auxiliary-light pass (light pools) ---- */
typedef struct {
    uint32_t type, entry_va, rva, size;
    const char *file, *name;
    int ready;
    uint32_t cached_fx, handle, last_tag;
} X3lrPass;
#define X3LR_NPASS 3u
static X3lrPass g_x3lr_passes[X3LR_NPASS];
static int x3lr_init(const PopModApi *api) {
    if (setting("clean_pools", 1)) {   /* Test113 module F: the clean pools take this pass's place */
        char path[2048];
        snprintf(path, sizeof path, "%s/clean-pools.bin", api->mod_dir(api));
        FILE *f = fopen(path, "rb");
        uint8_t *b = NULL;
        long n = -1;
        int ok = 0;
        if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < 16 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
            (b = (uint8_t *)malloc((size_t)n)) && fread(b, 1, (size_t)n, f) == (size_t)n)
            ok = cpl_parse(&g_cpl, b, (size_t)n);
        free(b);
        if (f) fclose(f);
        char line[200];
        snprintf(line, sizeof line, ok ? "core.nfsmw: clean pools: %u PC lamp lights (fade %.0f-%.0f m, strength %.2f, road normal %.2f)"
                                       : "core.nfsmw: clean pools: clean-pools.bin missing or rejected (%u)",
                 g_cpl.n, (double)g_cpl.params[0], (double)g_cpl.params[1], (double)g_cpl.params[2], (double)g_cpl.params[3]);
        api->log(api, line);
        if (!ok) return 0;
        g_cpl_mode = 1;
        g_x3lr_passes[0].file = "clean-world.fx";
        g_x3lr_passes[1].file = "clean-worldreflect.fx";
        if (setting("clean_pools_props", 0)) g_x3lr_passes[2].file = "clean-worldnormalmap.fx";   /* tunnel props; off by default (C9-C11, 7 Oct) */
        return 1;
    }
    if (api) return 0;   /* Retired 7 Oct 2026 (owner): the Xbox 360-derived light pools / scenery lights; clean pools off = no pools. */
    char path[2048];
    snprintf(path, sizeof path, "%s/x360-light-receivers.bin", api->mod_dir(api));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *b = NULL;
    long n = -1;
    int ok = 0;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < 8 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
        (b = (uint8_t *)malloc((size_t)n)) && fread(b, 1, (size_t)n, f) == (size_t)n)
        ok = x3lr_parse(&g_x3lr, b, (size_t)n);
    free(b);
    fclose(f);
    char line[160];
    snprintf(line, sizeof line, ok ? "core.nfsmw: Xbox light pools: %u terrain pieces, %u light groups"
                                   : "core.nfsmw: Xbox light pools: data rejected (%u/%u)", g_x3lr.nrecv, g_x3lr.ngroups);
    api->log(api, line);
    return ok;
}
/* The two effects that get the pass: WORLD (per-vertex, USA b14e04aa) and WORLDREFLECT (the roads; per pixel,
 * normal-mapped, as the USA aux pass 0637f379/e96d1cf2), each through its verified resource data entry. */
static X3lrPass g_x3lr_passes[X3LR_NPASS] = {
    {0u, 0x009c7668u, 0x005c83e0u, 49980u, "x360-world.fx", "WORLD", 0, 0, 0, UINT32_MAX},
    {1u, 0x009c7658u, 0x005dbf08u, 57784u, "x360-worldreflect.fx", "WORLDREFLECT", 0, 0, 0, UINT32_MAX},
    /* clean mode only (clean_pools_props): WORLDNORMALMAP, the tunnel props; the Xbox mode has no file here */
    {3u, 0x009c7638u, 0x005ecba0u, 55128u, NULL, "WORLDNORMALMAP", 0, 0, 0, UINT32_MAX},
};
static int x3lr_install_effect(const PopModApi *api, X3lrPass *ps) {
    if (!ps->file) return 0;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", api->mod_dir(api), ps->file);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long size = -1;
    if (get_u32(ps->entry_va) != ps->rva || get_u32(ps->entry_va + 4) != ps->size || fseek(f, 0, SEEK_END) != 0 ||
        (size = ftell(f)) <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    uint32_t address = 0;
    void *bytes = NULL;
    if (api->guest_alloc(api, (uint32_t)size, &address) != POP_OK ||
        api->guest_ptr(api, address, (uint32_t)size, &bytes) != POP_OK ||
        fread(bytes, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 0; }
    fclose(f);
    api->guest_write_u32(api, ps->entry_va, address - 0x00400000u);
    api->guest_write_u32(api, ps->entry_va + 4, (uint32_t)size);
    char line[160];
    snprintf(line, sizeof line, g_cpl_mode ? "core.nfsmw: clean pools: pass installed in the %s effect (%s)"
                                           : "core.nfsmw: Xbox light pools: auxiliary-light pass installed in the %s effect (%s)", ps->name, ps->file);
    api->log(api, line);
    return ps->ready = 1;
}
static int x3lr_pass_init(const PopModApi *api) {
    if (g_cpl_mode) {
        static const char names[] = "CleanPools\0CleanPoolCount\0CleanPoolParams";
        void *bytes = NULL;
        if (api->guest_alloc(api, 18 * 16 + sizeof names, &g_cpl_values) != POP_OK ||
            api->guest_ptr(api, g_cpl_values + 18 * 16, sizeof names, &bytes) != POP_OK) return 0;
        memcpy(bytes, names, sizeof names);
        int any = 0;
        for (unsigned i = 0; i < X3LR_NPASS; ++i) any |= x3lr_install_effect(api, &g_x3lr_passes[i]);
        /* TEST ONLY (NFSMW_TEST_AUX_PASS_KEY): F4 switches the pools off/on at run time, as for the Test76 pass */
        if (any && getenv("NFSMW_TEST_AUX_PASS_KEY") && api->on_key && !g_x3sl_key_hook)
            api->on_key(api, x3sl_test_key, NULL, &g_x3sl_key_hook);
        return any;
    }
    static const char name[] = "AuxiliaryGroups";
    void *bytes = NULL;
    if (api->guest_alloc(api, X3LR_BLOCK * 16 + sizeof name, &g_x3lr_values) != POP_OK ||
        api->guest_ptr(api, g_x3lr_values + X3LR_BLOCK * 16, sizeof name, &bytes) != POP_OK) return 0;
    memcpy(bytes, name, sizeof name);
    int any = 0;
    for (unsigned i = 0; i < X3LR_NPASS; ++i) any |= x3lr_install_effect(api, &g_x3lr_passes[i]);
    if (!any) return 0;
    g_x3_old_prop_space = getenv("NFSMW_TEST_X3_OLD_PROP_SPACE") != NULL;
    if (getenv("NFSMW_TEST_AUX_PASS_KEY") && api->on_key && !g_x3sl_key_hook)
        api->on_key(api, x3sl_test_key, NULL, &g_x3sl_key_hook);
    return 1;
}
static X3lrPass *x3lr_pass_for(uint32_t type) {
    for (unsigned i = 0; i < X3LR_NPASS; ++i)
        if (g_x3lr_passes[i].ready && g_x3lr_passes[i].type == type) return &g_x3lr_passes[i];
    return NULL;
}
/* Remaster run 10 fused pool shader (review/2026-10-07-fused-pool-shader-design.md; on for props since run 10 and
 * for the WORLD street pools since run 11, after the kit's no-lost-light fix (review/run11/design-A.md);
 * NFSMW_TEST_POOL_FUSION=0 (TEST ONLY) keeps the two-pass path, =1 fuses props only). A lit WORLDNORMALMAP (prop) entry keeps one pass and asks the kit to add the pool light
 * in that pass: CleanPoolCount.y = 1 (the kit reads it at pass 0's CommitChanges; the game never reads it). No pass
 * count 2, no pass-0 restore. A request still pending at the next entry of that effect that makes none is cleared
 * here (an entry the engine skips has no CommitChanges, so its request would reach the next one). */
static int g_cpl_fusion = -1;
static uint8_t g_cpl_fuse_pending[3], g_cpl_req_now;
static uint64_t g_cpl_fused_req, g_cpl_fuse_clears;
static int cpl_fusion_on(void) {   /* 0 two-pass, 1 props only, 2 props and WORLD streets (default since run 11) */
    if (g_cpl_fusion < 0) { const char *e = getenv("NFSMW_TEST_POOL_FUSION"); g_cpl_fusion = e && *e ? atoi(e) : 2; }
    return g_cpl_fusion;
}
/* Does pass k (0 WORLD, 1 WORLDREFLECT, 2 WORLDNORMALMAP) fuse its pools now? WORLDREFLECT (roads, with the wet-road
 * variant) stays two-pass. */
static int cpl_fuse_for(unsigned k) {
    const int f = cpl_fusion_on();
    return !g_pp_fusion_old && ((k == 2u && f >= 1) || (k == 0u && f >= 2));
}
static void cpl_fuse_clear(const PopModApi *api, unsigned k, uint32_t id3dx) {
    void *dst = NULL;
    if (!g_cpl_handles[k][1] || api->guest_ptr(api, g_cpl_values + 16 * 16, 16, &dst) != POP_OK || !dst) return;
    memset(dst, 0, 16);
    uint32_t r = 0;
    const uint32_t args[] = {id3dx, g_cpl_handles[k][1], g_cpl_values + 16 * 16, 16};
    api->guest_call(api, x3sl_fx(id3dx, 0x50), 0, args, 4, &r);      /* SetValue CleanPoolCount = 0 */
    g_cpl_fuse_pending[k] = 0;
    ++g_cpl_fuse_clears;
}
/* One WORLD / WORLDREFLECT render entry (from x3sl_draw_entry; the pass count is already 1): a receiver or a
 * verified instance gets the second pass with its groups. */
static void cpl_world_entry(const PopModApi *api, X3lrPass *ps, uint32_t eeffect, uint32_t index, uint32_t id3dx) {
    const unsigned k = (unsigned)(ps - g_x3lr_passes);
    if (ps->cached_fx != id3dx) {
        static const uint32_t off[3] = {0u, 11u, 26u};   /* name offsets in the guest name block */
        for (unsigned h = 0; h < 3; ++h) {
            const uint32_t args[] = {id3dx, 0, g_cpl_values + 18 * 16 + off[h]};
            g_cpl_handles[k][h] = 0;
            api->guest_call(api, x3sl_fx(id3dx, 0x24), 0, args, 3, &g_cpl_handles[k][h]);   /* GetParameterByName */
        }
        ps->cached_fx = id3dx;
        char line[200];
        snprintf(line, sizeof line, "core.nfsmw: clean pools: %s effect %08x, handles %08x %08x %08x", ps->name, id3dx,
                 g_cpl_handles[k][0], g_cpl_handles[k][1], g_cpl_handles[k][2]);
        api->log(api, line);
    }
    if (!g_cpl_handles[k][0] || !g_cpl_handles[k][1] || !g_cpl_handles[k][2] || index >= X3SL_MAX_ENTRIES) return;
    const uint64_t pp_t0 = g_pp_csv ? pp_now() : 0;   /* TEST ONLY (P1) */
    void *dst = NULL;
    if (api->guest_ptr(api, g_cpl_values, 18 * 16, &dst) != POP_OK || !dst) return;
    float *v = (float *)dst;
    memcpy(v, g_cpl_regs[index], sizeof g_cpl_regs[index]);
    const int fuse = cpl_fuse_for(k);   /* Remaster run 10 */
    v[64] = (float)g_cpl_count[index]; v[65] = fuse ? 1.0f : 0.0f; v[66] = v[67] = 0.0f;
    memcpy(v + 68, g_cpl.params, 16);
    {   /* owner slider clean_pools_strength (%, live): scales the clean side's global strength */
        static uint32_t calls;
        static float scale = 1.0f;
        if ((calls++ & 255u) == 0) scale = (float)setting("clean_pools_strength", 100) / 100.0f;
        v[70] *= scale;
    }
    static const uint32_t voff[3] = {0u, 16u * 16u, 17u * 16u}, vsize[3] = {16u * 16u, 16u, 16u};
    for (unsigned h = 0; h < 3; ++h) {
        uint32_t r = 0;
        const uint32_t args[] = {id3dx, g_cpl_handles[k][h], g_cpl_values + voff[h], vsize[h]};
        if (pp_t0) {   /* TEST ONLY (P1): repeated bytes for this pass, and each call's time */
            static uint8_t last[3][3][256];
            if (!memcmp(last[k][h], (const uint8_t *)dst + voff[h], vsize[h])) ++g_pp.sv_repeat[h];
            memcpy(last[k][h], (const uint8_t *)dst + voff[h], vsize[h]);
            const uint64_t t = pp_now();
            api->guest_call(api, x3sl_fx(id3dx, 0x50), 0, args, 4, &r);      /* SetValue */
            g_pp.sv_ns[h] += pp_now() - t;
            ++g_pp.sv_calls;
        } else
            api->guest_call(api, x3sl_fx(id3dx, 0x50), 0, args, 4, &r);      /* SetValue */
    }
    if (fuse) {   /* Remaster run 10: one pass; the kit fuses (or adds the pool pass natively) */
        g_cpl_fuse_pending[k] = 1;
        g_cpl_req_now = 1;
        if ((++g_cpl_fused_req & 0x3ffffu) == 1u) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsmw: pool fusion: %llu fuse requests, %llu pending requests cleared",
                     (unsigned long long)g_cpl_fused_req, (unsigned long long)g_cpl_fuse_clears);
            api->log(api, line);
        }
    } else {
        g_api->guest_write_u32(g_api, eeffect + 0x14, 2);
        g_x3sl_prev_aux = 1;
    }
    ++g_cpl_draws;
    if (pp_t0) {   /* TEST ONLY (P1) */
        const uint32_t list = get_u32(0x00982a20u);
        const int other = list && get_u32(list) != 0;
        ++g_pp.sched[k < 3 ? k : 2][other];
        const uint32_t nl = g_cpl_count[index];
        ++g_pp.lamps[nl <= 8 ? nl : 8];
        int black = 1;
        for (uint32_t j = 0; j < nl && j < CPL_MAX; ++j)
            black &= g_cpl_regs[index][2 * j + 1][0] <= 0.0f && g_cpl_regs[index][2 * j + 1][1] <= 0.0f &&
                     g_cpl_regs[index][2 * j + 1][2] <= 0.0f;
        if (v[70] <= 0.0f) ++g_pp.zero_strength;
        else if (black) ++g_pp.zero_colour;
        else if (other || g_pp_zclass[index] == 2) ++g_pp.zero_unknown;
        else if (g_pp_zclass[index] == 1) ++g_pp.zero_fade;
        else ++g_pp.maybe_lit;
        g_pp.entry_ns += pp_now() - pp_t0;
    }
}
static uint64_t g_mirror_pool_entries;   /* run 13: WORLD pool entries drawn in the mirror (mirror_pools) */
static void x3lr_world_entry(const PopModApi *api, X3lrPass *ps, uint32_t entry, uint32_t eeffect, uint32_t index) {
    const uint32_t tag = x3lr_entry_recv(&g_x3lr, index, get_u32(entry));
    const uint32_t id3dx = get_u32(eeffect + 0x48);
    if (tag == UINT32_MAX || !g_x3sl_pass_live || g_look == LOOK_PC || !id3dx) return;
    if (g_pp_props_old && ps->type == 3u) { ++g_pp.props_skipped; return; }   /* TEST ONLY (P1) props toggle */
    /* Prop pools (WORLDNORMALMAP) in the main view only, as the WORLD pools below (P1, 7 Oct: 78 % of the prop pool
     * passes were in other views - mirror, reflections - most of them in the rare heavy frames that caused the
     * Rosewood frame drops). NFSMW_TEST_PROPS_ALLVIEWS=1 (TEST ONLY) keeps the earlier all-views behaviour. */
    if (ps->type == 3u) {
        static int allviews = -1;
        if (allviews < 0) { const char *e = getenv("NFSMW_TEST_PROPS_ALLVIEWS"); allviews = e && *e == '1'; }
        const uint32_t list = get_u32(0x00982a20u);
        if (!allviews && list && get_u32(list) != 0) { ++g_x3lr_other_view; return; }
    }
    /* 006e0200 draws a single pass, whatever the count, for WORLD/WORLDBONE/type 4 when the effect's byte
     * +0x40 is set (a flag 006c8000 and others also read - never touched here), unless 00982b25 is set and
     * the view (first word of 00982a20's array) is 0. Main view: 00982b25 is set for this entry. Other
     * views (mirror, reflections): no auxiliary pass. */
    if (ps->type == 0u) {
        const uint32_t list = get_u32(0x00982a20u);
        const uint32_t b25 = (get_u32(0x00982b24u) >> 8) & 0xffu;
        if ((b25 == 0 || (list && get_u32(list) != 0)) && (get_u32(eeffect + 0x40) & 0xffu)) {
            if (list && get_u32(list) != 0) {
                /* Run 13 mirror parity (setting mirror_pools, off by default): street pools in the rear-view mirror
                 * (view id 3 at [[00982a20]+4]; not env or reflection views) through the fused single pass, so no
                 * extra draw; a fusion failure leaves the mirror as today (unlit). */
                static int64_t mp = 0;
                static uint32_t mp_tick = 0;
                if ((mp_tick++ & 255u) == 0) mp = setting("mirror_pools", 0);
                if (mp && g_cpl_mode && get_u32(list + 4) == 3u && cpl_fuse_for(0u)) {
                    ++g_mirror_pool_entries;
                    goto fused;
                }
                ++g_x3lr_other_view;
                return;
            }
            if (g_cpl_mode && cpl_fuse_for(0u)) goto fused;   /* run 10: one pass is what a fused entry wants */
            ++g_x3lr_single_pass;
            g_x3lr_b25_old = b25;
            g_x3lr_b25_set = 1;
            x3lr_set_b25(1);
        }
    }
fused:
    if (g_cpl_mode) { cpl_world_entry(api, ps, eeffect, index, id3dx); return; }
    if (ps->cached_fx != id3dx) {
        const uint32_t args[] = {id3dx, 0, g_x3lr_values + X3LR_BLOCK * 16};
        ps->handle = 0;
        api->guest_call(api, x3sl_fx(id3dx, 0x24), 0, args, 3, &ps->handle);    /* GetParameterByName */
        ps->cached_fx = id3dx;
        ps->last_tag = UINT32_MAX;
        char line[160];
        snprintf(line, sizeof line, "core.nfsmw: Xbox light pools: %s effect %08x, AuxiliaryGroups handle %08x", ps->name, id3dx, ps->handle);
        api->log(api, line);
    }
    if (!ps->handle) return;
    const int placed = index < X3SL_MAX_ENTRIES && g_entry_space[index] != 0;
    if (tag != ps->last_tag || placed) {
        static float block[X3LR_BLOCK][4];
        if (tag & 0x80000000u) {
            x3lr_block_set(&g_x3sl.sets[tag & 0x7fffffffu], block);
            if (!x3_lights_to_entry(index, block, 3, 4)) return;   /* model-space lights for a placed model */
        } else
            x3lr_block_receiver(&g_x3lr, tag, block);
        void *dst = NULL;
        if (api->guest_ptr(api, g_x3lr_values, sizeof block, &dst) != POP_OK || !dst) return;
        memcpy(dst, block, sizeof block);
        uint32_t r = 0;
        const uint32_t args[] = {id3dx, ps->handle, g_x3lr_values, (uint32_t)sizeof block};
        api->guest_call(api, x3sl_fx(id3dx, 0x50), 0, args, 4, &r);          /* SetValue */
        ps->last_tag = placed ? UINT32_MAX : tag;
    }
    g_api->guest_write_u32(g_api, eeffect + 0x14, 2);
    g_x3sl_prev_aux = 1;
    if (tag & 0x80000000u) ++g_x3lr_set_draws;
    else {
        ++g_x3lr_recv_draws;
        static int logged;
        if (logged < 4) {
            ++logged;
            char line[220];
            const X3lrRecv *r = &g_x3lr.recv[tag];
            snprintf(line, sizeof line, "core.nfsmw: Xbox light pools: drawing terrain piece %u (section %d, %u light groups, first USA context %d) with the %s pass",
                     tag, r->section, r->count, g_x3lr.groups[r->first].lc, ps->name);
            api->log(api, line);
        }
    }
}

/* Call site 006e03e1 (006e0200's per-entry call of 006c6d10; esi = render entry). */
static void fsp_entry(uint32_t eeffect);   /* fe_shadow_probe.h, TEST ONLY */
static void x3sl_draw_entry(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t entry = cpu->esi;
    x3lr_restore_flag();
    if (entry < 0x0093e878u || (entry - 0x0093e878u) % 0x44u) return;
    const uint32_t eeffect = get_u32(entry + 0x10);
    fsp_entry(eeffect);
    if (g_x3sl_pass_ready || g_x3lr_pass_ready) {
        if (g_x3sl_prev_aux && eeffect == g_x3sl_prev_effect && get_u32(eeffect + 0x48))
            x3sl_restore_pass0(api, get_u32(eeffect + 0x48), 1);
        g_x3sl_prev_aux = 0;
        g_x3sl_prev_effect = eeffect;
    }
    const uint32_t index = (entry - 0x0093e878u) / 0x44u;
    const uint32_t set = x3sl_entry_set(&g_x3sl, index, get_u32(entry));
    const int normalmap = eeffect && get_u32(eeffect + 4) == 3u;
    if (g_x3sl_pass_ready && normalmap)
        g_api->guest_write_u32(g_api, eeffect + 0x14, 1);
    if (g_x3lr_pass_ready && eeffect && x3lr_entry_recv(&g_x3lr, index, get_u32(entry)) != UINT32_MAX) {
        const uint32_t type = get_u32(eeffect + 4);
        ++g_x3lr_type_draws[type < 15u ? type : 15u];
    }
    if (g_x3lr_pass_ready && eeffect) {
        X3lrPass *ps = x3lr_pass_for(get_u32(eeffect + 4));
        if (ps) {
            g_api->guest_write_u32(g_api, eeffect + 0x14, 1);
            g_cpl_req_now = 0;
            x3lr_world_entry(api, ps, entry, eeffect, index);
            const unsigned k = (unsigned)(ps - g_x3lr_passes);   /* Remaster run 10: clear a pending fuse request */
            if (k < 3 && g_cpl_fuse_pending[k] && !g_cpl_req_now && get_u32(eeffect + 0x48) == ps->cached_fx)
                cpl_fuse_clear(api, k, ps->cached_fx);
        }
    }
    sign_trace_draw(index, get_u32(entry), (g_x3lr_pass_ready && eeffect && x3lr_pass_for(get_u32(eeffect + 4)) ? 1u : 0u) |
                           (set != UINT32_MAX ? 2u : 0u));   /* Test145 */
    if (set == UINT32_MAX) return;
    ++g_x3sl_tagged_draws;
    {
        const uint32_t type = eeffect ? get_u32(eeffect + 4) : 0xffu;
        ++g_x3sl_type_draws[type < 15u ? type : 15u];
    }
    if (g_x3sl_pass_ready && g_x3sl_pass_live && g_look != LOOK_PC && normalmap && get_u32(eeffect + 0x48)) {
        const uint32_t id3dx = get_u32(eeffect + 0x48);
        static uint32_t cached_fx, handle;
        if (cached_fx != id3dx) {
            const uint32_t args[] = {id3dx, 0, g_x3sl_values + 12 * 16 + 16};
            handle = 0;
            api->guest_call(api, x3sl_fx(id3dx, 0x24), 0, args, 3, &handle);    /* GetParameterByName */
            cached_fx = id3dx;
            char line[160];
            snprintf(line, sizeof line, "core.nfsmw: Xbox scenery lights: WORLDNORMALMAP effect %08x, AuxiliaryLights handle %08x, passes %u",
                     id3dx, handle, get_u32(eeffect + 0x14));
            api->log(api, line);
        }
        if (handle) {
            float regs[12][4];
            x3sl_pass_registers(&g_x3sl.sets[set], regs);
            if (!x3_lights_to_entry(index, regs, 0, 6)) return;   /* model-space lights for a placed model (Test76) */
            const uint32_t *w = (const uint32_t *)regs;
            for (unsigned i = 0; i < 48; ++i) g_api->guest_write_u32(g_api, g_x3sl_values + 4u * i, w[i]);
            uint32_t r = 0;
            const uint32_t args[] = {id3dx, handle, g_x3sl_values, 12 * 16};
            api->guest_call(api, x3sl_fx(id3dx, 0x50), 0, args, 4, &r);          /* SetValue */
            g_api->guest_write_u32(g_api, eeffect + 0x14, 2);
            g_x3sl_prev_aux = 1;
            ++g_x3sl_aux_draws;
        }
    }
    static int logged;
    if (logged < 3) {
        ++logged;
        const uint32_t effect = get_u32(entry + 0x10);
        char line[160];
        snprintf(line, sizeof line, "core.nfsmw: Xbox scenery lights: drawing entry %u with USA context %d, effect %08x type %u",
                 index, g_x3sl.sets[set].number, effect, effect ? get_u32(effect + 4) : 0);
        api->log(api, line);
    }
}

#endif
