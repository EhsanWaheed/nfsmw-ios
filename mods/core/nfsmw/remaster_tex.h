/* remaster_tex.h - NFSMW Remaster A1: texture keys, and the Remaster texture and A2C defaults (owner, 6 Oct).
 *
 * Keys every TPK texture the game creates with its name hash, so the host can dump it (E1 part 1) and, later,
 * replace it. FUN_006ddd90 is the only TPK texture creation site (TextureInfo in ECX; it also serves the movie
 * texture through 006e1880). It reuses ECX as a loop variable, so the TextureInfo is captured on entry by a WRAP
 * hook (challenge C-02 H1), the original runs, and then [[TI]+0]+0x18 is the IDirect3DTexture9 it created.
 *   TextureInfo+0x24  name hash
 *   TextureInfo+0x68  pack identity (logged here to settle the Xbox variant rule; C-02 M2)
 * Key = name hash << 32 | variant (variant 0 in step 1). Nothing in the game is written.
 * Enabled by NFSMW_REMASTER_TEXTAG=1. Hook slot 120 (free; g_hooks 100..127).
 * Settings (owner, 6 Oct: both on by default): remaster_textures turns tagging on and points the host at the pack
 * bundled with the app (Resources/RemasterPack, beside Resources/Game); remaster_a2c sets the host's alpha-to-coverage.
 * Explicit environment values (the harness) always win: RECOMP_REMASTER_PACK, NFSMW_REMASTER_TEXTAG,
 * RECOMP_REMASTER_A2C. The host reads them when the device is created, after this runs. */
#ifndef REMASTER_TEX_H
#define REMASTER_TEX_H
#include <stddef.h>
#include <limits.h>
#include <unistd.h>

static int g_rtex_on;
static uint64_t g_rtex_calls, g_rtex_tagged, g_rtex_no_texture, g_rtex_rejected, g_rtex_no_api;
#define RTEX_PACKS 16
static uint32_t g_rtex_pack[RTEX_PACKS];
static uint64_t g_rtex_pack_count[RTEX_PACKS], g_rtex_pack_other;

static void rtex_report(const PopModApi *api) {
    char line[320];
    int n = snprintf(line, sizeof line,
                     "core.nfsmw: Remaster textures: creations %llu tagged %llu no-texture %llu rejected %llu no-api %llu; packs",
                     (unsigned long long)g_rtex_calls, (unsigned long long)g_rtex_tagged,
                     (unsigned long long)g_rtex_no_texture, (unsigned long long)g_rtex_rejected,
                     (unsigned long long)g_rtex_no_api);
    for (int i = 0; i < RTEX_PACKS && g_rtex_pack[i] && n > 0 && n < (int)sizeof line - 24; ++i)
        n += snprintf(line + n, sizeof line - (size_t)n, " %08x:%llu", g_rtex_pack[i],
                      (unsigned long long)g_rtex_pack_count[i]);
    if (n > 0 && n < (int)sizeof line - 24 && g_rtex_pack_other)
        snprintf(line + n, sizeof line - (size_t)n, " other:%llu", (unsigned long long)g_rtex_pack_other);
    api->log(api, line);
}

static void rtex_count_pack(uint32_t pack) {
    for (int i = 0; i < RTEX_PACKS; ++i) {
        if (g_rtex_pack[i] == pack) { ++g_rtex_pack_count[i]; return; }
        if (!g_rtex_pack[i]) { g_rtex_pack[i] = pack; g_rtex_pack_count[i] = 1; return; }
    }
    ++g_rtex_pack_other;
}

/* TEST (NFSMW_REMASTER_TEXNAMES=1): one catalogue line per texture name hash, for the A2 pipeline and the hero
 * catalogue. TextureInfo layout from the CC0 decomp (knowledge/FACTS.md): DebugName[24] +0x0C, NameHash +0x24,
 * ClassNameHash +0x28, Width/Height +0x44/+0x46 (int16), ImageCompressionType +0x4A, NumMipMapLevels +0x4E,
 * TilableUV +0x4F, ScrollType +0x52, AlphaUsageType +0x55, AlphaBlendType +0x56. */
static int g_rtex_names;
#define RTEX_SEEN 8192
static uint32_t g_rtex_seen[RTEX_SEEN];
static uint32_t rtex_u8(uint32_t a) { return get_u32(a) & 0xffu; }
static void rtex_name(const PopModApi *api, uint32_t ti, uint32_t hash) {
    const uint32_t slot = hash % RTEX_SEEN;
    for (uint32_t i = 0; i < 16; ++i) { /* small open-addressed set: one line per hash */
        const uint32_t k = (slot + i) % RTEX_SEEN;
        if (g_rtex_seen[k] == hash) return;
        if (!g_rtex_seen[k]) { g_rtex_seen[k] = hash; break; }
    }
    char name[25];
    int n = 0;
    while (n < 24) { const uint32_t c = rtex_u8(ti + 0x0cu + (uint32_t)n); if (c < 0x20 || c > 0x7e) break; name[n++] = (char)c; }
    name[n] = 0;
    char line[200];
    snprintf(line, sizeof line,
             "core.nfsmw: Remaster texture catalogue %08x name=%s class=%08x size=%ux%u comp=%u mips=%u tile=%u scroll=%u alpha_usage=%u alpha_blend=%u",
             hash, name, get_u32(ti + 0x28u), get_u32(ti + 0x44u) & 0xffffu, get_u32(ti + 0x46u) & 0xffffu,
             rtex_u8(ti + 0x4au), rtex_u8(ti + 0x4eu), rtex_u8(ti + 0x4fu), rtex_u8(ti + 0x52u), rtex_u8(ti + 0x55u),
             rtex_u8(ti + 0x56u));
    api->log(api, line);
}

static void rtex_create_wrap(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t ti = cpu->ecx; /* captured on entry: the original reuses ECX */
    api->call_next(api, inv, cpu);
    ++g_rtex_calls;
    if (!ti) return;
    const uint32_t plat = get_u32(ti);
    const uint32_t tex = plat ? get_u32(plat + 0x18u) : 0;
    rtex_count_pack(get_u32(ti + 0x68u));
    if (!tex) { ++g_rtex_no_texture; return; }
    if (!api->d3d9_texture_tag) { ++g_rtex_no_api; return; }
    const uint64_t key = (uint64_t)get_u32(ti + 0x24u) << 32;
    if (g_rtex_names) rtex_name(api, ti, get_u32(ti + 0x24u));
    if (api->d3d9_texture_tag(api, tex, key) == POP_OK) ++g_rtex_tagged;
    else ++g_rtex_rejected;
    if ((g_rtex_calls & 1023u) == 0) rtex_report(api);
}

static void rtex_defaults(const PopModApi *api) {
    const int tex = setting("remaster_textures", 1) != 0, a2c = setting("remaster_a2c", 1) != 0;
    const char *pk = getenv("RECOMP_REMASTER_PACK");
    if (tex && !(pk && *pk)) {
        const char *exe = getenv("RECOMP_EXE"); /* .../Resources/Game/speed.exe */
        char dir[PATH_MAX], idx[PATH_MAX + 16];
        const char *slash = exe ? strrchr(exe, '/') : NULL;
        int ok = 0;
        if (slash && (size_t)(slash - exe) + 20 < sizeof dir) {
            snprintf(dir, sizeof dir, "%.*s/../RemasterPack", (int)(slash - exe), exe);
            snprintf(idx, sizeof idx, "%s/index.tsv", dir);
            ok = access(idx, R_OK) == 0;
        }
        if (ok) {
            setenv("RECOMP_REMASTER_PACK", dir, 0);
            if (!getenv("NFSMW_REMASTER_TEXTAG"))
                setenv("NFSMW_REMASTER_TEXTAG", "1", 0);
        }
        api->log(api, ok ? "core.nfsmw: Remaster textures on (bundled pack)"
                         : "core.nfsmw: Remaster textures on, but this app has no bundled pack");
    } else if (tex) {
        /* An explicit pack (RECOMP_REMASTER_PACK) needs the keys too (found in run 3: a pack path alone left tagging
         * off, so nothing was replaced). */
        if (!getenv("NFSMW_REMASTER_TEXTAG"))
            setenv("NFSMW_REMASTER_TEXTAG", "1", 0);
        api->log(api, "core.nfsmw: Remaster textures on (pack from RECOMP_REMASTER_PACK)");
    } else {
        api->log(api, "core.nfsmw: Remaster textures off (setting)");
        /* Run 14: grass and natural water find their textures by these keys. With the pack off (Look Xbox 360 / PC
         * read remaster_textures as Original) tagging stayed off, so both drew nothing (ALL14). Tagging alone replaces
         * nothing: the kit's replacement cache starts only with a pack. */
        const int64_t w = setting("natural_water", 1), g = setting("grass", 0);
        if ((w == 1 || w == 2 || (g >= 1 && g <= 3)) && !getenv("NFSMW_REMASTER_TEXTAG")) {
            setenv("NFSMW_REMASTER_TEXTAG", "1", 0);
            api->log(api, "core.nfsmw: Remaster texture keys on (no pack) for grass / natural water");
        }
    }
    if (!getenv("RECOMP_REMASTER_A2C"))
        setenv("RECOMP_REMASTER_A2C", a2c ? "1" : "0", 0);
    /* Soft particles (owner 6 Oct: approved, on by default): 2 = depth copy + fade. */
    if (setting("soft_particles", 1) != 0 && !getenv("RECOMP_REMASTER_SOFTFX"))
        setenv("RECOMP_REMASTER_SOFTFX", "2", 0);
    /* Wet roads (R2 item 1; owner 6 Oct: "loads better"; on by default, Original = 0): the native wet-road shading. */
    { const int64_t m_ = setting("wet_roads", 1);   /* 1 Natural, 2 Xbox 360 (run 13: our own 360 version) */
      if ((m_ == 1 || m_ == 2) && !getenv("RECOMP_REMASTER_WETROAD"))
          setenv("RECOMP_REMASTER_WETROAD", m_ == 1 ? "1" : "2", 0); }
    /* R2 PBR step 2 pilot (run 12; owner 8 Oct: relief looks right, gentle strength; on by default as finished): the
     * normal-mapped sun term on the five pilot facade textures, slot pack at Resources/PbrPilot. Needs the HD pack
     * (the relief matches the bound albedo; the kit also requires the albedo to be resident). */
    if (tex && setting("pbr_facades", 1) != 0 && !getenv("RECOMP_REMASTER_PBRWORLD")) {
        const char *exe = getenv("RECOMP_EXE");
        const char *slash = exe ? strrchr(exe, '/') : NULL;
        char dir[PATH_MAX], idx[PATH_MAX + 16];
        int ok = 0;
        if (slash && (size_t)(slash - exe) + 20 < sizeof dir) {
            snprintf(dir, sizeof dir, "%.*s/../PbrPilot", (int)(slash - exe), exe);
            snprintf(idx, sizeof idx, "%s/index.tsv", dir);
            ok = access(idx, R_OK) == 0;
        }
        if (ok) {
            setenv("RECOMP_REMASTER_PBRWORLD", "1", 0);
            setenv("RECOMP_REMASTER_PBRWORLD_PACK", dir, 0);
            setenv("RECOMP_REMASTER_PBRWORLD_STRENGTH", "1", 0);
        }
        api->log(api, ok ? "core.nfsmw: sunlit wall relief on (pilot, strength 1)"
                         : "core.nfsmw: sunlit wall relief on, but this app has no PbrPilot pack");
    }
    /* Run 13 road sheen (owner 7 Oct: road glare at a real intensity; default on since run 14, owner 8 Oct): the road
     * effect's sun highlight becomes a measured GGX sheen (kit PixelVariant.roadsheen). Run 14 (owner: "the reflection
     * intensity on rain seemed a bit strong"): road_sheen_rain % scales the sheen on a fully wet road (kit
     * RECOMP_REMASTER_ROADSHEEN_WETPCT, an integer so no locale decimal comma; lerped by the road's wetness). */
    if (setting("road_sheen", 1) != 0 && !getenv("RECOMP_REMASTER_ROADSHEEN")) {
        setenv("RECOMP_REMASTER_ROADSHEEN", "1", 0);
        char wp[16];
        snprintf(wp, sizeof wp, "%d", (int)setting("road_sheen_rain", 50));
        setenv("RECOMP_REMASTER_ROADSHEEN_WETPCT", wp, 0);
    }
    /* Run 13 natural water: WORLD water (ANM_WATERA) gets ripples, sky reflection and sun glint; 1 Definitive (physical),
     * 2 Xbox 360 (our own version of the 360 water) (kit PixelVariant.water). */
    { const int64_t w = setting("natural_water", 1);
      if ((w == 1 || w == 2) && !getenv("RECOMP_REMASTER_WATER"))
          setenv("RECOMP_REMASTER_WATER", w == 1 ? "1" : "2", 0); }
    /* Run 13 grass (our own version of the 360 GRASS fur), run 14 real blades (owner 8 Oct): Low/Medium/High = blade
       quality 1/2/3 near the camera plus 0/1/2 light shells beyond the blade fade (kit grass.h). Default off. */
    { const int64_t g = setting("grass", 0);
      if (g >= 1 && g <= 3 && !getenv("RECOMP_REMASTER_GRASS_BLADES"))
          setenv("RECOMP_REMASTER_GRASS_BLADES", g == 1 ? "1" : g == 2 ? "2" : "3", 0);
      if (g >= 1 && g <= 3 && !getenv("RECOMP_REMASTER_GRASS"))
          setenv("RECOMP_REMASTER_GRASS", g == 1 ? "0" : g == 2 ? "1" : "2", 0); }
    /* Run 13 wall parallax (our own version of the 360/X360Stuff parallax on WORLDNORMALMAP). */
    if (setting("wall_parallax", 1) != 0 && !getenv("RECOMP_REMASTER_PARALLAX"))
        setenv("RECOMP_REMASTER_PARALLAX", "1", 0);
    /* Run 13 natural glass (default Natural since run 14, owner 8 Oct): Fresnel-weighted building-glass reflection. */
    { const int64_t m_ = setting("natural_glass", 1);   /* 1 Natural, 2 Xbox 360 (run 13: our own 360 version) */
      if ((m_ == 1 || m_ == 2) && !getenv("RECOMP_REMASTER_GLASS"))
          setenv("RECOMP_REMASTER_GLASS", m_ == 1 ? "1" : "2", 0); }
    /* Run 13 car reflections (owner 7 Oct; default Natural since run 14, owner 8 Oct): the car effect's environment
     * strength follows Schlick's curve between each material's face-on value and its grazing limit (kit
     * VertexVariant.carenv). */
    { const int64_t m_ = setting("car_reflections", 1);   /* 1 Natural, 2 Xbox 360 (run 13: our own 360 version) */
      if ((m_ == 1 || m_ == 2) && !getenv("RECOMP_REMASTER_CARENV"))
          setenv("RECOMP_REMASTER_CARENV", m_ == 1 ? "1" : "2", 0); }
    /* R2 softer sun shadows (run 8): a 12-tap shadow filter in the translated shaders (kit PixelVariant.shadowpcf) */
    if (setting("soft_shadows", 1) != 0 && !getenv("RECOMP_REMASTER_SHADOWPCF"))
        setenv("RECOMP_REMASTER_SHADOWPCF", "1", 0);
}

/* Called from pop_mod_init after install() is defined. */
static PopModStatus rtex_init(const PopModApi *api) {
    rtex_defaults(api);
    const char *nm = getenv("NFSMW_REMASTER_TEXNAMES");
    g_rtex_names = nm && *nm == '1';
    const char *e = getenv("NFSMW_REMASTER_TEXTAG");
    g_rtex_on = e && *e == '1';
    if (!g_rtex_on) return POP_OK;
    if (api->size < offsetof(PopModApi, d3d9_texture_tag) + sizeof api->d3d9_texture_tag) {
        api->log(api, "core.nfsmw: Remaster textures: host has no d3d9_texture_tag; off");
        g_rtex_on = 0;
        return POP_OK;
    }
    PopModStatus s = install(0x006ddd90u, rtex_create_wrap, POP_HOOK_WRAP, 120);
    api->log(api, s == POP_OK ? "core.nfsmw: Remaster textures: TEST tagging on (006ddd90 wrap)"
                              : "core.nfsmw: Remaster textures: hook failed; off");
    if (s != POP_OK) g_rtex_on = 0;
    return POP_OK;
}

#endif
