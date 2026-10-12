/* Native named-texture substitution for the owner's TexWizardX360 pack.
 * Address leads: R-033/texwizard@51e7d29, MW_Address.h. Every ABI verified
 * against this project's pinned PE listings. No Windows ASI is loaded.
 * The build preparation tool converts the owner's metadata to sorted hash
 * pairs; copyrighted texture bytes remain a private external game input. */
#define X360_MAP_LIMIT 8192u
static struct { uint32_t from, to; } g_x360_map[X360_MAP_LIMIT];
static uint32_t g_x360_count, g_x360_path;
static int g_x360_requested;
static unsigned g_x360_hits;
static int g_x360_fallback;

static int x360_texture_map_init(const PopModApi *api) {
    if (!setting("x360_textures", 1)) return 0;
    char path[2048], magic[32];
    snprintf(path, sizeof path, "%s/x360-textures.map", api->mod_dir(api));
    FILE *f = fopen(path, "rb");
    if (!f) {
        api->log(api, "core.nfsmw: Xbox texture map absent; pack not active");
        return 0;
    }
    if (!fgets(magic, sizeof magic, f) || strcmp(magic, "NFSMW-X360-MAP-1\n")) {
        fclose(f); return 0;
    }
    unsigned from, to;
    int parsed;
    while ((parsed = fscanf(f, "%x %x", &from, &to)) == 2) {
        if (g_x360_count == X360_MAP_LIMIT || !from || !to ||
            (g_x360_count && from <= g_x360_map[g_x360_count - 1].from)) {
            g_x360_count = 0; fclose(f); return 0;
        }
        g_x360_map[g_x360_count].from = from;
        g_x360_map[g_x360_count++].to = to;
    }
    fclose(f);
    if (parsed != EOF || !g_x360_count) { g_x360_count = 0; return 0; }
    static const char pack[] = "TRACKS\\TexWizardX360\\textures.bin";
    void *guest = NULL;
    if (api->guest_alloc(api, sizeof pack, &g_x360_path) != POP_OK ||
        api->guest_ptr(api, g_x360_path, sizeof pack, &guest) != POP_OK)
        return 0;
    memcpy(guest, pack, sizeof pack);
    snprintf(path, sizeof path, "core.nfsmw: Xbox texture map ready, %u names", g_x360_count);
    api->log(api, path);
    return 1;
}

/* Match the original mod's global-resource loading boundary. Resource
 * registration and asynchronous completion stay inside the game engine. */
static void x360_load_textures(const PopModApi *api, pop_cpu_v1 *cpu,
                              PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    api->call_original(api, cpu->target, cpu);
    frontend_pad_load(api);
    if (g_x360_requested || !g_x360_count) return;
    if (g_x360_sky_ready) {
        const uint32_t sky_args[] = {g_x360_sky_path, 1, 0, 0, 0};
        const uint32_t sky_begin[] = {0, 0};
        uint32_t resource = 0, unused = 0;
        if (api->guest_call(api, 0x0065fd30u, 0, sky_args, 5, &resource) == POP_OK && resource)
            api->guest_call(api, 0x006616f0u, resource, sky_begin, 2, &unused);
    }
    const uint32_t create[] = {g_x360_path, 1, 0, 0, 0};
    const uint32_t begin[] = {0, 0};
    uint32_t resource = 0, unused = 0;
    if (api->guest_call(api, 0x0065fd30u, 0, create, 5, &resource) == POP_OK && resource &&
        api->guest_call(api, 0x006616f0u, resource, begin, 2, &unused) == POP_OK) {
        g_x360_requested = 1;
        api->log(api, "core.nfsmw: Xbox texture resource loading requested");
    }
}

/* Resolve only a named replacement present in the engine's texture table.
 * A missing/not-yet-loaded replacement falls back to the original name,
 * never the engine's default texture. No caching of unloadable pointers. */
static void x360_find_texture(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    uint32_t hash = get_u32(cpu->esp + 4), first = 0, last = g_x360_count;
    while (first < last) {
        uint32_t mid = first + (last - first) / 2;
        if (g_x360_map[mid].from < hash) first = mid + 1;
        else last = mid;
    }
    if (!g_x360_fallback && g_x360_requested && first < g_x360_count && g_x360_map[first].from == hash) {
        pop_cpu_v1 saved = *cpu;
        uint32_t default_flag = get_u32(cpu->esp + 8);
        api->guest_write_u32(api, saved.esp + 4, g_x360_map[first].to);
        api->guest_write_u32(api, saved.esp + 8, 0);
        api->call_original(api, cpu->target, cpu);
        api->guest_write_u32(api, saved.esp + 4, hash);
        api->guest_write_u32(api, saved.esp + 8, default_flag);
        if (cpu->eax) {
            if (g_x360_hits++ < 48) {
                char line[144];
                snprintf(line, sizeof line, "core.nfsmw: Xbox texture resolved %08x -> %08x, info=%08x",
                         hash, g_x360_map[first].to, cpu->eax);
                api->log(api, line);
            }
            return;
        }
        /* call_original may run only once per invocation. Its RET is already
         * committed; use a nested guest call for fallback and change only EAX. */
        const uint32_t args[] = {hash, default_flag, get_u32(saved.esp + 12)};
        uint32_t result = 0;
        g_x360_fallback = 1;
        PopModStatus status = api->guest_call(api, saved.target, 0, args, 3, &result);
        g_x360_fallback = 0;
        if (status == POP_OK) cpu->eax = result;
        return;
    }
    api->call_original(api, cpu->target, cpu);
}
