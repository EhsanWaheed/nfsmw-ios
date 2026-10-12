// Small, synthetic effects exercise the real guest ABI and CTAB binding.
// No game files, window, physical controller or sound device are involved.
#include <sys/resource.h>
#include "../dx.h"
#include "../d3d9_pipeline.h"
#include "../host_d9.h"
#include "../dds_volume.h"
#include "../d3d9_msaa.h"
#include "../../runtime/memory.h"
#include "guest_abi.h"
#include "../../host/gpu/metal/grass.h"   // run 14 grass blades (pure C++ parts)
#include "../../host/gpu/metal/foundation.h"   // run 14 foundation F1/F3 (pure C++ parts)
#include "../../host/gpu/metal/smoke_light.h"   // run 14 lit smoke (pure C++ parts)
#include "../../host/gpu/metal/atmosphere.h"   // run 14 B1 height fog (pure C++ twin)

#include <array>
#include <string>
#include <unordered_map>
#include <algorithm>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
        }                                                                                          \
    } while (0)

static HostD9Draw captured;
static void test_volume_dds() {
    std::vector<uint8_t> b(128 + (4 * 2 * 8 + 2 * 1 * 4 + 1 * 1 * 2 + 1) * 4, 0);
    auto put = [&](unsigned word, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            b[word * 4 + i] = uint8_t(value >> (8 * i));
    };
    const uint32_t header[32] = {0x20534444, 124,  0x821007,   2,        4,        0, 8,  4,
                                 0,          0,    0,          0,        0,        0, 0,  0,
                                 0,          0,    0,          32,       0x41,     0, 32, 0xff0000,
                                 0xff00,     0xff, 0xff000000, 0x40100a, 0x200000, 0, 0,  0};
    for (unsigned i = 0; i < 32; ++i)
        put(i, header[i]);
    for (size_t i = 128; i < b.size(); ++i)
        b[i] = uint8_t(i * 17);
    ddsvolume::View v;
    CHECK(ddsvolume::parse(b.data(), b.size(), v));
    CHECK(v.width == 4 && v.height == 2 && v.depth == 8 && v.levels == 4 && v.format == 21);
    CHECK(v.mip[1].width == 2 && v.mip[1].height == 1 && v.mip[1].depth == 4);
    CHECK(v.mip[3].offset == b.size() - 4 && v.mip[3].bytes == 4);
    CHECK(v.mip[0].row_pitch == 16 && v.mip[0].slice_pitch == 32);
    CHECK(!ddsvolume::parse(nullptr, b.size(), v));
    for (size_t n = 0; n < b.size(); ++n) {
        CHECK(!ddsvolume::parse(b.data(), n, v));
        CHECK(v.levels == 0 && v.mip[0].bytes == 0);
    }
    b.push_back(0);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    b.pop_back();
    for (auto [word, value] : std::initializer_list<std::pair<unsigned, uint32_t>>{
             {0, 0},   {1, 123},     {2, 0x21007},   {2, 0x821007 | 0x80000},
             {3, 0},   {4, 513},     {6, 0},         {7, 0},
             {7, 5},   {19, 31},     {20, 4},        {21, 0x30315844},
             {22, 24}, {23, 0xff},   {24, 0xff},     {25, 0xff0000},
             {26, 0},  {27, 0x1008}, {27, 0x400008}, {28, 0x200200},
             {29, 1},  {30, 1},      {31, 1},        {5, 16}}) {
        put(word, value);
        CHECK(!ddsvolume::parse(b.data(), b.size(), v));
        CHECK(v.width == 0 && v.mip[0].bytes == 0);
        put(word, header[word]);
    }
    put(2, header[2] | 8);
    put(5, 16);
    CHECK(ddsvolume::parse(b.data(), b.size(), v));
    put(5, 20); // unsupported padded rows cannot be silently read as packed
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    put(2, header[2]);
    put(5, 0);
    put(20, 0x40);
    put(26, 0);
    CHECK(ddsvolume::parse(b.data(), b.size(), v) && v.format == 22);
    put(3, 512);
    put(4, 512);
    put(6, 512);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v)); // reject budget before pointer arithmetic
    put(3, 0xffffffff);
    put(4, 0xffffffff);
    put(6, 0xffffffff);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    // A valid single-mip non-cubic texture without mip-count metadata.
    b.resize(128 + 3 * 1 * 5 * 4);
    put(2, 0x801007);
    put(3, 1);
    put(4, 3);
    put(6, 5);
    put(7, 0);
    put(27, 0x1008);
    CHECK(ddsvolume::parse(b.data(), b.size(), v) && v.levels == 1);
    put(7, 2);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
}
static unsigned draws = 0, rejected_draws = 0;
static bool admission_enabled = false;
static HostD9Draw rejected;
extern "C" void host_d9_admission_entry(uint64_t) {}
extern "C" int host_d9_admission_enabled(void) {
    return admission_enabled;
}
extern "C" void host_d9_rejected_draw(const HostD9Draw *d) {
    rejected = *d;
    ++rejected_draws;
}
extern "C" int host_d9_active(void) {
    return 1;
}
extern "C" void host_d9_draw(const HostD9Draw *d) {
    captured = *d;
    ++draws;
}

struct VolumeUpload {
    uint32_t id, slice, level, pitch;
    std::vector<uint8_t> data;
};
static std::unordered_map<uint32_t, HostD9TextureDesc> defined_textures;
static std::vector<VolumeUpload> volume_uploads;
static std::vector<uint32_t> dropped_textures;
extern "C" void host_d9_texture_define(const HostD9TextureDesc *d) {
    defined_textures[d->id] = *d;
}
extern "C" void host_d9_texture_upload(uint32_t id, uint32_t slice, uint32_t level,
                                       const uint8_t *bytes, uint32_t pitch) {
    const auto it = defined_textures.find(id);
    if (it == defined_textures.end() || it->second.kind != HOST_D9_TEX_VOLUME)
        return;
    const uint32_t h = std::max(it->second.height >> level, 1u);
    const auto *p = static_cast<const uint8_t *>(bytes);
    volume_uploads.push_back({id, slice, level, pitch, {p, p + pitch * h}});
}
extern "C" void host_d9_texture_drop(uint32_t id) {
    dropped_textures.push_back(id);
    defined_textures.erase(id);
}
static uint32_t make_volume(uint32_t dev, uint32_t pool = 1, uint32_t fmt = 21, uint32_t usage = 0,
                            uint32_t w = 2, uint32_t h = 2, uint32_t d = 2, uint32_t levels = 0) {
    CHECK(call_method(dev, 24, {w, h, d, levels, usage, fmt, pool, sc(0x600), 0}) == 0);
    CHECK(rd32(sc(0x600)) != 0);
    return rd32(sc(0x600));
}
static void test_volume(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086c;
    const uint32_t tex = make_volume(dev);
    const uint32_t id = com_this(tex)->id;
    CHECK(call_method(tex, 10) == 4); // D3DRTYPE_VOLUMETEXTURE
    CHECK(call_method(tex, 13) == 2);
    CHECK(call_method(tex, 17, {0, sc(0x620)}) == 0);
    const uint32_t expected[] = {21, 2, 0, 1, 2, 2, 2};
    CHECK(!memcmp(gm_ptr(sc(0x620)), expected, sizeof expected));
    CHECK(call_method(tex, 17, {1, sc(0x620)}) == 0);
    CHECK(rd32(sc(0x630)) == 1 && rd32(sc(0x634)) == 1 && rd32(sc(0x638)) == 1);
    CHECK(call_method(tex, 19, {0, sc(0x660), 0, 0}) == 0);
    CHECK(rd32(sc(0x660)) == 8 && rd32(sc(0x664)) == 16);
    const uint32_t data = rd32(sc(0x668));
    for (uint32_t i = 0; i < 32; ++i)
        gm_ptr(data)[i] = uint8_t(i);
    CHECK(call_method(tex, 19, {0, sc(0x680), 0, 0}) == invalid); // no nested lock
    CHECK(call_method(tex, 20, {0}) == 0);
    CHECK(call_method(tex, 20, {0}) == invalid);
    volume_uploads.clear();
    call_method(tex, 9); // PreLoad uploads canonical storage
    CHECK(defined_textures.at(id).depth == 2 && defined_textures.at(id).levels == 2);
    CHECK(volume_uploads.size() == 3);
    for (uint32_t z = 0; z < 2; ++z) {
        const auto &u = volume_uploads[z];
        CHECK(u.id == id && u.slice == z && u.level == 0 && u.pitch == 8 && u.data.size() == 16);
        for (uint32_t i = 0; i < 16; ++i)
            CHECK(u.data[i] == uint8_t(z * 16 + i));
    }
    volume_uploads.clear();
    call_method(tex, 9);
    CHECK(volume_uploads.empty()); // no duplicate unchanged upload
    // A non-origin sub-box returns full pitches and an offset into the same mip.
    const uint32_t box[] = {1, 1, 2, 2, 1, 2};
    memcpy(gm_ptr(sc(0x700)), box, sizeof box);
    CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0x8000}) == 0);
    CHECK(rd32(sc(0x660)) == 8 && rd32(sc(0x664)) == 16);
    CHECK(rd32(rd32(sc(0x668))) == 0x1f1e1d1c);
    wr32(rd32(sc(0x668)), 0xabcdef01);
    CHECK(call_method(tex, 20, {0}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads.empty()); // NO_DIRTY_UPDATE defers publication
    CHECK(call_method(tex, 21, {sc(0x700)}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads.size() == 3);
    CHECK(volume_uploads[1].data[12] == 1 && volume_uploads[1].data[15] == 0xab);
    volume_uploads.clear();
    CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0x10}) == 0);
    wr32(rd32(sc(0x668)), 0); // illegal guest write through READONLY cannot publish
    CHECK(call_method(tex, 20, {0}) == 0);
    CHECK(call_method(tex, 21, {0}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads[1].data[15] == 0xab);
    // Fail before writing outputs or mutating storage for bad extents/pointers/flags.
    for (uint32_t level : {2u, 0xffffffffu}) {
        wr32(sc(0x660), 0xdeadbeef);
        CHECK(call_method(tex, 17, {level, sc(0x660)}) == invalid);
        CHECK(rd32(sc(0x660)) == 0xdeadbeef);
        CHECK(call_method(tex, 19, {level, sc(0x660), 0, 0}) == invalid);
        CHECK(rd32(sc(0x660)) == 0xdeadbeef);
    }
    for (uint32_t off : {0u, 4u, 8u, 12u, 16u, 20u}) {
        memcpy(gm_ptr(sc(0x700)), box, sizeof box);
        wr32(sc(0x700) + off, 0xffffffffu);
        CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0}) == invalid);
    }
    for (uint32_t ptr : {0u, 0xfffffffcu}) {
        CHECK(call_method(tex, 19, {0, ptr, 0, 0}) == invalid);
        CHECK(call_method(tex, 17, {0, ptr}) == invalid);
        CHECK(call_method(tex, 18, {0, ptr}) == invalid);
    }
    CHECK(call_method(tex, 19, {0, sc(0x660), 0xfffffffcu, 0}) == invalid);
    for (uint32_t flags : {0x1000u, 0x2000u, 0x2010u, 0xffffffffu})
        CHECK(call_method(tex, 19, {0, sc(0x660), 0, flags}) == invalid);
    CHECK(call_method(tex, 11, {99}) == 0);
    CHECK(call_method(tex, 12) == 1);
    CHECK(call_method(tex, 11, {0}) == 1);
    CHECK(call_method(tex, 7, {19}) == 0 && call_method(tex, 8) == 19);
    // Prefix interface identity, a real mip interface and parent lifetime.
    const uint32_t base_iid[] = {0x580ca87e, 0x4d541d3c, 0xd3b71d99, 0xce98c2e3};
    const uint32_t tex_iid[] = {0x2518526c, 0x4111e789, 0xef47b9a7, 0xe6138d32};
    const uint32_t vol_iid[] = {0x24f416e6, 0x4aa71f67, 0x3fd38eb8, 0xa128316f};
    memcpy(gm_ptr(sc(0x740)), base_iid, 16);
    CHECK(call_method(tex, 0, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == tex);
    CHECK(call_method(tex, 2) == 1);
    CHECK(call_method(tex, 18, {0, sc(0x600)}) == 0);
    const uint32_t mip = rd32(sc(0x600)), mip_id = com_this(mip)->id;
    CHECK(com_this(tex)->refs == 2);
    CHECK(call_method(mip, 8, {sc(0x620)}) == 0);
    CHECK(!memcmp(gm_ptr(sc(0x620)), expected, sizeof expected));
    memcpy(gm_ptr(sc(0x740)), vol_iid, 16);
    CHECK(call_method(mip, 0, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == mip);
    CHECK(call_method(mip, 2) == 1);
    memcpy(gm_ptr(sc(0x740)), tex_iid, 16);
    CHECK(call_method(mip, 7, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == tex);
    CHECK(call_method(tex, 2) == 2);
    memset(gm_ptr(sc(0x740)), 0xaa, 16);
    CHECK(call_method(mip, 7, {sc(0x740), sc(0x600)}) == 0x80004002u);
    CHECK(rd32(sc(0x600)) == 0);
    CHECK(call_method(tex, 2) == 1); // external mip alone now keeps the resource alive
    CHECK(call_method(mip, 9, {sc(0x660), 0, 0x10}) == 0);
    CHECK(gm_ptr(rd32(sc(0x668)))[31] == 0xab);
    CHECK(call_method(mip, 10) == 0);
    CHECK(call_method(mip, 2) == 0);
    CHECK(!com_get(id) && !com_get(mip_id));
    CHECK(!defined_textures.count(id));
    // The longest dimension determines mip count, even when it is depth.
    const uint32_t deep = make_volume(dev, 1, 28, 0, 1, 1, 4);
    CHECK(call_method(deep, 13) == 3);
    CHECK(call_method(deep, 19, {2, sc(0x660), 0, 0}) == 0);
    CHECK(rd32(sc(0x660)) == 1 && rd32(sc(0x664)) == 1);
    CHECK(call_method(deep, 20, {2}) == 0);
    call_method(deep, 2);
    for (const auto dims : {std::array<uint32_t, 3>{0, 2, 2},
                            {2, 0, 2},
                            {2, 2, 0},
                            {513, 2, 2},
                            {2, 513, 2},
                            {2, 2, 513},
                            {512, 512, 512},
                            {0xffffffffu, 0xffffffffu, 0xffffffffu}}) {
        wr32(sc(0x600), 0xdeadbeef);
        CHECK(call_method(dev, 24, {dims[0], dims[1], dims[2], 0, 0, 21, 1, sc(0x600), 0}) ==
              invalid);
        CHECK(rd32(sc(0x600)) == 0);
    }
    for (const auto args : {std::array<uint32_t, 4>{3, 0, 21, 1},
                            {0, 1, 21, 1},
                            {0, 2, 21, 1},
                            {0, 0x400, 21, 1},
                            {0, 0x200, 21, 1},
                            {0, 0, 75, 1},
                            {0, 0, 0x31545844, 1},
                            {0, 0, 21, 4}}) {
        CHECK(call_method(dev, 24, {2, 2, 2, args[0], args[1], args[2], args[3], sc(0x600), 0}) ==
              invalid);
        CHECK(rd32(sc(0x600)) == 0);
    }
    // Default-pool dynamic locks and system-memory to GPU updates are distinct paths.
    const uint32_t dyn = make_volume(dev, 0, 22, 0x200);
    CHECK(call_method(dyn, 19, {0, sc(0x660), 0, 0x2000}) == 0);
    CHECK(call_method(dyn, 20, {0}) == 0);
    call_method(dyn, 2);
    const uint32_t src = make_volume(dev, 2), dst = make_volume(dev, 0);
    const uint32_t dst_id = com_this(dst)->id;
    CHECK(call_method(dst, 19, {0, sc(0x660), 0, 0}) == invalid);
    CHECK(call_method(dev, 65, {0, src}) == invalid); // system-memory resources cannot be sampled
    CHECK(call_method(src, 19, {0, sc(0x660), 0, 0}) == 0);
    memset(gm_ptr(rd32(sc(0x668))), 0x71, 32);
    CHECK(call_method(dev, 31, {src, dst}) == invalid); // locked resource, no partial update
    CHECK(call_method(src, 20, {0}) == 0);
    CHECK(call_method(dev, 31, {src, dst}) == 0);
    volume_uploads.clear();
    call_method(dst, 9);
    CHECK(volume_uploads.size() == 3 && volume_uploads[0].data[0] == 0x71);
    CHECK(call_method(dev, 65, {0, dst}) == 0);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0x600)}) == 0);
    const uint32_t declaration = rd32(sc(0x600));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    const float tri[] = {-1, 1, .5f, 3, 1, .5f, -1, -3, .5f};
    memcpy(gm_ptr(sc(0x400)), tri, sizeof tri);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(captured.sampler_texture[0] == dst_id);
    call_method(declaration, 2);
    CHECK(call_method(dev, 64, {0, sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == dst);
    call_method(dst, 2);             // returned GetTexture reference
    CHECK(call_method(dst, 2) == 1); // device binding alone retains it
    CHECK(com_get(dst_id));
    CHECK(call_method(dev, 65, {0, 0}) == 0);
    CHECK(!com_get(dst_id));
    call_method(src, 2);
}

struct Bytes {
    std::vector<uint8_t> data;
    uint32_t words(std::initializer_list<uint32_t> list) {
        const uint32_t start = uint32_t(data.size());
        for (uint32_t x : list)
            for (unsigned k = 0; k < 4; ++k)
                data.push_back(uint8_t(x >> (k * 8)));
        return start;
    }
    uint32_t raw(const std::vector<uint8_t> &b) {
        const uint32_t start = uint32_t(data.size());
        data.insert(data.end(), b.begin(), b.end());
        while (data.size() % 4)
            data.push_back(0);
        return start;
    }
    uint32_t text(const char *s) {
        uint32_t start = words({uint32_t(strlen(s) + 1)});
        raw(std::vector<uint8_t>(s, s + strlen(s) + 1));
        return start;
    }
};

// Hand-written CTAB: one BOOL parameter, independent register index in each
// shader. These bytes are a test fixture, not extracted proprietary assets.
static std::vector<uint8_t> shader(bool vertex, const char *name, uint32_t index, uint32_t count) {
    const uint32_t version = vertex ? 0xfffe0200u : 0xffff0200u;
    Bytes table;
    table.words({28, 0, version, 1, 28, 0, 0});
    table.words({64, index << 16, count, 48, 0});
    table.words({1u << 16, 0x00010001u, count, 0});
    table.raw(std::vector<uint8_t>(name, name + strlen(name) + 1));
    Bytes out;
    out.words({version, 0xfffeu | uint32_t((table.data.size() + 4) / 4) << 16, 0x42415443u});
    out.raw(table.data);
    if (vertex)
        out.words({0x0200001fu, 0x80000000u, 0x900f0000u, 0x02000001u, 0xc00f0000u, 0x90e40000u});
    else
        out.words({0x02000001u, 0x800f0800u, 0xa0e40000u});
    out.words({0xffffu});
    return out.data;
}

static std::vector<uint8_t> effect() {
    Bytes b;
    b.words({0}); // offset 0 is the null name
    const uint32_t sn = b.text("Switch"), mn = b.text("Many"), tn = b.text("Main"),
                   pn = b.text("P0");
    const uint32_t st = b.words({1, 0, sn, 0, 0, 1, 1});
    const uint32_t mt = b.words({1, 0, mn, 0, 2, 1, 1});
    const uint32_t sv = b.words({0}), mv = b.words({0, 1});
    const uint32_t vt = b.words({16, 4, 0, 0, 0}), pt = b.words({15, 4, 0, 0, 0});
    const uint32_t vi = b.words({1}), pi = b.words({2});
    const uint32_t start = b.words({2, 1, 0, 2});
    b.words({st, sv, 0, 0, mt, mv, 0, 0});
    b.words({tn, 0, 1, pn, 0, 2, 146, 0, vt, vi, 147, 0, pt, pi});
    b.words({2, 0}); // two shader objects, no extra resources
    for (bool vertex : {true, false}) {
        auto code = shader(vertex, vertex ? "Switch" : "Many", vertex ? 15 : 0, vertex ? 1 : 2);
        b.words({vertex ? 1u : 2u, uint32_t(code.size())});
        b.raw(code);
    }
    Bytes out;
    out.words({0xfeff0901u, start});
    out.raw(b.data);
    return out.data;
}

// Bounded readback (GetRenderTargetData + LockRect DONOTWAIT) against a scripted renderer.
static int rb_ready = 0, rb_sync_reads = 0, rb_forgets = 0, rb_requests = 0;
static uint32_t rb_ticket = 0, rb_source = 0;
extern "C" void host_d9_texture_read_async(uint32_t id, uint32_t, uint32_t, uint32_t ticket) {
    rb_source = id;
    rb_ticket = ticket;
    rb_ready = 0;
    ++rb_requests;
}
extern "C" int host_d9_texture_read_poll(uint32_t ticket, uint8_t *b, uint32_t pitch, uint32_t w,
                                         uint32_t h) {
    if (ticket != rb_ticket)
        return -1;
    if (!rb_ready)
        return 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = 0x11000000u + y * 16 + x;
            memcpy(b + (size_t)y * pitch + x * 4, &v, 4);
        }
    return 1;
}
extern "C" void host_d9_texture_read_forget(uint32_t) {
    ++rb_forgets;
}
extern "C" int host_d9_texture_read(uint32_t, uint32_t, uint32_t, uint8_t *b, uint32_t pitch) {
    ++rb_sync_reads;
    memset(b, 0x55, (size_t)pitch * 4);
    return 1;
}
static void test_state_getters_and_readback(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086c, still_drawing = 0x8876021c, not_found = 0x88760866;
    // Render and sampler states read back what was set, and the documented defaults otherwise.
    CHECK(call_method(dev, 57, {27, 1}) == 0);
    CHECK(call_method(dev, 58, {27, sc(0x10)}) == 0 && rd32(sc(0x10)) == 1);
    CHECK(call_method(dev, 58, {22, sc(0x10)}) == 0 && rd32(sc(0x10)) == 3); // CULLMODE ccw
    CHECK(call_method(dev, 58, {300, sc(0x10)}) == invalid);
    CHECK(call_method(dev, 69, {0, 5, 2}) == 0);
    CHECK(call_method(dev, 68, {0, 5, sc(0x10)}) == 0 && rd32(sc(0x10)) == 2);
    CHECK(call_method(dev, 68, {16, 5, sc(0x10)}) == invalid);
    // Render targets: a referenced surface for 0, NOTFOUND for an unset one.
    CHECK(call_method(dev, 38, {0, sc(0x1c)}) == 0);
    const uint32_t original_target = rd32(sc(0x1c));
    CHECK(original_target != 0);
    CHECK(call_method(dev, 28, {4, 4, 21, 0, 0, 0, sc(0x20), 0}) == 0);
    const uint32_t rt = rd32(sc(0x20));
    CHECK(rt != 0);
    CHECK(call_method(dev, 37, {0, rt}) == 0);
    CHECK(call_method(dev, 38, {0, sc(0x24)}) == 0 && com_this(rd32(sc(0x24))) == com_this(rt));
    call_method(rd32(sc(0x24)), 2);
    wr32(sc(0x24), 0x1234);
    CHECK(call_method(dev, 38, {1, sc(0x24)}) == not_found && rd32(sc(0x24)) == 0);
    // The vertex declaration: the one set, null after clearing it.
    const uint8_t decl[] = {0, 0, 0, 0, 3, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0x28)}) == 0);
    const uint32_t d = rd32(sc(0x28));
    CHECK(call_method(dev, 87, {d}) == 0);
    CHECK(call_method(dev, 88, {sc(0x2c)}) == 0 && com_this(rd32(sc(0x2c))) == com_this(d));
    call_method(rd32(sc(0x2c)), 2);
    CHECK(call_method(dev, 87, {0}) == 0);
    CHECK(call_method(dev, 88, {sc(0x2c)}) == 0 && rd32(sc(0x2c)) == 0);
    call_method(d, 2);
    // GetRenderTargetData queues the copy; a DONOTWAIT lock answers WASSTILLDRAWING until the
    // renderer has it, then locks the delivered image. A lock without the flag waits.
    CHECK(call_method(dev, 36, {4, 4, 21, 2, sc(0x30), 0}) == 0);
    const uint32_t sys = rd32(sc(0x30));
    CHECK(call_method(dev, 32, {rt, sys}) == 0);
    CHECK(rb_requests == 1 && rb_ticket == com_this(sys)->id && rb_source == com_this(rt)->id);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4000}) == still_drawing);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4010}) == still_drawing);
    rb_ready = 1;
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4010}) == 0);
    const uint32_t pitch = rd32(sc(0x40)), bits = rd32(sc(0x44));
    CHECK(pitch >= 16 && rd32(bits) == 0x11000000u && rd32(bits + pitch + 4) == 0x11000011u);
    CHECK(call_method(sys, 14) == 0);
    const int forgets = rb_forgets;
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4000}) == 0); // nothing pending: plain lock
    CHECK(call_method(sys, 14) == 0 && rb_forgets == forgets);
    CHECK(call_method(dev, 32, {rt, sys}) == 0 && rb_requests == 2);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0}) == 0 && rb_sync_reads == 1);
    CHECK(rd32(rd32(sc(0x44))) == 0x55555555u);
    CHECK(call_method(sys, 14) == 0);
    // Sizes must match.
    CHECK(call_method(dev, 36, {8, 4, 21, 2, sc(0x34), 0}) == 0);
    const uint32_t wide = rd32(sc(0x34));
    CHECK(call_method(dev, 32, {rt, wide}) == invalid && rb_requests == 2);
    call_method(wide, 2);
    CHECK(call_method(dev, 32, {rt, sys}) == 0 && rb_requests == 3);
    call_method(sys, 2); // destroyed while pending: the ticket is forgotten
    CHECK(rb_forgets > forgets);
    CHECK(call_method(dev, 37, {0, original_target}) == 0);
    call_method(original_target, 2);
    call_method(rt, 2);
}


// Test155: the four draw calls refuse a primitive count above the MaxPrimitiveCount this device reports in its caps
// (and indexed draws a vertex count above MaxVertexIndex + 1) with D3DERR_INVALIDCALL, before any size arithmetic,
// for every topology, and without growing the process. At the limit they are accepted.
static void test_draw_limits(uint32_t dev) {
    const uint32_t invalid = 0x8876086cu;
    CHECK(call_method(dev, 7, {sc(0x800)}) == 0); // GetDeviceCaps
    const uint32_t max_prims = rd32(sc(0x800) + 180), max_index = rd32(sc(0x800) + 184);
    CHECK(max_prims == 1048575 && max_index == 1048575);
    memset(gm_ptr(sc(0x1000)), 0, 0x1000); // degenerate vertices and indices
    struct rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    for (uint32_t prim = 1; prim <= 6; ++prim) {
        for (uint32_t count : {max_prims + 1, 0x7fffffffu, 0x80000000u, 0x80000000u + 350u, 0xffffffffu}) {
            CHECK(call_method(dev, 83, {prim, count, sc(0x1000), 16}) == invalid);          // DrawPrimitiveUP
            CHECK(call_method(dev, 84, {prim, 0, 3, count, sc(0x1000), 101, sc(0x1000), 16}) == invalid);
            CHECK(call_method(dev, 81, {prim, 0, count}) == invalid);                        // DrawPrimitive
            CHECK(call_method(dev, 82, {prim, 0, 0, 3, 0, count}) == invalid);              // DrawIndexedPrimitive
        }
        // vertex count over the index limit, primitive count fine
        CHECK(call_method(dev, 84, {prim, 0, max_index + 2, 1, sc(0x1000), 101, sc(0x1000), 16}) == invalid);
        CHECK(call_method(dev, 82, {prim, 0, 0, max_index + 2, 0, 1}) == invalid);
        // small valid draws are accepted
        CHECK(call_method(dev, 83, {prim, 1, sc(0x1000), 16}) == 0);
        CHECK(call_method(dev, 84, {prim, 0, 3, 1, sc(0x1000), 101, sc(0x1000), 16}) == 0);
    }
    // at and just below the limit: accepted (point lists, so the vertex count equals the primitive count; the
    // degenerate data lives anywhere in the arena)
    for (uint32_t count : {max_prims - 1, max_prims}) {
        CHECK(call_method(dev, 81, {1, 0, count}) == 0);
        CHECK(call_method(dev, 83, {1, count, 0x00100000u, 16}) == 0);
    }
    getrusage(RUSAGE_SELF, &after);
    // ru_maxrss: bytes on macOS, kilobytes on Linux; either way far below a runaway allocation
    CHECK((uint64_t)(after.ru_maxrss - before.ru_maxrss) < (uint64_t)512 * 1024 * 1024);
}

// Test155: with no depth-stencil surface set, GetDepthStencilSurface returns D3DERR_NOTFOUND and a null pointer
// (Direct3D 9), every time, and makes nothing; with one set it returns that one.
static void test_depth_stencil_get(uint32_t dev) {
    const uint32_t notfound = 0x88760866u, out = sc(0x900), made = sc(0x904);
    CHECK(call_method(dev, 39, {0}) == 0); // SetDepthStencilSurface(NULL)
    for (int i = 0; i < 64; ++i) {
        wr32(out, 0xdeadbeefu);
        CHECK(call_method(dev, 40, {out}) == notfound);
        CHECK(rd32(out) == 0);
    }
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0); // CreateDepthStencilSurface D24S8
    const uint32_t surface = rd32(made);
    CHECK(surface != 0);
    CHECK(call_method(dev, 39, {surface}) == 0);
    CHECK(call_method(dev, 40, {out}) == 0);
    CHECK(rd32(out) != 0);
    if (rd32(out))
        call_method(rd32(out), 2); // Release the reference GetDepthStencilSurface added
    CHECK(call_method(dev, 39, {0}) == 0);
    CHECK(call_method(dev, 40, {out}) == notfound);
    if (surface)
        call_method(surface, 2);
}

// Test157c: depth-stencil reference rules of Direct3D 9 (the audit found the kit diverged in all three):
// SetDepthStencilSurface AddRefs the bound surface (the device keeps it alive after the app releases it), and
// Reset drops only the device's own references, never the app's.
static void test_depth_stencil_refs(uint32_t dev) {
    const uint32_t made = sc(0x910), out = sc(0x914), pp = sc(0x940);
    auto addref = [](uint32_t o) { return call_method(o, 1); };
    auto release = [](uint32_t o) { return call_method(o, 2); };
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0);
    const uint32_t s = rd32(made), s_id = com_this(s)->id;
    CHECK(call_method(dev, 39, {s}) == 0);
    CHECK(addref(s) == 3); // app + device + this
    release(s);
    CHECK(release(s) == 1); // the app lets go; the device still holds it
    CHECK(com_get(s_id) != nullptr);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0);
    CHECK(rd32(out) && com_this(rd32(out)) && com_this(rd32(out))->id == s_id);
    if (rd32(out))
        release(rd32(out));
    CHECK(call_method(dev, 39, {0}) == 0); // unbinding drops the last reference
    CHECK(com_get(s_id) == nullptr);
    // Reset with the automatic depth buffer on, while an app-owned surface is bound
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0);
    const uint32_t e = rd32(made), e_id = com_this(e)->id;
    CHECK(call_method(dev, 39, {e}) == 0);
    memset(gm_ptr(pp), 0, 56);
    wr32(pp, 8);
    wr32(pp + 4, 8);
    wr32(pp + 36, 1);
    wr32(pp + 40, 75);
    CHECK(call_method(dev, 16, {pp}) == 0);
    CHECK(com_get(e_id) != nullptr); // the app's surface survives Reset
    CHECK(addref(e) == 2);           // app + this: the device let go of its binding only
    release(e);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0); // the new automatic depth buffer is bound
    const uint32_t auto1 = rd32(out) && com_this(rd32(out)) ? com_this(rd32(out))->id : 0;
    CHECK(auto1 != 0 && auto1 != e_id);
    if (rd32(out))
        release(rd32(out));
    CHECK(release(e) == 0); // the app's last reference
    CHECK(com_get(e_id) == nullptr);
    // a second Reset replaces the automatic depth buffer; the old one goes once nothing holds it
    CHECK(call_method(dev, 16, {pp}) == 0);
    CHECK(com_get(auto1) == nullptr);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0 && rd32(out));
    if (rd32(out))
        release(rd32(out));
}

// Test159: a texture level shares its texture's lifetime (Direct3D 9): releasing the level gives back the texture
// reference it took, in either release order, and a level obtained twice needs two releases.
static void test_surface_container_refs(uint32_t dev) {
    const uint32_t texp = sc(0x980), surfp = sc(0x984), surf2p = sc(0x988);
    auto release = [](uint32_t o) { return call_method(o, 2); };
    for (int order = 0; order < 2; ++order) {
        CHECK(call_method(dev, 23, {16, 16, 1, 1 /* RENDERTARGET */, 21, 0, texp, 0}) == 0); // CreateTexture
        const uint32_t tex = rd32(texp), tex_id = com_this(tex)->id;
        CHECK(call_method(tex, 18, {0, surfp}) == 0); // GetSurfaceLevel
        const uint32_t surf = rd32(surfp), surf_id = com_this(surf)->id;
        if (order == 0) {
            release(tex);                              // the app lets the texture go first...
            CHECK(com_get(tex_id) != nullptr);         // ...the level keeps it alive
            release(surf);
        } else {
            release(surf);
            CHECK(com_get(tex_id) != nullptr);         // the app still holds the texture
            release(tex);
        }
        CHECK(com_get(tex_id) == nullptr && com_get(surf_id) == nullptr);
    }
    // a level obtained twice, and through GetRenderTarget after SetRenderTarget
    CHECK(call_method(dev, 23, {16, 16, 1, 1, 21, 0, texp, 0}) == 0);
    const uint32_t tex = rd32(texp), tex_id = com_this(tex)->id;
    CHECK(call_method(tex, 18, {0, surfp}) == 0);
    CHECK(call_method(tex, 18, {0, surf2p}) == 0);
    const uint32_t s1 = rd32(surfp), s2 = rd32(surf2p);
    CHECK(call_method(dev, 37, {0, s1}) == 0);    // SetRenderTarget(0, level)
    CHECK(call_method(dev, 38, {0, surfp}) == 0); // GetRenderTarget(0)
    const uint32_t s3 = rd32(surfp);
    CHECK(call_method(dev, 38, {1, surfp}) != 0 || rd32(surfp) == 0); // nothing on slot 1
    CHECK(call_method(dev, 18, {0, 0, 0, surf2p + 4}) == 0); // GetBackBuffer: put the back buffer back on slot 0
    const uint32_t bb = rd32(surf2p + 4);
    CHECK(call_method(dev, 37, {0, bb}) == 0);
    release(bb);
    release(tex);
    release(s1);
    release(s2);
    CHECK(com_get(tex_id) != nullptr); // GetRenderTarget's reference is still outstanding
    release(s3);
    CHECK(com_get(tex_id) == nullptr);
}

static uint32_t make_device() {
    const uint32_t api = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    CHECK(api != 0);
    wr32(sc(0x100), 8);
    wr32(sc(0x104), 8);
    CHECK(call_method(api, 16, {0, 1, 0, 0, sc(0x100), sc(0)}) == 0);
    const uint32_t dev = rd32(sc(0));
    CHECK(dev != 0);
    call_method(api, 2);
    return dev;
}

static void test_device_booleans(uint32_t dev) {
    // D3D9 vtable Set/Get VS B=98/99, PS B=113/114. The actual guest
    // stack and vtable calls catch ABI errors as well as bounds mistakes.
    for (uint32_t set : {98u, 113u}) {
        for (uint32_t i = 0; i < 16; ++i)
            wr32(sc(0x200) + i * 4, i & 1 ? 0xfffffff9u : 0);
        CHECK(call_method(dev, set, {0, sc(0x200), 16}) == 0);
        CHECK(call_method(dev, set + 1, {0, sc(0x300), 16}) == 0);
        CHECK(memcmp(gm_ptr(sc(0x200)), gm_ptr(sc(0x300)), 64) == 0);
        wr32(sc(0x200), 3);
        for (const auto range :
             {std::array<uint32_t, 2>{15, 2}, {16, 1}, {0xffffffffu, 1}, {0, 0xffffffffu}}) {
            CHECK(call_method(dev, set, {range[0], sc(0x200), range[1]}) == 0x8876086cu);
            wr32(sc(0x300), 0xdeadbeefu);
            CHECK(call_method(dev, set + 1, {range[0], sc(0x300), range[1]}) == 0x8876086cu);
            CHECK(rd32(sc(0x300)) == 0xdeadbeefu);
        }
        CHECK(call_method(dev, set + 1, {15, sc(0x300), 1}) == 0);
        CHECK(rd32(sc(0x300)) == 0xfffffff9u); // invalid writes were atomic
        CHECK(call_method(dev, set, {0, 0, 1}) == 0x8876086cu);
        CHECK(call_method(dev, set + 1, {0, 0, 1}) == 0x8876086cu);
        CHECK(call_method(dev, set, {0, 0xfffffffcu, 2}) == 0x8876086cu);
        CHECK(call_method(dev, set + 1, {0, 0xfffffffcu, 2}) == 0x8876086cu);
    }
}

static void test_effect_booleans(uint32_t dev) {
    const auto fx = effect();
    // Minimal loaded PE resource directory. This exercises the same export
    // used by a game, with an allocated guest module instead of a disk image.
    const uint32_t module = heap_alloc(0x1000 + uint32_t(fx.size()), true, 16);
    CHECK(module != 0);
    wr32(module, 0x5a4d);
    wr32(module + 0x3c, 0x40);
    wr32(module + 0x40, 0x4550);
    wr32(module + 0x40 + 0x18 + 0x60 + 16, 0x200);
    const uint32_t root = module + 0x200;
    wr32(root + 12, 1u << 16);
    wr32(root + 16, 10);
    wr32(root + 20, 0x80000020u);
    wr32(root + 0x20 + 12, 1u << 16);
    wr32(root + 0x20 + 16, 1);
    wr32(root + 0x20 + 20, 0x80000040u);
    wr32(root + 0x40 + 12, 1u << 16);
    wr32(root + 0x40 + 16, 1033);
    wr32(root + 0x40 + 20, 0x60);
    wr32(root + 0x60, 0x1000);
    wr32(root + 0x64, uint32_t(fx.size()));
    memcpy(gm_ptr(module + 0x1000), fx.data(), fx.size());
    CHECK(call_shim(tramp("d3dx9_26.dll", "D3DXCreateEffectFromResourceA"),
                    {dev, module, 1, 0, 0, 0, 0, sc(0), 0}) == 0);
    const uint32_t e = rd32(sc(0));
    CHECK(e != 0);
    memcpy(gm_ptr(sc(0x200)), "Switch", 7);
    memcpy(gm_ptr(sc(0x220)), "Many", 5);
    const uint32_t s = call_method(e, 9, {0, sc(0x200)}), m = call_method(e, 9, {0, sc(0x220)});
    CHECK(s != 0 && m != 0 && s != m);
    CHECK(call_method(e, 63, {sc(0x20), 0}) == 0);
    CHECK(rd32(sc(0x20)) == 1);
    CHECK(call_method(e, 64, {0}) == 0);
    D9Pipeline &pl = d9_pipeline(com_this(dev)->id);
    CHECK(pl.vbool[15] == 0 && pl.pbool[0] == 0 && pl.pbool[1] == 1);
    CHECK(!pl.vs.empty() && !pl.ps.empty());
    for (uint32_t value : {1u, 0u, 1u}) {
        CHECK(call_method(e, 22, {s, value}) == 0);
        wr32(sc(0x240), value);
        wr32(sc(0x244), !value);
        CHECK(call_method(e, 24, {m, sc(0x240), 2}) == 0);
        CHECK(call_method(e, 65) == 0);
        CHECK(pl.vbool[15] == int32_t(value));
        CHECK(pl.pbool[0] == int32_t(value) && pl.pbool[1] == int32_t(!value));
    }
    // Test75: incremental CommitChanges (d3dx9.cpp bind_constants). The registers remember which
    // effect and shader wrote them; an unchanged commit keeps the values, a parameter written between
    // two commits reaches the registers, and a device constant setter in between makes the next
    // commit write every constant again (the shim's behaviour before Test75 too).
    CHECK(pl.vbinder.fx == com_this(e)->id && pl.pbinder.fx == com_this(e)->id);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1 && pl.pbool[0] == 1 && pl.pbool[1] == 0);
    wr32(sc(0x260), 0);
    CHECK(call_method(dev, 98, {15, sc(0x260), 1}) == 0); // SetVertexShaderConstantB
    CHECK(pl.vbool[15] == 0 && pl.vbinder.fx == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1 && pl.vbinder.fx == com_this(e)->id);
    CHECK(call_method(e, 22, {s, 0}) == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 0 && pl.pbool[0] == 1);
    CHECK(call_method(e, 22, {s, 1}) == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0)}) == 0);
    const uint32_t declaration = rd32(sc(0));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    const float triangle[] = {-1, 1, 0.5f, 3, 1, 0.5f, -1, -3, 0.5f};
    memcpy(gm_ptr(sc(0x400)), triangle, sizeof triangle);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 1);
    CHECK(captured.vbool[15] == 1 && captured.pbool[0] == 1 && captured.pbool[1] == 0);
    CHECK(captured.admission.serial == 0 && rejected_draws == 0);
    // Finish the legacy first-eight draw descriptions using harmless no-IB
    // calls before supplying deliberately invalid UP pointers below.
    for (int i = 0; i < 3; ++i)
        CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 0 && draws == 1);
    admission_enabled = true;
    CHECK(call_method(dev, 26, {36, 0, 0, 0, sc(0x50), 0}) == 0);
    uint32_t vb = rd32(sc(0x50));
    CHECK(call_method(dev, 100, {0, vb, 0, 12}) == 0);
    CHECK(call_method(dev, 27, {6, 0, 101, 0, sc(0x54), 0}) == 0);
    uint32_t ib = rd32(sc(0x54));
    CHECK(call_method(dev, 104, {ib}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(draws == 2 && captured.admission.serial != 0);
    CHECK(captured.admission.api == 82 && captured.admission.device == dev);
    CHECK(captured.admission.caller != 0);
    CHECK(captured.admission.resource_identity[0] == vb);
    CHECK(captured.admission.resource_identity[8] == ib);
    uint64_t serial = captured.admission.serial;
    CHECK(call_method(dev, 104, {0}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 1 && draws == 2 && rejected.admission.reason == 3);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 82);
    CHECK(rejected.stream[0].buffer == com_this(vb)->id);
    CHECK(rejected.admission.resource_identity[0] == vb);
    CHECK(rejected.target.color[0].id == captured.target.color[0].id);
    CHECK(call_method(dev, 87, {0}) == 0);
    CHECK(call_method(dev, 81, {4, 0, 1}) == 0);
    CHECK(rejected_draws == 2 && draws == 2 && rejected.admission.reason == 2);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 81);
    CHECK(call_method(dev, 87, {declaration}) == 0);
    auto saved_vs = std::move(pl.vs);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(rejected_draws == 3 && draws == 2 && rejected.admission.reason == 1);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 83);
    pl.vs = std::move(saved_vs);
    CHECK(call_method(dev, 83, {4, 1, 0xfffffff0u, 12}) == 0);
    CHECK(rejected_draws == 4 && rejected.admission.reason == 4);
    CHECK(rejected.admission.serial == ++serial);
    CHECK(call_method(dev, 84, {4, 0, 3, 1, 0xfffffff0u, 101, sc(0x400), 12}) == 0);
    CHECK(rejected_draws == 5 && draws == 2 && rejected.admission.reason == 4);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 84);
    CHECK(!rejected.inline_vertices && !rejected.inline_indices);
    CHECK(rejected.admission.inline_indices == 0xfffffff0u);
    CHECK(rejected.admission.inline_vertices == sc(0x400));
    CHECK(rejected.admission.resource_identity[0] == 0);
    CHECK(rejected.stream[0].buffer == 0 && rejected.index_buffer == 0);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 3 && captured.admission.reason == 0);
    CHECK(captured.admission.serial == ++serial && captured.admission.api == 83);
    admission_enabled = false;
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 5 && draws == 3); // opt-out retains old behavior
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 4 && captured.admission.serial == 0);
    call_method(dev, 100, {0, 0, 0, 0});
    call_method(vb, 2);
    call_method(ib, 2);
    call_method(e, 66);
    call_method(e, 67);
    call_method(e, 2);
    call_method(declaration, 2);
}

// Remaster run 13 natural water: the three WORLD program shapes water_find accepts (kit/dx/d3d9_shadergen.cpp), from
// the default MSL translations of the census (work/census13/bay/*.ps.msl), cut to the lines water_find reads.
namespace d9msl {
bool water_find(const std::string &s, size_t *l_end, std::string *call);
bool water_patch(std::string *s, int mode);
}
static const char kWaterHead[] =
    "fragment D9PSOut ps_main(D9V2F vin [[stage_in]], constant float4 *c [[buffer(0)]]) {\n"
    "    float4 odepth = float4(vin.pos.z);\n";
static const char kWaterTail[] = "    oc[0] = ((r[0].xyzw));\n    float4 col = oc[0];\n}\n";
// ps 629cd2e4c94c69f8 (shadow receiver, form 3)
static const char kWater629[] =
    "    r[1] = (tx1.sample(sm1, (((t[3].xyzw) / ((t[3].xyzw)).w)).xy));\n"
    "    r[0] = (tx0.sample(sm0, ((t[0].xyzw)).xy));\n"
    "    { float4 w_ = (float4(1.0 / ((t[3].wwww)).x)); r[1].w = w_.w; }\n"
    "    { float4 w_ = ((t[3].yyyy) * (-(r[1].wwww)) + (c[1].xxxx)); r[1].w = w_.w; }\n"
    "    { float4 w_ = saturate((r[1].wwww) * (c[1].yyyy)); r[1].w = w_.w; }\n"
    "    { float4 w_ = ((r[1].xxxx) + (r[1].wwww)); r[1].w = w_.w; }\n"
    "    { float4 w_ = ((r[0].xyzw) * (iv[1].xyzw)); r[1].xyz = w_.xyz; }\n"
    "    { float4 w_ = saturate((r[1].wwww) + (c[0].xxxx)); r[1].w = w_.w; }\n"
    "    { float4 w_ = ((r[0].xyzw) * (iv[0].xyzw)); r[0].xyz = w_.xyz; }\n"
    "    { float4 w_ = ((r[1].xyzw) * (r[1].wwww) + (r[0].xyzw)); r[0].xyz = w_.xyz; }\n"
    "    { float4 w_ = ((r[0].wwww) * (iv[0].wwww)); r[0].w = w_.w; }\n"
    "    { float4 w_ = ((c[1].zzzz) * (r[0].xyzw) + (t[2].xyzw)); r[0].xyz = w_.xyz; }\n";
// ps cc1387a106b0820e (four-tap shadow, form 2), from the stage-0 sample on
static const char kWaterCc1[] =
    "    r[0] = (tx0.sample(sm0, ((t[0].xyzw)).xy));\n"
    "    { float4 w_ = ((r[1].wwww) * (c[3].zzzz) + (r[2].wwww)); r[1].w = w_.w; }\n"
    "    { float4 w_ = ((r[0].xyzw) * (iv[1].xyzw)); r[1].xyz = w_.xyz; }\n"
    "    { float4 w_ = saturate((r[1].wwww) + (c[1].xxxx)); r[1].w = w_.w; }\n"
    "    { float4 w_ = ((r[0].xyzw) * (iv[0].xyzw)); r[0].xyz = w_.xyz; }\n"
    "    { float4 w_ = ((r[1].xyzw) * (r[1].wwww) + (r[0].xyzw)); r[0].xyz = w_.xyz; }\n"
    "    { float4 w_ = ((r[0].wwww) * (iv[0].wwww)); r[0].w = w_.w; }\n"
    "    { float4 w_ = ((c[3].wwww) * (r[0].xyzw) + (t[2].xyzw)); r[0].xyz = w_.xyz; }\n";
static void test_water_shapes() {
    const std::string w629 = std::string(kWaterHead) + kWater629 + kWaterTail;
    const std::string wcc1 = std::string(kWaterHead) + kWaterCc1 + kWaterTail;
    const std::string kC = "    { float4 w_ = ((r[1].xyzw) * (r[1].wwww) + (r[0].xyzw)); r[0].xyz = w_.xyz; }\n";
    size_t at = 0;
    std::string call;
    // form 3: accepted, the call goes right after C with the shadow r1.w and 1 / c1.z
    CHECK(d9msl::water_find(w629, &at, &call));
    CHECK(at == w629.find(kC) + kC.size());
    CHECK(call.find("d9w_water(r[0].xyz, tc[0].xy, vin.t4.xyz, vin.t5.xyz, vin.t6.xyz, iv[1].xyz, tc[2].xyz, P.pad1, "
                    "saturate(r[1].w), 1.0 / max(c[1].z, 1.0e-3), iv[1].xyz * saturate(r[1].w) + iv[0].xyz") !=
          std::string::npos);
    for (int mode : {1, 2}) {
        std::string p = w629;
        CHECK(d9msl::water_patch(&p, mode));
        const size_t fn = p.find("static float3 d9w_water("), use = p.find("    r[0].xyz = d9w_water(r[0].xyz");
        CHECK(fn != std::string::npos && use != std::string::npos && fn < p.find("fragment D9PSOut ps_main("));
        CHECK(use == p.find(kC) + kC.size());
        CHECK(p.find(mode == 2 ? "1.0e-3), iv[1].xyz * saturate(r[1].w) + iv[0].xyz, 2, float3("
                               : "1.0e-3), iv[1].xyz * saturate(r[1].w) + iv[0].xyz, 1, float3(") != std::string::npos);
    }
    // form 2 still matches as form 2 (brightness c3.w), not as form 3
    CHECK(d9msl::water_find(wcc1, &at, &call));
    CHECK(call.find("1.0 / max(c[3].w, 1.0e-3)") != std::string::npos);
    // refused: a body read of t4..t7 (the pbr vertex variant writes them), no S line, S after C, B anchor twice
    std::string bad = w629;
    bad.insert(bad.find(kC), "    r[2] = vin.t5;\n");
    CHECK(!d9msl::water_find(bad, &at, &call));
    const std::string kS = "    { float4 w_ = saturate((r[1].wwww) + (c[0].xxxx)); r[1].w = w_.w; }\n";
    bad = w629;
    bad.erase(bad.find(kS), kS.size());
    CHECK(!d9msl::water_find(bad, &at, &call));
    bad.insert(bad.find(kC) + kC.size(), kS);
    CHECK(!d9msl::water_find(bad, &at, &call));
    bad = w629;
    const std::string kB = "    { float4 w_ = ((c[1].zzzz) * (r[0].xyzw) + (t[2].xyzw)); r[0].xyz = w_.xyz; }\n";
    bad.insert(bad.find(kB), kB);
    CHECK(!d9msl::water_find(bad, &at, &call));
}

// Remaster run 14 grass blades (kit/host/gpu/metal/grass.h, review/run14/grass-blades-design.md 5): root scatter density,
// triangulation independence, shared-edge rule, slope cut, determinism, the vertex decoder, the cell cache and the two
// shader patches (on a WORLD VS shaped like the census translation, work/census13/bay/090160e8e9a32890.vs.msl).
// GRASS_MSL_OUT=<dir> GRASS_VS=<vs.msl> GRASS_PS=<ps.msl>: also writes the patched census pair for an offline
// `xcrun metal -c` check.
namespace gb {
using remaster_grass::Layout;
using remaster_grass::Mesh;
struct Vtx {
    float p[3], n[3];
    uint8_t col[4];
    float uv[2];
};
static_assert(sizeof(Vtx) == 36, "test vertex");
static const uint8_t kDecl[] = {0, 0, 0, 0,  2, 0, 0, 0,    // POSITION0 FLOAT3 @0
                                0, 0, 12, 0, 2, 0, 3, 0,    // NORMAL0 FLOAT3 @12
                                0, 0, 24, 0, 4, 0, 10, 0,   // COLOR0 D3DCOLOR @24
                                0, 0, 28, 0, 1, 0, 5, 0,    // TEXCOORD0 FLOAT2 @28
                                0xff, 0, 0, 0, 17, 0, 0, 0};
static d9sh::Program program() {
    d9sh::Program p;
    p.ok = true;
    const uint32_t usage[4] = {0, 3, 10, 5};
    for (uint32_t r = 0; r < 4; ++r) {
        d9sh::DclIn d;
        d.type = d9sh::R_INPUT;
        d.index = r;
        d.usage = usage[r];
        p.inputs.push_back(d);
    }
    return p;
}
// An n x n grid of quads over [0, size]^2 (z = slope x), two triangles each, as a list.
struct Grid {
    std::vector<Vtx> v;
    std::vector<uint32_t> tri;
    Mesh m;
    Grid(int n, float size, float slope = 0.0f) {
        const float nl = std::sqrt(1.0f + slope * slope);
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                Vtx x = {};
                x.p[0] = size * i / n; x.p[1] = size * j / n; x.p[2] = slope * x.p[0];
                x.n[0] = -slope / nl; x.n[2] = 1.0f / nl;
                x.col[0] = 10; x.col[1] = 200; x.col[2] = 30; x.col[3] = 255;
                x.uv[0] = x.p[0] * 0.25f; x.uv[1] = x.p[1] * 0.25f;
                v.push_back(x);
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const uint32_t a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
                tri.insert(tri.end(), {a, b, d, a, d, c});
            }
        m.vb[0] = (const uint8_t *)v.data();
        m.vb_bytes[0] = v.size() * sizeof(Vtx);
        m.stride[0] = sizeof(Vtx);
        m.ib = (const uint8_t *)tri.data();
        m.ib_bytes = tri.size() * 4;
        m.index_size = 4;
        m.primitive = 4;
        m.count = (uint32_t)tri.size() / 3;
    }
    std::vector<uint8_t> scatter(const Layout &L, float s, bool jitter = true) const {
        std::vector<uint8_t> out;
        for (int cy = -1; cy <= 2; ++cy)
            for (int cx = -1; cx <= 2; ++cx)
                remaster_grass::scatter_cell(m, L, tri.data(), tri.size() / 3, cx, cy, s, remaster_grass::kCell, 0.0f,
                                             &out, jitter);
        return out;
    }
};
static std::vector<std::pair<float, float>> xy(const std::vector<uint8_t> &r, const Layout &L) {
    std::vector<std::pair<float, float>> o;
    for (size_t at = 0; at + L.stride <= r.size(); at += L.stride) {
        float p[4];
        memcpy(p, r.data() + at + L.in[L.pos].field, 16);
        o.push_back({p[0], p[1]});
    }
    std::sort(o.begin(), o.end());
    return o;
}
// The census WORLD VS shape (prelude cut): inputs, the vv loads, the body's first lines, the output block.
static const char kVS[] =
    "struct D9V2F { float4 pos [[position]]; float4 t7; };\n"
    "struct D9VIn {\n"
    "    float4 v0 [[attribute(0)]];\n"
    "    float4 v1 [[attribute(1)]];\n"
    "    float4 v2 [[attribute(2)]];\n"
    "    float4 v3 [[attribute(3)]];\n"
    "};\n"
    "vertex D9V2F vs_main(D9VIn vin [[stage_in]], constant float4 *c [[buffer(16)]],\n"
    "                     constant D9VSParams &P [[buffer(17)]]) {\n"
    "    float4 ot[8] = {};\n"
    "    float4 vv[16] = {};\n"
    "    vv[0] = vin.v0 * P.ascale[0];\n"
    "    vv[1] = vin.v1 * P.ascale[1];\n"
    "    vv[2] = vin.v2 * P.ascale[2];\n"
    "    vv[3] = vin.v3 * P.ascale[3];\n"
    "    { float4 w_ = ((vv[3].xyzw) + (c[7].xyzw)); ot[0].xy = w_.xy; }\n"
    "    { float4 w_ = ((-(vv[0].xyzw)) + (c[14].xyzw)); r[0].xyz = w_.xyz; }\n"
    "    D9V2F o;\n"
    "    o.pos = opos;\n"
    "    return o;\n"
    "}\n";
static const char kPS[] =
    "fragment D9PSOut ps_main(D9V2F vin [[stage_in]], constant float4 *c [[buffer(0)]]) {\n"
    "    float4 odepth = float4(vin.pos.z);\n"
    "    r[0] = (tx0.sample(sm0, ((t[0].xyzw)).xy));\n"
    "    { float4 w_ = ((r[0].xyzw) * (iv[0].xyzw)); r[0].xyz = w_.xyz; }\n"
    "    oc[0] = ((r[0].xyzw));\n"
    "    float4 col = oc[0];\n"
    "}\n";
static std::string slurp(const char *path) {
    std::string s;
    if (FILE *f = path ? fopen(path, "rb") : nullptr) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;)
            s.append(b, n);
        fclose(f);
    }
    return s;
}
} // namespace gb

static void test_grass_blades() {
    using namespace gb;
    const d9sh::Program vp = program();
    Layout L;
    CHECK(remaster_grass::make_layout(kDecl, sizeof kDecl, vp, &L));
    CHECK(L.n == 4 && L.pos == 0 && L.nrm == 1 && L.tex == 3);
    CHECK(L.in[0].wide && !L.in[1].wide && !L.in[2].wide && L.in[3].wide);
    CHECK(L.stride == 16 + 8 + 8 + 16 + 8 && L.extra == 48);
    {   // a type the decoder does not know: FORMAT
        uint8_t bad[sizeof kDecl];
        memcpy(bad, kDecl, sizeof bad);
        bad[4] = 17;
        Layout B;
        CHECK(!remaster_grass::make_layout(bad, sizeof bad, vp, &B));
    }
    // (a) density on a flat 10 x 10 m quad within 5 %
    const float s = 1.0f / std::sqrt(180.0f);
    const Grid g2(1, 10.0f), g8(2, 10.0f);
    const std::vector<uint8_t> r2 = g2.scatter(L, s);
    const size_t n2 = r2.size() / L.stride;
    CHECK(n2 > 0.95 * 18000 && n2 < 1.05 * 18000);
    // (b) the same quad as 2 or 8 triangles: the same root XY set (and the same bytes, sorted)
    const std::vector<uint8_t> r8 = g8.scatter(L, s);
    CHECK(xy(r2, L) == xy(r8, L));
    // (c) shared edges: no duplicates, also with lattice points exactly on the diagonals (no jitter)
    {
        const std::vector<std::pair<float, float>> a = xy(g8.scatter(L, 0.1f, false), L);
        CHECK(std::adjacent_find(a.begin(), a.end()) == a.end());
        // 100 x 100 lattice points over the quad, the outer edges by the top-left rule (bottom and right out; the top
        // row is k x 0.1 = 10.000000000000002 in double, outside)
        CHECK(a.size() == 9900 && std::binary_search(a.begin(), a.end(), std::make_pair(3.0f, 3.0f)));
        const std::vector<std::pair<float, float>> b8 = xy(r8, L);
        CHECK(std::adjacent_find(b8.begin(), b8.end()) == b8.end());
        size_t diag = 0;
        for (const auto &p : a)
            diag += p.first == p.second;
        CHECK(diag == 99);   // (k / 10, k / 10), k = 1..99 (two shared diagonals and the inner vertex): each once
    }
    // (d) a 70-degree slope gives none; 30 degrees still gives roots
    CHECK(Grid(1, 10.0f, std::tan(70.0f * 3.14159265f / 180.0f)).scatter(L, s).empty());
    CHECK(!Grid(1, 10.0f, std::tan(30.0f * 3.14159265f / 180.0f)).scatter(L, s).empty());
    // (e) determinism; interpolated values: the flat quad's z = 0, normal (0, 0, 1), colour, uv = xy / 4, seed in [0, 1)
    CHECK(g2.scatter(L, s) == r2);
    {
        bool ok = true;
        for (size_t at = 0; at + L.stride <= r2.size(); at += L.stride) {
            float p[4], t[4], e[2];
            _Float16 n[4], cl[4];
            memcpy(p, r2.data() + at, 16);
            memcpy(n, r2.data() + at + 16, 8);
            memcpy(cl, r2.data() + at + 24, 8);
            memcpy(t, r2.data() + at + 32, 16);
            memcpy(e, r2.data() + at + 48, 8);
            ok = ok && p[2] == 0.0f && p[3] == 1.0f && (float)n[2] == 1.0f && std::fabs((float)cl[1] - 200 / 255.0f) < 2e-3f &&
                 std::fabs(t[0] - p[0] * 0.25f) < 1e-4f && std::fabs(t[1] - p[1] * 0.25f) < 1e-4f && e[0] >= 0.0f &&
                 e[0] < 1.0f && p[0] >= 0.0f && p[0] <= 10.0f && p[1] >= 0.0f && p[1] <= 10.0f;
        }
        CHECK(ok);
    }
    // (f) the decoder against vertex_format's Metal formats x ascale
    {
        float o[4];
        auto is = [&](float a, float b, float c, float d) {
            return std::fabs(o[0] - a) < 1e-5f && std::fabs(o[1] - b) < 1e-5f && std::fabs(o[2] - c) < 1e-5f &&
                   std::fabs(o[3] - d) < 1e-5f;
        };
        const float f4[4] = {1.5f, -2.0f, 3.0f, 4.0f};
        CHECK(remaster_grass::decode_input(0, (const uint8_t *)f4, o) && is(1.5f, 0, 0, 1));
        CHECK(remaster_grass::decode_input(2, (const uint8_t *)f4, o) && is(1.5f, -2, 3, 1));
        CHECK(remaster_grass::decode_input(3, (const uint8_t *)f4, o) && is(1.5f, -2, 3, 4));
        const uint8_t c4[4] = {0x10, 0x20, 0x30, 0x40};
        CHECK(remaster_grass::decode_input(4, c4, o) && is(0x30 / 255.0f, 0x20 / 255.0f, 0x10 / 255.0f, 0x40 / 255.0f));
        CHECK(remaster_grass::decode_input(5, c4, o) && is(16, 32, 48, 64));
        CHECK(remaster_grass::decode_input(8, c4, o) && is(16 / 255.0f, 32 / 255.0f, 48 / 255.0f, 64 / 255.0f));
        const int16_t sh[4] = {-32768, 100, 32767, -32767};
        CHECK(remaster_grass::decode_input(6, (const uint8_t *)sh, o) && is(-32767, 100, 0, 1));
        CHECK(remaster_grass::decode_input(7, (const uint8_t *)sh, o) && is(-32767, 100, 32767, -32767));
        CHECK(remaster_grass::decode_input(9, (const uint8_t *)(sh + 2), o) && is(1, -1, 0, 1));
        CHECK(remaster_grass::decode_input(10, (const uint8_t *)sh, o) && is(-1, 100 / 32767.0f, 1, -1));
        const uint16_t us[4] = {65535, 0, 32768, 1};
        CHECK(remaster_grass::decode_input(11, (const uint8_t *)us, o) && is(1, 0, 0, 1));
        CHECK(remaster_grass::decode_input(12, (const uint8_t *)us, o) && is(1, 0, 32768 / 65535.0f, 1 / 65535.0f));
        const uint32_t u3 = 1023u | 5u << 10 | 0u << 20 | 3u << 30, d3 = 511u | 0x201u << 10 | 0u << 20 | 3u << 30;
        CHECK(remaster_grass::decode_input(13, (const uint8_t *)&u3, o) && is(1023, 5, 0, 1));
        CHECK(remaster_grass::decode_input(14, (const uint8_t *)&d3, o) && is(1, -1, 0, -1));
        const _Float16 h4[4] = {(_Float16)1.5f, (_Float16)-2.0f, (_Float16)0.25f, (_Float16)8.0f};
        CHECK(remaster_grass::decode_input(15, (const uint8_t *)h4, o) && is(1.5f, -2, 0, 1));
        CHECK(remaster_grass::decode_input(16, (const uint8_t *)h4, o) && is(1.5f, -2, 0.25f, 8));
        CHECK(!remaster_grass::decode_input(17, c4, o) && remaster_grass::format_bytes(17) == 0);
        CHECK(remaster_grass::format_bytes(13) == 4 && remaster_grass::format_bytes(16) == 8);
    }
    // the cell cache: builds near cells once, draws them, evicts them when the eye leaves
    {
        remaster_grass::Blades b;
        b.init(3);
        std::vector<uint8_t> arena(b.page.size() * remaster_grass::kPage);
        b.base = arena.data();
        b.tick(1.0f);
        std::vector<remaster_grass::Run> runs;
        remaster_grass::Skip why = remaster_grass::FORMAT;
        const float eye[3] = {5.0f, 5.0f, 1.5f};
        CHECK(b.prepare(42, g8.m, kDecl, sizeof kDecl, vp, eye, &runs, &why));
        CHECK(!runs.empty() && b.cells_built == 4 && b.roots_built > 0.95 * 18000 && b.roots_drawn == b.roots_built);
        CHECK(!memcmp(arena.data() + runs[0].offset + 48 + 4, &b.time, 4));   // birth time
        b.tick(1.1f);
        CHECK(b.prepare(42, g8.m, kDecl, sizeof kDecl, vp, eye, &runs, &why) && b.cells_built == 4);
        const float away[3] = {500.0f, 5.0f, 1.5f};
        b.tick(1.2f);
        CHECK(b.prepare(42, g8.m, kDecl, sizeof kDecl, vp, away, &runs, &why) && runs.empty());
        b.tick(1.3f);
        CHECK(b.chunks[42].cells.empty() && b.pages_used() == 0);
        const D9GrassBlade G = b.params(0.001f);
        CHECK(G.fade[0] == 25.0f && G.fade[1] == 35.0f && G.misc[1] == 3.0f && G.eye[0] == 500.0f);
    }
    // (g) the blade VS patch: once, [[vertex_id]] / buffer(19) / attribute(16), refused when t<slot> is written
    {
        std::string v = kVS;
        CHECK(d9msl::grass_blade_vs_patch(&v, 0, 1, 3, 7));
        CHECK(v.find("uint g_vid [[vertex_id]]") != std::string::npos && v.find("[[buffer(19)]]") != std::string::npos &&
              v.find("float4 g_r [[attribute(16)]]") != std::string::npos);
        CHECK(v.find("        vv[0].xyz = g_root + ") != std::string::npos && v.find("ot[7] = float4(g_t, g_seed") != std::string::npos);
        CHECK(v.find("g_tx.sample(g_sm, vv[3].xy, level(2.0))") != std::string::npos);
        CHECK(v.find("vv[0].xyz = g_root") < v.find("{ float4 w_ = ((vv[3].xyzw) + (c[7].xyzw))"));   // before the body
        CHECK(!d9msl::grass_blade_vs_patch(&v, 0, 1, 3, 7));   // once
        std::string w = kVS;
        w.insert(w.find("    D9V2F o;\n"), "    ot[7] = vv[2];\n");
        CHECK(!d9msl::grass_blade_vs_patch(&w, 0, 1, 3, 7));
        CHECK(d9msl::grass_blade_vs_patch(&w, 0, 1, 3, 6));
        std::string x = kVS;
        CHECK(!d9msl::grass_blade_vs_patch(&x, 0, 1, 5, 7));   // no TEXCOORD0 load
        // (h) blade PS: no discard, no alpha change; the shells keep their cut-out and get the fade-in decode
        std::string p = std::string(kPS), q = p;
        CHECK(d9msl::grass_ps_patch(&p, 7, true));
        CHECK(p.find("discard_fragment") == std::string::npos && p.find("col.a =") == std::string::npos &&
              p.find("col.rgb *= g_k;") > p.find("    float4 col = oc[0];\n") && p.find("tc[7].x") != std::string::npos);
        CHECK(d9msl::grass_ps_patch(&q, 7) && q.find("g_end = floor(P.pad1 / 256.0)") != std::string::npos &&
              q.find("discard_fragment") != std::string::npos);
        std::string r = std::string(kPS);
        r.insert(r.find("    float4 col"), "    r[1] = tc[7];\n");
        CHECK(!d9msl::grass_ps_patch(&r, 7, true));
        // variant keys differ from the plain and shell keys
        d9msl::VertexVariant a, bb, sh;
        bb.grass_blade = 7;
        sh.grass = 7;
        d9msl::PixelVariant pa, pb;
        pb.grass_blade = 7;
        CHECK(a.key() != bb.key() && sh.key() != bb.key() && pa.key() != pb.key());
    }
    // offline Metal compile input: the census pair, patched
    if (const char *out = getenv("GRASS_MSL_OUT")) {
        std::string v = gb::slurp(getenv("GRASS_VS")), f = gb::slurp(getenv("GRASS_PS"));
        const bool vok = d9msl::grass_blade_vs_patch(&v, 0, 1, 3, 7), fok = d9msl::grass_ps_patch(&f, 7, true);
        std::string sv = gb::slurp(getenv("GRASS_PS"));
        const bool sok = d9msl::grass_ps_patch(&sv, 7);
        CHECK(vok && fok && sok);
        const std::pair<const char *, const std::string *> files[3] = {
            {"/blade.vs.metal", &v}, {"/blade.ps.metal", &f}, {"/shell.ps.metal", &sv}};
        for (const auto &kv : files)
            if (FILE *fp = fopen((std::string(out) + kv.first).c_str(), "wb")) {
                fputs(kv.second->c_str(), fp);
                fclose(fp);
            }
    }
}


// Remaster run 14 foundation F1/F3 (review/run14/foundation-design.md 4.1 / 4.3 offline checks): the clip-register
// analysis, the VP plurality pick, the projection-shape check, the reprojection and the Halton jitter.
namespace fndt {
using namespace remaster_fnd;
// vs_2_0: dcl_position v0; dp4 oPos.{x,y,z,w}, v0, c[r0..r3]; optionally dp4 oT0.{x,y,z,w}, v0, c[t0..t0+3]
static std::vector<uint8_t> vs(const int r[4], int t0) {
    std::vector<uint32_t> w = {0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u};
    for (int k = 0; k < 4; ++k)
        w.insert(w.end(), {0x03000009u, 0xC0000000u | (1u << (16 + k)), 0x90E40000u, 0xA0E40000u | (uint32_t)r[k]});
    for (int k = 0; t0 >= 0 && k < 4; ++k)
        w.insert(w.end(), {0x03000009u, 0xE0000000u | (1u << (16 + k)), 0x90E40000u, 0xA0E40000u | (uint32_t)(t0 + k)});
    w.push_back(0x0000FFFFu);
    std::vector<uint8_t> b(w.size() * 4);
    memcpy(b.data(), w.data(), b.size());
    return b;
}
static M4 translate(double x, double y, double z) {
    M4 t = identity();
    t.m[3][0] = x, t.m[3][1] = y, t.m[3][2] = z;
    return t;
}
static M4 yaw(double a) {   // about the view's y axis (y down, z forward)
    M4 r = identity();
    r.m[0][0] = std::cos(a), r.m[0][2] = -std::sin(a), r.m[2][0] = std::sin(a), r.m[2][2] = std::cos(a);
    return r;
}
// world -> view for a camera at c (world), looking along +z rotated by `a`, the game's y-down view
static M4 view(double cx, double cy, double cz, double a) { return mul(translate(-cx, -cy, -cz), yaw(a)); }
static M4 proj(double fov, double aspect, double n, double f, bool reversed = false) {
    M4 p{};
    const double ys = 1.0 / std::tan(0.5 * fov);
    p.m[0][0] = ys / aspect;
    p.m[1][1] = -ys;   // view y down -> clip y up
    p.m[2][2] = reversed ? -n / (f - n) : f / (f - n);
    p.m[2][3] = 1.0;
    p.m[3][2] = reversed ? n * f / (f - n) : -n * f / (f - n);
    return p;
}
static void block_of(const M4 &m, float *c) {   // the 4 registers the game would set (transposed)
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            c[4 * j + i] = (float)m.m[i][j];
}
static void rows_of(const M4 &m, float *f) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            f[4 * i + j] = (float)m.m[i][j];
}
// a world point to its Metal pixel (continuous, pixel centres at i + 0.5 - 0.5 = i) and stored depth
static bool pixel_of(const M4 &vp, const double X[3], uint32_t w, uint32_t h, double hx, double hy, double *i, double *j,
                     double *d) {
    const double v[4] = {X[0], X[1], X[2], 1.0};
    double o[4];
    xform(v, vp, o);
    if (!(o[3] > 0.0))
        return false;
    *i = (o[0] / o[3] + hx + 1.0) * 0.5 * w - 0.5;
    *j = (1.0 - o[1] / o[3] - hy) * 0.5 * h - 0.5;
    *d = o[2] / o[3];
    return true;
}
} // namespace fndt

static void test_foundation() {
    using namespace fndt;
    // (1) vertex_clip_regs agrees with vertex_clip_shadow; palettes / non-consecutive rows are refused
    {
        const int r[4] = {4, 5, 6, 7}, bad[4] = {4, 5, 7, 6};
        uint32_t a = 99, b = 99, c = 99;
        const d9sh::Program &both = d9sh::program_for(vs(r, 8));
        CHECK(both.ok);
        CHECK(d9msl::vertex_clip_shadow(both, &a, &b) && a == 4 && b == 8);
        CHECK(d9msl::vertex_clip_regs(both, &c) && c == a);
        const d9sh::Program &clip_only = d9sh::program_for(vs(r, -1));
        CHECK(!d9msl::vertex_clip_shadow(clip_only, &a, &b));
        CHECK(d9msl::vertex_clip_regs(clip_only, &c) && c == 4);
        const d9sh::Program &shuffled = d9sh::program_for(vs(bad, -1));
        CHECK(!d9msl::vertex_clip_regs(shuffled, &c));
    }
    const double fov = 2.0 * 3.14159265358979 * 13000.0 / 65536.0, aspect = 2560.0 / 1440.0, n = 0.5, f = 10000.0;
    const M4 P = proj(fov, aspect, n, f);
    const M4 V = view(1234.5, -356.25, 41.0, 0.7);
    const M4 VP = mul(V, P);
    float blk[16], vrows[16];
    block_of(VP, blk);
    rows_of(V, vrows);
    // (2) the plurality: 12 scenery draws with VP, 8 instanced (40 %) with W x VP (W a translation each)
    {
        Plurality pl;
        for (int k = 0; k < 20; ++k) {
            pl.world();
            if (k % 5 < 3) {
                pl.add(blk);
            } else {
                float ib[16];
                block_of(mul(translate(5.0 * k, 0, 2.0), VP), ib);
                pl.add(ib);
            }
        }
        const Plurality::Result r = pl.pick();
        CHECK(r.valid && !r.tie && r.count == 12 && r.world == 20 && r.distinct == 9);
        CHECK(r.best && !memcmp(r.best->block, blk, sizeof blk));
        M4 inv;
        CHECK(inverse(from_const_block(r.best->block), &inv));
        CHECK(pl.identity_draws(inv, 1e-4) == 12);
        // a tie: the block seen first wins (counted)
        Plurality t;
        float other[16];
        block_of(mul(translate(9, 9, 9), VP), other);
        for (int k = 0; k < 16; ++k) {   // 8 and 8
            t.world();
            t.add(k & 1 ? blk : other);
        }
        const Plurality::Result rt = t.pick();
        CHECK(rt.tie && rt.best && !memcmp(rt.best->block, other, sizeof other) && rt.valid);
        // no WORLD draws; too few draws; too small a share
        Plurality e;
        CHECK(!e.pick().valid && !e.pick().best);
        Plurality few;
        for (int k = 0; k < 7; ++k) {
            few.world();
            few.add(blk);
        }
        CHECK(!few.pick().valid);
        Plurality thin;
        for (int k = 0; k < 40; ++k)
            thin.world();
        for (int k = 0; k < 9; ++k)
            thin.add(blk);
        CHECK(!thin.pick().valid);   // 9 of 40 = 22.5 % < 25 %
        // CONF14B: VP split into 12 last-bit variants (the game's per-object WVP rounding), 3 instanced blocks of 15
        // draws each: an exact hash sees no plurality VP; the clusters do, and with the camera the VP cluster wins
        // although an instanced block is larger
        Plurality fr;
        for (int k = 0; k < 12; ++k) {
            float v[16];
            memcpy(v, blk, sizeof v);
            v[(k * 5) % 16] = std::nextafter(v[(k * 5) % 16], k & 1 ? 1e30f : -1e30f);
            CHECK(same_block(v, blk));
            fr.world();
            fr.add(v);
        }
        for (int k = 0; k < 45; ++k) {
            float ib[16];
            block_of(mul(translate(5.0 + 7.0 * (k % 3), 0, 2.0), VP), ib);
            CHECK(!same_block(ib, blk));
            fr.world();
            fr.add(ib);
        }
        CHECK(fr.pick().best && !same_block(fr.pick().best->block, blk));   // the exact-hash plurality misses VP
        M4 vg;
        CHECK(inverse(from_rows(vrows), &vg));
        const Pick nocam = pick_clusters(fr, 8, 0.25, nullptr, 0.0);
        CHECK(nocam.clusters == 4 && nocam.distinct == 15 && nocam.top[0] == 15 && nocam.top[3 - 1] == 15);
        CHECK(nocam.valid && !nocam.by_camera && !same_block(nocam.block, blk));   // the design's rule alone: wrong pick
        const Pick cam = pick_clusters(fr, 8, 0.25, &vg, 1300.0);
        CHECK(cam.valid && cam.by_camera && cam.rank == 3 && cam.count == 12 && same_block(cam.block, blk));
        // no WORLD draws; a camera but no cluster of its projection: the share rule decides
        Plurality none;
        CHECK(!pick_clusters(none, 8, 0.25, &vg, 1300.0).valid);
        Plurality inst;
        for (int k = 0; k < 20; ++k) {
            float ib[16];
            block_of(mul(translate(5.0, 1.0, 2.0), VP), ib);
            inst.world();
            inst.add(ib);
        }
        const Pick only_inst = pick_clusters(inst, 8, 0.25, &vg, 1300.0);
        CHECK(only_inst.valid && !only_inst.by_camera && !only_inst.form_any);   // valid, then G/K disagree resets
    }
    // (3) the projection shape from the float data the kit gets: P = V_g^-1 x VP_k
    {
        M4 vinv;
        CHECK(inverse(from_rows(vrows), &vinv));
        const M4 Pk = mul(vinv, from_const_block(blk));
        double c[3];
        camera_position(from_rows(vrows), c);
        CHECK(std::fabs(c[0] - 1234.5) < 1e-3 && std::fabs(c[1] + 356.25) < 1e-3 && std::fabs(c[2] - 41.0) < 1e-3);
        const double tr = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        const ProjCheck pc = check_projection(Pk, fov, aspect, n, f, tr);
        CHECK(pc.agree() && pc.y_down && !pc.reversed);
        // CONF14B: the mod's widescreen fix scales the projection (|P11| = 1.21 / tan(fov/2), P00 by (4/3)/aspect/1.047)
        // and its far plane is not the camera's: still the same camera (info flags only)
        M4 ws = Pk;
        ws.m[1][1] *= 1.2122;
        ws.m[0][0] *= 0.75 / 1.047486;
        const ProjCheck pw = check_projection(ws, fov, aspect, n, 15000.0, tr);
        CHECK(pw.agree() && !pw.fov_ok && !pw.aspect_ok && !pw.far_ok);
        CHECK(std::fabs(pc.near - n) < 1e-3 * n && std::fabs(pc.far - f) < 2e-2 * f && std::fabs(pc.fov - fov) < 1e-5);
        CHECK(!check_projection(Pk, fov * 1.01, aspect, n, f, tr).fov_ok);
        CHECK(!check_projection(Pk, fov, 4.0 / 3.0, n, f, tr).aspect_ok);
        CHECK(!check_projection(Pk, fov, aspect, 1.0, f, tr).near_ok);
        M4 sheared = Pk;   // an off-centre / non-projective P is not the form
        sheared.m[2][0] = 0.01;
        CHECK(!check_projection(sheared, fov, aspect, n, f, tr).form);
        CHECK(check_projection(proj(fov, aspect, n, f, true), fov, aspect, n, f, 0.0).reversed);
        CHECK(!check_projection(mul(view(0, 0, 0, 0.3), P), fov, aspect, n, f, 0.0).form);   // a VP that is not P
    }
    // (4) reprojection: self test, parked camera (float data) gives 0, known translation and yaw match the direct
    // projection to 1e-3 px, the sky (rotation only) does not move under translation
    {
        CHECK(self_test(from_const_block(blk)) < 1e-5);
        const uint32_t w = 2560, h = 1440;
        const double hx = (63.0 / 64.0) / w, hy = -(63.0 / 64.0) / h;
        M4 R;
        CHECK(reprojection(from_const_block(blk), from_const_block(blk), &R));
        double mx = 0.0;
        for (double i : {0.0, 640.0, 1279.0, 2559.0})
            for (double j : {0.0, 719.0, 1439.0})
                for (double d : {0.2, 0.99, 0.99995}) {
                    double ax, ay;
                    CHECK(motion(R, i, j, d, w, h, hx, hy, &ax, &ay));
                    mx = std::max(mx, std::max(std::fabs(ax), std::fabs(ay)));
                }
        CHECK(mx < 1e-4);
        const M4 Vs[2] = {view(1234.8, -356.25, 41.4, 0.7), view(1234.5, -356.25, 41.0, 0.7 + 0.02)};
        for (const M4 &Vc : Vs) {
            const M4 cur = mul(Vc, P);
            CHECK(reprojection(cur, VP, &R));
            int tested = 0;
            double err = 0.0;
            for (int k = 0; k < 64; ++k) {
                const double dist = 3.0 + 40.0 * (k % 8), side = -0.5 + (k / 8) / 7.0;
                // a point in front of the current camera: back from its view space
                M4 vinv;
                CHECK(inverse(Vc, &vinv));
                const double pv[4] = {side * dist, -0.3 * side * dist, dist, 1.0};
                double X[4];
                xform(pv, vinv, X);
                double i, j, d, pi, pj, pd;
                if (!pixel_of(cur, X, w, h, hx, hy, &i, &j, &d) || !pixel_of(VP, X, w, h, hx, hy, &pi, &pj, &pd))
                    continue;
                double ax, ay, prev_d;
                CHECK(motion(R, i, j, d, w, h, hx, hy, &ax, &ay, &prev_d));
                err = std::max({err, std::fabs(ax - (pi - i)), std::fabs(ay - (pj - j))});
                CHECK(std::fabs(prev_d - pd) < 1e-9);
                ++tested;
            }
            CHECK(tested >= 48 && err < 1e-3);
        }
        // sky: translation only, rotation-only matrices -> no motion at depth 1
        const M4 Vt = view(1240.0, -356.25, 41.0, 0.7);
        M4 Rs;
        CHECK(reprojection(mul(rotation_only(Vt), P), mul(rotation_only(V), P), &Rs));
        double ax, ay;
        CHECK(motion(Rs, 100.0, 200.0, 1.0, w, h, hx, hy, &ax, &ay) && std::fabs(ax) < 1e-6 && std::fabs(ay) < 1e-6);
        CHECK(std::fabs(linear_depth(P.m[2][2] + P.m[3][2] / 25.0, n, f) - 25.0) < 1e-6);
    }
    // (5) Halton(2,3) jitter: the table, the phase counts, the mean, the halfpix size and sign
    {
        CHECK(halton(1, 2) == 0.5 && halton(2, 2) == 0.25 && halton(3, 2) == 0.75 && halton(4, 2) == 0.125);
        CHECK(std::fabs(halton(1, 3) - 1.0 / 3) < 1e-12 && std::fabs(halton(2, 3) - 2.0 / 3) < 1e-12 &&
              std::fabs(halton(3, 3) - 1.0 / 9) < 1e-12);
        CHECK(jitter_phases(1.0) == 8 && jitter_phases(0.0) == 8 && jitter_phases(1.5) == 18 && jitter_phases(2.0) == 32);
        double sx = 0.0, sy = 0.0, jx, jy;
        jitter(0, &jx, &jy);
        CHECK(jx == 0.0 && std::fabs(jy - (1.0 / 3 - 0.5)) < 1e-12);
        for (uint32_t k = 0; k < 8; ++k) {
            jitter(k, &jx, &jy);
            CHECK(jx >= -0.5 && jx < 0.5 && jy >= -0.5 && jy < 0.5);
            sx += jx;
            sy += jy;
        }
        CHECK(std::fabs(sx / 8) < 0.07 && std::fabs(sy / 8) < 0.07);
        float dx, dy;
        jitter_halfpix(0.25, 0.25, 1000.0, 500.0, &dx, &dy);
        CHECK(std::fabs(dx - 0.0005f) < 1e-9f && std::fabs(dy + 0.001f) < 1e-9f);
        // a halfpix change of dx moves the geometry by dx x w / 2 pixels: the requested jitter
        CHECK(std::fabs(dx * 1000.0 / 2.0 - 0.25) < 1e-6 && std::fabs(-dy * 500.0 / 2.0 - 0.25) < 1e-6);
    }
    // (6) history resets
    {
        Camera a, b;
        a.have = b.have = true;
        a.V = V;
        a.fov = b.fov = fov;
        a.frame = 10;
        b.frame = 11;
        b.V = view(1234.5 + 3.0, -356.25, 41.0, 0.7);
        CHECK(camera_resets(a, b) == 0);
        b.V = view(1234.5 + 11.0, -356.25, 41.0, 0.7);
        CHECK(camera_resets(a, b) == RESET_JUMP);
        b.V = V;
        b.fov = fov + 3.0 * 3.14159265 / 180.0;
        CHECK(camera_resets(a, b) == RESET_FOV);
        b.fov = fov;
        b.frame = 10;
        b.flags = 1;
        CHECK(camera_resets(a, b) == (RESET_PAUSE | RESET_CUT));
        CHECK(camera_resets(a, Camera{}) == RESET_NO_CAMERA);
    }
}

// Remaster run 14 B1 height fog (host/gpu/metal/atmosphere.h): the maths twin of the Metal pass against an
// independent double reference (numeric integration), zero density = the exact picture, the height falloff, the
// camera reconstruction from a WORLD view-projection, the start distance, the opacity cap and the glow direction.
static double atm_ref_density(double h, double rho, double base, double H) {
    return rho * (h <= base ? 1.0 : std::exp(-(h - base) / H));
}
static double atm_ref_tau(double h0, double dz, double s, const double l[2][3]) {
    const int n = 20000;
    double tau = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = (i + 0.5) * s / n, h = h0 + t * dz;
        tau += (atm_ref_density(h, l[0][0], l[0][1], l[0][2]) + atm_ref_density(h, l[1][0], l[1][1], l[1][2])) * s / n;
    }
    return tau;
}
static remaster_fnd::M4 atm_test_vp(const double e[3], const double f_in[3], double fov, double aspect, double zn,
                                    double zf) {
    double f[3], r[3], dn[3];
    const double fl = std::sqrt(f_in[0] * f_in[0] + f_in[1] * f_in[1] + f_in[2] * f_in[2]);
    for (int i = 0; i < 3; ++i)
        f[i] = f_in[i] / fl;
    // right = f x up (z up), down = f x right
    r[0] = f[1];
    r[1] = -f[0];
    r[2] = 0.0;
    const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    for (int i = 0; i < 3; ++i)
        r[i] /= rl;
    dn[0] = f[1] * r[2] - f[2] * r[1];
    dn[1] = f[2] * r[0] - f[0] * r[2];
    dn[2] = f[0] * r[1] - f[1] * r[0];
    remaster_fnd::M4 V{}, P{};
    for (int i = 0; i < 3; ++i) {
        V.m[i][0] = r[i];
        V.m[i][1] = dn[i];
        V.m[i][2] = f[i];
    }
    V.m[3][0] = -(e[0] * r[0] + e[1] * r[1] + e[2] * r[2]);
    V.m[3][1] = -(e[0] * dn[0] + e[1] * dn[1] + e[2] * dn[2]);
    V.m[3][2] = -(e[0] * f[0] + e[1] * f[1] + e[2] * f[2]);
    V.m[3][3] = 1.0;
    const double ty = std::tan(fov / 2);
    P.m[0][0] = 1.0 / (ty * aspect);
    P.m[1][1] = -1.0 / ty;   // view y down -> clip y up (the game's camera)
    P.m[2][2] = zf / (zf - zn);
    P.m[3][2] = -zn * zf / (zf - zn);
    P.m[2][3] = 1.0;
    return remaster_fnd::mul(V, P);
}
static void test_atmosphere() {
    using namespace remaster_atm;
    // 1. one layer, analytic vs numeric: level, up, down, crossing the base both ways, camera below the base
    {
        const double l[2][3] = {{0.0, 0.0, 1.0}, {4e-3, 25.0, 15.0}};
        const float cases[][3] = {{30, 0, 800}, {10, 0, 800}, {90, -0.05f, 1500}, {10, 0.08f, 1500}, {200, -0.2f, 1200},
                                  {24.99f, 0.0001f, 900}, {60, 0.3f, 300}, {25, -0.0005f, 2000}, {5, -0.3f, 500}};
        for (const auto &c : cases) {
            const double ref = atm_ref_tau(c[0], c[1], c[2], l);
            const double got = atm_layer(c[0], c[1], c[2], 4e-3f, 25.0f, 15.0f);
            CHECK(std::fabs(got - ref) <= 1e-3 * std::max(ref, 1e-3));
        }
        // height falloff: a level ray at base + k H has exp(-k) of the density at the base; below the base: flat
        const float at_base = atm_layer(25.0f, 0.0f, 1000.0f, 4e-3f, 25.0f, 15.0f);
        CHECK(std::fabs(at_base - 4.0f) < 1e-4f);
        for (int k = 1; k <= 4; ++k)
            CHECK(std::fabs(atm_layer(25.0f + 15.0f * k, 0.0f, 1000.0f, 4e-3f, 25.0f, 15.0f) / at_base -
                            (float)std::exp(-k)) < 1e-4f);
        CHECK(atm_layer(5.0f, 0.0f, 1000.0f, 4e-3f, 25.0f, 15.0f) == at_base);
        CHECK(atm_layer(1500.0f, 0.0f, 1000.0f, 4e-3f, 25.0f, 15.0f) >= 0.0f);   // underflow, no NaN
        CHECK(std::isfinite(atm_layer(1500.0f, -1.0f, 1400.0f, 4e-3f, 25.0f, 15.0f)));
        CHECK(atm_layer(30.0f, 0.0f, 0.0f, 4e-3f, 25.0f, 15.0f) == 0.0f && atm_layer(30.0f, 0.0f, 100.0f, 0.0f, 25.0f, 15.0f) == 0.0f);
    }
    // 2. the camera from a WORLD view-projection, and the whole pixel against the reference
    const double eye[3] = {2714.7, 918.9, 92.0}, fwd[3] = {0.0, -1.0, -0.02};
    const remaster_fnd::M4 vp = atm_test_vp(eye, fwd, 1.0, 16.0 / 9.0, 0.5, 3000.0);
    Params p{};
    p.a[0] = 0.3f, p.a[1] = -0.8f, p.a[2] = 0.2f, p.a[3] = 0.75f;          // sun, g
    p.a[4] = 0.6e-4f, p.a[5] = 1.1e-4f, p.a[6] = 2.4e-4f, p.a[11] = 0.6f;    // Br (blue), sun intensity
    p.a[8] = 1.0e-4f, p.a[9] = 1.0e-4f, p.a[10] = 1.0e-4f;                  // Bm (grey)
    p.a[12] = 1.0f, p.a[13] = 0.9f, p.a[14] = 0.74f;
    p.b[0] = 2e-4f, p.b[1] = 0.0f, p.b[2] = 600.0f, p.b[3] = 4e-3f, p.b[4] = 25.0f, p.b[5] = 15.0f;
    p.b[6] = 20.0f, p.b[7] = 2500.0f, p.b[8] = 0.95f, p.b[9] = 0.1f, p.b[10] = 0.12f, p.b[11] = 0.15f;
    p.b[12] = 1.0f, p.b[13] = 4.0f, p.b[15] = 1.0f;
    CHECK(atm_active(p));
    AtmU u;
    CHECK(atm_uniforms(vp.m, p, false, &u));
    CHECK(std::fabs(u.eye[0] - eye[0]) < 1e-2 && std::fabs(u.eye[1] - eye[1]) < 1e-2 && std::fabs(u.eye[2] - eye[2]) < 1e-2);
    const double l[2][3] = {{2e-4, 0.0, 600.0}, {4e-3, 25.0, 15.0}};
    const double sl = std::sqrt(0.09 + 0.64 + 0.04), sun[3] = {0.3 / sl, -0.8 / sl, 0.2 / sl};
    int checked = 0;
    for (int k = 0; k < 64; ++k) {
        // world points ahead of the camera: across the bay, low and high, near and far
        const double dist = 30.0 + 2400.0 * ((k * 37) % 64) / 63.0;
        const double ang = -0.5 + (k % 8) / 7.0, el = -0.12 + 0.03 * (k / 8);
        double X[4] = {eye[0] + dist * std::sin(ang) * std::cos(el), eye[1] - dist * std::cos(ang) * std::cos(el),
                       eye[2] + dist * std::sin(el), 1.0}, c[4];
        remaster_fnd::xform(X, vp, c);
        if (!(c[3] > 0.5) || std::fabs(c[0] / c[3]) > 1.0 || std::fabs(c[1] / c[3]) > 1.0)
            continue;
        const float nx = (float)(c[0] / c[3]), ny = (float)(c[1] / c[3]), d = (float)(c[2] / c[3]);
        const AtmOut o = atm_pixel(u, nx, ny, d);
        const double dir[3] = {(X[0] - eye[0]) / dist, (X[1] - eye[1]) / dist, (X[2] - eye[2]) / dist};
        const double s = std::min(dist, 2500.0), s0 = 20.0;
        const double tau = s > s0 ? atm_ref_tau(eye[2] + dir[2] * s0, dir[2], s - s0, l) : 0.0;
        const double T = std::max(std::exp(-tau), 0.05);
        // the far points sit near depth 1, where float depth carries metres of error: tolerance grows with distance
        CHECK(std::fabs(o.t - T) < 2e-3 + 2e-6 * dist);
        const double cth = dir[0] * sun[0] + dir[1] * sun[1] + dir[2] * sun[2];
        const double pr = 0.75 * (1 + cth * cth), kk = 1 + 0.75 * 0.75 - 1.5 * cth;
        const double pm = std::min((1 - 0.5625) / (kk * std::sqrt(kk)), 4.0);
        const double br[3] = {0.6e-4, 1.1e-4, 2.4e-4}, sc[3] = {1.0, 0.9, 0.74}, amb[3] = {0.1, 0.12, 0.15};
        const float got[3] = {o.r, o.g, o.b};
        for (int i = 0; i < 3; ++i) {
            const double ref = sc[i] * 0.6 * (br[i] * pr + 1e-4 * pm) / (br[i] + 1e-4) + amb[i];
            CHECK(std::fabs(got[i] - ref) < 2e-3 * std::max(1.0, ref) + 1e-3);
        }
        ++checked;
    }
    CHECK(checked >= 40);
    // nearer than the start distance: exactly clear; the sky (depth 1) uses the far distance; the cap holds
    CHECK(atm_pixel(u, 0.0f, 0.0f, 0.0f).t == 1.0f);
    {
        Params q = p;
        q.b[0] = 1.0f;   // absurd haze: the max opacity caps it
        AtmU uq;
        CHECK(atm_uniforms(vp.m, q, false, &uq));
        CHECK(std::fabs(atm_pixel(uq, 0.0f, 0.0f, 1.0f).t - 0.05f) < 1e-6f);
    }
    // 3. zero density (or strength 0): no pass, and the maths alone gives T = 1 and the exact picture
    {
        Params z = p;
        z.b[0] = z.b[3] = 0.0f;
        CHECK(!atm_active(z));
        Params zs = p;
        zs.b[12] = 0.0f;
        CHECK(!atm_active(zs));
        for (const Params *q : {&z, &zs}) {
            AtmU uz;
            CHECK(atm_uniforms(vp.m, *q, false, &uz));
            uint32_t seed = 12345u;
            bool exact = true;
            for (int i = 0; i < 4096; ++i) {
                seed = seed * 1664525u + 1013904223u;
                const float nx = (float)((seed >> 8) & 0xffff) / 32768.0f - 1.0f;
                const float ny = (float)((seed >> 4) & 0xffff) / 32768.0f - 1.0f;
                const float d = (float)(seed >> 9) / 8388608.0f;
                const AtmOut o = atm_pixel(uz, nx, ny, d);
                const float col = (float)(seed & 0xff) / 255.0f * 3.0f;   // HDR values too
                exact = exact && o.t == 1.0f && atm_blend(col, o) == col;
            }
            CHECK(exact);
        }
    }
    // 4. the glow: toward the sun brighter and warmer (more Mie) than away from it; away is bluer
    {
        Params q = p;
        q.a[0] = 0.0f, q.a[1] = -1.0f, q.a[2] = 0.05f;   // the sun ahead, low
        AtmU uq;
        CHECK(atm_uniforms(vp.m, q, false, &uq));
        const AtmOut toward = atm_pixel(uq, 0.0f, 0.1f, 1.0f);
        Params b2 = q;
        b2.a[1] = 1.0f;   // behind the camera
        AtmU ub;
        CHECK(atm_uniforms(vp.m, b2, false, &ub));
        const AtmOut away = atm_pixel(ub, 0.0f, 0.1f, 1.0f);
        CHECK(toward.r > 2.0f * away.r && toward.g > away.g);
        CHECK(away.b / away.r > toward.b / toward.r);
    }
    // 6. COMB14B: the view-projection pick. The scene VP arrives as last-bit variants (one draw each) and is outnumbered
    // by an instanced object's W x VP; with the camera the scene cluster is chosen, its P kept for a frame without it.
    {
        using namespace remaster_fnd;
        remaster_fnd::M4 V{};
        const remaster_fnd::M4 I = identity();
        // V alone: atm_test_vp with an identity projection is not available, so recover V = VP x P^-1 from the test VP
        const double ty = std::tan(0.5);
        remaster_fnd::M4 P{};
        P.m[0][0] = 1.0 / (ty * 16.0 / 9.0), P.m[1][1] = -1.0 / ty, P.m[2][2] = 3000.0 / 2999.5;
        P.m[3][2] = -0.5 * 3000.0 / 2999.5, P.m[2][3] = 1.0;
        remaster_fnd::M4 Pinv{};
        CHECK(inverse(P, &Pinv));
        V = mul(vp, Pinv);
        auto block = [](const remaster_fnd::M4 &m, float *c, float wiggle) {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    c[4 * j + i] = (float)m.m[i][j] * (1.0f + wiggle);
        };
        remaster_fnd::M4 W = I;
        W.m[3][0] = 40.0, W.m[3][1] = -12.0, W.m[3][2] = 3.0;   // an instanced object 40 m away
        const remaster_fnd::M4 obj = mul(W, vp);
        Plurality pl;
        float c[16];
        for (int k = 0; k < 12; ++k) {   // the scene: 12 draws, 12 last-bit variants
            pl.world();
            block(vp, c, (float)(k % 3) * 1.2e-7f);
            pl.add(c);
        }
        for (int k = 0; k < 40; ++k) {   // the object: 40 draws, one block
            pl.world();
            block(obj, c, 0.0f);
            pl.add(c);
        }
        remaster_fnd::M4 kept{}, got{};
        bool have = false;
        CHECK(atm_choose_vp(pl, &V, &kept, &have, &got) == ATM_VP_CAMERA && have);
        double vs = 1.0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                vs = std::max(vs, std::fabs(vp.m[i][j]));
        CHECK(max_abs_diff(got, vp) < 1e-5 * vs);
        CHECK(pl.e.size() >= 3);   // the variants really are distinct blocks
        AtmU uc;
        CHECK(atm_uniforms(got.m, p, false, &uc) && std::fabs(uc.eye[2] - eye[2]) < 0.05);
        Plurality few;   // a frame with only the object: the camera and the kept P
        for (int k = 0; k < 40; ++k) {
            few.world();
            block(obj, c, 0.0f);
            few.add(c);
        }
        CHECK(atm_choose_vp(few, &V, &kept, &have, &got) == ATM_VP_KEPT_P);
        CHECK(max_abs_diff(got, vp) < 1e-5 * vs);
        bool none = false;
        CHECK(atm_choose_vp(few, &V, &kept, &none, &got) == ATM_VP_NONE);           // no camera match, nothing kept
        CHECK(atm_choose_vp(few, nullptr, &kept, &none, &got) == ATM_VP_CLUSTER);   // no camera: the share rule
    }
    // 5. a singular camera is refused; the Metal source carries the same maths with the Metal address space
    {
        double zero[4][4] = {};
        CHECK(!atm_uniforms(zero, p, false, &u));
        const std::string src = atm_source();
        CHECK(src.find("#define ATM_CONST constant") != std::string::npos);
        CHECK(src.find("atm_pixel(ATM_CONST AtmU &u") != std::string::npos ||
              src.find("atm_pixel(ATM_CONST AtmU & u") != std::string::npos);
        CHECK(src.find("fragment float4 atm_fs") != std::string::npos && src.find("ATM_MATHS") == std::string::npos);
    }
}

// Remaster run 14 lit smoke (review/run14/smoke-realism-design.md 4 stage 1, 7): the variant text, off = unchanged,
// the lighting twin (smoke_light.h) and the camera from clip rows. SMOKE_MSL_OUT=<dir> writes the generated particle
// pair per kind for a runtime Metal compile.
namespace smk {
static std::vector<uint8_t> bytes(const std::vector<uint32_t> &w) {
    std::vector<uint8_t> b(w.size() * 4);
    memcpy(b.data(), w.data(), b.size());
    return b;
}
// The particle pixel shader as the census shows it (IDI_PARTICLES_FX ps 97086d94d4253d2d): ps_2_0, texldp of the
// shadow map at t1, the sprite at t0, x vertex colour x 2, x saturate(shadow + c0.x).
static std::vector<uint8_t> ps() {
    return bytes({0xFFFF0200u,
                  0x0200001Fu, 0x80000000u, 0xB00F0000u,   // dcl t0
                  0x0200001Fu, 0x80000000u, 0xB00F0001u,   // dcl t1
                  0x0200001Fu, 0x80000000u, 0x900F0000u,   // dcl v0
                  0x0200001Fu, 0x90000000u, 0xA00F0800u,   // dcl_2d s0
                  0x0200001Fu, 0x90000000u, 0xA00F0801u,   // dcl_2d s1
                  0x03010042u, 0x800F0001u, 0xB0E40001u, 0xA0E40801u,   // texldp r1, t1, s1
                  0x03000042u, 0x800F0000u, 0xB0E40000u, 0xA0E40800u,   // texld r0, t0, s0
                  0x03000002u, 0x80180001u, 0x80000001u, 0xA0000000u,   // add_sat r1.w, r1.x, c0.x
                  0x03000005u, 0x800F0000u, 0x80E40000u, 0x90E40000u,   // mul r0, r0, v0
                  0x03000002u, 0x800F0000u, 0x80E40000u, 0x80E40000u,   // add r0, r0, r0
                  0x03000005u, 0x80070000u, 0x80FF0001u, 0x80E40000u,   // mul r0.xyz, r1.w, r0
                  0x02000001u, 0x800F0800u, 0x80E40000u,                // mov oC0, r0
                  0x0000FFFFu});
}
// The particle vertex shader (vs 888602da494b62ed): vs_2_0, POSITION v0, COLOR v1, TEXCOORD v2; oPos = c0..c3 . v0,
// oT1 = c4..c7 . v0 (the shadow coordinate), oD0 = v1, oT0.xy = v2. t7 free.
static std::vector<uint8_t> vs(bool writes_t7 = false) {
    std::vector<uint32_t> w = {0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x0200001Fu, 0x8000000Au, 0x900F0001u,
                               0x0200001Fu, 0x80000005u, 0x900F0002u};
    for (int k = 0; k < 4; ++k)
        w.insert(w.end(), {0x03000009u, 0xC0000000u | (1u << (16 + k)), 0x90E40000u, 0xA0E40000u | (uint32_t)k});
    for (int k = 0; k < 4; ++k)
        w.insert(w.end(), {0x03000009u, 0xE0000001u | (1u << (16 + k)), 0x90E40000u, 0xA0E40000u | (uint32_t)(4 + k)});
    w.insert(w.end(), {0x02000001u, 0xD00F0000u, 0x90E40001u});   // mov oD0, v1
    w.insert(w.end(), {0x02000001u, 0xE0030000u, 0x90E40002u});   // mov oT0.xy, v2
    if (writes_t7)
        w.insert(w.end(), {0x02000001u, 0xE00F0007u, 0x90E40000u});   // mov oT7, v0
    w.push_back(0x0000FFFFu);
    return bytes(w);
}
static void write(const char *dir, const std::string &name, const std::string &src) {
    if (FILE *fp = fopen((std::string(dir) + "/" + name).c_str(), "wb")) {
        fputs(src.c_str(), fp);
        fclose(fp);
    }
}
} // namespace smk

static void test_lit_smoke() {
    using namespace remaster_smoke;
    const d9sh::Program &pp = d9sh::program_for(smk::ps()), &vp = d9sh::program_for(smk::vs());
    CHECK(pp.ok && pp.pixel && vp.ok && !vp.pixel);
    d9msl::PixelVariant base;
    base.depth_mask = 2;   // the shadow map at stage 1, as the kit binds it
    std::string plain, plain2, lit, vplain, vlit, why;
    CHECK(d9msl::pixel_source(pp, base, &plain, &why));
    // (a) off: byte-identical text (smoke 0 is the default variant), and no smoke names in it
    d9msl::PixelVariant zero = base;
    zero.smoke = 0;
    CHECK(d9msl::pixel_source(pp, zero, &plain2, &why) && plain2 == plain);
    CHECK(plain.find("smk_") == std::string::npos && plain.find("D9SmokeParams") == std::string::npos);
    CHECK(d9msl::vertex_source(vp, d9msl::VertexVariant{}, &vplain, &why) && vplain.find("run 14 lit smoke") == std::string::npos);
    // (b) the variant: parameter block, the epilogue before the alpha test and fog, the shadow as the sun's visibility
    d9msl::VertexVariant vv;
    vv.smoke = 1;
    CHECK(d9msl::vertex_source(vp, vv, &vlit, &why));
    CHECK(vlit.find("    ot[7] = float4(vv[0].xyz, 1.0);") != std::string::npos);
    CHECK(vlit.find("ot[7] = float4(vv[0]") < vlit.find("o.t4 = ot[4]; o.t5 = ot[5]; o.t6 = ot[6]; o.t7 = ot[7];"));
    std::string why7;
    CHECK(!d9msl::vertex_source(d9sh::program_for(smk::vs(true)), vv, &vlit, &why7) && why7.find("t7") != std::string::npos);
    CHECK(d9msl::vertex_source(vp, vv, &vlit, &why));
    for (uint8_t kind = 1; kind <= KIND_MAX; ++kind) {
        d9msl::PixelVariant v = base;
        v.smoke = kind;
        CHECK(d9msl::pixel_source(pp, v, &lit, &why));
        CHECK(lit.find("constant D9SmokeParams &SP [[buffer(5)]]") != std::string::npos);
        CHECK(lit.find("struct D9SmokeParams { float4 sun; float4 eye; float4 k; float4 k2; float4 amb; float4 light; "
                       "float4 lcol; float4 nearf; };") != std::string::npos);
        const size_t e = lit.find("// run 14 lit smoke, kind " + std::to_string(kind)), at = lit.find("if (P.alpha_func != 8"),
                     fog = lit.find("if (P.fog_mode != 0)"), col = lit.find("float4 col = oc[0];");
        CHECK(e != std::string::npos && col < e && e < at && at < fog);
        CHECK(lit.find("smk_vis *= d9_shadow(tx1, t[1] / t[1].w).r;") != std::string::npos);
        const bool flat = kind == KIND_FOG, flare = kind == KIND_FLARE_RED || kind == KIND_FLARE_GREEN;
        CHECK((lit.find("dfdx(smk_p)") == std::string::npos) == flat);   // fog: no sprite normal
        CHECK((lit.find("const float smk_wrap = 0.5;") != std::string::npos) == flat);
        CHECK((lit.find("float3(0.90, 0.96, 1.08)") != std::string::npos) == flat);   // fog's grey-blue shade
        CHECK((lit.find("SP.lcol.rgb * (SP.light.w") != std::string::npos) == flare);
        CHECK((lit.find("smoothstep(0.0, max(SP.amb.w") != std::string::npos) == (kind == KIND_STEAM));
        CHECK(lit.find("col.a *= saturate((1.0 / vin.pos.w - SP.nearf.x) * SP.nearf.y);") != std::string::npos);
        d9msl::PixelVariant k2 = base;
        k2.smoke = kind == 1 ? 2 : 1;
        CHECK(v.key() != base.key() && v.key() != k2.key());
        if (const char *dir = getenv("SMOKE_MSL_OUT")) {
            smk::write(dir, "kind" + std::to_string(kind) + ".ps.metal", lit);
            d9msl::PixelVariant vs2 = v;
            vs2.soft = 1;   // with the soft fade too (fog's wide fade)
            std::string ls;
            CHECK(d9msl::pixel_source(pp, vs2, &ls, &why));
            smk::write(dir, "kind" + std::to_string(kind) + ".soft.ps.metal", ls);
        }
    }
    if (const char *dir = getenv("SMOKE_MSL_OUT")) {
        smk::write(dir, "smoke.vs.metal", vlit);
        smk::write(dir, "plain.ps.metal", plain);
    }
    {   // stage 0 must be a plain 2D sprite
        d9msl::PixelVariant v = base;
        v.smoke = 1;
        v.depth_mask = 3;
        std::string w0;
        CHECK(!d9msl::pixel_source(pp, v, &lit, &w0) && w0.find("smoke") != std::string::npos);
        v.depth_mask = 2;
        v.smoke = 8;
        CHECK(!d9msl::pixel_source(pp, v, &lit, &w0));
        d9msl::VertexVariant a, b;
        b.smoke = 1;
        CHECK(a.key() != b.key());
    }
    // (c) the lighting twin: the side-lit reference is exactly 1; facing the sun brighter, sun behind flatter; thin
    // parts glow more when backlit (silver lining); no sun or strength 0 = unchanged; fog's glow is the weakest
    for (uint32_t kind = 1; kind <= KIND_MAX; ++kind) {
        const Kind k = kind_defaults(kind);
        CHECK(std::fabs(ratio(k, 0.5f, 0.5f, 0.0f, 1.0f) - 1.0f) < 1e-6f);
        CHECK(std::fabs(factor(k, 0.9f, 0.2f, 0.95f, 0.0f) - 1.0f) < 1e-6f);   // in full shade: the game's colour
        CHECK(ratio(k, 0.5f, 0.5f, 0.95f, 1.0f) > 1.05f);    // looking into the sun
        CHECK(ratio(k, 0.5f, 0.5f, -0.95f, 1.0f) < 1.0f);    // sun behind the viewer
        CHECK(ratio(k, 0.5f, 0.1f, 0.95f, 1.0f) > ratio(k, 0.5f, 0.9f, 0.95f, 1.0f));   // thin edges glow
        if (!k.flat) {
            CHECK(ratio(k, 1.0f, 0.8f, 0.0f, 1.0f) > ratio(k, 0.0f, 0.8f, 0.0f, 1.0f));   // lit top vs underside
            CHECK(ratio(k, 0.0f, 0.9f, 0.0f, 1.0f) >= 0.35f);
        }
        Kind off = k;
        off.s = 0.0f;
        CHECK(factor(off, 1.0f, 0.1f, 1.0f, 1.0f) == 1.0f);
    }
    CHECK(std::fabs(hg(0.3f, 0.0f) - 1.0f) < 1e-6f);   // isotropic: 1 everywhere
    {   // HG is normalised: its mean over the sphere (mu uniform in [-1, 1]) is 1
        double m = 0.0;
        for (int i = 0; i < 20000; ++i)
            m += hg(-1.0f + (i + 0.5f) / 10000.0f, 0.75f);
        CHECK(std::fabs(m / 20000.0 - 1.0) < 0.01);
    }
    CHECK(ratio(kind_defaults(KIND_FOG), 0.5f, 0.3f, 0.95f, 1.0f) < ratio(kind_defaults(KIND_STACK_STEAM), 0.5f, 0.3f, 0.95f, 1.0f));
    // (d) the camera from clip rows: a look-at view x projection, eye (10, -20, 3)
    {
        const float ex = 10.0f, ey = -20.0f, ez = 3.0f;
        // view: x right = (1,0,0), y up = (0,0,1), z forward = (0,1,0) (D3D row-vector, transposed into clip rows)
        const float R[3][3] = {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}};
        const float t[3] = {-(R[0][0] * ex + R[0][1] * ey + R[0][2] * ez), -(R[1][0] * ex + R[1][1] * ey + R[1][2] * ez),
                            -(R[2][0] * ex + R[2][1] * ey + R[2][2] * ez)};
        const float xs = 1.2f, ys = 1.6f, zn = 0.5f, zf = 5000.0f, A = zf / (zf - zn), B = -zn * zf / (zf - zn);
        float rows[16] = {xs * R[0][0], xs * R[0][1], xs * R[0][2], xs * t[0], ys * R[1][0], ys * R[1][1], ys * R[1][2], ys * t[1],
                          A * R[2][0],  A * R[2][1],  A * R[2][2],  A * t[2] + B, R[2][0], R[2][1], R[2][2], t[2]};
        float eye[3];
        CHECK(eye_from_clip(rows, rows + 4, rows + 12, eye));
        CHECK(std::fabs(eye[0] - ex) < 1e-3f && std::fabs(eye[1] - ey) < 1e-3f && std::fabs(eye[2] - ez) < 1e-3f);
        // the block: sun normalised with its strength in w, flare position only for flares, near fade for fog
        uint32_t rs[256] = {};
        rs[RS_KIND] = KIND_FOG;
        const float sun[3] = {0.0f, 0.6f, 0.3f};
        memcpy(&rs[RS_SUN], sun, 12);
        D9SmokeParams p;
        CHECK(fill(rs, rows, 0.0f, kind_defaults(KIND_FOG), &p));
        CHECK(std::fabs(p.sun[3] - std::sqrt(0.45f)) < 1e-5f && std::fabs(p.sun[1] - 0.6f / std::sqrt(0.45f)) < 1e-5f);
        CHECK(std::fabs(p.eye[1] - ey) < 1e-3f && p.light[3] == 0.0f && std::fabs(p.nearf[1] - 1.0f / 3.0f) < 1e-6f);
        CHECK(std::fabs(p.amb[1] - ref_term(kind_defaults(KIND_FOG))) < 1e-7f);
        rs[RS_KIND] = KIND_FLARE_RED;
        const float pos[3] = {5.0f, 6.0f, 7.0f};
        memcpy(&rs[RS_POS], pos, 12);
        CHECK(fill(rs, rows, 1.0f, kind_defaults(KIND_FLARE_RED), &p) && p.light[0] == 5.0f && p.light[2] == 7.0f);
        CHECK(p.light[3] > 0.4f && p.light[3] < 0.7f && p.lcol[0] == 1.0f && p.nearf[1] == 0.0f);
        memset(&rs[RS_SUN], 0, 12);   // no sun: strength 0, still drawn lit (the fades), ratio 1
        CHECK(fill(rs, rows, 0.0f, kind_defaults(KIND_FLARE_RED), &p) && p.sun[3] == 0.0f && p.sun[2] == 1.0f);
        rs[RS_KIND] = 0;
        CHECK(!fill(rs, rows, 0.0f, kind_defaults(1), &p));
        rs[RS_KIND] = 8;
        CHECK(!fill(rs, rows, 0.0f, kind_defaults(1), &p));
        rs[RS_KIND] = 1;
        CHECK(!fill(rs, nullptr, 0.0f, kind_defaults(1), &p));
        static_assert(sizeof(D9SmokeParams) == 128, "eight float4, as the MSL struct");
    }
}

// Owner 8 Oct (Off / 2x / 4x): the D3D9 multisample request as a sample count, the 2x cap the core mod sets, and the
// renderer's fall to the next lower supported count. Unset cap: every count exactly as before (4x stays 4x).
static void test_msaa_count() {
    using namespace d9msaa;
    CHECK(count(0, 0) == 0 && count(1, 0) == 2 && count(1, 1) == 4 && count(2, 0) == 2 && count(4, 0) == 4 &&
          count(8, 0) == 4 && count(3, 0) == 2);
    CHECK(parse_cap(nullptr) == 0 && parse_cap("") == 0 && parse_cap("4") == 0 && parse_cap("2") == 2 &&
          parse_cap("22") == 0 && parse_cap("x") == 0);
    CHECK(capped(4, 0) == 4 && capped(4, 2) == 2 && capped(2, 2) == 2 && capped(0, 2) == 0 && capped(2, 0) == 2);
    // the game's 4x (NONMASKABLE quality 1): 4 without the cap, 2 with it, single-sampled when off
    unsetenv("RECOMP_D3D9_MSAA_SAMPLES");
    CHECK(count_env(1, 1) == 4 && count_env(0, 0) == 0);
    setenv("RECOMP_D3D9_MSAA_SAMPLES", "2", 1);
    CHECK(count_env(1, 1) == 2 && count_env(0, 0) == 0 && count_env(1, 0) == 2);
    setenv("RECOMP_D3D9_MSAA_SAMPLES", "4", 1);
    CHECK(count_env(1, 1) == 4);
    unsetenv("RECOMP_D3D9_MSAA_SAMPLES");
    auto all = [](uint32_t) { return true; };
    auto upto2 = [](uint32_t k) { return k <= 2; };
    auto none = [](uint32_t k) { return k <= 1; };
    CHECK(fit(4, all) == 4 && fit(2, all) == 2 && fit(8, all) == 4 && fit(0, all) == 1 && fit(1, all) == 1);
    CHECK(fit(4, upto2) == 2 && fit(2, upto2) == 2);
    CHECK(fit(4, none) == 1 && fit(2, none) == 1);
}

int main() {
    test_msaa_count();
    test_volume_dds();
    test_lit_smoke();
    test_water_shapes();
    test_grass_blades();
    test_foundation();
    test_atmosphere();
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch)
        return 1;
    cpu_reset();
    const uint32_t dev = make_device();
    if (!dev)
        return 1;
    test_device_booleans(dev);
    test_draw_limits(dev);
    test_depth_stencil_get(dev);
    test_surface_container_refs(dev);
    test_effect_booleans(dev);
    test_volume(dev);
    test_state_getters_and_readback(dev);
    const uint32_t bound = make_volume(dev);
    const uint32_t bound_id = com_this(bound)->id;
    const uint32_t child_id = com_this(bound)->surfaces[0];
    CHECK(call_method(dev, 65, {0, bound}) == 0);
    CHECK(call_method(bound, 2) == 1);
    call_method(dev, 2);
    CHECK(!com_get(bound_id) && !com_get(child_id));
    if (const uint32_t refs_dev = make_device()) // Test157c, on a device of its own (it Resets)
        test_depth_stencil_refs(refs_dev);
    printf("d3d9 guest/effect: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
