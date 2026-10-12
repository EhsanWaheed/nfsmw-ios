/* Boot movies (Test58; owner decision 2026-09-29): EA HD bumper -> PSA -> opening movie.
 *
 * PC 1.3 BootFlowManager (ctor 0058e9c0) builds its screen list from four static
 * boot lists (008f3c58.., 008f3c74.., 008f3c94.., 008f3cb0..). Each lists LS_EAlogo.fng
 * (string 008a0128) before LS_PSA.fng and, except the no-attract list, MW_LS_AttractFMV.fng.
 * MovieScreen (ctor near 0058ed22) selects its clip by matching the screen's package name
 * against the MovieData table at 008f3cd8 {package, movie, object hash, bypass, subtitles}:
 * LS_EALogo.fng -> ealogo and LS_EA_hidef.fng -> eahd_bumper, both not bypassable and
 * without subtitles, the same flags the USA Xbox 360 table (828fec00) gives them. The PC
 * front-end bundle already carries LS_EA_hidef.fng, and the screen factory (004c2776)
 * registers it.
 *
 * So the first boot screen is swapped from LS_EAlogo.fng to LS_EA_hidef.fng in those four
 * lists, once, before the boot flow is built: exactly three boot clips, played, skipped,
 * subtitled and completed by the game's own MovieScreen and the native USA movie adapter.
 * The ealogo asset and its MovieData entry stay untouched (provenance, other uses).
 * Per-title gain: PC 1.3 has no volume entry for eahd_bumper, so its own lookup (005178b0)
 * gives the default 127, as it would for any unlisted clip.
 *
 * Evidence note (not applied, owner choice stands): the USA 360 boot flow (822b7eb8) on HD
 * output queues WS_LS_EA_hidef.fng and then WS_LS_EAlogo.fng; it drops the HD bumper only in
 * SD widescreen (flag 828f4a8e set by the video-mode setup at 82440188).
 *
 * The PC menu-bar/cursor prompt helper 005864b0 exempts the boot movie screens by package
 * hash (bStringHash of the upper-case name); LS_EA_HIDEF.FNG is added to that exemption so
 * the swapped screen behaves as the one it replaces. */
#define BOOT_FLOW_CTOR 0x0058e9c0u
#define BOOT_OVERLAY_ALLOWED 0x005864b0u
#define BOOT_PKG_EALOGO 0x008a0128u   /* "LS_EAlogo.fng" */
#define BOOT_PKG_EA_HIDEF 0x008972e4u /* "LS_EA_hidef.fng" */
#define BOOT_HASH_EA_HIDEF 0xdf63654bu /* bStringHash("LS_EA_HIDEF.FNG") */
static const uint32_t k_boot_first_slots[4] = {0x008f3c5cu, 0x008f3c7cu, 0x008f3c9cu, 0x008f3cb8u};
static int g_boot_hd_state;   /* 0 not yet, 1 applied, -1 refused */

static int boot_guest_str_is(const PopModApi *api, uint32_t addr, const char *expect) {
    const size_t n = strlen(expect) + 1;
    void *host = NULL;
    return api->guest_ptr(api, addr, (uint32_t)n, &host) == POP_OK && host && !memcmp(host, expect, n);
}
/* Idempotent. Refuses (and leaves the original lists) unless every slot and string is the
 * exact PC 1.3 content, so a different executable can never be patched blindly. */
static void boot_movies_apply(const PopModApi *api) {
    if (g_boot_hd_state) return;
    int ok = boot_guest_str_is(api, BOOT_PKG_EALOGO, "LS_EAlogo.fng") &&
             boot_guest_str_is(api, BOOT_PKG_EA_HIDEF, "LS_EA_hidef.fng");
    for (unsigned i = 0; ok && i < 4; ++i) {
        const uint32_t v = get_u32(k_boot_first_slots[i]);
        ok = v == BOOT_PKG_EALOGO || v == BOOT_PKG_EA_HIDEF;
    }
    if (!ok) {
        g_boot_hd_state = -1;
        api->log(api, "core.nfsmw: boot movies: unexpected boot lists; original order kept");
        return;
    }
    for (unsigned i = 0; i < 4; ++i)
        api->guest_write_u32(api, k_boot_first_slots[i], BOOT_PKG_EA_HIDEF);
    g_boot_hd_state = 1;
    api->log(api, "core.nfsmw: boot movies: EA HD bumper -> PSA -> opening movie");
}
/* BEFORE BootFlowManager::BootFlowManager. */
static void boot_flow_ctor(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    boot_movies_apply(api);
}
/* REPLACE 005864b0 (thiscall, returns bool in AL). */
static void boot_overlay_allowed(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t screen = get_u32(cpu->ecx + 0x18);
    const uint32_t hash = screen ? get_u32(screen + 0x10) : 0;
    api->call_original(api, cpu->target, cpu);
    if (setting("input_trace", 0)) {
        static uint32_t last;
        if (hash != last) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsmw: overlay check screen=%08x allowed=%u", hash, cpu->eax & 0xffu);
            api->log(api, line);
            last = hash;
        }
    }
    if (g_boot_hd_state == 1 && hash == BOOT_HASH_EA_HIDEF)
        cpu->eax &= ~0xffu;
}
