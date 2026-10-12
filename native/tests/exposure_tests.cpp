// x360_exposure.h: the retail adaptation contract (Test64/Test67) and the GPU half's tables (Test68);
// x360_look.h: the Xbox 360 look's per-frame values (Test77). No game data (synthetic layouts).
#include "../../mods/core/nfsmw/x360_exposure.h"
#include "../../mods/core/nfsmw/x360_look.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <vector>
static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    X360Exposure e;
    x360x_init(&e);
    CHECK(e.adapted == 0.13f && e.reset == 1);
    // measurement: sum of bytes x 2^-20; all 128 -> 0.5; all 255 -> 255/256
    std::vector<uint8_t> t(4096, 128);
    CHECK(x360x_measure_bytes(t.data(), 4096) == 0.5f);
    std::fill(t.begin(), t.end(), 255);
    CHECK(x360x_measure_bytes(t.data(), 4096) == 255.0f / 256.0f);
    // first update snaps to M (boot reset), then clears the reset
    CHECK(x360x_update(&e, 1, 0.40f) && e.adapted == 0.40f && e.reset == 0);
    // repeated display frame: no second step
    CHECK(!x360x_update(&e, 1, 0.08f) && e.adapted == 0.40f && e.updates == 1);
    CHECK(!x360x_update(&e, 0, 0.08f));
    // dark target: k = 0.015 (d <= 0), fused
    CHECK(x360x_update(&e, 2, 0.08f));
    CHECK(e.adapted == fmaf(0.015f, 0.08f - 0.40f, 0.40f));
    // bright target: k = 0.02
    float a = e.adapted;
    CHECK(x360x_update(&e, 3, 0.9f) && e.adapted == fmaf(0.02f, 0.9f - a, a));
    // equal: d = 0 takes the 0.015 branch and stays
    a = e.adapted;
    CHECK(x360x_update(&e, 4, a) && e.adapted == a);
    // floor: below 0.06 -> exactly 0.06 with no step that call
    X360Exposure f; x360x_init(&f);
    x360x_update(&f, 1, 0.01f);            // reset: A = 0.01
    CHECK(f.adapted == 0.01f);
    CHECK(x360x_update(&f, 2, 0.5f) && f.adapted == 0.06f);   // clamp, no step toward 0.5
    CHECK(x360x_update(&f, 3, 0.5f) && f.adapted == fmaf(0.02f, 0.5f - 0.06f, 0.06f));
    // dark scenes floor: repeated 0 converges to >= 0.06 - one step
    for (uint64_t n = 4; n < 2000; ++n) x360x_update(&f, n, 0.0f);
    CHECK(f.adapted >= 0.06f * (1.0f - 0.015f) - 1e-7f && f.adapted <= 0.0601f);
    // recurrence timing (Test64): 0.40 -> 0.08 half-way after 46 calls; 0.08 -> 0.40 after 35
    X360Exposure g; x360x_init(&g); x360x_update(&g, 1, 0.40f);
    int calls = 0; for (uint64_t n = 2; g.adapted > 0.24f; ++n, ++calls) x360x_update(&g, n, 0.08f);
    CHECK(calls == 46);
    X360Exposure h; x360x_init(&h); x360x_update(&h, 1, 0.08f);
    calls = 0; for (uint64_t n = 2; h.adapted < 0.24f; ++n, ++calls) x360x_update(&h, n, 0.40f);
    CHECK(calls == 35);
    // resource re-creation snaps again
    x360x_resources_recreated(&h);
    CHECK(x360x_update(&h, 1000, 0.2f) && h.adapted == 0.2f);
    // consumers
    CHECK(std::fabs(x360x_brightpass_scale(&h) - 0.14f / 0.201f) < 1e-6f);
    CHECK(std::fabs(x360x_adaptive_coeff(&h) - 0.28f / 0.201f) < 1e-6f);
    // ---- GPU-half tables (Test68), as the USA code computes them ----
    unsigned w, hh;
    x360x_sizes(1280, 720, &w, &hh);  CHECK(w == 1280 && hh == 720);
    x360x_sizes(3840, 2160, &w, &hh); CHECK(w == 1280 && hh == 720);   // resolution-independent
    x360x_sizes(1920, 1200, &w, &hh); CHECK(w == 1152 && hh == 720);
    x360x_sizes(2560, 1080, &w, &hh); CHECK(w == 1704 && hh == 720);   // rounded down to 8 as 82221b80
    float off[16][4], wt[16][4];
    x360x_offsets_downscale4x4(1280, 720, off);
    CHECK(off[0][0] == -1.5f / 1280 && off[0][1] == -1.5f / 720);
    CHECK(off[15][0] == 1.5f / 1280 && off[15][1] == 1.5f / 720);
    CHECK(off[6][0] == 0.5f / 1280 && off[6][1] == -0.5f / 720 && off[6][2] == 0 && off[6][3] == 0);
    float lum[9][4];
    x360x_offsets_luminance(lum);
    const float t192 = 1.0f / 192.0f;
    CHECK(std::fabs(lum[0][0] + t192) < 1e-7f && std::fabs(lum[0][1] + t192) < 1e-7f);
    CHECK(lum[4][0] == 0 && lum[4][1] == 0);
    CHECK(lum[5][0] == 0 && std::fabs(lum[5][1] - t192) < 1e-7f);
    CHECK(std::fabs(lum[8][0] - t192) < 1e-7f && std::fabs(lum[8][1] - t192) < 1e-7f);
    CHECK(x360x_gauss5x5(320, 180, off, wt) == 13);
    float sum = 0; for (int i = 0; i < 13; ++i) sum += wt[i][0];
    CHECK(std::fabs(sum - 1.0f) < 1e-6f);
    const double ring = 1 + 4 * std::exp(-0.5) + 4 * std::exp(-1.0) + 4 * std::exp(-2.0);
    CHECK(std::fabs(wt[6][0] - 1.0 / ring) < 1e-6 && off[6][0] == 0 && off[6][1] == 0);  // centre
    CHECK(off[0][0] == -2.0f / 320 && off[0][1] == 0 && wt[0][3] == wt[0][0]);
    CHECK(off[12][0] == 2.0f / 320 && wt[13][0] == 0 && off[13][0] == 0);
    x360x_bloom(160, 1, off, wt);
    const double w0 = 2.0 / std::sqrt(2 * M_PI * 9);
    CHECK(std::fabs(wt[0][0] - w0) < 1e-6 && wt[0][3] == 1.0f && off[0][0] == 0);
    CHECK(std::fabs(wt[7][0] - w0 * std::exp(-49.0 / 18)) < 1e-6);
    CHECK(off[3][0] == 3.0f / 160 && off[3][1] == 0 && off[10][0] == -3.0f / 160 && wt[10][0] == wt[3][0]);
    CHECK(wt[15][0] == 0 && wt[15][3] == 0 && wt[14][3] == 1.0f);
    sum = 0; for (int i = 0; i < 15; ++i) sum += wt[i][0];
    CHECK(sum > 1.9f && sum < 2.0f);   // multiplier 2 over a deviation-3 kernel cut at +-7
    x360x_bloom(90, 0, off, wt);
    CHECK(off[2][0] == 0 && off[2][1] == 2.0f / 90 && off[9][1] == -2.0f / 90);
    std::vector<uint8_t> img(64 * 64 * 4, 0);
    for (size_t i = 0; i < img.size(); i += 4) { img[i] = 128; img[i + 3] = 255; }
    CHECK(x360x_measure_argb(img.data(), 256, 64, 64) == 0.5f);   // alpha (255) is not read
    // ---- Test77: x360_look.h ----
    {
        std::vector<uint8_t> file(16 + 3 * 0xe0, 0);
        std::memcpy(file.data(), "X3VL", 4);
        const uint32_t head[3] = {1, 3, 0xe0};
        std::memcpy(file.data() + 4, head, sizeof head);
        float lay[3][56] = {};
        const float ys[3][4] = {{0.1f, 0.3f, 0.9f, 1.0f}, {0.2f, 0.5f, 1.1f, 1.0f}, {0.0f, 0.6f, 1.0f, 1.0f}};
        const float tint[3][3] = {{0.8f, 0.7f, 0.2f}, {0.6f, 0.8f, 0.9f}, {0.1f, 0.1f, 0.8f}};
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < 4; ++i) { lay[k][16 + 4 * i] = i / 3.0f; lay[k][16 + 4 * i + 1] = ys[k][i]; }
            for (int j = 0; j < 3; ++j) lay[k][48 + j] = tint[k][j];
            lay[k][51] = 1.0f; lay[k][52] = 1.5f + 0.1f * k; lay[k][53] = 0.5f - 0.1f * k; lay[k][55] = 0.8f + 0.1f * k;
        }
        std::memcpy(file.data() + 16, lay, sizeof lay);
        X3LookData d;
        CHECK(x3vl_parse(&d, file.data(), file.size()) && d.ready);
        CHECK(!x3vl_parse(&d, file.data(), file.size() - 1) && !d.ready);
        std::vector<uint8_t> bad = file; bad[0] = 'Y';
        CHECK(!x3vl_parse(&d, bad.data(), bad.size()));
        bad = file; const float big = 1e9f; std::memcpy(bad.data() + 16 + 4 * 7, &big, 4);
        CHECK(!x3vl_parse(&d, bad.data(), bad.size()));
        CHECK(x3vl_parse(&d, file.data(), file.size()));
        // weight from one blended field, clamped; flat field 0
        CHECK(x3look_weight(0.88f, 0.57f, 0.88f) == 0.0f && x3look_weight(0.88f, 0.57f, 0.57f) == 1.0f);
        CHECK(std::fabs(x3look_weight(0.88f, 0.57f, 0.725f) - 0.5f) < 1e-5f);
        CHECK(x3look_weight(0.88f, 0.57f, 0.95f) == 0.0f && x3look_weight(0.88f, 0.57f, 0.2f) == 1.0f);
        CHECK(x3look_weight(0.5f, 0.5f, 0.7f) == 0.0f);
        // modes: 1 CopCam, 2 Midday alone, else the blend
        X3VisualLook l;
        x3look_select(&d, 1, 0.3f, &l); CHECK(l.f[48] == tint[2][0]);
        x3look_select(&d, 2, 0.9f, &l); CHECK(l.f[48] == tint[0][0]);
        x3look_select(&d, 0, 0.25f, &l); CHECK(std::fabs(l.f[48] - (0.75f * 0.8f + 0.25f * 0.6f)) < 1e-6f);
        // the power-form curve equals the Bezier of the control points' y values, scaled as 82223aa0
        x3look_select(&d, 0, 0.0f, &l);
        float c[4][4];
        x3look_coeffs(&l, c);
        const float dd = l.f[55], ci = l.f[52];
        int ok = 1;
        for (int n = 0; n <= 20; ++n) {
            const float t = n / 20.0f, u = 1.0f - t;
            const float b = u * u * u * ys[0][0] + 3 * t * u * u * ys[0][1] + 3 * t * t * u * ys[0][2] + t * t * t * ys[0][3];
            if (std::fabs(x3look_curve(c, 0, t) - (b * dd + 1.0f - dd)) > 1e-5f) ok = 0;
            for (int j = 0; j < 3; ++j)
                if (std::fabs(x3look_curve(c, 1 + j, t) - b * ci * tint[0][j]) > 1e-5f) ok = 0;
        }
        CHECK(ok);
        CHECK(std::fabs(x3look_curve(c, 0, 0.0f) - (ys[0][0] * dd + 1.0f - dd)) < 1e-6f);
        // a flat curve at 1 with no intensity is the identity grade (q = (1, 0, 0, 0))
        X3VisualLook flat = l;
        for (int i = 0; i < 4; ++i) flat.f[16 + 4 * i + 1] = 1.0f;
        flat.f[55] = 0.0f; flat.f[52] = 0.0f;
        x3look_coeffs(&flat, c);
        CHECK(c[0][0] == 1.0f && c[1][0] == 0.0f && c[0][1] == 0.0f && x3look_curve(c, 0, 0.37f) == 1.0f);
        // CombinedBrightness = 2 x the bright-pass scale, x (1 + brightness)
        {
            X360Exposure e2; x360x_init(&e2);
            CHECK(x3look_combined(e2.adapted, 0.0f) == x360x_adaptive_coeff(&e2));
            CHECK(std::fabs(x3look_combined(0.279f, 0.5f) - 1.5f) < 1e-5f);
        }
    }
    // ---- Hybrid finish (2 Oct 2026) ----
    {
        float p[4];
        CHECK(!x3look_hybrid_finish(0, 0, 0.3f, 1, p));                 // both 0: nothing to draw
        CHECK(x3look_hybrid_finish(50, 0, 0.3f, 1, p) && p[0] == 0.5f && p[2] == 1.0f && p[3] == 0.0f);
        CHECK(x3look_hybrid_finish(0, 100, 0.0f, 0, p) && std::fabs(p[3] - 0.3f) < 1e-6f && p[2] == 0.0f);
        CHECK(x3look_hybrid_finish(250, -5, 1e9f, 1, p) && p[0] == 1.0f && p[1] == 0.0f && p[3] == 0.0f);   // clamped
        // strength 0 and no lift: the colour is unchanged whatever the texture holds
        x3look_hybrid_finish(0, 0, 0.5f, 1, p);
        CHECK(x3look_hybrid_finish_apply(p, 0.42f, 0.8f, 0.3f) == 0.42f);
        // full vignette: colour x vig.z + vig.y x effect amount
        x3look_hybrid_finish(100, 0, 0.5f, 1, p);
        CHECK(std::fabs(x3look_hybrid_finish_apply(p, 0.5f, 0.2f, 0.6f) - (0.5f * 0.6f + 0.2f * 0.5f)) < 1e-6f);
        // half strength: half way to the vignette
        x3look_hybrid_finish(50, 0, 0.0f, 1, p);
        CHECK(std::fabs(x3look_hybrid_finish_apply(p, 0.5f, 0.0f, 0.6f) - 0.5f * 0.8f) < 1e-6f);
        // no texture: neutral (no darkening) even at full strength
        x3look_hybrid_finish(100, 0, 0.5f, 0, p);
        CHECK(x3look_hybrid_finish_apply(p, 0.5f, 0.9f, 0.1f) == 0.5f);
        // shadow lift: black stays black, white stays white, dark tones rise, mid-tones nearly unchanged
        x3look_hybrid_finish(0, 50, 0.0f, 0, p);
        CHECK(x3look_hybrid_finish_apply(p, 0.0f, 0, 1) == 0.0f && std::fabs(x3look_hybrid_finish_apply(p, 1.0f, 0, 1) - 1.0f) < 1e-6f);
        CHECK(x3look_hybrid_finish_apply(p, 0.05f, 0, 1) > 0.06f && x3look_hybrid_finish_apply(p, 0.05f, 0, 1) < 0.09f);
        CHECK(std::fabs(x3look_hybrid_finish_apply(p, 0.7f, 0, 1) - 0.7f) < 0.02f);
    }
    // Hybrid's reduced auto exposure: 1 at the daylight reference, none at strength 0, limits x0.5..x2
    CHECK(x3look_hybrid_gain(0.37f, 0.5f) == 1.0f && x3look_hybrid_gain(0.08f, 0.0f) == 1.0f);
    CHECK(std::fabs(x3look_hybrid_gain(0.1f, 0.5f) - std::sqrt(0.371f / 0.101f)) < 1e-5f);
    CHECK(x3look_hybrid_gain(0.0f, 1.0f) == 2.0f && x3look_hybrid_gain(0.06f, 0.5f) == 2.0f);
    CHECK(x3look_hybrid_gain(0.9f, 0.5f) < 1.0f && x3look_hybrid_gain(50.0f, 1.0f) == 0.5f);
    CHECK(x3look_hybrid_gain(0.2f, 0.5f) < x3look_hybrid_gain(0.2f, 1.0f));
    // ---- Motion blur and depth of field (Test121). Expected values: the Xbox 360 Stuff Pack's
    // GetMotionBlurParameters run under an x86 emulator with these inputs (verification/test121
    // emulate_gmb.py); its constants are the Xbox 360's (USA 82223d40). ----
    {
        auto near = [](const float *a, const float *b, int n) {
            for (int i = 0; i < n; ++i) if (std::fabs(a[i] - b[i]) > 2e-8f + 1e-5f * std::fabs(b[i])) return false;
            return true;
        };
        X3MotionIn in{};
        in.matrix[0][0] = in.matrix[1][1] = in.matrix[2][2] = 1.0f;
        in.aspect = 1280.0f / 720.0f; in.allowed = 1; in.fix_diagonal = 1;
        float v[4], p[4];
        in.velocity[2] = 10.0f;                                    // below 15 m/s: no blur
        CHECK(!x3look_motion_blur(&in, v, p) && p[0] == 1.0f && p[1] == 0.0f);
        in.velocity[2] = 30.0f;
        { const float e[4] = {-0.0010227272f, -0.0010227272f, 0.0010227272f, 0.0010227272f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4) && p[0] == 1.0f && p[1] == 0.0f); }
        in.velocity[2] = 60.0f; in.radial = 1.0f;
        { const float e[4] = {-0.0049857958f, -0.0049857958f, 0.0049857958f, 0.0049857958f}, ep[2] = {1.1f, 1.0f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4) && near(p, ep, 2)); }
        in.velocity[2] = 200.0f; in.radial = 0.0f;                 // capped at 0.65
        { const float e[4] = {-0.004875f, -0.004875f, 0.004875f, 0.004875f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4)); }
        in.velocity[0] = 5.0f; in.velocity[2] = 40.0f; in.radial = 1.0f;
        { const float e[4] = {-0.0031305598f, -0.0027827199f, 0.0024348798f, 0.0027827199f}, ep[2] = {1.1f, 0.80622572f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4) && near(p, ep, 2)); }
        in.radial = 0.0f; in.fix_diagonal = 0;                     // the Xbox 360 as shipped
        { const float e[4] = {-0.0014983877f, -0.0017124431f, 0.0019264984f, 0.0017124431f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4)); }
        in.fix_diagonal = 1; in.aspect = 2560.0f / 1080.0f;        // 21:9
        { const float e[4] = {-0.0018729847f, -0.0017124431f, 0.0015519016f, 0.0017124431f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4)); }
        in.aspect = 1280.0f / 720.0f;
        in.velocity[0] = 3.0f; in.velocity[1] = 4.0f;
        { const float e[4] = {-0.0018408763f, -0.0018836875f, 0.0015840098f, 0.0015411988f};
          CHECK(x3look_motion_blur(&in, v, p) && near(v, e, 4)); }
        {   // a camera turned 0.5 rad about y
            X3MotionIn r = in;
            const float c = std::cos(0.5f), sn = std::sin(0.5f);
            const float m[3][3] = {{c, 0, -sn}, {0, 1, 0}, {sn, 0, c}};
            std::memcpy(r.matrix, m, sizeof m);
            r.velocity[0] = 2.0f; r.velocity[1] = 1.0f; r.velocity[2] = 50.0f; r.radial = 0.3f;
            const float e[4] = {-0.003892296f, -0.0024902972f, 0.0009748973f, 0.0023768963f}, ep[2] = {1.03f, 0.3f};
            CHECK(x3look_motion_blur(&r, v, p) && near(v, e, 4) && near(p, ep, 2));
        }
        in.velocity[0] = in.velocity[1] = 0.0f; in.velocity[2] = 60.0f; in.chase_camera = 1;   // chase camera
        CHECK(x3look_motion_blur(&in, v, p) && p[0] == 0.0f);
        in.chase_camera = 0; in.allowed = 0; in.radial = 0.5f;     // cut-scene camera: no movement blur
        CHECK(!x3look_motion_blur(&in, v, p) && std::fabs(p[0] - 1.05f) < 1e-6f && p[1] == 0.0f);
        // the mask: near things blur, far ones do not; no depth = everywhere the vignette allows
        const float pp[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        CHECK(x3look_motion_mask(pp, 0.0f, 0.0f, 0.0f, 1) == 1.0f);           // zDist 1
        CHECK(x3look_motion_mask(pp, 0.0f, 0.0f, 1.0f, 1) == 0.0f);           // the sky
        CHECK(std::fabs(x3look_motion_mask(pp, 0.0f, 0.0f, 1.0f - 1.0f / 150.0f, 1) - 0.7f) < 1e-4f);   // zDist 150
        CHECK(x3look_motion_mask(pp, 0.0f, 0.0f, 1.0f, 0) == 1.0f);
        const float chase[4] = {0.0f, 0.5f, 0.0f, 0.0f};
        CHECK(std::fabs(x3look_motion_mask(chase, 0.4f, 0.2f, 0.0f, 1) - 0.5f) < 1e-6f);   // 0.4 x 1 + 0.2 x 0.5
        // depth of field
        float d[4];
        CHECK(!x3look_dof(0.0f, 0.0f, d) && d[2] == 0.0f && d[3] == 0.0f);
        CHECK(x3look_dof(5.0f, 2.0f, d) && d[0] == 10.0f && d[1] == 6.0f && std::fabs(d[2] - 1.0f / 6.0f) < 1e-7f && d[3] == 1.0f);
        CHECK(x3look_dof(0.0f, 0.0001f, d) && d[3] == 1.0f);
    }
    std::printf("exposure_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
