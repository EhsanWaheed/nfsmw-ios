// Surface/impact rumble through the production output chain (Test58).
// The core mod's impact_feedback.h runs against a fixture guest; its pulses go
// through the real host arbitration (controls::Vpad) and the real motor router
// (controls::RumbleRouter). Checks read the router's controller commands -- the
// values gamepad_rumble() would send -- not the mod's request trace.
#include "../../kit/mods/pop_mod_api.h"
#include "../../kit/host/controls/vpad.h"
#include "../../kit/host/controls/haptics.h"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static std::map<uint32_t, uint32_t> mem;
static std::map<std::string, int64_t> settings;
static const PopModApi *g_api;
static uint32_t get_u32(uint32_t a) { auto it = mem.find(a); return it == mem.end() ? 0 : it->second; }
static int64_t setting(const char *k, int64_t fb) { auto it = settings.find(k); return it == settings.end() ? fb : it->second; }
static uint64_t fixture_ms;
static uint64_t fixture_clock() { return fixture_ms; }
#include "../../mods/core/nfsmw/impact_feedback.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

static const uint32_t W = 0x01000000u;              // FFB wrapper
static controls::Vpad pad;
struct Cmd { uint64_t ms; uint16_t low, high; uint32_t dur; };
static std::vector<Cmd> cmds;
static controls::RumbleRouter router(controls::RumbleOutputs{
    [](uint16_t l, uint16_t h, uint32_t ms) { cmds.push_back({fixture_ms, l, h, ms}); }, nullptr});
static bool connected = true;
static PopModApi api{};

static uint16_t motor() { return cmds.empty() ? 0 : cmds.back().low; }
static void pump() {                                  // controls_host.cpp pump order
    pad.expire_rumble_pulses(fixture_ms * 1000000ull);
    const uint64_t serial = pad.rumble_serial();
    uint16_t l = 0, h = 0;
    pad.rumble(&l, &h);
    router.update(serial, l, h, controls::rumble_sink(connected, false), fixture_ms * 1000000ull);
}
static void road(uint32_t wave, int32_t mag, uint32_t period, uint32_t device = 0, uint32_t wrapper = W) {
    pop_cpu_v1 cpu{}; cpu.ecx = wrapper; cpu.esp = 0x2000;
    mem[0x2004] = device; mem[0x2008] = wave; mem[0x200c] = (uint32_t)mag; mem[0x2010] = period;
    road_feedback(&api, &cpu, nullptr, nullptr);
}
static void impact(int32_t mag) {
    pop_cpu_v1 cpu{}; cpu.ecx = W; cpu.esp = 0x3000; mem[0x3004] = 0; mem[W + 0x4f48] = (uint32_t)mag;
    impact_feedback(&api, &cpu, nullptr, nullptr);
}
static void reset() {
    impact_feedback_stop(&api); pump();
    mem.clear(); settings.clear(); cmds.clear(); connected = true;
    pad.set_controller_connected(true);
    mem[0x009203c4u] = W; mem[0x00925e90u] = 6; mem[W + 0x4458] = 1;
    g_road = {}; g_impact_until_ms = 0; g_impact_level = 0; g_feedback_sent_ms = 0; g_feedback_sent_level = 0;
    g_feedback_sent_high = 0; x360_reset_state();
    settings["rumble_model"] = 0;                     // sections 1-13: the PC wheel adaptation
    settings["rumble_floor"] = 0;                     // exact levels; section 14 tests the cut-off
    fixture_ms += 10000;
}
// Runs `ms` of game at `fps` (one road call + one tick per guest frame when
// `calls`), pumping the host every ~8 ms; returns router motor samples per pump.
static std::vector<uint16_t> drive(uint32_t ms, double fps, uint32_t wave, int32_t mag, uint32_t period,
                                   bool calls = true, bool ticks = true) {
    std::vector<uint16_t> out;
    const uint64_t end = fixture_ms + ms;
    double next_frame = (double)fixture_ms;
    while (fixture_ms < end) {
        if ((double)fixture_ms >= next_frame) {
            if (calls) road(wave, mag, period);
            if (ticks) impact_feedback_tick(&api);
            next_frame += 1000.0 / fps;
        }
        pump();
        out.push_back(motor());
        fixture_ms += 1;
    }
    return out;
}
static double mean(const std::vector<uint16_t> &v, size_t from = 0) {
    double s = 0; for (size_t i = from; i < v.size(); ++i) s += v[i]; return s / (double)(v.size() - from) / 65535.0;
}

// ---- Test59: USA Xbox 360 model (x360_feedback.h) through the same output chain ----
static const uint32_t FB = 0x05000000u;               // PC IFeedback subobject (vtable 008a7bd4)
static const uint32_t S_ASPHALT = 0x06000000u, S_DIRT = 0x06001000u, S_WOOD = 0x06002000u;
static std::map<std::pair<uint32_t, uint64_t>, uint32_t> graphs;   // (surface, key<<8|index) -> record
static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static void put_record(uint32_t surface, uint32_t key, uint32_t index, uint32_t at, std::initializer_list<float> v) {
    uint32_t a = at; for (float f : v) { mem[a] = fbits(f); a += 4; }
    graphs[{surface, ((uint64_t)key << 8) | index}] = at;
}
static void graphs_fixture() {                        // the three arrays of attributes.bin (PC == USA 360)
    graphs.clear();
    mem[FB] = X360_FEEDBACK_VTABLE;
    for (uint32_t s : {S_ASPHALT, S_DIRT, S_WOOD})   // TireSlip, inherited from the paved parent
        put_record(s, 0x8fd11d27u, 0, s + 0x100, {0, 0, 0, 0.5f, 1, 0.5f, 0.5f, 20});
    put_record(S_DIRT, 0xba1297dau, 0, S_DIRT + 0x200, {0, 0.25f, 0.5f, 5, 1, 0.5f, 0.5f, 50});
    put_record(S_DIRT, 0xba1297dau, 1, S_DIRT + 0x220, {0, 0, 0, 1, 4, 0.5f, 0, 20});
    put_record(S_WOOD, 0xba1297dau, 0, S_WOOD + 0x200, {1, 0.5f, 0, 5, 4, 0.25f, 0.25f, 50});
}
static void fb(PopHookFn fn, std::initializer_list<uint32_t> args, uint32_t ecx = FB) {
    pop_cpu_v1 cpu{}; cpu.ecx = ecx; cpu.esp = 0x4000; uint32_t a = 0x4004;
    for (uint32_t v : args) { mem[a] = v; a += 4; }
    fn(&api, &cpu, nullptr, nullptr);
}
struct Wheel { uint32_t surface; float slip, skid; };
// One 006ee490 driver call: Resume, BeginUpdate, per wheel Slip/Skid/RoadNoise, then events.
static void driver(const std::vector<Wheel> &w, float speed, uint32_t potential = 0, int nos = 0,
                   int blown = 0, int shifting = 0, uint32_t garbage = 0) {
    fb(x360_resume, {}); fb(x360_begin, {});
    for (size_t i = 0; i < w.size(); ++i) {
        const uint32_t front = (i < 2 ? 1u : 0u) | garbage;   // only the low byte is the bool
        fb(x360_slip, {front, w[i].surface, fbits(w[i].slip)});
        fb(x360_skid, {front, w[i].surface, fbits(w[i].skid)});
        fb(x360_road, {front, w[i].surface, fbits(speed)});
    }
    fb(x360_shift_potential, {potential});
    fb(x360_nos, {(uint32_t)nos | garbage, fbits(1.0f)});
    fb(x360_blown, {(uint32_t)blown | garbage});
    fb(x360_shifting, {(uint32_t)shifting | garbage});
}
static void x360_reset_fixture() { reset(); settings.erase("rumble_model"); graphs_fixture(); g_x360_hooked = 1; }
static std::vector<Wheel> four(uint32_t s, float slip_rear = 0, float slip_front = 0) {
    return {{s, slip_front, 0}, {s, slip_front, 0}, {s, slip_rear, 0}, {s, slip_rear, 0}};
}
// Runs `ms` at `fps` (driver + device tick per frame), pumping every 1 ms; low/high samples.
static void run(uint32_t ms, double fps, const std::vector<Wheel> &w, float speed, std::vector<uint16_t> &lo,
                std::vector<uint16_t> &hi, uint32_t potential = 0, int nos = 0, int blown = 0, int shifting = 0) {
    const uint64_t end = fixture_ms + ms;
    double next = (double)fixture_ms;
    while (fixture_ms < end) {
        if ((double)fixture_ms >= next) {
            driver(w, speed, potential, nos, blown, shifting);
            impact_feedback_tick(&api);
            next += 1000.0 / fps;
        }
        pump();
        lo.push_back(cmds.empty() ? 0 : cmds.back().low);
        hi.push_back(cmds.empty() ? 0 : cmds.back().high);
        fixture_ms += 1;
    }
}
static uint16_t vmax(const std::vector<uint16_t> &v, size_t from = 0) { uint16_t m = 0; for (size_t i = from; i < v.size(); ++i) m = std::max(m, v[i]); return m; }
static uint16_t vmin(const std::vector<uint16_t> &v, size_t from = 0) { uint16_t m = 65535; for (size_t i = from; i < v.size(); ++i) m = std::min(m, v[i]); return m; }
static uint16_t high_motor() { return cmds.empty() ? 0 : cmds.back().high; }

static void x360_tests() {
    api.guest_call = [](const PopModApi *, uint32_t addr, uint32_t ecx, const uint32_t *args, uint32_t n, uint32_t *out) {
        if (addr != 0x00454810u || n != 2) return POP_E_RANGE;
        auto it = graphs.find({ecx, ((uint64_t)args[0] << 8) | args[1]});
        *out = it == graphs.end() ? 0 : it->second;
        return POP_OK;
    };
    // X1. The record evaluator (823802a8): below x0 or x1 <= x0 or NaN -> nothing; clamped at x1.
    {
        const float r[8] = {0, 0, 0, 0.5f, 1, 0.5f, 0.5f, 20};
        float f = -1, a = -1, o = -1;
        CHECK(!x360_graph_eval(r, 0.49f, &f, &a, &o));
        CHECK(x360_graph_eval(r, 0.5f, &f, &a, &o) && f == 0 && a == 0 && o == 0);
        CHECK(x360_graph_eval(r, 10.25f, &f, &a, &o) && std::fabs(f - 0.5f) < 1e-6f && std::fabs(a - 0.25f) < 1e-6f);
        CHECK(x360_graph_eval(r, 500, &f, &a, &o) && f == 1 && a == 0.5f && o == 0.5f);
        CHECK(!x360_graph_eval(r, NAN, &f, &a, &o));
        const float flat[8] = {0, 1, 1, 5, 0, 1, 1, 5};
        CHECK(!x360_graph_eval(flat, 6, &f, &a, &o));
    }
    // X2. Plain asphalt, rolling without slip: nothing at all (no road noise on the 360).
    x360_reset_fixture();
    std::vector<uint16_t> lo, hi;
    run(1000, 60, four(S_ASPHALT), 40, lo, hi);
    CHECK(vmax(lo) == 0 && vmax(hi) == 0 && cmds.empty());
    // X3. Full wheelspin on asphalt (rear slip 25 m/s): left motor 0..1 at 1 Hz, right none.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(3000, 60, four(S_ASPHALT, 25), 10, lo, hi);
    int rises = 0; for (size_t i = 1; i < lo.size(); ++i) rises += lo[i - 1] < 32768 && lo[i] >= 32768;
    printf("x360 wheelspin asphalt: low %u..%u, high max %u, half-level rises in 3 s = %d\n",
           vmin(lo, 100), vmax(lo, 100), vmax(hi), rises);
    CHECK(vmax(lo) > 0.97 * 65535 && vmin(lo, 100) < 0.03 * 65535 && vmax(hi) == 0);
    CHECK(rises >= 2 && rises <= 4);
    // Half slip (10.25 m/s): amplitude and offset 0.25, 0.5 Hz: the level stays within 0..0.5.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(4000, 60, four(S_ASPHALT, 10.25f), 10, lo, hi);
    CHECK(vmax(lo) <= 32768 && vmax(lo) > 0.47 * 65535 && vmax(hi) == 0);
    // X4. Dirt at 5 m/s: offset 0.5 plus a 0.84 Hz +-0.105 swing, left motor only.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(3000, 60, four(S_DIRT), 5, lo, hi);
    printf("x360 dirt 5 m/s: low %u..%u high max %u\n", vmin(lo, 100), vmax(lo, 100), vmax(hi));
    CHECK(vmin(lo, 100) > 0.38 * 65535 && vmax(lo, 100) < 0.62 * 65535 && vmax(hi) == 0);
    CHECK(vmax(lo, 100) - vmin(lo, 100) > 0.15 * 65535);
    // ...and at 60 m/s both records saturate: 0.5 + 0.5 sin(1 Hz) + 0.5 sin(4 Hz), clamped 0..1.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(3000, 60, four(S_DIRT), 60, lo, hi);
    CHECK(vmax(lo, 100) == 65535 && vmin(lo, 100) < 0.1 * 65535 && vmax(hi) == 0);
    // Below the first record's x0 only the 4 Hz record plays (dirt at 3 m/s).
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(2000, 60, four(S_DIRT), 3, lo, hi);
    CHECK(vmax(lo, 100) > 0 && vmax(lo, 100) < 0.08 * 65535);
    // Wood: offset may be below the swing; the negative half is clipped at 0 by the mixer.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(2000, 60, four(S_WOOD), 5, lo, hi);
    CHECK(vmax(lo, 100) >= 0.49 * 65535 && vmax(lo, 100) <= 32768 && vmin(lo, 100) == 0);
    // X5. Wheels merge by maximum: one spinning wheel is enough; a slower one does not reduce it.
    x360_reset_fixture();
    {
        std::vector<Wheel> w = {{S_ASPHALT, 0, 0}, {S_ASPHALT, 0, 0}, {S_ASPHALT, 30, 0}, {S_ASPHALT, 2, 0}};
        driver(w, 10);
        CHECK(g_x360.osc[8].amp == 0.5f && g_x360.osc[8].freq == 1.0f && g_x360.osc[8].offset == 0.5f);
        std::vector<Wheel> w2 = {{S_ASPHALT, 2, 0}, {S_ASPHALT, 0, 0}, {S_ASPHALT, 0, 0}, {S_ASPHALT, 0, 0}};
        driver(w2, 10);                                // BeginUpdate cleared the old frame
        CHECK(std::fabs(g_x360.osc[8].amp - 0.5f * 1.5f / 19.5f) < 1e-6f);
    }
    // X6. No surface graph for skid anywhere: skid does nothing; the front x0.25 is on skid only.
    x360_reset_fixture();
    driver({{S_DIRT, 0, 40}, {S_DIRT, 0, 40}, {S_DIRT, 0, 40}, {S_DIRT, 0, 40}}, 0);
    CHECK(g_x360.osc[4].amp == 0 && g_x360.osc[5].amp == 0 && g_x360.osc[4].offset == 0);
    // X7. Wheelspin stops: the next frame's BeginUpdate ends the effect at once.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(700, 60, four(S_ASPHALT, 25), 10, lo, hi);
    CHECK(motor());
    lo.clear(); hi.clear(); run(100, 60, four(S_ASPHALT, 0), 10, lo, hi);
    CHECK(motor() == 0);
    // X8. Nitrous: left 1.0 -> 0.5 over 2 s then off; holding it does not retrigger.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(2600, 60, four(S_ASPHALT), 30, lo, hi, 0, 1);
    printf("x360 nos: low at 20 ms %u, 1 s %u, 1.9 s %u, 2.2 s %u; high max %u\n", lo[20], lo[1000], lo[1900], lo[2200], vmax(hi));
    CHECK(lo[20] > 0.98 * 65535 && std::abs((int)lo[1000] - 49151) < 700 && lo[1900] > 0.49 * 65535 && lo[2200] == 0);
    CHECK(vmax(hi) == 0);
    // Releasing clears it at once.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(300, 60, four(S_ASPHALT), 30, lo, hi, 0, 1);
    run(40, 60, four(S_ASPHALT), 30, lo, hi, 0, 0);
    CHECK(motor() == 0);
    // X9. Engine blown: both motors 1.0 -> 0.6 over 1 s.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(1300, 60, four(S_ASPHALT), 0, lo, hi, 0, 0, 1);
    CHECK(lo[20] > 0.98 * 65535 && hi[20] > 0.98 * 65535 && std::abs((int)lo[500] - 52428) < 900 && lo[1200] == 0);
    // X10. Shifting: both motors a constant 0.4 for 0.5 s; a held flag does not retrigger.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(800, 60, four(S_ASPHALT), 20, lo, hi, 0, 0, 0, 1);
    CHECK(lo[30] == 26214 && hi[30] == 26214 && lo[400] == 26214 && lo[700] == 0);
    run(100, 60, four(S_ASPHALT), 20, lo, hi, 0, 0, 0, 0);
    run(100, 60, four(S_ASPHALT), 20, lo, hi, 0, 0, 0, 1);
    CHECK(motor() == 26214);                           // false -> true retriggers
    // X11. Shift potential: a rise to 3/4/5 pulses 0.5/0.75/1.0 and the next call clears it.
    x360_reset_fixture();
    driver(four(S_ASPHALT), 20, 0); impact_feedback_tick(&api); pump();   // mixer clock starts
    fixture_ms += 16; driver(four(S_ASPHALT), 20, 3); impact_feedback_tick(&api); pump();
    CHECK(motor() == 32767 && high_motor() == 32767);  // truncated like the console
    fixture_ms += 16; driver(four(S_ASPHALT), 20, 3); impact_feedback_tick(&api); pump();
    CHECK(motor() == 0);                               // same value again: cleared (823a4c84)
    fixture_ms += 16; driver(four(S_ASPHALT), 20, 5); impact_feedback_tick(&api); pump();
    CHECK(motor() == 65535);
    fixture_ms += 16; driver(four(S_ASPHALT), 20, 4); impact_feedback_tick(&api); pump();
    CHECK(motor() == 0);                               // a fall never pulses
    // X12. Pause: output 0 and Update calls ignored; Resume restores; Reset zeroes.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(300, 60, four(S_DIRT), 30, lo, hi);
    CHECK(motor());
    fb(x360_pause, {}); fb(x360_nos, {1, fbits(1)}); impact_feedback_tick(&api); pump();
    CHECK(motor() == 0 && g_x360.nos == 0);
    fb(x360_resume, {}); fb(x360_begin, {});
    for (int i = 0; i < 4; ++i) fb(x360_road, {0, S_DIRT, fbits(30)});
    fixture_ms += 16; impact_feedback_tick(&api); pump();
    CHECK(motor());
    fb(x360_reset, {}); fixture_ms += 16; impact_feedback_tick(&api); pump();
    CHECK(motor() == 0 && !g_x360.enabled);
    // X13. Calls on another object (not vtable 008a7bd4) are ignored.
    x360_reset_fixture();
    mem[FB + 0x40] = 0x008a7b9cu;
    fb(x360_resume, {}, FB + 0x40); CHECK(!g_x360.enabled);
    // X14. Collision (Development 50) is kept and max-mixed on both motors.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(200, 60, four(S_ASPHALT, 25), 10, lo, hi);
    impact(8000); impact_feedback_tick(&api); pump();
    CHECK(motor() >= 52428 && high_motor() == 52428);
    // X15. surface_rumble scales the 360 model; 0 turns it off but not the collision pulse.
    x360_reset_fixture(); settings["surface_rumble"] = 50; lo.clear(); hi.clear();
    run(1000, 60, four(S_ASPHALT), 0, lo, hi, 0, 0, 1);
    CHECK(lo[20] > 0.49 * 65535 && lo[20] <= 32768);
    x360_reset_fixture(); settings["surface_rumble"] = 0; lo.clear(); hi.clear();
    run(300, 60, four(S_DIRT, 25), 30, lo, hi, 0, 1);
    CHECK(vmax(lo) == 0 && vmax(hi) == 0);
    impact(10000); impact_feedback_tick(&api); pump(); CHECK(motor() == 65535);
    // X15b. Hooks refused (older executable): the PC wheel adaptation remains in effect.
    x360_reset_fixture(); g_x360_hooked = 0;
    road(1, 10000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 65535);
    g_x360_hooked = 1;
    // X16. rumble_model 0 ignores the IFeedback hooks; model 1 ignores the PC wheel bridge.
    x360_reset_fixture(); settings["rumble_model"] = 0; lo.clear(); hi.clear();
    run(300, 60, four(S_DIRT, 25), 30, lo, hi);
    CHECK(vmax(lo) == 0 && !g_x360.enabled);
    x360_reset_fixture();
    road(1, 10000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 0);
    // X17. Driver lapse (no feedback calls for 250 ms while enabled): silent; stall: host expiry.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(300, 60, four(S_DIRT), 30, lo, hi);
    for (int i = 0; i < 20; ++i) { fixture_ms += 16; impact_feedback_tick(&api); pump(); }
    CHECK(motor() == 0);
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(300, 60, four(S_DIRT), 30, lo, hi);
    for (int i = 0; i < 70; ++i) { fixture_ms += 1; pump(); }   // no ticks at all
    CHECK(motor() == 0);
    // X18. Game paused / front end (impact_feedback_tick's gate) stops it, and no mixer
    //      time passes meanwhile: a nitrous decay resumes where it was.
    x360_reset_fixture(); lo.clear(); hi.clear();
    run(1000, 60, four(S_ASPHALT), 30, lo, hi, 0, 1);
    const float left_before = g_x360.decay[4].left;
    mem[W + 0x4460] = 1;
    for (int i = 0; i < 60; ++i) { fixture_ms += 16; fb(x360_pause, {}); impact_feedback_tick(&api); pump(); }
    CHECK(motor() == 0);
    mem[W + 0x4460] = 0; lo.clear(); hi.clear();
    run(50, 60, four(S_ASPHALT), 30, lo, hi, 0, 1);
    CHECK(g_x360.decay[4].left > left_before - 0.1f && motor() > 0.7 * 65535);
    // X19. Frame rate independence: 40 and 120 fps give the same 1 Hz wheelspin.
    for (double fps : {40.0, 120.0}) {
        x360_reset_fixture(); lo.clear(); hi.clear();
        run(3000, fps, four(S_ASPHALT, 25), 10, lo, hi);
        int r = 0; for (size_t i = 1; i < lo.size(); ++i) r += lo[i - 1] < 32768 && lo[i] >= 32768;
        CHECK(r >= 2 && r <= 4 && vmax(lo, 100) > 0.95 * 65535);
    }
    // X20. The sim driver wrapper (006ee490): the PC device interface ([this+0x5c]) is hidden
    //      for the IFeedback path, then given its frame as the original would, after the gate.
    {
        static const uint32_t SELF = 0x07000000u, DEV = 0x07100000u, DVT = 0x07200000u;
        static int gate, active, device_updates, original_device_path;
        static uint32_t update_arg;
        api.guest_write_u32 = [](const PopModApi *, uint32_t a, uint32_t v) { mem[a] = v; return POP_OK; };
        api.call_original = [](const PopModApi *, uint32_t target, pop_cpu_v1 *cpu) {
            if (target != 0x006ee490u) return POP_OK;
            const uint32_t self = cpu->ecx;            // the original driver, as disassembled
            if (!get_u32(self + 0x58)) return POP_OK;
            pop_cpu_v1 g{}; g.eax = (uint32_t)gate; x360_gate(&api, &g, nullptr, nullptr);
            if (!gate) { fb(x360_pause, {}); return POP_OK; }
            const uint32_t dev = get_u32(self + 0x5c);
            if (dev && active) { ++original_device_path; ++device_updates; update_arg = self + 0x4c; return POP_OK; }
            driver(four(S_DIRT), 30);
            return POP_OK;
        };
        api.guest_call = [](const PopModApi *, uint32_t addr, uint32_t ecx, const uint32_t *args, uint32_t n, uint32_t *out) {
            if (addr == 0x07300000u) { *out = ecx == DEV ? (uint32_t)active : 0; return POP_OK; }
            if (addr == 0x07300010u) { ++device_updates; update_arg = n ? args[0] : 0; *out = 0; return POP_OK; }
            if (addr != 0x00454810u || n != 2) return POP_E_RANGE;
            auto it = graphs.find({ecx, ((uint64_t)args[0] << 8) | args[1]});
            *out = it == graphs.end() ? 0 : it->second;
            return POP_OK;
        };
        auto setup = [&]() {
            x360_reset_fixture();
            mem[SELF + 0x58] = FB; mem[SELF + 0x5c] = DEV; mem[DEV] = DVT;
            mem[DVT + 0xc] = 0x07300000u; mem[DVT + 4] = 0x07300010u;
            gate = 1; active = 1; device_updates = 0; original_device_path = 0; update_arg = 0;
        };
        auto call_driver = [&]() { pop_cpu_v1 cpu{}; cpu.ecx = SELF; cpu.target = 0x006ee490u; x360_driver(&api, &cpu, nullptr, nullptr); };
        setup(); call_driver();
        CHECK(g_x360.driver_calls == 1 && g_x360.enabled && device_updates == 1 && !original_device_path);
        CHECK(update_arg == SELF + 0x4c && get_u32(SELF + 0x5c) == DEV);
        fixture_ms += 16; impact_feedback_tick(&api); fixture_ms += 16; call_driver(); impact_feedback_tick(&api); pump();
        CHECK(motor() > 0.3 * 65535);                  // dirt at 30 m/s reaches the pad
        setup(); gate = 0; call_driver();               // gate refused: paused, no device frame
        CHECK(!g_x360.enabled && device_updates == 0 && get_u32(SELF + 0x5c) == DEV);
        setup(); active = 0; call_driver();             // a device that declines: IFeedback only
        CHECK(g_x360.driver_calls == 1 && device_updates == 0);
        setup(); settings["rumble_model"] = 0; call_driver();   // PC model: the original, untouched
        CHECK(original_device_path == 1 && device_updates == 1 && g_x360.driver_calls == 0);
        setup(); mem[SELF + 0x5c] = 0; call_driver();   // no device interface: the original IFeedback path
        CHECK(g_x360.driver_calls == 1 && device_updates == 0);
        api.call_original = [](const PopModApi *, uint32_t, pop_cpu_v1 *) { return POP_OK; };
        api.guest_write_u32 = nullptr;
    }
    // ---- Test61: IFeedback::ReportCollision (360 823a4f30, PC stub 00628930) ----
    // X21. The 360 arithmetic, case by case (types: 1 OBJECT, 2 WORLD, 3 GROUND).
    {
        const float z[3] = {0, 0, 0}, fwd[3] = {20, 0, 0}, fwd2[3] = {15, 1, 0}, back[3] = {-20, 0, 0};
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        CHECK(near(x360_collision_strength(3, 1.0f, 0, 0, 0, z, z), 0.0f));
        CHECK(near(x360_collision_strength(3, 2.5f, 0, 0, 0, z, z), 0.5f));
        CHECK(near(x360_collision_strength(3, 9.0f, 0, 0, 0, z, z), 1.0f));
        CHECK(x360_collision_strength(3, 0.5f, 0, 0, 0, z, z) < 0.0f);   // below: no effect (m <= 0)
        CHECK(near(x360_collision_strength(2, 15.0f, 0, 0, 0, z, z), 0.5f));
        CHECK(near(x360_collision_strength(2, 90.0f, 0, 0, 0, z, z), 1.0f));
        CHECK(near(x360_collision_strength(1, 5.0f, 0, 0, 0, z, z), 0.0f));
        CHECK(near(x360_collision_strength(1, 10.0f, 0, 0, 0, z, z), 0.25f));
        CHECK(near(x360_collision_strength(1, 40.0f, 0, 0, 0, z, z), 1.0f));
        // Glancing contact with a car going the same way: half the +0x50 speed stands in.
        CHECK(near(x360_collision_strength(1, 1.0f, 30, 3, 1, fwd, fwd2), 0.25f));
        // ...opposite directions: the own-speed ramp (20..50) instead, if both differences > 2.
        CHECK(near(x360_collision_strength(1, 1.0f, 35, 3, 1, fwd, back), 0.5f));
        CHECK(near(x360_collision_strength(1, 1.0f, 35, 1.5f, 1, fwd, back), 0.0f));
        // A prop (not a vehicle) pushed at speed: own-speed ramp; at rest or slow: nothing.
        CHECK(near(x360_collision_strength(1, 0.0f, 65, 3, 0, fwd, fwd), 1.0f));
        CHECK(near(x360_collision_strength(1, 0.0f, 15, 3, 0, fwd, fwd), 0.0f));
        CHECK(near(x360_collision_strength(1, 0.0f, 35, 0, 0, fwd, fwd), 0.0f));  // no other object
        CHECK(near(x360_collision_strength(0, 50.0f, 50, 50, 1, fwd, fwd), 0.0f));
        CHECK(near(x360_collision_strength(4, 50.0f, 50, 50, 1, fwd, fwd), 0.0f));
    }
    // X22. Through the hook and the real output chain: a guest with a simable table, velocity
    //      vfuncs and the IVehicle query; decay 0 on the LEFT motor only, m high for m seconds.
    {
        static const uint32_t CI = 0x08000000u, TBL = 0x08100000u, OWN = 0x08200000u, OTH = 0x08300000u,
                              VT = 0x08400000u, VEL = 0x08500000u, SCR = 0x08600000u;
        static float own_v[3], oth_v[3]; static int oth_vehicle;
        api.guest_call = [](const PopModApi *, uint32_t addr, uint32_t ecx, const uint32_t *args, uint32_t n, uint32_t *out) {
            if (addr == 0x005d51f0u && n == 3) {            // FindInstance(begin, end, handle)
                *out = args[2] == 0x11u ? TBL : args[2] == 0x22u ? TBL + 8 : 0; return POP_OK; }
            if (addr == VEL && n == 1) {                     // ISimable::GetLinearVelocity(out)
                const float *v = ecx == OWN ? own_v : oth_v;
                for (int i = 0; i < 3; ++i) mem[args[0] + 4u * (uint32_t)i] = fbits(v[i]);
                return POP_OK; }
            if (addr == 0x005d59f0u && n == 1) {             // QueryInterface<IVehicle>
                *out = (ecx == OTH + 0x40 && args[0] == 0x004040e0u && oth_vehicle) ? 0x0900 : 0; return POP_OK; }
            if (out) *out = 0;
            return POP_OK;
        };
        auto setup = [&](int type, float closing_x, int other) {
            x360_reset_fixture(); g_x360_scratch = SCR; g_x360_collision_hooked = 1;
            mem[0x0092feb4u] = TBL; mem[0x0092febcu] = 2;
            mem[TBL + 4] = OWN; mem[TBL + 12] = OTH;
            mem[OWN] = VT; mem[OTH] = VT; mem[VT + 0x68] = VEL; mem[VT + 4] = 0; mem[OTH + 4] = OTH + 0x40;
            for (uint32_t i = 0; i < 0x80; i += 4) mem[CI + i] = 0;
            mem[CI + 0x1c] = (uint32_t)type | 0x80u;          // sliding bit set, as seen in the game
            mem[CI + 0x3c] = 0x11u; mem[CI + 0x4c] = other ? 0x22u : 0u;
            own_v[0] = 30; own_v[1] = own_v[2] = 0;          // now
            mem[CI + 0x50] = fbits(30 + closing_x);          // objAVel recorded
            oth_v[0] = oth_v[1] = oth_v[2] = 0; oth_vehicle = 1;
            fb(x360_resume, {});                             // the player is driving
            settings["collision_rumble_fade"] = 100;          // the console's fade (Test132 default is 50)
        };
        auto report = [&](int iama = 1) { fb(x360_collision, {CI, (uint32_t)iama | 0xab00u}); };
        std::vector<uint16_t> lo, hi;
        // A hard car hit: dv 25 m/s -> m = 1.0 for 1 s, left motor only, then silence.
        setup(1, 25, 1); run(50, 60, four(S_ASPHALT), 30, lo, hi); report();
        CHECK(std::fabs(g_x360.decay[0].start - 1.0f) < 1e-5f && std::fabs(g_x360.decay[0].left - 1.0f) < 1e-5f);
        lo.clear(); hi.clear(); run(60, 60, four(S_ASPHALT), 30, lo, hi);
        CHECK(vmax(lo) > 0.85 * 65535 && vmax(hi) == 0);
        lo.clear(); hi.clear(); run(1100, 60, four(S_ASPHALT), 30, lo, hi);
        CHECK(lo.back() == 0 && hi.back() == 0);           // separated: the thump has ended
        CHECK(vmax(lo, 900) < 0.2 * 65535);                // falling linearly towards 0
        // A weaker contact (dv 9 -> 0.2) lasts 0.2 s; repeated contact extends by maximum only.
        setup(1, 9, 1); report(); CHECK(std::fabs(g_x360.decay[0].start - 0.2f) < 1e-4f);
        report(); report(); CHECK(std::fabs(g_x360.decay[0].left - 0.2f) < 1e-4f);  // no accumulation
        lo.clear(); hi.clear(); run(400, 60, four(S_ASPHALT), 30, lo, hi);
        CHECK(lo.back() == 0);
        // Police/traffic brushing past in the same direction (dv 1): 0.5 x |objAVel| stands in.
        setup(1, 1, 1); mem[CI + 0x60] = fbits(25); mem[CI + 0x64] = fbits(1); report();
        CHECK(std::fabs(g_x360.decay[0].start - 0.525f) < 1e-3f);
        setup(1, 1, 1); report();                          // objBVel zero: not "same way" -> ramp
        CHECK(std::fabs(g_x360.decay[0].start - 0.33333f) < 1e-3f);
        // A smackable prop hit at 30 m/s: not a vehicle; own-speed ramp (30-20)/30.
        setup(1, 0.5f, 1); oth_vehicle = 0; oth_v[0] = 5; report();
        CHECK(std::fabs(g_x360.decay[0].start - 0.33333f) < 1e-3f);
        // World (wall): dv 15 -> 0.5; ground: dv 2.5 -> 0.5; iamA=0 uses objB's recorded velocity.
        setup(2, 15, 0); report(); CHECK(std::fabs(g_x360.decay[0].start - 0.5f) < 1e-4f);
        setup(3, 0, 0); mem[CI + 0x4c] = 0x11u; mem[CI + 0x3c] = 0; mem[CI + 0x60] = fbits(32.5f); report(0);
        CHECK(std::fabs(g_x360.decay[0].start - 0.5f) < 1e-4f);
        // Paused (AI control, menus): nothing; unknown own object: nothing.
        setup(1, 25, 1); fb(x360_pause, {}); report(); CHECK(g_x360.decay[0].left == 0.0f);
        setup(1, 25, 1); mem[CI + 0x3c] = 0x99u; report(); CHECK(g_x360.decay[0].left == 0.0f);
        // Reset clears an active thump (360 Reset zeroes decay 0).
        setup(1, 25, 1); report(); fb(x360_reset, {}); CHECK(g_x360.decay[0].left == 0.0f);
        // Another IFeedback (not the PC implementation) is left alone.
        setup(1, 25, 1); fb(x360_collision, {CI, 1}, FB + 0x40); CHECK(g_x360.decay[0].left == 0.0f);
        // X24. The two strength settings stay independent in the 360 model: impact_rumble
        //      scales only the collision decay, surface_rumble only the driving sources.
        setup(1, 25, 1); settings["impact_rumble"] = 0; report();
        lo.clear(); hi.clear(); run(60, 60, four(S_ASPHALT), 30, lo, hi);
        CHECK(g_x360.left > 0.85f && vmax(lo) == 0);        // mixer unchanged; output off
        setup(1, 25, 1); settings["surface_rumble"] = 0; report();
        lo.clear(); hi.clear(); run(60, 60, four(S_ASPHALT), 30, lo, hi);
        CHECK(vmax(lo) > 0.85 * 65535);                      // driving off; collision kept
        setup(1, 25, 1); settings["impact_rumble"] = 50; report();
        lo.clear(); hi.clear(); run(60, 60, four(S_ASPHALT), 30, lo, hi);
        CHECK(vmax(lo) > 0.42 * 65535 && vmax(lo) <= 0.5 * 65535 + 1);
        // X25 (Test132). collision_rumble_fade 50: same strength, half the fade time.
        setup(1, 25, 1); settings["collision_rumble_fade"] = 50; report();
        CHECK(std::fabs(g_x360.decay[0].start - 1.0f) < 1e-5f && std::fabs(g_x360.decay[0].left - 0.5f) < 1e-5f);
        settings.erase("collision_rumble_fade");
        g_x360_scratch = 0; g_x360_collision_hooked = 0;
    }
    // X23. The PC wheel impact heuristic: not used when the 360 collision report is hooked;
    //      still used if that hook was refused, and always in rumble_model 0.
    {
        std::vector<uint16_t> lo, hi;
        x360_reset_fixture(); g_x360_collision_hooked = 1;
        run(50, 60, four(S_ASPHALT), 30, lo, hi); impact(9000); impact_feedback_tick(&api); pump();
        CHECK(motor() == 0 && high_motor() == 0);
        x360_reset_fixture(); g_x360_collision_hooked = 0;
        run(50, 60, four(S_ASPHALT), 30, lo, hi); impact(9000); impact_feedback_tick(&api); pump();
        CHECK(motor() > 0.85 * 65535 && high_motor() > 0.85 * 65535);
        x360_reset_fixture(); g_x360_collision_hooked = 1; settings["rumble_model"] = 0;
        impact(9000); impact_feedback_tick(&api); pump();
        CHECK(motor() > 0.85 * 65535);
        g_x360_collision_hooked = 0;
    }
    // X25. PORT ENHANCEMENT paved_surface_rumble (Test69): on a PC-rumbling paved surface (layout
    //      +0xfa intensity, e.g. cobble 120) with no 360 RoadNoise graph, the PC wheel effect of the
    //      same frame drives the left motor; never on a RoadNoise surface, when off, paused or lapsed.
    {
        static const uint32_t S_COBBLE = 0x06003000u, L_COBBLE = 0x06003800u, L_DIRT = 0x06004800u,
                              L_ASPHALT = 0x06005800u;
        api.guest_call = [](const PopModApi *, uint32_t addr, uint32_t ecx, const uint32_t *args, uint32_t n, uint32_t *out) {
            if (addr != 0x00454810u || n != 2) return POP_E_RANGE;   // GetAttributePointer (X22 replaced it)
            auto it = graphs.find({ecx, ((uint64_t)args[0] << 8) | args[1]});
            *out = it == graphs.end() ? 0 : it->second;
            return POP_OK;
        };
        auto fixture = [&] {
            x360_reset_fixture();
            put_record(S_COBBLE, 0x8fd11d27u, 0, S_COBBLE + 0x100, {0, 0, 0, 0.5f, 1, 0.5f, 0.5f, 20});
            mem[S_COBBLE + 8] = L_COBBLE; mem[L_COBBLE + 0xf8] = (120u << 16) | 100u;
            mem[S_DIRT + 8] = L_DIRT; mem[L_DIRT + 0xf8] = (120u << 16) | 100u;
            mem[S_ASPHALT + 8] = L_ASPHALT; mem[L_ASPHALT + 0xf8] = 0;
        };
        auto go = [&](uint32_t ms, const std::vector<Wheel> &w, float speed, int32_t pc_mag, bool pc_calls,
                      std::vector<uint16_t> &lo, std::vector<uint16_t> &hi) {
            const uint64_t end = fixture_ms + ms;
            double next = (double)fixture_ms;
            while (fixture_ms < end) {
                if ((double)fixture_ms >= next) {
                    driver(w, speed);
                    if (pc_calls) road(2, pc_mag, 100);   // 006dcda0 in the same frame (triangle)
                    impact_feedback_tick(&api);
                    next += 1000.0 / 60.0;
                }
                pump();
                lo.push_back(cmds.empty() ? 0 : cmds.back().low);
                hi.push_back(cmds.empty() ? 0 : cmds.back().high);
                fixture_ms += 1;
            }
        };
        std::vector<uint16_t> lo, hi;
        fixture(); go(2000, four(S_COBBLE), 20, 4000, true, lo, hi);
        printf("paved cobble (PC 0.4 triangle 100 ms): low %u..%u, high max %u\n", vmin(lo, 100), vmax(lo, 100), vmax(hi));
        CHECK(vmax(lo, 100) > 0.36 * 65535 && vmax(lo, 100) <= 0.4 * 65535 + 1 && vmin(lo, 100) < 0.08 * 65535 && vmax(hi) == 0);
        // A wheel on a 360 RoadNoise surface: the console's road noise alone (dirt at 3 m/s < 0.08).
        fixture(); lo.clear(); hi.clear();
        go(1500, {{S_COBBLE, 0, 0}, {S_COBBLE, 0, 0}, {S_DIRT, 0, 0}, {S_COBBLE, 0, 0}}, 3, 10000, true, lo, hi);
        printf("paved with a dirt wheel at 3 m/s: low %u..%u\n", vmin(lo, 100), vmax(lo, 100));
        CHECK(vmax(lo, 100) > 0 && vmax(lo, 100) < 0.08 * 65535);
        // Asphalt: PC intensity 0, so nothing even if a PC magnitude were reported.
        fixture(); lo.clear(); hi.clear();
        go(1000, four(S_ASPHALT), 20, 4000, true, lo, hi);
        CHECK(vmax(lo) == 0 && vmax(hi) == 0);
        // Setting off: the 360 behaviour (nothing on cobble).
        fixture(); settings["paved_surface_rumble"] = 0; lo.clear(); hi.clear();
        go(1000, four(S_COBBLE), 20, 4000, true, lo, hi);
        CHECK(vmax(lo) == 0 && vmax(hi) == 0);
        // surface_rumble 50 halves it.
        fixture(); settings["surface_rumble"] = 50; lo.clear(); hi.clear();
        go(2000, four(S_COBBLE), 20, 4000, true, lo, hi);
        CHECK(vmax(lo, 100) > 0.18 * 65535 && vmax(lo, 100) <= 0.2 * 65535 + 1);
        // PC calls stop (airborne, a stall): silent within 100 ms.
        fixture(); lo.clear(); hi.clear();
        go(500, four(S_COBBLE), 20, 4000, true, lo, hi);
        lo.clear(); hi.clear(); go(300, four(S_COBBLE), 20, 4000, false, lo, hi);
        CHECK(vmax(lo, 150) == 0);
        // Paused (the tick's gate): nothing.
        fixture(); mem[W + 0x4460] = 1; lo.clear(); hi.clear();
        go(500, four(S_COBBLE), 20, 4000, true, lo, hi);
        CHECK(vmax(lo) == 0 && vmax(hi) == 0);
        // Wheelspin on cobble keeps the console's TireSlip (max-merged, never reduced).
        fixture(); lo.clear(); hi.clear();
        run(1500, 60, four(S_COBBLE, 25), 10, lo, hi);
        printf("wheelspin on cobble: low %u..%u\n", vmin(lo, 100), vmax(lo, 100));
        CHECK(vmax(lo, 100) > 0.97 * 65535);
    }
    api.guest_call = nullptr;
}

int main() {
    g_feedback_clock = fixture_clock;
    api.size = sizeof api; api.mod_index = 7;
    api.log = [](const PopModApi *, const char *) { return POP_OK; };
    api.call_original = [](const PopModApi *, uint32_t, pop_cpu_v1 *) { return POP_OK; };
    api.rumble_pulse = [](const PopModApi *a, uint16_t l, uint16_t h, uint32_t ms) {
        return pad.rumble_pulse(a->mod_index, l, h, ms, fixture_ms * 1000000ull) ? POP_OK : POP_E_RANGE; };
    g_api = &api;
    fixture_ms = 1000000;

    // 1. Same-source downward step: 1.0 then 0.1 after 10 ms reaches the motor at once.
    reset();
    road(1, 10000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 65535 && cmds.back().low == cmds.back().high);
    fixture_ms += 10; road(1, 1000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 6554);
    // ...while an independent 0.8 impact still dominates, then yields when it ends.
    reset();
    road(1, 10000, 150); impact(8000); impact_feedback_tick(&api); pump();
    CHECK(motor() == 65535);
    fixture_ms += 10; road(1, 1000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 52428);
    fixture_ms += 130; road(1, 1000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 52428);                          // impact lasts 150 ms
    fixture_ms += 15; road(1, 1000, 150); impact_feedback_tick(&api); pump();
    CHECK(motor() == 6554 || motor() == 0);          // square may be in its low half
    // A stronger surface beats a weaker impact; the impact does not cancel it.
    reset();
    road(1, 10000, 150); impact(2000); impact_feedback_tick(&api); pump();
    CHECK(motor() == 65535);

    // 2. 100 ms synthetic sine at 60 Hz (the Test57 fixture): valleys survive.
    reset();
    auto s = drive(2000, 60, 0, 10000, 100);
    double m = mean(s, 200); uint16_t lo = 65535, hi = 0;
    for (size_t i = 200; i < s.size(); ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
    printf("sine100@60: mean=%.3f min=%.3f max=%.3f cmds=%zu/2s\n", m, lo / 65535.0, hi / 65535.0, cmds.size());
    CHECK(m > 0.40 && m < 0.60);
    CHECK(lo < 0.10 * 65535 && hi > 0.90 * 65535);

    // 3. Cobble's recorded 120 ms period is 8.33 Hz: count rising half-level crossings.
    reset();
    s = drive(2400, 60, 0, 10000, 120);
    int rises = 0; for (size_t i = 1; i < s.size(); ++i) rises += s[i - 1] < 32768 && s[i] >= 32768;
    printf("sine120@60: rising crossings in 2.4 s = %d (20 cycles; the first starts on the midline)\n", rises);
    CHECK(rises >= 19 && rises <= 21);

    // 4. Phase continuity: a period change keeps the running phase; a waveform change restarts it.
    reset();
    drive(50, 60, 0, 10000, 120);
    float before = g_road.phase; uint64_t t0 = g_road.phase_ms;
    fixture_ms += 16; road(0, 10000, 60); impact_feedback_tick(&api);
    float expect = before + (float)(fixture_ms - t0) / 60.0f; expect -= floorf(expect);
    CHECK(std::fabs(g_road.phase - expect) < 1e-4f);
    fixture_ms += 16; road(2, 10000, 60); impact_feedback_tick(&api);
    CHECK(g_road.phase == 0.0f);

    // 5. All five waveforms; indices without a DirectInput GUID give nothing.
    CHECK(std::fabs(road_wave(3, 0.25f) - 0.25f) < 1e-6f && std::fabs(road_wave(4, 0.25f) - 0.75f) < 1e-6f);
    CHECK(road_wave(1, 0.49f) == 1.0f && road_wave(1, 0.51f) == 0.0f);
    CHECK(std::fabs(road_wave(2, 0.5f) - 1.0f) < 1e-6f);
    reset(); s = drive(300, 60, 5, 10000, 100); CHECK(motor() == 0);
    reset(); s = drive(300, 60, 3, 10000, 100); CHECK(mean(s, 50) > 0.35 && mean(s, 50) < 0.65);

    // 5b. Sampling rule: periods under 50 ms (grass/sand: 40) run at the steady mean level;
    //     60 ms (gravel) is drawn as its waveform.
    reset(); s = drive(500, 60, 0, 10000, 40);
    lo = 65535; hi = 0; for (size_t i = 100; i < s.size(); ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
    CHECK(lo == 32768 && hi == 32768);
    reset(); s = drive(500, 60, 0, 10000, 60);
    lo = 65535; hi = 0; for (size_t i = 100; i < s.size(); ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
    CHECK(lo < 0.2 * 65535 && hi > 0.8 * 65535);

    // 6. Parked (magnitude 0), asphalt (0) and the air (no calls): nothing, or a prompt stop.
    reset(); s = drive(300, 60, 0, 0, 100); CHECK(motor() == 0 && cmds.empty());
    reset(); drive(200, 60, 0, 10000, 100);
    s = drive(300, 60, 0, 0, 0);                      // onto asphalt
    CHECK(motor() == 0);
    reset(); drive(200, 60, 0, 10000, 100);
    const uint64_t air = fixture_ms;
    s = drive(300, 60, 0, 10000, 100, false, true);   // airborne: ticks, no calls
    size_t stop_at = 0; for (size_t i = 0; i < s.size(); ++i) if (!s[i]) { stop_at = i; break; }
    printf("airborne: motor stops %zu ms after the last call\n", stop_at);
    CHECK(motor() == 0 && stop_at <= 100 + 17);
    (void)air;

    // 7. Guest stall: no calls and no ticks; the host clock still stops the motor.
    reset(); drive(200, 60, 0, 10000, 100);
    s = drive(300, 60, 0, 10000, 100, false, false);
    stop_at = 0; for (size_t i = 0; i < s.size(); ++i) if (!s[i]) { stop_at = i; break; }
    printf("stall: motor stops %zu ms after the last tick\n", stop_at);
    CHECK(motor() == 0 && stop_at <= 60 + 17);

    // 8. surface_rumble: 0 disables only the surface; values over 200 % are clamped.
    reset(); settings["surface_rumble"] = 0;
    s = drive(200, 60, 1, 10000, 150); CHECK(motor() == 0);
    impact(10000); impact_feedback_tick(&api); pump(); CHECK(motor() == 65535);
    reset(); settings["surface_rumble"] = 1000;
    road(1, 2500, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 32768);

    // 9. Pause, menus/front end, other devices and foreign wrappers.
    reset(); drive(50, 60, 1, 10000, 150); CHECK(motor());
    mem[W + 0x4460] = 1; impact_feedback_tick(&api); pump(); CHECK(motor() == 0);
    reset(); drive(50, 60, 1, 10000, 150);
    mem[0x00925e90u] = 3; impact_feedback_tick(&api); pump(); CHECK(motor() == 0);
    reset(); road(1, 10000, 150, 1); impact_feedback_tick(&api); pump(); CHECK(motor() == 0);
    reset(); road(1, 10000, 150, 0, W + 4); impact_feedback_tick(&api); pump(); CHECK(motor() == 0);

    // 10. Invalid values: negative magnitude, oversize magnitude, period 0 and oversize period.
    reset(); road(1, -500, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 0);
    reset(); road(1, 50000, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 65535);
    reset(); road(0, 10000, 0); impact_feedback_tick(&api); pump(); CHECK(motor() == 32768);
    reset(); road(0, 10000, 900); CHECK(g_road.period == 150);

    // 11. Disconnect clears the pulse at once; reconnect resumes only from new calls.
    reset(); drive(50, 60, 1, 10000, 150); CHECK(motor());
    connected = false; pad.set_controller_connected(false); pump(); CHECK(motor() == 0);
    connected = true; pad.set_controller_connected(true); pump(); CHECK(motor() == 0);
    drive(20, 60, 1, 10000, 150); CHECK(motor());

    // 12. Unload stops the owner's pulse; another owner's pulse is untouched.
    reset(); drive(50, 60, 1, 10000, 150);
    pad.rumble_pulse(3, 1000, 1000, 500, fixture_ms * 1000000ull);
    impact_feedback_stop(&api); pump(); CHECK(motor() == 1000);
    pad.rumble_pulse(3, 0, 0, 0, fixture_ms * 1000000ull); pump(); CHECK(motor() == 0);

    // 13. 40/60/120 fps and a 300 ms guest hitch: same shape, bounded stop.
    for (double fps : {40.0, 60.0, 120.0}) {
        reset();
        s = drive(2400, fps, 0, 10000, 120);
        int r = 0; for (size_t i = 1; i < s.size(); ++i) r += s[i - 1] < 32768 && s[i] >= 32768;
        printf("sine120@%.0f: crossings=%d mean=%.3f cmds=%zu/2.4s\n", fps, r, mean(s, 200), cmds.size());
        CHECK(r >= 18 && r <= 22);
        CHECK(mean(s, 200) > 0.40 && mean(s, 200) < 0.60);
    }
    reset(); drive(200, 60, 1, 10000, 150);
    s = drive(300, 60, 1, 10000, 150, false, false);  // hitch
    CHECK(motor() == 0);
    s = drive(50, 60, 1, 10000, 150);                // recovers from fresh calls
    CHECK(motor());                                   // phase restarted: square's high half

    x360_tests();

    // 14. Test156 rumble_floor: a level under the cut-off is not played; one at or above it is unchanged.
    reset(); settings["rumble_floor"] = 3;
    road(1, 250, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 0);      // 2.5 % < 3 %
    reset(); settings["rumble_floor"] = 3;
    road(1, 400, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 2621);   // 4 % played as computed
    reset(); settings.erase("rumble_floor");                                        // the default is 1 %
    road(1, 50, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 0);       // 0.5 % < 1 %
    reset(); settings.erase("rumble_floor");
    road(1, 250, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 1638);   // 2.5 % >= 1 %
    reset(); settings["rumble_floor"] = 0;
    road(1, 250, 150); impact_feedback_tick(&api); pump(); CHECK(motor() == 1638);   // 0: every level
    printf("rumble feedback: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
