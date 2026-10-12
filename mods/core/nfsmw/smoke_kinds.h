/* Lit smoke (run 14): the emitter -> kind table (pure C, no dependencies; tests/test_smoke_kinds.py checks every key
 * against Attrib::StringHash32 of its name and the kinds against the kit's host/gpu/metal/smoke_light.h).
 * Keys: emitterdata collection keys (Jenkins lookup2, init 0xABCDEF00, of the names below; the census and
 * review/run14/smoke-realism-design.md 3 and 7 list them). One line per key: `0x........u, SMK_..., /` `* name *` `/`. */
#ifndef SMOKE_KINDS_H
#define SMOKE_KINDS_H
#include <stdint.h>

enum {
    SMK_TYRE = 1, SMK_FLARE_RED = 2, SMK_STACK_STEAM = 3, SMK_STACK_SOOT = 4, SMK_FOG = 5, SMK_STEAM = 6, SMK_FLARE_GREEN = 7,
    SMK_KIND_MAX = 7
};
static const struct { uint32_t key, kind; } k_smk_keys[] = {
    {0xe00557c4u, SMK_TYRE},          /* emtd_sk_asphaltfastsmk1 */
    {0xe586fefbu, SMK_TYRE},          /* emtd_sk_asphaltfastsmk2 */
    {0x81a6b886u, SMK_TYRE},          /* emtd_sl_asphaltfastsmk1 */
    {0xe580828bu, SMK_TYRE},          /* emtd_sk_cobblefastsmk1 */
    {0xdd458eb9u, SMK_TYRE},          /* emtd_dr_asphalt_smk */
    {0x583c808au, SMK_TYRE},          /* emtd_dr_cobblefastsmk1 */
    {0x6da6dd9du, SMK_TYRE},          /* emtd_sk_extrasmoke1 */
    {0x00b4e4efu, SMK_TYRE},          /* emtd_sk_asphaltsmokedark1 */
    {0xe463623au, SMK_TYRE},          /* emcar_tireblow_smoke */
    {0x2bd502f7u, SMK_FLARE_RED},     /* fxgame_flare_smoke */
    {0xa16258a8u, SMK_FLARE_GREEN},   /* fxgame_flare_smoke_grn */
    {0xce3e1528u, SMK_STACK_STEAM},   /* emsmk_env_white */
    {0x94f78424u, SMK_STACK_STEAM},   /* emsmk_env_long */
    {0xdf2dc6a5u, SMK_STACK_STEAM},   /* emsmk_env_chimney1 */
    {0x89dc9394u, SMK_STACK_SOOT},    /* emsmk_env_black */
    {0xea0792e0u, SMK_STACK_SOOT},    /* emsmk_env_brown */
    {0x0dafce9fu, SMK_FOG},           /* emenv_fog1 */
    {0x6f0d446fu, SMK_FOG},           /* emenv_fog2 */
    {0xc8bec47cu, SMK_FOG},           /* emenv_fog3 */
    {0xf952cb44u, SMK_FOG},           /* emenv_fog4 */
    {0xdfafdacbu, SMK_FOG},           /* emenv_fog_far1 */
    {0xef053473u, SMK_FOG},           /* emenv_fog_fe1 */
    {0x33d0ef59u, SMK_FOG},           /* emenv_fog_fe2 */
    {0xea173019u, SMK_STEAM},         /* emmis_small_steam1 */
    {0x61568f73u, SMK_STEAM},         /* emmis_small_steam2 */
    {0xd19f07a9u, SMK_STEAM},         /* emmis_small_steam2b */
    {0xeab5d69cu, SMK_STEAM},         /* emmis_small_steam3 */
    {0x2d2035d1u, SMK_STEAM},         /* emmis_small_steam3b */
    {0x31950f18u, SMK_STEAM},         /* emnis_steamjet1 */
};
static uint32_t smk_kind(uint32_t key) {
    for (unsigned i = 0; i < sizeof k_smk_keys / sizeof k_smk_keys[0]; ++i)
        if (k_smk_keys[i].key == key) return k_smk_keys[i].kind;
    return 0;
}
#endif
