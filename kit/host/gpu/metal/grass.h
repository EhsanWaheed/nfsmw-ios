// grass.h - Remaster run 13 grass shells (owner 8 Oct: grass back in, our own version of the Xbox 360 GRASS effect with
// a quality option; review/run13/x360-shader-map.md GRASS rows, review/run13/x360-study-design.md 1 and 2(b)). The 360
// grass.fx is reference only; nothing of it is used.
//
// Main-view WORLD / WORLDNORMALMAP draws whose stage-0 texture is a grass terrain texture are re-issued N times right
// after the draw itself, which stays unchanged and first. Each shell lifts the ground along its normal (shell i of N at
// (i - 0.5) / N of the grass height) and alpha-tests (or, in an MSAA pass, covers) our own blade noise, darkened toward
// the root, only where the base texel is green-dominant and not grey: the TRNS strips mix pavement, so the mask is a
// per-pixel colour test, not a per-texture one. LOD in the pixel half: shell 1 fades out from 40 to 80 m, the others
// from 25 to 40 m. The shader text is d3d9_shadergen.cpp grass_vs_patch / grass_ps_patch. Included once, by d3d9_metal.mm.
//
// Remaster run 14 grass blades (owner 8 Oct, PRODUCT_CONTRACT "grass = real blades"; review/run14/grass-blades-design.md):
// near the camera the same grass draws also get real blade geometry. Roots are a jittered world-space grid (independent
// of the triangulation: no seam doubles, LOD swaps do not reshuffle) scattered on the CPU from the buffers' shadow copies
// into 8 m cells (cached, LRU, per-frame build budget), each root holding every VS input the program reads (barycentric,
// already in Direct3D's values). The blade draw is the program's own VS with grass_blade_vs_patch (instanced 5/7-vertex
// strips, only POSITION moves) and its own PS with the blade colour terms; opaque, no alpha test, cull none. Shells,
// when also on, fade in across the blade fade band (P.pad1 + 256 x fade end). The blade parts hold no Metal types (the
// scatter, decoder and cache are unit-tested in dx/tests/d3d9_tests.cpp).
//
// RECOMP_REMASTER_GRASS=<shells>: 0 or unset off; 1..5 shells (run 13 mod quality Low 2, Medium 4, High 5; run 14: with
// blades the mod sets 0/1/2 light shells).
// RECOMP_REMASTER_GRASS_BLADES=<quality>: 0 or unset off; 1 Low, 2 Medium, 3 High (Quality below).
// A/B switch grass (recomp_ab::GRASS): OLD periods draw no shells and no blades.
// TEST ONLY tuning: RECOMP_REMASTER_GRASS_HEIGHT=<metres>, the shell height (default 0.09);
// RECOMP_REMASTER_GRASS_BLADE_HEIGHT=<metres>, _DENSITY=<roots/m2 near>, _WIND=<amplitude>: the blade starting values;
// RECOMP_REMASTER_GRASS_SHELLS=0: no shells even when the mod set them (the blades-only cost pair).
#pragma once

#include "../../../dx/d3d9_msl.h"
#include "../../../dx/d3d9_shader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace remaster_grass {

// knowledge/TEXTURE_CATALOGUE.tsv, category terrain, name containing GRASS (66): TRNS_GRASS*, TRNS_GCGRASS*, the
// DIRTPATH and PAVEMENT transitions and TRN_*GRASS*. Sorted catalogue hashes; the tag key is hash << 32.
inline constexpr uint32_t kKeys[] = {
    0x030f2699u, 0x030f269au, 0x03103f5bu, 0x0319b1a2u, 0x074bc2d0u, 0x0db0c0bdu, 0x10bbf63au, 0x10bbf63bu,
    0x10bd0efcu, 0x10bd9b5du, 0x11df44cdu, 0x156e7175u, 0x1908a9ddu, 0x20d6e344u, 0x232c0be5u, 0x23f259a6u,
    0x28f9d695u, 0x319706cau, 0x337911e9u, 0x367e5485u, 0x395ebeebu, 0x3f06347fu, 0x47a7bdf3u, 0x4df53922u,
    0x50af3777u, 0x54422703u, 0x56974fa4u, 0x598eab30u, 0x5a588f65u, 0x5ab2c9d2u, 0x5c5c9cacu, 0x61b36441u,
    0x61f706aeu, 0x633b764cu, 0x6cdd051eu, 0x6f6adeb4u, 0x6fa3d64fu, 0x768829bfu, 0x7d096209u, 0x8362e6f4u,
    0x896e0aadu, 0x8ab5e8c3u, 0x8beffd79u, 0x90704a30u, 0x92aa09a3u, 0xa08432d1u, 0xa502cdd4u, 0xa66834c2u,
    0xaaaba4b6u, 0xab7ee48bu, 0xad906c35u, 0xc3b2bd91u, 0xc3beec68u, 0xc48ab616u, 0xc9f5ec67u, 0xd97a7da0u,
    0xda45656fu, 0xe4ea131fu, 0xe5390352u, 0xe63658d1u, 0xea50a0a7u, 0xeeee48deu, 0xefe3eca0u, 0xf1c98404u,
    0xf257c5fbu, 0xf2bb60f6u,
};

inline bool key(uint64_t k) {
    return !(uint32_t)k && std::binary_search(std::begin(kKeys), std::end(kKeys), (uint32_t)(k >> 32));
}

// Why a grass draw got no shells / no blades. FORMAT..ARENA are blade-only.
enum Skip { OTHER_VIEW, RENDER_STATE, PROGRAMS, BUILD, FORMAT, INLINE, EYE, ARENA, kSkips };

// ---- run 14 blades: the pure C++ parts (unit-tested) -----------------------------------------------------------------

// Quality (design 3): blade radius full / fade end (m), near density (roots/m2), base height (m; UNV starting value, the
// owner judges), strip vertices, light shells (the mod's RECOMP_REMASTER_GRASS), arena bytes.
struct Quality {
    float r_full = 0, r_end = 0, density = 0, h = 0;
    int verts = 0, shells = 0;
    size_t arena = 0;
};
inline Quality quality(int q) {
    switch (q) {
    case 1: return {10.0f, 15.0f, 90.0f, 0.10f, 5, 0, (size_t)8 << 20};
    case 2: return {18.0f, 25.0f, 130.0f, 0.12f, 7, 1, (size_t)16 << 20};
    case 3: return {25.0f, 35.0f, 180.0f, 0.12f, 7, 2, (size_t)32 << 20};
    default: return {};
    }
}

inline uint32_t hash32(uint32_t s) {
    s ^= s >> 16; s *= 0x7feb352du; s ^= s >> 15; s *= 0x846ca68bu; s ^= s >> 16;
    return s;
}
inline uint32_t hash2(int32_t i, int32_t j) { return hash32((uint32_t)i * 0x8da6b343u ^ (uint32_t)j * 0xd8163841u); }

// Bytes of a D3DDECLTYPE (0 = not one d3d9_metal.mm vertex_format accepts).
inline uint32_t format_bytes(uint32_t type) {
    static const uint8_t n[17] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
    return type < 17 ? n[type] : 0;
}
// One vertex element as the VS sees it in vv[] (Metal's fetch with d3d9_metal.mm vertex_format, times ascale): the same
// channel order, normalization and (0, 0, 0, 1) fill. False for a type vertex_format does not accept.
inline bool decode_input(uint32_t type, const uint8_t *p, float o[4]) {
    o[0] = o[1] = o[2] = 0.0f;
    o[3] = 1.0f;
    auto i16 = [&](int k) { int16_t v; memcpy(&v, p + 2 * k, 2); return v; };
    auto u16 = [&](int k) { uint16_t v; memcpy(&v, p + 2 * k, 2); return v; };
    auto sn = [](float c, float m) { return std::max(c / m, -1.0f); };
    switch (type) {
    case 0: case 1: case 2: case 3:
        memcpy(o, p, 4 * (type + 1));
        return true;
    case 4:   // D3DCOLOR: bytes B, G, R, A -> (R, G, B, A)
        o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f;
        return true;
    case 5: case 8:   // UBYTE4 (integers: unorm x 255), UBYTE4N
        for (int k = 0; k < 4; ++k)
            o[k] = type == 5 ? (float)p[k] : p[k] / 255.0f;
        return true;
    case 6: case 7: case 9: case 10: {   // SHORT2/4 (integers: snorm x 32767), SHORT2N/4N
        const int n = type == 6 || type == 9 ? 2 : 4;
        for (int k = 0; k < n; ++k)
            o[k] = sn((float)i16(k), 32767.0f) * (type <= 7 ? 32767.0f : 1.0f);
        return true;
    }
    case 11: case 12:
        for (int k = 0; k < (type == 11 ? 2 : 4); ++k)
            o[k] = u16(k) / 65535.0f;
        return true;
    case 13: case 14: {   // UDEC3 (xyz integers: unorm x 1023, w unorm), DEC3N
        uint32_t v;
        memcpy(&v, p, 4);
        for (int k = 0; k < 3; ++k) {
            const uint32_t c = v >> (10 * k) & 1023u;
            o[k] = type == 13 ? (float)c : sn((float)((int32_t)(c << 22) >> 22), 511.0f);
        }
        o[3] = type == 13 ? (float)(v >> 30) / 3.0f : sn((float)((int32_t)v >> 30), 1.0f);
        return true;
    }
    case 15: case 16: {
        _Float16 h[4];
        memcpy(h, p, type == 15 ? 4 : 8);
        for (int k = 0; k < (type == 15 ? 2 : 4); ++k)
            o[k] = (float)h[k];
        return true;
    }
    default:
        return false;
    }
}

// The VS inputs a root record carries and the record layout: POSITION / TEXCOORD as float4, the rest half4, then the
// extra float2 (seed in [0, 1), cell birth time) for attribute 16. type 0xff = not in the declaration (zero, as the
// draw's zero buffer).
struct Input {
    uint8_t reg = 0, stream = 0, type = 0xff, wide = 1;
    uint16_t offset = 0;
    uint32_t field = 0;
};
struct Layout {
    Input in[16];
    int n = 0, pos = -1, nrm = -1, tex = -1;
    uint32_t stride = 0, extra = 0;
    uint64_t hash = 0;
};
// From the declaration and the program's inputs (as d3d9_metal.mm vertex_descriptor matches them). False (FORMAT) when
// a matched element has a type the decoder does not know, or POSITION0 / NORMAL0 / TEXCOORD0 is missing.
inline bool make_layout(const uint8_t *decl, uint32_t decl_size, const d9sh::Program &vp, Layout *L) {
    *L = Layout{};
    uint64_t h = 0xcbf29ce484222325ull;
    for (const d9sh::DclIn &d : vp.inputs) {
        if (d.type != d9sh::R_INPUT || L->n >= 16)
            continue;
        Input &x = L->in[L->n];
        x.reg = (uint8_t)(d.index & 15);
        for (uint32_t i = 0; i + 8 <= decl_size; i += 8) {
            const uint8_t *e = decl + i;
            const uint16_t stream = (uint16_t)(e[0] | e[1] << 8);
            if (stream == 0xff)
                break;
            if (e[6] != d.usage || e[7] != d.usage_index || stream >= 8)
                continue;
            if (!format_bytes(e[4]))
                return false;
            x.stream = (uint8_t)stream;
            x.offset = (uint16_t)(e[2] | e[3] << 8);
            x.type = e[4];
            break;
        }
        x.wide = d.usage == 0 || d.usage == 5;
        x.field = L->stride;
        L->stride += x.wide ? 16 : 8;
        if (d.usage_index == 0 && x.type != 0xff) {
            if (d.usage == 0) L->pos = L->n;
            if (d.usage == 3) L->nrm = L->n;
            if (d.usage == 5) L->tex = L->n;
        }
        h = (h ^ ((uint64_t)x.reg | (uint64_t)x.wide << 8 | (uint64_t)x.field << 16)) * 0x100000001b3ull;
        ++L->n;
    }
    L->extra = L->stride;
    L->stride += 8;
    L->hash = (h ^ L->stride) * 0x100000001b3ull;
    return L->pos >= 0 && L->nrm >= 0 && L->tex >= 0;
}

// A draw's geometry on the CPU (the buffers' shadow copies): per stream the bytes from the stream offset.
struct Mesh {
    const uint8_t *vb[8] = {};
    size_t vb_bytes[8] = {};
    uint32_t stride[8] = {};
    const uint8_t *ib = nullptr;   // from index 0 of the buffer; null = not indexed
    size_t ib_bytes = 0;
    uint32_t index_size = 2, primitive = 4, count = 0, start = 0;
    int32_t base_vertex = 0;
};
inline bool fetch(const Mesh &m, const Input &x, uint32_t v, float o[4]) {
    if (x.type == 0xff) {
        o[0] = o[1] = o[2] = o[3] = 0.0f;
        return true;
    }
    const size_t at = (size_t)v * m.stride[x.stream] + x.offset;
    if (!m.vb[x.stream] || at + format_bytes(x.type) > m.vb_bytes[x.stream])
        return false;
    return decode_input(x.type, m.vb[x.stream] + at, o);
}
// The triangles (vertex numbers) of a list or strip; fans and others give none. False when an index is out of range.
inline bool triangles(const Mesh &m, std::vector<uint32_t> *tri) {
    tri->clear();
    if (m.primitive != 4 && m.primitive != 5)
        return true;
    auto index = [&](uint32_t k, uint32_t *v) {
        if (!m.ib) {
            *v = m.start + k;
            return true;
        }
        const size_t at = ((size_t)m.start + k) * m.index_size;
        if (at + m.index_size > m.ib_bytes)
            return false;
        uint32_t i = 0;
        memcpy(&i, m.ib + at, m.index_size);
        *v = (uint32_t)((int64_t)i + m.base_vertex);
        return true;
    };
    for (uint32_t t = 0; t < m.count; ++t) {
        uint32_t a, b, c;
        const uint32_t k = m.primitive == 4 ? t * 3 : t;
        if (!index(k, &a) || !index(k + 1, &b) || !index(k + 2, &c))
            return false;
        if (a == b || b == c || a == c)
            continue;
        tri->insert(tri->end(), {a, b, c});
    }
    return true;
}
// Sampled content stamp of a draw's geometry (Buf revisions are only kept when tracing): 64 index bytes and 64
// 16-byte samples of the position stream.
inline uint64_t stamp(const Mesh &m, const Layout &L) {
    uint64_t h = 0x84222325cbf29ce4ull;
    auto mixin = [&](const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 0x100000001b3ull;
    };
    if (m.ib && m.ib_bytes)
        for (uint32_t k = 0; k < 64; ++k)
            mixin(m.ib + (m.ib_bytes - 1) * k / 63, 1);
    const Input &x = L.in[L.pos];
    if (m.vb[x.stream] && m.vb_bytes[x.stream] >= 16)
        for (uint32_t k = 0; k < 64; ++k)
            mixin(m.vb[x.stream] + (m.vb_bytes[x.stream] - 16) * k / 63 / 4 * 4, 16);
    return h;
}

// The roots of cell (cx, cy) (cell x cell metres of the POSITION XY plane) on the triangles tri[3 k ..] (vertex numbers),
// appended to *out as records of layout L: the jittered lattice of spacing s (point (i + jx, j + jy) x s, jitter from
// hash2(i, j); none when !jitter), each point that falls in the cell (half-open) and in a triangle (top-left rule on the
// XY projection: a point on a shared edge belongs to exactly one triangle) whose geometric normal has |z| >= min_nz.
// POSITION xy is the lattice point itself; every other value is barycentric. Returns the number of roots.
// Deterministic: the same input gives the same bytes.
inline size_t scatter_cell(const Mesh &m, const Layout &L, const uint32_t *tri, size_t ntri, int cx, int cy, float s,
                           float cell, float birth, std::vector<uint8_t> *out, bool jitter = true,
                           float min_nz = 0.55f) {
    size_t roots = 0;
    const double x0 = (double)cx * cell, y0 = (double)cy * cell, x1 = x0 + cell, y1 = y0 + cell;
    for (size_t t = 0; t < ntri; ++t) {
        uint32_t iv[3] = {tri[3 * t], tri[3 * t + 1], tri[3 * t + 2]};
        float V[3][16][4];   // every input at the three corners
        bool ok = true;
        for (int v = 0; v < 3 && ok; ++v)
            ok = fetch(m, L.in[L.pos], iv[v], V[v][L.pos]);
        if (!ok)
            continue;
        const float *A = V[0][L.pos], *B = V[1][L.pos], *C = V[2][L.pos];
        const double ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2];
        const double wx = C[0] - A[0], wy = C[1] - A[1], wz = C[2] - A[2];
        const double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(nl > 0.0) || std::fabs(nz) < min_nz * nl)
            continue;
        double px[3] = {A[0], B[0], C[0]}, py[3] = {A[1], B[1], C[1]};
        int ord[3] = {0, 1, 2};
        if (nz < 0.0) {   // counter-clockwise in XY
            std::swap(px[1], px[2]);
            std::swap(py[1], py[2]);
            std::swap(ord[1], ord[2]);
        }
        const double area = std::fabs(nz);
        const double mnx = std::max(std::min({px[0], px[1], px[2]}), x0);
        const double mxx = std::min(std::max({px[0], px[1], px[2]}), x1);
        const double mny = std::max(std::min({py[0], py[1], py[2]}), y0);
        const double mxy = std::min(std::max({py[0], py[1], py[2]}), y1);
        if (mnx > mxx || mny > mxy)
            continue;
        for (int v = 0; v < 3 && ok; ++v)
            for (int k = 0; k < L.n && ok; ++k)
                ok = k == L.pos || fetch(m, L.in[k], iv[v], V[v][k]);
        if (!ok)
            continue;
        const int i0 = (int)std::floor(mnx / s) - 1, i1 = (int)std::floor(mxx / s) + 1;
        const int j0 = (int)std::floor(mny / s) - 1, j1 = (int)std::floor(mxy / s) + 1;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const uint32_t h = hash2(i, j);
                const double jx = jitter ? (h & 0xffffu) / 65536.0 : 0.0, jy = jitter ? (h >> 16) / 65536.0 : 0.0;
                const double x = (i + jx) * s, y = (j + jy) * s;
                if (x < x0 || x >= x1 || y < y0 || y >= y1)
                    continue;
                double w[3];   // w[e]: the weight of (ordered) corner e, from edge e+1 -> e+2
                bool in = true;
                for (int e = 0; e < 3 && in; ++e) {
                    const int p0 = (e + 1) % 3, p1 = (e + 2) % 3;
                    const double ex = px[p1] - px[p0], ey = py[p1] - py[p0];
                    w[e] = ex * (y - py[p0]) - ey * (x - px[p0]);
                    const bool top_left = ey < 0.0 || (ey == 0.0 && ex < 0.0);
                    in = w[e] > 0.0 || (w[e] == 0.0 && top_left);
                }
                if (!in)
                    continue;
                const size_t at = out->size();
                out->resize(at + L.stride);
                uint8_t *r = out->data() + at;
                for (int k = 0; k < L.n; ++k) {
                    float o[4];
                    for (int c = 0; c < 4; ++c)
                        o[c] = (float)((w[0] * V[ord[0]][k][c] + w[1] * V[ord[1]][k][c] + w[2] * V[ord[2]][k][c]) /
                                       area);
                    if (k == L.pos) {   // exactly the lattice point (no barycentric rounding in XY)
                        o[0] = (float)x;
                        o[1] = (float)y;
                    }
                    if (L.in[k].wide) {
                        memcpy(r + L.in[k].field, o, 16);
                    } else {
                        const _Float16 hv[4] = {(_Float16)o[0], (_Float16)o[1], (_Float16)o[2], (_Float16)o[3]};
                        memcpy(r + L.in[k].field, hv, 8);
                    }
                }
                const float ex[2] = {(float)(hash32(h ^ 0x9e3779b9u) >> 8) * (1.0f / 16777216.0f), birth};
                memcpy(r + L.extra, ex, 8);
                ++roots;
            }
    }
    return roots;
}

// The arena of root records: 64 KB pages, a cell owns a contiguous page range; freed pages wait kQuiet frames before
// reuse (frames in flight may still read them).
constexpr uint32_t kPage = 64u << 10, kQuiet = 4;
constexpr float kCell = 8.0f;
struct Run {
    uint32_t offset = 0, count = 0;
    float dist = 0;
};
struct Cell {
    uint32_t page = 0, pages = 0, roots = 0;
};
struct Chunk {
    uint64_t stamp = 0, used = 0;
    bool binned = false;
    Layout L;
    std::vector<uint32_t> tri;
    std::unordered_map<uint64_t, std::vector<uint32_t>> bins;   // cell -> triangle numbers
    std::unordered_map<uint64_t, Cell> cells;                    // built cells
};
inline uint64_t cell_key(int cx, int cy) { return (uint64_t)(uint32_t)cx << 32 | (uint32_t)cy; }
inline float cell_dist(uint64_t k, float ex, float ey) {
    const float x0 = (float)(int32_t)(uint32_t)(k >> 32) * kCell, y0 = (float)(int32_t)(uint32_t)k * kCell;
    const float dx = std::max({x0 - ex, 0.0f, ex - x0 - kCell}), dy = std::max({y0 - ey, 0.0f, ey - y0 - kCell});
    return std::sqrt(dx * dx + dy * dy);
}

struct Blades {
    Quality q;
    float height = 0, density = 0, wind = 0.15f, spacing = 1.0f;
    uint8_t *base = nullptr;     // the arena (shared-storage contents, or a test vector)
    std::vector<int64_t> page;   // 0 free, 1 used, < 0 freed at frame -x
    std::unordered_map<uint64_t, Chunk> chunks;
    uint64_t frame = 0;
    float time = 0;   // seconds, wrapped at 1000
    float eye[3] = {};
    int64_t budget = 40000;
    uint32_t runs_frame = 0;
    uint64_t cells_built = 0, roots_built = 0, roots_drawn = 0, runs = 0, cells_full = 0;   // cumulative
    double build_ms = 0;

    void init(int quality_level) {
        q = quality(quality_level);
        auto envf = [](const char *n, float d, float hi) {
            const char *e = getenv(n);
            const float v = e ? strtof(e, nullptr) : 0.0f;
            return v > 0.0f && v <= hi ? v : d;
        };
        height = envf("RECOMP_REMASTER_GRASS_BLADE_HEIGHT", q.h, 1.0f);
        density = envf("RECOMP_REMASTER_GRASS_DENSITY", q.density, 1000.0f);
        wind = envf("RECOMP_REMASTER_GRASS_WIND", 0.15f, 2.0f);
        spacing = density > 0 ? 1.0f / std::sqrt(density) : 1.0f;
        page.assign(q.arena / kPage, 0);
    }
    float r_build() const { return q.r_end + 12.0f; }
    int64_t alloc(uint32_t n) {
        const int64_t quiet = -(int64_t)(frame > kQuiet ? frame - kQuiet : 0);
        for (size_t i = 0, run = 0; i < page.size(); ++i) {
            run = page[i] == 0 || (page[i] < 0 && page[i] >= quiet) ? run + 1 : 0;
            if (run == n) {
                for (size_t k = i + 1 - n; k <= i; ++k)
                    page[k] = 1;
                return (int64_t)(i + 1 - n);
            }
        }
        return -1;
    }
    void release(const Cell &c) {
        for (uint32_t k = c.page; k < c.page + c.pages; ++k)
            page[k] = -(int64_t)(frame ? frame : 1);
    }
    size_t pages_used() const {
        size_t n = 0;
        for (int64_t p : page)
            n += p > 0;
        return n;
    }
    // Per frame: time, eviction of cells beyond r_build + 10 m and of chunks not drawn for 600 frames, the budget.
    void tick(float seconds) {
        ++frame;
        time = std::fmod(seconds, 1000.0f);
        budget = 40000;
        runs_frame = 0;
        for (auto it = chunks.begin(); it != chunks.end();) {
            const bool stale = frame - it->second.used > 600;
            for (auto c = it->second.cells.begin(); c != it->second.cells.end();) {
                if (stale || cell_dist(c->first, eye[0], eye[1]) > r_build() + 10.0f) {
                    release(c->second);
                    c = it->second.cells.erase(c);
                } else {
                    ++c;
                }
            }
            it = stale ? chunks.erase(it) : std::next(it);
        }
    }
    // Frees the built cell farthest from the eye (any chunk) if it is farther than d; false if none.
    bool evict_farthest(float d) {
        Chunk *bc = nullptr;
        uint64_t bk = 0;
        float bd = d;
        for (auto &ch : chunks)
            for (auto &kv : ch.second.cells) {
                const float cd = cell_dist(kv.first, eye[0], eye[1]);
                if (cd > bd && kv.second.pages) {
                    bd = cd;
                    bc = &ch.second;
                    bk = kv.first;
                }
            }
        if (!bc)
            return false;
        release(bc->cells[bk]);
        bc->cells.erase(bk);
        return true;
    }
    // One grass draw (eye e in POSITION space): (re)bins the chunk if new or changed, builds its missing cells within
    // r_build nearest first (frame budget 40k roots), returns the draw runs of its built cells within the fade end + 2 m
    // (records contiguous in the arena merged; at most 96 runs per frame, nearest first). False = skipped (*why).
    bool prepare(uint64_t chunk_key, const Mesh &m, const uint8_t *decl, uint32_t decl_size, const d9sh::Program &vp,
                 const float e[3], std::vector<Run> *out, Skip *why) {
        out->clear();
        memcpy(eye, e, sizeof eye);
        Chunk &c = chunks[chunk_key];
        const bool fresh = !c.used;
        c.used = frame ? frame : 1;
        if (fresh && !make_layout(decl, decl_size, vp, &c.L))
            c.L.pos = -1;
        if (c.L.pos < 0) {
            *why = FORMAT;
            return false;
        }
        const uint64_t st = stamp(m, c.L);
        if (st != c.stamp || !c.binned) {   // new or changed: drop the cells and bin again
            for (auto &kv : c.cells)
                release(kv.second);
            c.cells.clear();
            c.bins.clear();
            c.stamp = st;
            c.binned = true;
            if (!triangles(m, &c.tri)) {
                c.tri.clear();
                c.L.pos = -1;
                *why = FORMAT;
                return false;
            }
            for (size_t t = 0; t * 3 < c.tri.size(); ++t) {
                float p[3][4];
                bool ok = true;
                for (int v = 0; v < 3 && ok; ++v)
                    ok = fetch(m, c.L.in[c.L.pos], c.tri[3 * t + v], p[v]);
                if (!ok)
                    continue;
                const int x0 = (int)std::floor(std::min({p[0][0], p[1][0], p[2][0]}) / kCell);
                const int x1 = (int)std::floor(std::max({p[0][0], p[1][0], p[2][0]}) / kCell);
                const int y0 = (int)std::floor(std::min({p[0][1], p[1][1], p[2][1]}) / kCell);
                const int y1 = (int)std::floor(std::max({p[0][1], p[1][1], p[2][1]}) / kCell);
                if (x1 - x0 > 64 || y1 - y0 > 64)
                    continue;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        c.bins[cell_key(x, y)].push_back((uint32_t)t);
            }
        }
        std::vector<std::pair<float, uint64_t>> want;
        for (const auto &kv : c.bins) {
            const float d = cell_dist(kv.first, e[0], e[1]);
            if (d <= r_build() && !c.cells.count(kv.first))
                want.push_back({d, kv.first});
        }
        std::sort(want.begin(), want.end());
        std::vector<uint8_t> rec;
        std::vector<uint32_t> tv;
        bool full = false;
        for (const auto &w : want) {
            if (budget <= 0 || !base)
                break;
            const auto t0 = std::chrono::steady_clock::now();
            const std::vector<uint32_t> &bin = c.bins[w.second];
            tv.clear();
            for (uint32_t t : bin)
                tv.insert(tv.end(), {c.tri[3 * t], c.tri[3 * t + 1], c.tri[3 * t + 2]});
            rec.clear();
            Cell cell;
            cell.roots = (uint32_t)scatter_cell(m, c.L, tv.data(), bin.size(), (int32_t)(uint32_t)(w.second >> 32),
                                                (int32_t)(uint32_t)w.second, spacing, kCell, time, &rec);
            cell.pages = (uint32_t)((rec.size() + kPage - 1) / kPage);
            budget -= (int64_t)cell.roots + 64;
            if (cell.pages) {
                int64_t p = alloc(cell.pages);
                for (int tries = 0; p < 0 && tries < 8 && evict_farthest(w.first); ++tries)
                    p = alloc(cell.pages);
                if (p < 0) {
                    full = true;
                    ++cells_full;
                    break;
                }
                cell.page = (uint32_t)p;
                memcpy(base + (size_t)cell.page * kPage, rec.data(), rec.size());
            }
            c.cells[w.second] = cell;
            ++cells_built;
            roots_built += cell.roots;
            build_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        std::vector<Run> rs;
        for (const auto &kv : c.cells) {
            const float d = cell_dist(kv.first, e[0], e[1]);
            if (kv.second.roots && d <= q.r_end + 2.0f)
                rs.push_back({kv.second.page * kPage, kv.second.roots, d});
        }
        std::sort(rs.begin(), rs.end(), [](const Run &a, const Run &b) { return a.offset < b.offset; });
        for (const Run &r : rs) {
            if (!out->empty() && out->back().offset + out->back().count * c.L.stride == r.offset) {
                out->back().count += r.count;
                out->back().dist = std::min(out->back().dist, r.dist);
            } else {
                out->push_back(r);
            }
        }
        std::sort(out->begin(), out->end(), [](const Run &a, const Run &b) { return a.dist < b.dist; });
        const uint32_t room = runs_frame < 96 ? 96 - runs_frame : 0;
        if (out->size() > room)
            out->resize(room);
        runs_frame += (uint32_t)out->size();
        for (const Run &r : *out)
            roots_drawn += r.count;
        runs += out->size();
        if (full && out->empty()) {
            *why = ARENA;
            return false;
        }
        return true;
    }
    // The blade draw's buffer(19). px: metres per pixel at 1 m (the host's estimate from the pass height).
    D9GrassBlade params(float px) const {
        D9GrassBlade g = {};
        g.eye[0] = eye[0]; g.eye[1] = eye[1]; g.eye[2] = eye[2]; g.eye[3] = time;
        g.fade[0] = q.r_full; g.fade[1] = q.r_end; g.fade[2] = height; g.fade[3] = 0.0055f;
        g.wind[0] = 0.8f; g.wind[1] = 0.6f; g.wind[2] = wind; g.wind[3] = 1.0f;
        g.misc[0] = px; g.misc[1] = (float)((q.verts - 1) / 2); g.misc[2] = 0.3f; g.misc[3] = 0.0f;
        return g;
    }
};

// ---- the per-device state ----------------------------------------------------------------------------------------------

struct State {
    const int shells = [] {
        const char *e = getenv("RECOMP_REMASTER_GRASS");
        const long n = e ? strtol(e, nullptr, 10) : 0;
        const char *o = getenv("RECOMP_REMASTER_GRASS_SHELLS");   // TEST ONLY: 0 = blades only
        return o && *o == '0' ? 0 : (int)std::clamp(n, 0L, 5L);
    }();
    const float height = [] {
        const char *e = getenv("RECOMP_REMASTER_GRASS_HEIGHT");
        const float h = e ? strtof(e, nullptr) : 0.0f;
        return h > 0.0f && h <= 1.0f ? h : 0.09f;
    }();
    const int blades = [] {   // run 14: blade quality 0..3
        const char *e = getenv("RECOMP_REMASTER_GRASS_BLADES");
        const long n = e ? strtol(e, nullptr, 10) : 0;
        return (int)std::clamp(n, 0L, 3L);
    }();
    State() {
        if (blades)
            b.init(blades);
    }
    bool on() const { return shells > 0 || blades > 0; }
    bool blades_on() const { return blades > 0; }
    // The lift of shell i (1..n): (i - 0.5) / n of the grass height, in the position's units (metres).
    float lift(int i) const { return (i - 0.5f) / (float)shells * height; }
    // P.pad1 for shell i: index x 16 + count (grass_ps_patch); run 14: + 256 x the blade fade end (shell fade-in).
    float code(int i) const { return (float)(i * 16 + shells) + (blades ? 256.0f * b.q.r_end : 0.0f); }

    unsigned long main_w = 0;   // the widest colour target seen: the main view (as water_select)
    std::unordered_map<uint64_t, uint8_t> slots;   // (vs, ps) -> texture-coordinate slot, 0 = no shells
    uint64_t draws = 0, drawn = 0, skip[kSkips] = {}, frames = 0, draws_at = 0, drawn_at = 0;
    // run 14 blades
    Blades b;
    std::unordered_map<uint64_t, uint8_t> blade_slots;   // (vs, ps) -> slot, 0 = no blades
    std::unordered_map<uint64_t, int> eyes;              // vs -> eye constant, -1 = none
    uint64_t bdraws = 0, bskip[kSkips] = {}, bdraws_at = 0, roots_at = 0, runs_at = 0;
    const std::chrono::steady_clock::time_point t_start = std::chrono::steady_clock::now();

    bool main_view(unsigned long w) {
        main_w = std::max(main_w, w);
        return w && w == main_w;
    }
    uint8_t slot(uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp, const d9sh::Program &pp) {
        const uint64_t k = vs_key * 0x9e3779b97f4a7c15ull ^ ps_key;
        auto it = slots.find(k);
        if (it == slots.end()) {
            it = slots.emplace(k, d9msl::grass_slot(vp, pp)).first;
            if (it->second)
                fprintf(stderr, "[remaster] grass: vs %016llx ps %016llx: shells (slot t%u)\n",
                        (unsigned long long)vs_key, (unsigned long long)ps_key, it->second);
            else
                fprintf(stderr, "[remaster] grass: vs %016llx ps %016llx: NO shells (no POSITION0/NORMAL0, no free slot "
                                "or no plain stage-0 lookup)\n",
                        (unsigned long long)vs_key, (unsigned long long)ps_key);
        }
        return it->second;
    }
    void block(uint64_t vs_key, uint64_t ps_key) { slots[vs_key * 0x9e3779b97f4a7c15ull ^ ps_key] = 0; }
    uint8_t blade_slot(uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp, const d9sh::Program &pp) {
        const uint64_t k = vs_key * 0x9e3779b97f4a7c15ull ^ ps_key;
        auto it = blade_slots.find(k);
        if (it == blade_slots.end()) {
            it = blade_slots.emplace(k, d9msl::grass_slot(vp, pp, true)).first;
            fprintf(stderr, "[remaster] grass: vs %016llx ps %016llx: %s (slot t%u)\n", (unsigned long long)vs_key,
                    (unsigned long long)ps_key,
                    it->second ? "blades" : "NO blades (no POSITION0/NORMAL0/TEXCOORD0 or no free slot)", it->second);
        }
        return it->second;
    }
    void block_blades(uint64_t vs_key, uint64_t ps_key) { blade_slots[vs_key * 0x9e3779b97f4a7c15ull ^ ps_key] = 0; }
    // The WORLD VS eye constant (vertex_sun_flow's c[eye] - POSITION term), or -1.
    int eye(uint64_t vs_key, const d9sh::Program &vp) {
        auto it = eyes.find(vs_key);
        if (it == eyes.end()) {
            const d9msl::SunFlow f = d9msl::vertex_sun_flow(vp);
            it = eyes.emplace(vs_key, f.ok && f.pbr_ok ? (int)f.eye : -1).first;
        }
        return it->second;
    }
    void frame() {
        if (!on())
            return;
        if (++frames == 1) {
            fprintf(stderr, "[remaster] grass: on (%d shells, height %.3f m)\n", shells, height);
            if (blades)
                fprintf(stderr, "[remaster] grass: blades quality %d (radius %.0f/%.0f m, %.0f roots/m2, height %.3f m, "
                                "%d-vertex strips, wind %.2f, arena %zu MB)\n",
                        blades, b.q.r_full, b.q.r_end, b.density, b.height, b.q.verts, b.wind, b.page.size() / 16);
        }
        if (blades)
            b.tick(std::chrono::duration<float>(std::chrono::steady_clock::now() - t_start).count());
        if (frames % 600)
            return;
        size_t n = 0;
        for (const auto &kv : slots)
            n += kv.second != 0;
        fprintf(stderr, "[remaster] grass: frame %llu grass draws %llu shells %llu skipped %llu\n",
                (unsigned long long)frames, (unsigned long long)draws, (unsigned long long)drawn,
                (unsigned long long)(skip[OTHER_VIEW] + skip[RENDER_STATE] + skip[PROGRAMS] + skip[BUILD]));
        fprintf(stderr, "[remaster] grass: frame %llu skipped: other view %llu, render state %llu, programs %llu, "
                        "build failed %llu; program pairs with shells %zu of %zu\n",
                (unsigned long long)frames, (unsigned long long)skip[OTHER_VIEW], (unsigned long long)skip[RENDER_STATE],
                (unsigned long long)skip[PROGRAMS], (unsigned long long)skip[BUILD], n, slots.size());
        if (blades) {
            size_t cells = 0;
            for (const auto &kv : b.chunks)
                cells += kv.second.cells.size();
            fprintf(stderr, "[remaster] grass: frame %llu blades: draws %llu chunks %zu cells %zu built %llu roots built "
                            "%llu roots submitted %llu runs %llu arena %zu/%zu pages full %llu build %.1f ms\n",
                    (unsigned long long)frames, (unsigned long long)bdraws, b.chunks.size(), cells,
                    (unsigned long long)b.cells_built, (unsigned long long)b.roots_built,
                    (unsigned long long)b.roots_drawn, (unsigned long long)b.runs, b.pages_used(), b.page.size(),
                    (unsigned long long)b.cells_full, b.build_ms);
            fprintf(stderr, "[remaster] grass: frame %llu blades skipped: other view %llu, render state %llu, programs "
                            "%llu, build %llu, FORMAT %llu, inline %llu, eye %llu, arena %llu\n",
                    (unsigned long long)frames, (unsigned long long)bskip[OTHER_VIEW],
                    (unsigned long long)bskip[RENDER_STATE], (unsigned long long)bskip[PROGRAMS],
                    (unsigned long long)bskip[BUILD], (unsigned long long)bskip[FORMAT],
                    (unsigned long long)bskip[INLINE], (unsigned long long)bskip[EYE], (unsigned long long)bskip[ARENA]);
        }
    }
    // Per frame dump (in-run A/B pairs: OLD periods show 0).
    void dump(unsigned present) const {
        if (on())
            fprintf(stderr, "[remaster] grass: dump present_%05u grass draws %llu shells %llu blade draws %llu roots "
                            "%llu runs %llu\n",
                    present, (unsigned long long)(draws - draws_at), (unsigned long long)(drawn - drawn_at),
                    (unsigned long long)(bdraws - bdraws_at), (unsigned long long)(b.roots_drawn - roots_at),
                    (unsigned long long)(b.runs - runs_at));
    }
    void mark() {
        draws_at = draws;
        drawn_at = drawn;
        bdraws_at = bdraws;
        roots_at = b.roots_drawn;
        runs_at = b.runs;
    }
};

} // namespace remaster_grass
