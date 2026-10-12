// Test71 (TEST ONLY): an A/B of CPU-side changes inside one run, so both sides see the same
// traffic and scene mix (separate runs of the busy city vary by more than the changes measured).
// RECOMP_AB_PERIOD=<seconds>: during odd periods of the uptime clock (the clock the frame timings'
// sealed_s column uses) the switches that ask recomp_ab::old(which) take their earlier behaviour; during
// even periods, the current one. The phase is set once per presented frame on the main thread.
// Unset (every normal run): always the current behaviour; a switch costs one relaxed load.
#pragma once

#include "os.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace recomp_ab {
inline std::atomic<int> old_phase{0};
inline uint64_t period_ns() {
    static const uint64_t p = [] {
        const char *e = recomp_env("AB_PERIOD");
        long s = e ? strtol(e, nullptr, 10) : 0;
        return s > 0 ? (uint64_t)s * 1000000000ull : 0ull;
    }();
    return p;
}
inline void tick() {
    // Test75: RECOMP_AB_FORCE_OLD=1 keeps the earlier behaviour for the whole run (image comparisons).
    static const bool force_old = [] {
        const char *e = recomp_env("AB_FORCE_OLD");
        return e && !strcmp(e, "1");
    }();
    if (force_old)
        old_phase.store(1, std::memory_order_relaxed);
    else if (const uint64_t p = period_ns())
        old_phase.store((int)((os_monotonic_ns() / p) & 1), std::memory_order_relaxed);
}
// Which switches take part: RECOMP_AB_SWITCHES=stride,sampler,imports,view,preshader,audio (default: all).
enum : uint32_t {
    STRIDE = 1u,
    SAMPLER = 2u,
    IMPORTS = 4u,
    VIEW = 8u,
    PRESHADER = 16u,
    AUDIO = 32u,
    CONSTS = 64u, // Test75: incremental effect constant binding (d3dx9.cpp bind_constants)
    MIRRORS = 128u, // Test75: per-draw lookups (d3d9.cpp resource mirrors, decoded programs)
    AABB = 256u,    // Test75: native bounding-box routines (native/aabb.cpp)
    COMMIT = 512u,  // Remaster C1: CommitChanges re-applies only parameter-dependent pass states (d3dx9.cpp)
    A2C = 1024u,    // Remaster A3 step A: alpha-to-coverage variant (d3d9_metal.mm; needs RECOMP_REMASTER_A2C=1)
    NULLDRAW = 2048u, // Remaster A4 gate: draws skipped in OLD periods (d3d9.cpp null_draw); opt-in only
    NULLFX = 4096u,   // Remaster A4 gate: effect passes not applied in OLD periods (d3dx9.cpp apply_pass); opt-in only
    SDELTA = 8192u,   // Remaster A4-lite step 1: state-table deltas in the queue (OLD: today's full puts)
    BATCH = 16384u,   // Remaster A4-lite: queue hand-over size (OLD: 48 commands; NEW: RECOMP_REMASTER_BATCH)
    SOFTFX = 32768u,  // Remaster S3 trial: particle pass break + depth copy (OLD: none; needs RECOMP_REMASTER_SOFTFX=1)
    WETROAD = 65536u, // Remaster R2 wet roads: native wet-road shading on road shaders (OLD: the game's; needs RECOMP_REMASTER_WETROAD=1)
    SHADOWPCF = 131072u, // Remaster R2 softer sun shadows: 12-tap shadow filter (OLD: the single 2x2 compare; needs RECOMP_REMASTER_SHADOWPCF=1)
    POOLNULL = 262144u,  // Remaster P1 (TEST ONLY): pool-pass draws skipped in OLD periods; opt-in, needs RECOMP_REMASTER_POOLDRAW_NULL=1
    PBRWORLD = 524288u,  // Remaster run 12 PBR step 2 pilot: normal-mapped sun term on pilot facades (OLD: the game's; needs RECOMP_REMASTER_PBRWORLD=1)
    ROADSHEEN = 1048576u, // Remaster run 13 road sheen: measured GGX sun highlight on roads (OLD: the game's; needs RECOMP_REMASTER_ROADSHEEN=1)
    CARENV = 2097152u,    // Remaster run 13 car reflections: Fresnel-shaped environment strength (OLD: the game's; needs RECOMP_REMASTER_CARENV=1)
    WATER = 4194304u,     // Remaster run 13 natural water: ripple normal, sky Fresnel and sun glint on WORLD water (OLD: the game's; needs RECOMP_REMASTER_WATER=1)
    GLASS = 8388608u,     // Remaster run 13 natural glass: Fresnel-weighted building-glass reflection (OLD: the game's; needs RECOMP_REMASTER_GLASS=1)
    FEFLOOR = 16777216u,  // Remaster run 13 front-end floor shadows: the car's shadow on the garage floor (OLD: the floor as today; needs RECOMP_REMASTER_FEFLOOR=1)
    SKY = 1073741824u,    // Remaster run 13 Xbox 360 sky (ours): the 360-order sky combine and horizon in-scatter (OLD: the PC sky program; needs RECOMP_REMASTER_SKY=1)
    PARALLAX = 33554432u, // Remaster run 13 wall parallax: WORLDNORMALMAP uv offset from the normal-map alpha (OLD: the game's; needs RECOMP_REMASTER_PARALLAX=1)
    ATMOS = 67108864u,    // Remaster run 14 B1 height fog: the atmosphere pass before the particles (OLD: none; needs RECOMP_REMASTER_ATMOS=1); name heightfog
    GRASS = 536870912u,   // Remaster run 13 grass shells: shell grass over grass terrain (OLD: no shells; needs RECOMP_REMASTER_GRASS=<shells>)
    LITSMOKE = 134217728u, // Remaster run 14 lit smoke: particle relight on the core mod's kind marker (OLD: the game's; needs setting lit_smoke); name litsmk ("smoke" is the mod's denser-smoke switch)
};
inline uint32_t switches() {
    static const uint32_t m = [] {
        const char *e = recomp_env("AB_SWITCHES");
        if (!e || !*e)
            return ~0u & ~(NULLDRAW | NULLFX | POOLNULL); // nulldraw, nullfx and poolnull must be named explicitly
        uint32_t bits = 0;
        if (strstr(e, "stride"))
            bits |= STRIDE;
        if (strstr(e, "sampler"))
            bits |= SAMPLER;
        if (strstr(e, "imports"))
            bits |= IMPORTS;
        if (strstr(e, "view"))
            bits |= VIEW;
        if (strstr(e, "preshader"))
            bits |= PRESHADER;
        if (strstr(e, "audio")) // Test73: the native sound-mixer loops (native/audio_mix.cpp)
            bits |= AUDIO;
        if (strstr(e, "consts"))
            bits |= CONSTS;
        if (strstr(e, "mirrors"))
            bits |= MIRRORS;
        if (strstr(e, "aabb"))
            bits |= AABB;
        if (strstr(e, "commit"))
            bits |= COMMIT;
        if (strstr(e, "a2c"))
            bits |= A2C;
        if (strstr(e, "nulldraw"))
            bits |= NULLDRAW;
        if (strstr(e, "nullfx"))
            bits |= NULLFX;
        if (strstr(e, "sdelta"))
            bits |= SDELTA;
        if (strstr(e, "batch"))
            bits |= BATCH;
        if (strstr(e, "softfx"))
            bits |= SOFTFX;
        if (strstr(e, "wetroad"))
            bits |= WETROAD;
        if (strstr(e, "shadowpcf"))
            bits |= SHADOWPCF;
        if (strstr(e, "poolnull"))
            bits |= POOLNULL;
        if (strstr(e, "pbrworld"))
            bits |= PBRWORLD;
        if (strstr(e, "roadsheen"))
            bits |= ROADSHEEN;
        if (strstr(e, "carenv"))
            bits |= CARENV;
        if (strstr(e, "water"))
            bits |= WATER;
        if (strstr(e, "glass"))
            bits |= GLASS;
        if (strstr(e, "fefloor"))
            bits |= FEFLOOR;
        if (strstr(e, "parallax"))
            bits |= PARALLAX;
        if (strstr(e, "grass"))
            bits |= GRASS;
        if (strstr(e, "sky"))
            bits |= SKY;
        if (strstr(e, "litsmk"))
            bits |= LITSMOKE;
        if (strstr(e, "heightfog"))
            bits |= ATMOS;
        return bits;
    }();
    return m;
}
inline bool old(uint32_t which) {
    return old_phase.load(std::memory_order_relaxed) != 0 && (switches() & which) != 0;
}
} // namespace recomp_ab
