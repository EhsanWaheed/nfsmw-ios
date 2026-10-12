/* USA Xbox 360 vehicle rumble, reproduced on the PC game's own IFeedback calls (Test59).
 *
 * Source: the pinned decoded USA image (native/verification/xbox-decoded-reference), read
 * with work/usa_ppc.py. Addresses below are that image's unless marked PC.
 *
 * The sim's per-frame feedback driver (360 8246e420, PC 006ee490 -- the same code) calls
 * the player's IFeedback: PauseEffects if the player may not feel feedback, otherwise
 * ResumeEffects, BeginUpdate, then per wheel UpdateTireSlip(front, surface, |slip|),
 * UpdateTireSkid(front, surface, |skid|) and UpdateRoadNoise(front, surface, body speed),
 * then UpdateRPM, UpdateShiftPotential, UpdateNOS, UpdateEngineBlown/UpdateShifting and
 * EndUpdate. On PC that interface (vtable PC 008a7bd4) is a stub: every Update* is an
 * empty `ret n` (PC 006288b0..00628930), so the arguments the PC game already computes
 * are thrown away. On the 360 (IFeedback vtable at 820867c8) they drive a per-player
 * mixer block (82919810 + 0x1b0 x player):
 *
 *  - 16 oscillators {phase, amp, freq Hz, offset}: value = offset + amp*sin(2*pi*phase),
 *    phase += freq*dt_ms/1000 (823a3540; if freq*amp <= 0 the value is the offset).
 *  - 10 linear decays {time left s, duration s, start, end} at +0x100..+0x190:
 *    time -= dt_ms/1000; value = end + (start-end)*time/duration, not below 0 (823a34d8).
 *  - mixer 823a36c8, per frame with the wall-clock dt in ms (823a4050: timebase delta):
 *    left  = max(0, osc0+osc1, osc4+osc5, osc8+osc9, osc12+osc13, decays 0,2,4,6,8)
 *    right = max(0, osc2+osc3, osc6+osc7, osc10+osc11, osc14+osc15, decays 1,3,5,7,9)
 *    each clamped to 0..1, sent as trunc(x*65535) to XInput left (low) / right (high)
 *    motors (823a3a70 -> actuator SetValues 8261cbd8).
 *  - BeginUpdate (823a4808): amp, freq and offset of oscillators 0..11 = 0 (phase kept).
 *  - UpdateRoadNoise/TireSkid/TireSlip (823a4870/4968/4a78): for record i = 0..3 of the
 *    surface's graph attribute (simsurface keys ba1297da / 0c149044 / 8fd11d27, 32 bytes:
 *    freq0 amp0 offset0 x0 freq1 amp1 offset1 x1), evaluate at x (823802a8: nothing if
 *    x < x0 or x1 <= x0; t = min((x-x0)/(x1-x0), 1); linear) and max-merge freq, amp and
 *    offset into oscillator base+i (base 0 / 4 / 8). `front` only scales skid x by 0.25.
 *  - UpdateRPM (823a4b70) is empty.
 *  - UpdateShiftPotential (823a4b78): if the previous value was nonzero, decays 2 and 3 are
 *    cleared; on a rise to 3/4/5 both are set to a constant 0.5/0.75/1.0 for 3.0 s.
 *  - UpdateNOS (823a4df8), on change: engaged -> decay 4 (left) 1.0 -> 0.5 over 2.0 s;
 *    released -> cleared. UpdateEngineBlown (823a4d40), on change: blown -> decays 6 and 7
 *    1.0 -> 0.6 over 1.0 s; else cleared. UpdateShifting (823a4eb8), on becoming true:
 *    decays 8 and 9 constant 0.4 for 0.5 s.
 *  - Pause/Resume (823a4630/46a8) clear/set the enable flag and send; Reset (823a4720)
 *    disables, zeroes both outputs and decay 0, and sends. Everything above is skipped
 *    while disabled.
 *
 * The graphs come from GLOBAL/attributes.bin. The USA disc's attributes.bin and the PC one
 * hold the same 47 simsurface collections with the same entries and the same three graph
 * arrays (Test59 README): paved surfaces inherit only a TireSlip graph; the dirt/grass/
 * gravel/sand/hay/mud/snow family and the wood family add RoadNoise; no surface has TireSkid.
 *
 * Test61: ReportCollision (823a4f30) reproduced below on the PC stub 00628930 (decay 0, left
 * motor); it replaces the Development 50 PC impact pulse in this model. Oscillators 12..15 are
 * never set by any USA IFeedback method (they stay 0).
 * PORT CHOICES (labelled): the mixer steps once per game device update on the host's
 * monotonic clock and its result is sent in the same update (the 360 sends the previous
 * step's result first: at most one frame earlier here); `surface_rumble` % scales both
 * motors (100 = console levels); a driver lapse of 250 ms or more silences the output. */
#define X360_FEEDBACK_VTABLE 0x008a7bd4u   /* PC IFeedback implementation (object + 0x2c) */

typedef struct { float phase, amp, freq, offset; } X360Osc;
typedef struct { float left, duration, start, end; } X360Decay;
static struct {
    X360Osc osc[16];
    X360Decay decay[10];
    float left, right;          /* mixed motor levels (360 +0x1a0 / +0x1a4) */
    float drive_left, coll_left; /* native split of left: all sources except decay 0 / decay 0 (collision) */
    int enabled;                /* +0x1ac */
    int32_t shift_potential;    /* +0x2e4 */
    uint8_t nos, blown, shifting; /* +0x2e0..0x2e2 */
    uint64_t step_ms, driver_ms;
    uint32_t driver_calls, steps;
    /* PORT ENHANCEMENT (Test69, `paved_surface_rumble`): this frame's wheels touched a surface with
       no 360 RoadNoise graph but with PC wheel-effect data (cobble, stone, rooftile, metal, railroad),
       and/or a surface with a RoadNoise graph (the 360 model then owns the road feel alone). */
    uint8_t paved_seen, roadnoise_seen;
    /* trace only */
    float trace_speed, trace_slip, trace_skid;
    uint32_t trace_surface;
    uint64_t trace_ms;
    uint32_t trace_calls, trace_steps;
} g_x360;

static int g_x360_hooked;   /* all eleven IFeedback hooks installed */
static void x360_reset_state(void) { memset(&g_x360, 0, sizeof g_x360); }
static int x360_model(void) { return g_x360_hooked && setting("rumble_model", 1) == 1; }

static float x360_osc_step(X360Osc *o, float dt_ms) {
    if (o->freq * o->amp > 0.0f) {
        o->phase = o->freq * dt_ms * 0.001f + o->phase;
        if (o->phase > 1.0f) o->phase -= 1.0f;
        if (o->phase > 1.0f) o->phase -= floorf(o->phase); /* same sine; bounded float */
        return sinf(o->phase * 6.2831855f) * o->amp + o->offset;
    }
    return o->offset;
}
static float x360_decay_step(X360Decay *d, float dt_ms) {
    if (d->left > 0.0f && d->duration > 0.0f) {
        d->left = d->left - dt_ms * 0.001f;
        float v = (d->start - d->end) / d->duration * d->left + d->end;
        return v < 0.0f ? 0.0f : v;
    }
    return 0.0f;
}
static void x360_mix(float dt_ms) {
    if (!(dt_ms > 0.0f) || !g_x360.enabled) return;
    float o[16], d[10];
    for (int i = 0; i < 16; ++i) o[i] = x360_osc_step(&g_x360.osc[i], dt_ms);
    for (int i = 0; i < 10; ++i) d[i] = x360_decay_step(&g_x360.decay[i], dt_ms);
    float l = 0.0f, r = 0.0f;
    for (int g = 0; g < 16; g += 4) {
        l = fmaxf(l, o[g] + o[g + 1]);
        r = fmaxf(r, o[g + 2] + o[g + 3]);
    }
    for (int i = 2; i < 10; i += 2) l = fmaxf(l, d[i]);
    for (int i = 0; i < 10; i += 2) r = fmaxf(r, d[i + 1]);
    /* Native split so the two strength settings stay independent; at 100/100 the
       recombined max(drive, collision) is exactly the console's left motor. */
    g_x360.drive_left = l >= 1.0f ? 1.0f : (l < 0.0f ? 0.0f : l);
    g_x360.coll_left = d[0] >= 1.0f ? 1.0f : (d[0] < 0.0f ? 0.0f : d[0]);
    l = fmaxf(l, d[0]);
    g_x360.left = l >= 1.0f ? 1.0f : (l < 0.0f ? 0.0f : l);
    g_x360.right = r >= 1.0f ? 1.0f : (r < 0.0f ? 0.0f : r);
    ++g_x360.steps;
}
/* 823802a8: one graph record at x; 0 when it contributes nothing. */
static int x360_graph_eval(const float rec[8], float x, float *freq, float *amp, float *offset) {
    const float x0 = rec[3], x1 = rec[7];
    if (!(x >= x0) || !(x1 > x0)) return 0;
    float t = (x - x0) / (x1 - x0);
    if (t >= 1.0f) t = 1.0f;
    *freq = (rec[4] - rec[0]) * t + rec[0];
    *amp = (rec[5] - rec[1]) * t + rec[1];
    *offset = (rec[6] - rec[2]) * t + rec[2];
    return 1;
}
/* Returns how many records of `key` the surface has (whether or not they contribute at x). */
static int x360_merge_graph(const PopModApi *api, uint32_t surface, uint32_t key, int base, float x) {
    int found = 0;
    if (!surface || !api->guest_call) return 0;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t args[2] = {key, i}, rec = 0;
        if (api->guest_call(api, 0x00454810u, surface, args, 2, &rec) != POP_OK || !rec) continue;
        ++found;
        float v[8], f, a, off;
        for (unsigned j = 0; j < 8; ++j) { uint32_t b = get_u32(rec + 4 * j); memcpy(&v[j], &b, 4); }
        if (!x360_graph_eval(v, x, &f, &a, &off)) continue;
        X360Osc *o = &g_x360.osc[base + (int)i];
        o->freq = fmaxf(f, o->freq);   /* 823a4900..4944: each field by maximum */
        o->amp = fmaxf(a, o->amp);
        o->offset = fmaxf(off, o->offset);
    }
    return found;
}
static void x360_set_decay(int i, float duration, float start, float end) {
    g_x360.decay[i].left = duration; g_x360.decay[i].duration = duration;
    g_x360.decay[i].start = start; g_x360.decay[i].end = end;
}
static int x360_ours(pop_cpu_v1 *cpu) {
    return cpu->ecx && get_u32(cpu->ecx) == X360_FEEDBACK_VTABLE && x360_model();
}
static float x360_arg_float(pop_cpu_v1 *cpu, unsigned n) {
    uint32_t b = get_u32(cpu->esp + 4 * n); float f; memcpy(&f, &b, 4); return f;
}
static void x360_trace(const PopModApi *api, const char *what, int value) {
    if (!setting("input_trace", 0)) return;
    char line[128];
    snprintf(line, sizeof line, "core.nfsmw: x360 %s %d", what, value);
    api->log(api, line);
}

/* BEFORE hooks on the PC stubs (thiscall; arguments at esp+4..). */
static void x360_pause(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu)) return;
    if (g_x360.enabled) x360_trace(api, "pause", 0);
    g_x360.enabled = 0;
}
static void x360_resume(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user; (void)api;
    if (!x360_ours(cpu)) return;
    g_x360.enabled = 1;
}
static void x360_reset(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu)) return;
    g_x360.enabled = 0;
    g_x360.left = g_x360.right = 0.0f;
    memset(&g_x360.decay[0], 0, sizeof g_x360.decay[0]);
    x360_trace(api, "reset", 0);
}
static void x360_begin(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user; (void)api;
    if (!x360_ours(cpu)) return;
    for (int i = 0; i < 12; ++i) g_x360.osc[i].amp = g_x360.osc[i].freq = g_x360.osc[i].offset = 0.0f;
    g_x360.paved_seen = g_x360.roadnoise_seen = 0;
    g_x360.driver_ms = g_feedback_clock();
    ++g_x360.driver_calls;
}
static void x360_road(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu) || !g_x360.enabled) return;
    const uint32_t surface = get_u32(cpu->esp + 8);
    if (x360_merge_graph(api, surface, 0xba1297dau, 0, x360_arg_float(cpu, 3)))
        g_x360.roadnoise_seen = 1;
    else if (surface) {
        /* The PC surface layout (instance +8) WheelEffectIntensity byte (+0xfa), as road_surface_trace reads it. */
        const uint32_t layout = get_u32(surface + 8);
        if (layout && ((get_u32(layout + 0xf8) >> 16) & 0xffu)) g_x360.paved_seen = 1;
    }
    g_x360.trace_speed = x360_arg_float(cpu, 3);
    g_x360.trace_surface = get_u32(get_u32(cpu->esp + 8) + 4);
}
static void x360_skid(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu) || !g_x360.enabled) return;
    float x = x360_arg_float(cpu, 3);
    g_x360.trace_skid = fmaxf(g_x360.trace_skid, x);
    if (get_u32(cpu->esp + 4) & 0xffu) x *= 0.25f;
    x360_merge_graph(api, get_u32(cpu->esp + 8), 0x0c149044u, 4, x);
}
static void x360_slip(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu) || !g_x360.enabled) return;
    x360_merge_graph(api, get_u32(cpu->esp + 8), 0x8fd11d27u, 8, x360_arg_float(cpu, 3));
    g_x360.trace_slip = fmaxf(g_x360.trace_slip, x360_arg_float(cpu, 3));
}
static void x360_shift_potential(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu) || !g_x360.enabled) return;
    const int32_t now = (int32_t)get_u32(cpu->esp + 4), prev = g_x360.shift_potential;
    float level = 0.0f; int fresh = 0;
    if (now == 3) { level = 0.5f; fresh = prev != 3; }
    else if (now == 4) { level = 0.75f; fresh = prev != 4; }
    else if (now == 5) { level = 1.0f; fresh = prev != 5; }
    if (prev != 0) { memset(&g_x360.decay[2], 0, 2 * sizeof g_x360.decay[0]); }
    g_x360.shift_potential = now;
    if (!(level > 0.0f) || !fresh || prev >= now) return;
    x360_set_decay(2, 3.0f, level, level);
    x360_set_decay(3, 3.0f, level, level);
    x360_trace(api, "shift_potential", now);
}
static void x360_nos(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu)) return;
    const uint8_t on = (uint8_t)(get_u32(cpu->esp + 4) & 0xffu);
    if (on == g_x360.nos || !g_x360.enabled) return;
    if (on) x360_set_decay(4, 2.0f, 1.0f, 0.5f);
    else memset(&g_x360.decay[4], 0, sizeof g_x360.decay[0]);
    g_x360.nos = on;
    x360_trace(api, "nos", on);
}
static void x360_blown(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu)) return;
    const uint8_t on = (uint8_t)(get_u32(cpu->esp + 4) & 0xffu);
    if (on == g_x360.blown || !g_x360.enabled) return;
    if (on) {
        x360_set_decay(6, 1.0f, 1.0f, 0.6f);
        const float start = fmaxf(g_x360.decay[7].start, 1.0f);   /* 823a4da4..4dc0 */
        x360_set_decay(7, 1.0f, start, 0.6f);
    } else {
        memset(&g_x360.decay[6], 0, 2 * sizeof g_x360.decay[0]);
    }
    g_x360.blown = on;
    x360_trace(api, "engine_blown", on);
}
static void x360_shifting(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu)) return;
    const uint8_t on = (uint8_t)(get_u32(cpu->esp + 4) & 0xffu);
    if (on == g_x360.shifting || !g_x360.enabled) return;
    if (on) { x360_set_decay(8, 0.5f, 0.4f, 0.4f); x360_set_decay(9, 0.5f, 0.4f, 0.4f); }
    g_x360.shifting = on;
    x360_trace(api, "shifting", on);
}
/* IFeedback::ReportCollision(cinfo, iamA) -- 360 823a4f30, PC stub 00628930 (RET 8).
 * PC caller: LocalPlayer::OnCollision 006f2370 (listener at LocalPlayer+0x54, mFFB at +0x58),
 * the player's Sim::Collision listener, so every collision of the player's car arrives here:
 * traffic, police, props and world. It calls only if PlayerSettings.Rumble ([settings+4]) and
 * the global 008f86e0 are set (the 360 does the same, reconstruction LocalPlayer.cpp:391).
 * 360 logic (Test59 README section 8, re-read for Test61), only while enabled:
 *   own = FindInstance(iamA ? objA : objB), other = FindInstance(iamA ? objB : objA)
 *   (PC 005d51f0 on the simable table 0092feb4/0092febc; entry+4 = ISimable);
 *   v = own->GetLinearVelocity() (ISimable slot 0x68 on both); own speed = |v|;
 *   rec = iamA ? objAVel (+0x50) : objBVel (+0x60); closing = |v - rec|;
 *   other_dv = |other->GetLinearVelocity() - rec| (0 without an other);
 *   type (+0x1c, low 3 bits on PC = the 360's top 3 bits):
 *     3 GROUND: m = min((closing - 1)/3, 1)      2 WORLD: m = min(closing/30, 1)
 *     1 OBJECT: closing >= 5: m = min((closing - 5)*0.05, 1); otherwise, if other is a vehicle
 *       (QueryInterface<IVehicle>: 360 8212c3f0, PC 005d59f0 handle 004040e0) and objAVel,
 *       objBVel point the same way (normalised dot > 0.1): closing = 0.5*|objAVel|, and if > 5
 *       m = min((closing - 5)*0.05, 1); else if own speed > 2 and other_dv > 2:
 *       m = clamp((own speed - 20)/(50 - 20), 0, 1) (820e0ed0); else nothing.
 *   m > 0: decay 0 (left motor) falls from m to 0 over m seconds; an active one is extended
 *   field by field by maximum (time left, duration, start), end 0. */
static uint32_t g_x360_scratch;      /* guest scratch: GetLinearVelocity output */
static int g_x360_collision_hooked;
static struct { uint32_t calls, reported, ignored_disabled, no_own; } g_x360_coll;
static int x360_find_simable(const PopModApi *api, uint32_t handle, uint32_t *simable) {
    *simable = 0;
    if (!handle) return 0;
    const uint32_t begin = get_u32(0x0092feb4u), count = get_u32(0x0092febcu);
    uint32_t args[3] = {begin, begin + count * 8u, handle}, entry = 0;
    if (!begin || api->guest_call(api, 0x005d51f0u, 0, args, 3, &entry) != POP_OK || !entry) return 0;
    *simable = get_u32(entry + 4);
    return *simable != 0;
}
static int x360_velocity(const PopModApi *api, uint32_t simable, float v[3]) {
    const uint32_t vt = simable ? get_u32(simable) : 0;
    if (!vt || !g_x360_scratch) return 0;
    uint32_t arg = g_x360_scratch;
    if (api->guest_call(api, get_u32(vt + 0x68), simable, &arg, 1, NULL) != POP_OK) return 0;
    for (int i = 0; i < 3; ++i) { uint32_t b = get_u32(g_x360_scratch + 4u * (uint32_t)i); memcpy(&v[i], &b, 4); }
    return 1;
}
static void x360_vec(uint32_t addr, float v[3]) {
    for (int i = 0; i < 3; ++i) { uint32_t b = get_u32(addr + 4u * (uint32_t)i); memcpy(&v[i], &b, 4); }
}
static float x360_len(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
/* Pure 360 arithmetic (823a5090..52d0), separated for the fixture tests. */
static float x360_collision_strength(int type, float closing, float own_speed, float other_dv,
                                     int other_is_vehicle, const float a_vel[3], const float b_vel[3]) {
    float m = 0.0f;
    if (type == 3) {
        m = (closing - 1.0f) * 0.33333334f;
        if (m >= 1.0f) m = 1.0f;
    } else if (type == 2) {
        m = closing * 0.033333335f;
        if (m >= 1.0f) m = 1.0f;
    } else if (type == 1) {
        if (closing >= 5.0f) {
            m = (closing - 5.0f) * 0.05f;
            if (m >= 1.0f) m = 1.0f;
        } else {
            int same_way = 0;
            if (other_is_vehicle) {
                float la = x360_len(a_vel), lb = x360_len(b_vel);
                float dot = 0.0f;
                if (la > 0.0f && lb > 0.0f)   /* 820e1268 normalises; a zero vector stays zero */
                    dot = (a_vel[0] * b_vel[0] + a_vel[1] * b_vel[1] + a_vel[2] * b_vel[2]) / (la * lb);
                if (dot > 0.1f) { closing = la * 0.5f; same_way = 1; }
            }
            if (same_way && closing > 5.0f) {
                m = (closing - 5.0f) * 0.05f;
                if (m >= 1.0f) m = 1.0f;
            } else {
                if (!(own_speed > 2.0f) || !(other_dv > 2.0f)) return 0.0f;
                m = (own_speed - 20.0f) / 30.0f;
                if (m >= 1.0f) m = 1.0f;
                if (m < 0.0f) m = 0.0f;
            }
        }
    }
    return m;
}
/* Test132 (owner, 2 Oct 2026: strength right, the fade after a scrape too long): 'collision_rumble_fade' % scales
 * only the fade time of the collision thump (decay 0); its strength (start) stays the console's m. 100 = the Xbox 360
 * (m seconds). A sustained scrape still renews the thump every frame, so it lasts while the contact lasts; only the
 * tail after the contact ends gets shorter. */
static void x360_collision_apply(float m) {
    if (!(m > 0.0f)) return;
    int64_t pct = setting("collision_rumble_fade", 50);
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    const float t = m * (float)pct / 100.0f;
    X360Decay *d = &g_x360.decay[0];
    if (d->left > 0.0f) {
        d->duration = fmaxf(d->duration, t);
        d->left = fmaxf(d->left, t);
        d->start = fmaxf(d->start, m);
    } else {
        d->left = d->duration = t;
        d->start = m;
    }
    d->end = 0.0f;
}
/* BEFORE 00628930 (thiscall; cinfo, iamA). */
static void x360_collision(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!x360_ours(cpu)) return;
    ++g_x360_coll.calls;
    const uint32_t cinfo = get_u32(cpu->esp + 4);
    const int iama = (get_u32(cpu->esp + 8) & 0xffu) != 0;
    const int trace = (int)setting("input_trace", 0);
    if (!cinfo) return;
    if (!g_x360.enabled && !trace) { ++g_x360_coll.ignored_disabled; return; }
    const int type = (int)(get_u32(cinfo + 0x1c) & 7u);
    uint32_t own = 0, other = 0;
    x360_find_simable(api, get_u32(cinfo + (iama ? 0x3c : 0x4c)), &own);
    x360_find_simable(api, get_u32(cinfo + (iama ? 0x4c : 0x3c)), &other);
    float v[3], ov[3], rec[3], av[3], bv[3];
    if (!own || !x360_velocity(api, own, v)) { ++g_x360_coll.no_own; return; }
    x360_vec(cinfo + (iama ? 0x50 : 0x60), rec);
    x360_vec(cinfo + 0x50, av);
    x360_vec(cinfo + 0x60, bv);
    const float d0[3] = {v[0] - rec[0], v[1] - rec[1], v[2] - rec[2]};
    const float closing = x360_len(d0), own_speed = x360_len(v);
    float other_dv = 0.0f;
    int other_vehicle = 0;
    if (other && x360_velocity(api, other, ov)) {
        const float d1[3] = {ov[0] - rec[0], ov[1] - rec[1], ov[2] - rec[2]};
        other_dv = x360_len(d1);
    }
    if (other && type == 1) {
        uint32_t iv = 0, h = 0x004040e0u;
        other_vehicle = api->guest_call(api, 0x005d59f0u, get_u32(other + 4), &h, 1, &iv) == POP_OK && iv;
    }
    const float m = x360_collision_strength(type, closing, own_speed, other_dv, other_vehicle, av, bv);
    if (g_x360.enabled) { x360_collision_apply(m); ++g_x360_coll.reported; }
    else ++g_x360_coll.ignored_disabled;
    if (trace) {
        uint32_t other_type = 0;
        if (other) {   /* ISimable::GetSimableType, slot 4 (trace only) */
            const uint32_t ovt = get_u32(other);
            if (ovt) api->guest_call(api, get_u32(ovt + 4), other, NULL, 0, &other_type);
        }
        char line[256];
        snprintf(line, sizeof line,
                 "core.nfsmw: x360 collision type=%d iamA=%d sliding=%u own_speed=%.2f closing=%.2f other=%s(type %u%s) "
                 "other_dv=%.2f m=%.3f enabled=%d decay0=%.2f/%.2f",
                 type, iama, (get_u32(cinfo + 0x1c) >> 7) & 1u, own_speed, closing, other ? "yes" : "no",
                 other_type & 0xffu, other_vehicle ? ",vehicle" : "", other_dv, m, g_x360.enabled,
                 g_x360.decay[0].left, g_x360.decay[0].start);
        api->log(api, line);
    }
}
/* The sim driver 006ee490 (360 8246e420) first asks the player's input device for a second
 * interface ([this+0x5c], handle 00627880; 360 8246b778): if that interface's slot 3 says so,
 * the driver hands it the frame (slot 1, this+0x4c) and returns before any IFeedback call.
 * On PC every DirectInput device answers yes -- it is the wheel force-feedback path (00643xxx,
 * where the Development 50 collision pulse and the Development 52 surface bridge come from).
 * A 360 pad does not take that path, so the 360 feels IFeedback. With the 360 model this
 * wrapper runs the driver with the device interface hidden (the IFeedback path, exactly as the
 * 360 does for a pad), then gives the device its frame as the original would have, when the
 * driver's own gate (006ee400) passed. Order within the frame: IFeedback first, then the device
 * (the original ran only the device). Without the 360 model the original runs untouched. */
static int g_x360_gate_passed;
static uint32_t g_x360_dbg_driver, g_x360_dbg_gate_true, g_x360_dbg_gate_false, g_x360_dbg_device;
static void x360_gate(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user; (void)api;
    g_x360_gate_passed = (cpu->eax & 0xffu) != 0;
    if (g_x360_gate_passed) ++g_x360_dbg_gate_true; else ++g_x360_dbg_gate_false;
}
static void x360_driver(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t self = cpu->ecx, device = self ? get_u32(self + 0x5c) : 0;
    ++g_x360_dbg_driver;
    if (!x360_model() || !device || !get_u32(self + 0x58) || !api->guest_call) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    api->guest_write_u32(api, self + 0x5c, 0);
    g_x360_gate_passed = 0;
    api->call_original(api, cpu->target, cpu);
    api->guest_write_u32(api, self + 0x5c, device);
    if (!g_x360_gate_passed) return;   /* the original paused feedback and returned */
    const uint32_t vt = get_u32(device);
    uint32_t active = 0, arg = self + 0x4c, ignored = 0;
    if (!vt || api->guest_call(api, get_u32(vt + 0xc), device, NULL, 0, &active) != POP_OK || !(active & 0xffu))
        return;
    ++g_x360_dbg_device;
    api->guest_call(api, get_u32(vt + 4), device, &arg, 1, &ignored);
}
/* Called from impact_feedback_tick: steps the mixer; returns 1 and the motor levels
 * (0..1) while the 360 model owns the continuous feedback. */
static float g_x360_trace_paved;   /* input_trace: the paved-surface rumble's peak this second (Test69) */
static void x360_trace_second(const PopModApi *api, uint64_t now) {
    if (!setting("input_trace", 0)) return;
    if (!g_x360.trace_ms) { g_x360.trace_ms = now; return; }
    if (now - g_x360.trace_ms < 1000) return;
    char line[256];
    snprintf(line, sizeof line,
             "core.nfsmw: x360 %llums driver=%u steps=%u enabled=%d left=%.3f right=%.3f speed=%.2f "
             "max_slip=%.2f max_skid=%.2f collection=%08x osc0=%.2f/%.2f/%.2f osc8=%.2f/%.2f/%.2f "
             "[sim driver %u gate %u/%u device %u] paved=%.3f",
             (unsigned long long)(now - g_x360.trace_ms), g_x360.driver_calls - g_x360.trace_calls,
             g_x360.steps - g_x360.trace_steps, g_x360.enabled, g_x360.left, g_x360.right,
             g_x360.trace_speed, g_x360.trace_slip, g_x360.trace_skid, g_x360.trace_surface,
             g_x360.osc[0].freq, g_x360.osc[0].amp, g_x360.osc[0].offset,
             g_x360.osc[8].freq, g_x360.osc[8].amp, g_x360.osc[8].offset,
             g_x360_dbg_driver, g_x360_dbg_gate_true, g_x360_dbg_gate_false, g_x360_dbg_device, (double)g_x360_trace_paved);
    g_x360_trace_paved = 0.0f;
    g_x360_dbg_driver = g_x360_dbg_gate_true = g_x360_dbg_gate_false = g_x360_dbg_device = 0;
    api->log(api, line);
    g_x360.trace_ms = now; g_x360.trace_calls = g_x360.driver_calls; g_x360.trace_steps = g_x360.steps;
    g_x360.trace_slip = g_x360.trace_skid = 0.0f;
}
static float g_x360_out_drive_left, g_x360_out_coll_left;   /* parts of the last *left */
static int x360_feedback_levels(uint64_t now, float *left, float *right) {
    *left = *right = 0.0f;
    g_x360_out_drive_left = g_x360_out_coll_left = 0.0f;
    if (!x360_model()) return 0;
    const float dt = g_x360.step_ms && now > g_x360.step_ms ? (float)(now - g_x360.step_ms) : 0.0f;
    g_x360.step_ms = now;
    x360_mix(dt);
    if (!g_x360.enabled || !g_x360.driver_ms || now - g_x360.driver_ms >= 250) return 1;
    *left = g_x360.left;
    *right = g_x360.right;
    g_x360_out_drive_left = g_x360.drive_left;
    g_x360_out_coll_left = g_x360.coll_left;
    return 1;
}
