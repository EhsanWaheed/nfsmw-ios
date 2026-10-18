// d3d9_metal.mm - the Direct3D 9 device's renderer on Metal: a D9Backend
// (host/gpu/d3d9_backend.h), driven by host/gpu/d3d9_host.cpp. It is
// Objective-C++, so the hosts compile it into their present sources.
//
// Shaders are translated to MSL (dx/d3d9_msl.h) and compiled once per
// program. Textures, surfaces and buffers are mirrored under the shim's ids.
// Draws are encoded on the guest thread into one command buffer per frame,
// which Present commits before handing the back buffer to the presenter as a
// GPU texture: a frame never comes back to the CPU unless the game reads a
// render target or a frame dump asks for it.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <MetalFX/MetalFX.h> // Remaster run 13 upscaling; weak-linked, checked at run time
#include <atomic>

#include "../../../dx/d3d9_msl.h"
#include "../../../dx/d3d9_shader.h"
#include "../../../dx/d3d9_volume.h"
#include "../../../dx/host_d9.h"
#include "../../../dx/d3d9_msaa.h"
#include "../../../platform/os.h"
#include "../../../platform/ab_phase.h"
#include "../../../runtime/guest.h"
#include "../../../runtime/display_seam.h"
#include "../../d3d_render.h"
#include "metal_device.h"
#include "shader_warm_queue.h"
#include "pipeline_warm_spec.h"
#include "../../present.h"
#include "../../gpu_time.h"
#include "../d3d9_backend.h"
#include "../d3d9_common.h"
#include "../hdr_convert.h"   // PLAN_R2 item 10 real HDR
#include "../d3d9_trace.h"
#include "../d3d9_capture.h"
#include "../upscale_control.h"
#include "remaster_tex.h"
#include "remaster_cache.h"
#include "grass.h"
#include "foundation.h"   // Remaster run 14 foundation F1/F3 (pure C++)
#include "d3d9_encoders.h"   // v4 Metal 4: the command buffer and encoders the renderer encodes into
#include "smoke_light.h"  // Remaster run 14 lit smoke (pure C++: kinds, the lighting twin, the parameter block)
#include "atmosphere.h"   // Remaster run 14 B1 height fog (pure C++ and its Metal source)
#include <future>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <TargetConditionals.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <cctype>
#include <cerrno>
#if TARGET_OS_OSX
#include <libproc.h> // not in the iOS SDK
#endif
#include <thread>
#include <deque>
#include <condition_variable>
#include <pthread.h>
#include <algorithm>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace d9gpu;

// host_upscale_set_rows (upscale_control.h): the scene height a host asked for; -1 not controlled. The last
// upscaled scene's sizes are published for the host's menus. Read and written from any thread.
std::atomic<int> g_upscale_rows{-1};
std::atomic<uint64_t> g_upscale_in{0}, g_upscale_out{0};
// v2.5 MetalFX temporal (host_upscale_temporal_*, upscale_control.h): the panel's jitter-sign test switch, and whether
// the last scene was scaled temporally (1) or fell back to spatial (0); -1 before a scene. Any thread.
std::atomic<int> g_temporal_flip{0}, g_temporal_active{-1}, g_temporal_debug{0};
// host_d9_diag_set (upscale_control.h): the iOS panel's reflection test, live, for the flicker at 90/120 fps.
// 1 holds the car reflection maps (draws and clears into cube faces are skipped, the last contents stay), 2 holds
// the shadow maps (square depth textures the scene samples), 4 logs those passes, one line a frame for 600 frames.
std::atomic<uint32_t> g_d9_diag{0};
uint64_t pack_size(NSUInteger w, NSUInteger h) {
    return (uint64_t(uint32_t(w)) << 32) | uint32_t(h);
}

// How a D3DFORMAT is stored in Metal.
struct MtlFormat {
    MTLPixelFormat pixel = MTLPixelFormatInvalid;
    Conv conv = Conv::Unsupported;
    uint32_t bytes = 4; // bytes per source pixel, or per 4x4 block for DXT
    bool block = false;
};

MtlFormat format_info(uint32_t fmt, bool bc) {
    const FormatInfo f = d9gpu::format_info(fmt, bc);
    MtlFormat m;
    m.conv = f.conv;
    m.bytes = f.bytes;
    m.block = f.block;
    switch (f.store) {
    case Store::BGRA8:
        m.pixel = MTLPixelFormatBGRA8Unorm;
        break;
    case Store::A8:
        m.pixel = MTLPixelFormatA8Unorm;
        break;
    case Store::RGBA16:
        m.pixel = MTLPixelFormatRGBA16Unorm;
        break;
    case Store::R16F:
        m.pixel = MTLPixelFormatR16Float;
        break;
    case Store::RGBA16F:
        m.pixel = MTLPixelFormatRGBA16Float;
        break;
    case Store::R32F:
        m.pixel = MTLPixelFormatR32Float;
        break;
    case Store::RGBA32F:
        m.pixel = MTLPixelFormatRGBA32Float;
        break;
    case Store::BC1:
        m.pixel = MTLPixelFormatBC1_RGBA;
        break;
    case Store::BC2:
        m.pixel = MTLPixelFormatBC2_RGBA;
        break;
    case Store::BC3:
        m.pixel = MTLPixelFormatBC3_RGBA;
        break;
    case Store::Depth:
        m.pixel = MTLPixelFormatDepth32Float_Stencil8;
        break;
    }
    return m;
}

struct Tex {
    HostD9TextureDesc desc{};
    MtlFormat info;
    id<MTLTexture> texture = nil;
    gpu::Texture imported;
    uint64_t used = 0; // the last frame serial that read or wrote it
    uint64_t written = 0;
    uint64_t outside_frame = 0; // v2.4 follow_begin: 1 + the present count when a pass last drew into it outside a scene
    uint32_t generation = 0, revision = 0; // opt-in metadata trace identity
    float scale = 1.0f; // physical pixels per guest pixel: render targets are rendered larger
    bool cube = false, depth = false; // what `texture` is, without asking it
    // A multisampled target draws into `msaa` and resolves into `texture`,
    // which is what everything else reads.
    id<MTLTexture> msaa = nil;
    uint32_t samples = 1;
    // Test121: for a depth buffer, the Metal texture its last pass drew depth into (its own
    // texture, its multisampled one, or a fitted one), so a StretchRect from it copies that.
    id<MTLTexture> drawn_depth = nil;
    // Remaster A1: how often the game wrote the top level; a texture written again (a movie) never binds the
    // pack's replacement for its key (the per-key cache, remaster_cache.h, holds the replacements).
    uint32_t top_uploads = 0;
    bool no_replace = false;
    // Remaster run 13 MetalFX upscaling: the back buffer and the scene depth have a second backing, made at the
    // first scene marker (never while upscaling is off). Between the markers the scene backing is the one in
    // `texture`/`msaa`/`scale`/`samples` (aliased); the other one waits here. Swapping the backing is the id alias.
    id<MTLTexture> alt_texture = nil, alt_msaa = nil, alt_drawn = nil;
    float alt_scale = 1.0f;
    uint32_t alt_samples = 1;
    bool aliased = false;
    // PLAN_R2 item 10 real HDR (review/run14/hdr-design.md s2.1): while the frame is float the back buffer (and the
    // mirror) draw into an RGBA16Float `texture`/`msaa`; the guest's 8-bit ones wait here. `desc` and `info.conv/bytes`
    // stay the guest's (GetDesc still says 21/22); readbacks convert (hdr_read).
    id<MTLTexture> sdr_texture = nil, sdr_msaa = nil;
    bool promoted = false;
};
// A buffer replaced while a frame still read it, kept for reuse once that
// frame is done. [lo, hi) is what has been written since it was replaced.
struct SpareBuf {
    id<MTLBuffer> buffer = nil;
    uint64_t retired = 0;
    uint32_t lo = 0, hi = 0;
};
struct Buf {
    id<MTLBuffer> buffer = nil;
    std::vector<uint8_t> shadow; // the contents, for fans and for copy-on-write
    uint64_t used = 0;
    uint32_t generation = 0, revision = 0;
    std::vector<SpareBuf> spares;
    uint32_t win_uploads = 0, win_spares = 0, win_replaced = 0;   // v4 diagnostics, since the last perf line
    uint64_t win_copied = 0;                                       // bytes copied into replacements
};

MTLBlendFactor blend_factor(uint32_t b, bool alpha) {
    switch (b) {
    case 1:
        return MTLBlendFactorZero;
    case 2:
        return MTLBlendFactorOne;
    case 3:
        return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
    case 4:
        return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
    case 5:
    case 12:
        return MTLBlendFactorSourceAlpha;
    case 6:
    case 13:
        return MTLBlendFactorOneMinusSourceAlpha;
    case 7:
        return MTLBlendFactorDestinationAlpha;
    case 8:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case 9:
        return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
    case 10:
        return alpha ? MTLBlendFactorOneMinusDestinationAlpha
                     : MTLBlendFactorOneMinusDestinationColor;
    case 11:
        return MTLBlendFactorSourceAlphaSaturated;
    case 14:
        return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 15:
        return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
    default:
        return MTLBlendFactorOne;
    }
}
MTLBlendOperation blend_op(uint32_t op) {
    switch (op) {
    case 2:
        return MTLBlendOperationSubtract;
    case 3:
        return MTLBlendOperationReverseSubtract;
    case 4:
        return MTLBlendOperationMin;
    case 5:
        return MTLBlendOperationMax;
    default:
        return MTLBlendOperationAdd;
    }
}
MTLCompareFunction compare_fn(uint32_t f) {
    switch (f) {
    case 1:
        return MTLCompareFunctionNever;
    case 2:
        return MTLCompareFunctionLess;
    case 3:
        return MTLCompareFunctionEqual;
    case 4:
        return MTLCompareFunctionLessEqual;
    case 5:
        return MTLCompareFunctionGreater;
    case 6:
        return MTLCompareFunctionNotEqual;
    case 7:
        return MTLCompareFunctionGreaterEqual;
    default:
        return MTLCompareFunctionAlways;
    }
}
MTLStencilOperation stencil_op(uint32_t op) {
    switch (op) {
    case 2:
        return MTLStencilOperationZero;
    case 3:
        return MTLStencilOperationReplace;
    case 4:
        return MTLStencilOperationIncrementClamp;
    case 5:
        return MTLStencilOperationDecrementClamp;
    case 6:
        return MTLStencilOperationInvert;
    case 7:
        return MTLStencilOperationIncrementWrap;
    case 8:
        return MTLStencilOperationDecrementWrap;
    default:
        return MTLStencilOperationKeep;
    }
}
MTLSamplerAddressMode address_mode(uint32_t a) {
    switch (a) {
    case 2:
        return MTLSamplerAddressModeMirrorRepeat;
    case 3:
        return MTLSamplerAddressModeClampToEdge;
    case 4:
        return MTLSamplerAddressModeClampToBorderColor;
    case 5:
        return MTLSamplerAddressModeMirrorClampToEdge;
    default:
        return MTLSamplerAddressModeRepeat;
    }
}

// A D3DDECLTYPE as a Metal vertex format, and the scale that turns what the
// shader receives back into the value Direct3D would have handed it.
bool vertex_format(uint32_t type, MTLVertexFormat *fmt, float scale[4]) {
    for (int k = 0; k < 4; ++k)
        scale[k] = 1.0f;
    switch (type) {
    case 0:
        *fmt = MTLVertexFormatFloat;
        return true;
    case 1:
        *fmt = MTLVertexFormatFloat2;
        return true;
    case 2:
        *fmt = MTLVertexFormatFloat3;
        return true;
    case 3:
        *fmt = MTLVertexFormatFloat4;
        return true;
    case 4:
        *fmt = MTLVertexFormatUChar4Normalized_BGRA;
        return true;
    case 5:
        *fmt = MTLVertexFormatUChar4Normalized;
        for (int k = 0; k < 4; ++k)
            scale[k] = 255.0f;
        return true;
    case 6:
        *fmt = MTLVertexFormatShort2Normalized;
        scale[0] = scale[1] = 32767.0f;
        return true;
    case 7:
        *fmt = MTLVertexFormatShort4Normalized;
        for (int k = 0; k < 4; ++k)
            scale[k] = 32767.0f;
        return true;
    case 8:
        *fmt = MTLVertexFormatUChar4Normalized;
        return true;
    case 9:
        *fmt = MTLVertexFormatShort2Normalized;
        return true;
    case 10:
        *fmt = MTLVertexFormatShort4Normalized;
        return true;
    case 11:
        *fmt = MTLVertexFormatUShort2Normalized;
        return true;
    case 12:
        *fmt = MTLVertexFormatUShort4Normalized;
        return true;
    case 13:
        *fmt = MTLVertexFormatUInt1010102Normalized;
        scale[0] = scale[1] = scale[2] = 1023.0f;
        return true;
    case 14:
        *fmt = MTLVertexFormatInt1010102Normalized;
        return true;
    case 15:
        *fmt = MTLVertexFormatHalf2;
        return true;
    case 16:
        *fmt = MTLVertexFormatHalf4;
        return true;
    default:
        return false;
    }
}

const char *kUtilitySource = R"MSL(#include <metal_stdlib>
using namespace metal;
struct QuadOut { float4 pos [[position]]; float2 uv; };
struct QuadIn { float4 rect; float4 uv; float z; float pad0; float pad1; float pad2; float4 color; };
vertex QuadOut quad_vs(uint vid [[vertex_id]], constant QuadIn &q [[buffer(0)]]) {
    float2 c = float2(float(vid & 1), float((vid >> 1) & 1));
    QuadOut o;
    o.pos = float4(mix(q.rect.xy, q.rect.zw, c), q.z, 1.0);
    o.uv = mix(q.uv.xy, q.uv.zw, c);
    return o;
}
fragment float4 copy_fs(QuadOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, i.uv);
}
fragment float4 clear_fs(QuadOut i [[stage_in]], constant QuadIn &q [[buffer(0)]]) {
    return q.color;
}
// Test121: a depth buffer copied into a colour target (StretchRect from a depth surface), the
// stored depth as written; a multisampled depth buffer gives the nearest of its samples (the
// smallest depth), so a foreground edge keeps its own depth (Metal's MIN resolve choice;
// review 2 Oct, Ledger L1).
fragment float4 depth_copy_fs(QuadOut i [[stage_in]], depth2d<float> t [[texture(0)]]) {
    uint2 size = uint2(t.get_width(), t.get_height());
    uint2 p = min(uint2(i.uv * float2(size)), size - uint2(1));
    return float4(t.read(p));
}
fragment float4 depth_copy_ms_fs(QuadOut i [[stage_in]], depth2d_ms<float> t [[texture(0)]]) {
    uint2 size = uint2(t.get_width(), t.get_height());
    uint2 p = min(uint2(i.uv * float2(size)), size - uint2(1));
    float d = t.read(p, 0);
    for (uint k = 1; k < t.get_num_samples(); ++k)
        d = min(d, t.read(p, k));
    return float4(d);
}
// Remaster run 13 MetalFX upscaling: the upscaled scene into the native back buffer with the scene's depth (by UV,
// nearest; a multisampled depth gives its nearest sample), so what draws after the scene tests against it.
struct UpscaleOut { float4 color [[color(0)]]; float depth [[depth(any)]]; };
// v2.5 MetalFX temporal: zoff = the scene's jitter (scene pixels): the depth is read where the jittered scene put
// this pixel's surface, so the effects after the scene (AO, fog, depth of field) see it unjittered, as the colour is.
fragment UpscaleOut upscale_compose_fs(QuadOut i [[stage_in]], texture2d<float> t [[texture(0)]],
                                       depth2d<float> z [[texture(1)]], sampler s [[sampler(0)]],
                                       constant float2 &zoff [[buffer(0)]]) {
    uint2 size = uint2(z.get_width(), z.get_height());
    uint2 p = uint2(clamp(i.uv * float2(size) + zoff, float2(0.0), float2(size - uint2(1))));
    UpscaleOut o;
    o.color = t.sample(s, i.uv);
    o.depth = z.read(p);
    return o;
}
fragment UpscaleOut upscale_compose_ms_fs(QuadOut i [[stage_in]], texture2d<float> t [[texture(0)]],
                                          depth2d_ms<float> z [[texture(1)]], sampler s [[sampler(0)]],
                                          constant float2 &zoff [[buffer(0)]]) {
    uint2 size = uint2(z.get_width(), z.get_height());
    uint2 p = uint2(clamp(i.uv * float2(size) + zoff, float2(0.0), float2(size - uint2(1))));
    float d = z.read(p, 0);
    for (uint k = 1; k < z.get_num_samples(); ++k)
        d = min(d, z.read(p, k));
    UpscaleOut o;
    o.color = t.sample(s, i.uv);
    o.depth = d;
    return o;
}
)MSL";

// Remaster run 14 foundation F3 (review/run14/foundation-design.md 3; TEST ONLY, RECOMP_REMASTER_FOUNDATION=1): camera
// motion vectors at the scene end (252), before the scaler, at the scene size. Its own library, compiled only when the
// foundation is on (off: no new MSL anywhere). R is clip_cur -> clip_prev (row-vector, formed in double on the CPU,
// foundation.h reprojection); remaster_fnd::motion is the CPU reference of the same arithmetic. Output RG16Float,
// current -> previous in render pixels (x right, y down) from the unjittered pixel centre; the sky (depth 1) uses the
// rotation-only Rsky. Also: this frame's depth into nz (next frame's pz), and TEST counters per frame (simdgroup
// reductions, one atomic per simdgroup): the reprojected-depth error of the jitter-compensated position against the
// previous frame's depth (bilinear in linear depth), |mv| sum (64-bit fixed point 1/65536 px) and max.
const char *kFndSource = R"MSL(#include <metal_stdlib>
using namespace metal;
struct FndMv {
    float4x4 R, Rsky;   // columns: v * R is the row-vector product
    float2 size, h;     // render pixels; the unjittered halfpix (NDC)
    float2 jc, jp;      // jitter this frame / the previous frame (render pixels, x right, y down)
    float zn, zf;       // near / far (linear depth for the error)
    uint flags, pad;    // 1 the previous depth is valid, 2 debug motion, 4 debug reprojection error, 8 TEST counters,
                        // 32 v2.5: per-object motion in om (UV units; x >= 30000: none, the camera vector stays)
};
// q (32 words): 0 pixels, 1 sky, 2 behind the previous camera, 3/4 sum |mv| lo/hi (1/65536 px), 5 max |mv|,
// 6..12 reprojected depth error < 0.1 / 0.25 / 0.5 / 1 / 2 / 5 / >= 5 %, 13 disoccluded, 14 off screen, 15 edge,
// 16 moves with the camera (COMB14B).
static void fnd_add(device atomic_uint *q, uint i, uint v) {
    if (v)
        atomic_fetch_add_explicit(&q[i], v, memory_order_relaxed);
}
static float fnd_lin(float d, float zn, float zf) { return zn * zf / max(zf - d * (zf - zn), 1e-6); }
static float3 fnd_hue(float a) {
    float3 k = clamp(abs(fract(float3(a, a + 2.0 / 3.0, a + 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return k;
}
static void fnd_body(uint2 p, bool in, float d, constant FndMv &u, texture2d<float, access::write> mv,
                     texture2d<float, access::read> pz, texture2d<float, access::write> nz,
                     texture2d<float, access::write> dbg, device atomic_uint *q, texture2d<float, access::read> om) {
    const float2 ndc = float2(2.0 * (float(p.x) + 0.5) / u.size.x - 1.0 - u.h.x,
                              1.0 - 2.0 * (float(p.y) + 0.5) / u.size.y - u.h.y);
    const bool sky = d >= 1.0;
    const float4 o = float4(ndc, d, 1.0) * (sky ? u.Rsky : u.R);
    const bool front = o.w > 1e-6;
    float2 m = front ? (o.xy / o.w - ndc) * float2(0.5, -0.5) * u.size : float2(0.0);
    if (in && (u.flags & 32u)) {   // v2.5 MetalFX temporal: the draw's own motion where it wrote one (cars, wheels)
        const float2 ob = om.read(p).xy;
        if (ob.x < 30000.0)
            m = ob * u.size;
    }
    uint c[17] = {};
    if (in) {
        if (u.flags & 8u)   // the previous depth: only for the counters / debug 8 (CONF14B cost)
            nz.write(float4(d), p);
        mv.write(float4(m, 0.0, 0.0), p);
        c[0] = !sky && front ? 1u : 0u;
        c[1] = sky ? 1u : 0u;
        c[2] = front ? 0u : 1u;
    }
    const float am = in && front ? length(m) : 0.0;
    const float am_px = sky ? 0.0 : am;   // the counted mean / max: non-sky pixels (q[0])
    float3 col = float3(0.0);
    if (in && !sky && front && (u.flags & 1u)) {
        // the surface this pixel shows sits at the unjittered centre p - jc; the previous frame drew it at its
        // reprojected position + jp
        const float2 nj = ndc - float2(2.0 * u.jc.x / u.size.x, -2.0 * u.jc.y / u.size.y);
        const float4 r = float4(nj, d, 1.0) * u.R;
        if (r.w > 1e-6) {
            const float2 pn = r.xy / r.w;
            const float2 pp = float2((pn.x + u.h.x + 1.0) * 0.5 * u.size.x, (1.0 - pn.y - u.h.y) * 0.5 * u.size.y) + u.jp;
            const float want = fnd_lin(r.z / r.w, u.zn, u.zf);
            const float2 b = pp - 0.5;
            const int2 i0 = int2(floor(b));
            const float2 f = b - float2(i0);
            if (i0.x < 0 || i0.y < 0 || i0.x + 1 >= int(u.size.x) || i0.y + 1 >= int(u.size.y)) {
                c[14] = 1u;
            } else {
                const float z00 = fnd_lin(pz.read(uint2(i0)).x, u.zn, u.zf);
                const float z10 = fnd_lin(pz.read(uint2(i0 + int2(1, 0))).x, u.zn, u.zf);
                const float z01 = fnd_lin(pz.read(uint2(i0 + int2(0, 1))).x, u.zn, u.zf);
                const float z11 = fnd_lin(pz.read(uint2(i0 + int2(1, 1))).x, u.zn, u.zf);
                const float lo = min(min(z00, z10), min(z01, z11)), hi = max(max(z00, z10), max(z01, z11));
                const float got = mix(mix(z00, z10, f.x), mix(z01, z11, f.x), f.y);
                const float e = abs(got - want) / max(want, 1e-6);
                // COMB14B: a pixel the camera-only vector misses but whose depth the previous frame had at the same
                // pixel moves with the camera (the player's car in a chase view: a constant ~8 % of the pixels at
                // any speed): counted apart (16), not as a static-world error, until cars are tagged (F2 S1)
                // (only for a miss, never for a depth edge: a flat road has the same depth at the same pixel too)
                const float zc = fnd_lin(d, u.zn, u.zf), zs = fnd_lin(pz.read(p).x, u.zn, u.zf);
                const bool dis = got < want * 0.95, edge = hi > lo * 1.05;
                if ((dis || (!edge && e >= 0.005)) && abs(zs - zc) < 0.005 * zc) {
                    c[16] = 1u;
                    col = float3(1.0, 1.0, 0.0);
                } else if (dis) {
                    c[13] = 1u;
                    col = float3(0.0, 0.0, 1.0);
                } else if (edge) {
                    c[15] = 1u;
                    col = float3(0.5, 0.0, 0.5);
                } else {
                    c[e < 0.001 ? 6 : e < 0.0025 ? 7 : e < 0.005 ? 8 : e < 0.01 ? 9 : e < 0.02 ? 10 : e < 0.05 ? 11 : 12] = 1u;
                    col = e < 0.005 ? float3(0.0, 1.0 - e / 0.005, 0.0) : float3(min(1.0, e / 0.05), 0.0, 0.0);
                }
            }
        }
    }
    if (in && (u.flags & 2u))
        dbg.write(float4(am > 1e-4 ? fnd_hue(atan2(m.y, m.x) / 6.2831853 + 0.5) * saturate(am / 8.0 + 0.1) : float3(0.0), 1.0), p);
    else if (in && (u.flags & 4u))
        dbg.write(float4(col, 1.0), p);
    // simdgroup reductions, then one atomic per counter per simdgroup (TEST: RECOMP_REMASTER_FOUNDATION_LOG only)
    if (!(u.flags & 8u))
        return;
    const float sm = simd_sum(min(am_px, 1024.0) * 65536.0);   // <= 2^31 per simdgroup of 32
    const uint mx = simd_max(uint(min(am_px * 65536.0, 2147483647.0)));
    for (uint k = 0; k < 17; ++k)
        c[k] = simd_sum(c[k]);
    if (simd_is_first()) {
        for (uint k = 0; k < 17; ++k)
            if (k < 3 || k > 5)
                fnd_add(q, k, c[k]);
        const uint add = uint(min(sm, 4.0e9));
        if (add) {
            const uint old = atomic_fetch_add_explicit(&q[3], add, memory_order_relaxed);
            if (old + add < old)
                atomic_fetch_add_explicit(&q[4], 1u, memory_order_relaxed);
        }
        atomic_fetch_max_explicit(&q[5], mx, memory_order_relaxed);
    }
}
kernel void fnd_motion_cs(depth2d<float, access::read> z [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],
                          texture2d<float, access::read> pz [[texture(2)]], texture2d<float, access::write> nz [[texture(3)]],
                          texture2d<float, access::write> dbg [[texture(4)]], constant FndMv &u [[buffer(0)]],
                          device atomic_uint *q [[buffer(1)]], texture2d<float, access::read> om [[texture(5)]],
                          uint2 p [[thread_position_in_grid]]) {
    const bool in = p.x < uint(u.size.x) && p.y < uint(u.size.y);
    fnd_body(p, in, in ? z.read(p) : 1.0, u, mv, pz, nz, dbg, q, om);
}
kernel void fnd_motion_ms_cs(depth2d_ms<float, access::read> z [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],
                             texture2d<float, access::read> pz [[texture(2)]], texture2d<float, access::write> nz [[texture(3)]],
                             texture2d<float, access::write> dbg [[texture(4)]], constant FndMv &u [[buffer(0)]],
                             device atomic_uint *q [[buffer(1)]], texture2d<float, access::read> om [[texture(5)]],
                             uint2 p [[thread_position_in_grid]]) {
    const bool in = p.x < uint(u.size.x) && p.y < uint(u.size.y);
    float d = 1.0;
    if (in) {   // the nearest sample, as the scene compose takes it (flag 16: sample 0 only, the cheaper read)
        d = z.read(p, 0);
        for (uint k = 1; k < z.get_num_samples() && !(u.flags & 16u); ++k)
            d = min(d, z.read(p, k));
    }
    fnd_body(p, in, d, u, mv, pz, nz, dbg, q, om);
}
// v2.5 MetalFX temporal: the scene depth copied into a plain Depth32Float when the scaler will not take the scene's own.
vertex float4 tmp_z_vs(uint vid [[vertex_id]]) {
    const float2 t = float2(float((vid << 1) & 2u), float(vid & 2u));
    return float4(t * 2.0 - 1.0, 0.0, 1.0);
}
struct TmpZ { float d [[depth(any)]]; };
fragment TmpZ tmp_z_fs(float4 pos [[position]], depth2d<float, access::read> z [[texture(0)]]) {
    TmpZ o;
    o.d = z.read(uint2(pos.xy));
    return o;
}
)MSL";
struct FndMv {   // kFndSource FndMv
    float R[16], Rsky[16];
    float size[2], h[2], jc[2], jp[2];
    float zn, zf;
    uint32_t flags, pad;
};

struct QuadIn {
    float rect[4];
    float uv[4];
    float z, pad0, pad1, pad2;
    float color[4];
};

// ---- shader warm-up ------------------------------------------------------------------------
// Compiling MSL is synchronous at first use. With a cold system Metal cache (the first launch of a
// new bundle, or after the system purged it) the first main-menu frame held 1.1-1.3 s compiling
// 15-17 libraries (Test65; pipelines took under 2 ms). When RECOMP_D3D9_SHADER_WARM_DIR names a
// directory (the launcher puts it in the profile, outside the signed app), every MSL source that
// compiles is recorded there once (content-addressed by SHA-256 of the options identity + source,
// first use order kept in order.txt), and at start-up a background thread compiles the recorded
// sources in that order. RECOMP_D3D9_SHADER_WARM_SEED may name a read-only list in the same format
// (shipped inside the application, compiled first). The draw path takes a pre-compiled library when
// its key matches and
// otherwise compiles as before, so a missing, stale or tampered entry only costs the normal compile.
// Bounded: at most 8192 entries of at most 1 MiB each; only hex-named files are read.
// Requests are coalesced (Test66): a draw that needs the library the worker is compiling right now
// waits for that one compile instead of starting a second, and a queued entry the draw path compiles
// itself is struck from the worker's list. No lock is held while any library compiles.
// Test70: the pixel and vertex formats a recorded pipeline may name (the ones this renderer creates).
bool warm_formats_ok(const PipeSpec &p) {
    static const uint32_t color[] = {
        (uint32_t)MTLPixelFormatBGRA8Unorm,  (uint32_t)MTLPixelFormatBGRA8Unorm_sRGB, (uint32_t)MTLPixelFormatRGBA8Unorm,
        (uint32_t)MTLPixelFormatA8Unorm,     (uint32_t)MTLPixelFormatRGBA16Unorm,     (uint32_t)MTLPixelFormatR16Float,
        (uint32_t)MTLPixelFormatRGBA16Float, (uint32_t)MTLPixelFormatR32Float,        (uint32_t)MTLPixelFormatRGBA32Float,
        (uint32_t)MTLPixelFormatBGR10A2Unorm, (uint32_t)MTLPixelFormatRGB10A2Unorm};
    static const uint32_t vertex[] = {
        (uint32_t)MTLVertexFormatFloat, (uint32_t)MTLVertexFormatFloat2, (uint32_t)MTLVertexFormatFloat3,
        (uint32_t)MTLVertexFormatFloat4, (uint32_t)MTLVertexFormatUChar4Normalized_BGRA,
        (uint32_t)MTLVertexFormatUChar4Normalized, (uint32_t)MTLVertexFormatShort2Normalized,
        (uint32_t)MTLVertexFormatShort4Normalized, (uint32_t)MTLVertexFormatUShort2Normalized,
        (uint32_t)MTLVertexFormatUShort4Normalized, (uint32_t)MTLVertexFormatUInt1010102Normalized,
        (uint32_t)MTLVertexFormatInt1010102Normalized, (uint32_t)MTLVertexFormatHalf2, (uint32_t)MTLVertexFormatHalf4};
    auto in = [](uint32_t v, const uint32_t *set, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (set[i] == v)
                return true;
        return false;
    };
    if (p.depth && p.depth != (uint32_t)MTLPixelFormatDepth32Float_Stencil8)
        return false;
    for (const PipeSpec::Color &c : p.color)
        if (c.format && !in(c.format, color, sizeof color / sizeof *color))
            return false;
    for (const PipeSpec::Attr &a : p.attr)
        if (a.format && !in(a.format, vertex, sizeof vertex / sizeof *vertex))
            return false;
    return true;
}

// Test70: the recorded identity of a pipeline descriptor (every field the draw path sets).
PipeSpec warm_spec_of(MTLRenderPipelineDescriptor *pd, const std::string &vkey, const std::string &fkey) {
    PipeSpec p;
    p.vkey = vkey;
    p.fkey = fkey;
    p.samples = (uint32_t)pd.rasterSampleCount;
    p.depth = (uint32_t)pd.depthAttachmentPixelFormat;
    if (pd.stencilAttachmentPixelFormat != pd.depthAttachmentPixelFormat)
        p.samples = 0;   // not a combination the renderer records (refused by valid())
    for (int i = 0; i < 4; ++i) {
        MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid)
            continue;
        PipeSpec::Color &c = p.color[i];
        c.format = (uint32_t)a.pixelFormat;
        c.mask = (uint32_t)a.writeMask;
        if (a.blendingEnabled) {
            c.blend = 1;
            c.src_rgb = (uint32_t)a.sourceRGBBlendFactor;
            c.dst_rgb = (uint32_t)a.destinationRGBBlendFactor;
            c.rgb_op = (uint32_t)a.rgbBlendOperation;
            c.src_a = (uint32_t)a.sourceAlphaBlendFactor;
            c.dst_a = (uint32_t)a.destinationAlphaBlendFactor;
            c.a_op = (uint32_t)a.alphaBlendOperation;
        }
    }
    if (MTLVertexDescriptor *vd = pd.vertexDescriptor) {
        for (int i = 0; i < 16; ++i) {
            MTLVertexAttributeDescriptor *at = vd.attributes[i];
            if (at.format == MTLVertexFormatInvalid)
                continue;
            p.attr[i] = {(uint32_t)at.format, (uint32_t)at.offset, (uint32_t)at.bufferIndex};
        }
        for (int i = 0; i < 9; ++i) {
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            if (!l.stride)
                continue;
            p.layout[i] = {(uint32_t)l.stride, (uint32_t)l.stepFunction, (uint32_t)l.stepRate};
        }
    }
    return p;
}

// Test70: the descriptor a recorded identity describes (the inverse of warm_spec_of).
MTLRenderPipelineDescriptor *warm_descriptor(const PipeSpec &p, id<MTLFunction> vfn, id<MTLFunction> ffn) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.rasterSampleCount = p.samples;
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < 16; ++i)
        if (const PipeSpec::Attr &a = p.attr[i]; a.format) {
            vd.attributes[i].format = (MTLVertexFormat)a.format;
            vd.attributes[i].offset = a.offset;
            vd.attributes[i].bufferIndex = a.buffer;
        }
    for (int i = 0; i < 9; ++i)
        if (const PipeSpec::Layout &l = p.layout[i]; l.stride) {
            vd.layouts[i].stride = l.stride;
            vd.layouts[i].stepFunction = (MTLVertexStepFunction)l.step;
            vd.layouts[i].stepRate = l.rate;
        }
    pd.vertexDescriptor = vd;
    for (int i = 0; i < 4; ++i)
        if (const PipeSpec::Color &c = p.color[i]; c.format) {
            MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
            a.pixelFormat = (MTLPixelFormat)c.format;
            a.writeMask = (MTLColorWriteMask)c.mask;
            if (c.blend) {
                a.blendingEnabled = YES;
                a.sourceRGBBlendFactor = (MTLBlendFactor)c.src_rgb;
                a.destinationRGBBlendFactor = (MTLBlendFactor)c.dst_rgb;
                a.rgbBlendOperation = (MTLBlendOperation)c.rgb_op;
                a.sourceAlphaBlendFactor = (MTLBlendFactor)c.src_a;
                a.destinationAlphaBlendFactor = (MTLBlendFactor)c.dst_a;
                a.alphaBlendOperation = (MTLBlendOperation)c.a_op;
            }
        }
    if (p.depth) {
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)p.depth;
        pd.stencilAttachmentPixelFormat = (MTLPixelFormat)p.depth;
    }
    return pd;
}

class ShaderWarm {
  public:
    ShaderWarm(id<MTLDevice> dev, std::string dir, std::string seed, std::string identity)
        : dev_(dev), dir_(std::move(dir)), identity_(std::move(identity)) {
        if (!seed.empty())
            load_order(seed);   // read-only list shipped with the application
        if (!dir_.empty()) {
            mkdir(dir_.c_str(), 0700);
            load_order(dir_);
        }
        // Test70: recorded pipelines (seed first). RECOMP_D3D9_PIPELINE_WARM=0 (TEST ONLY) turns the
        // pipeline half off for comparisons; libraries are unaffected.
        const char *pw = recomp_env("D3D9_PIPELINE_WARM");
        pipes_on_ = !(pw && !strcmp(pw, "0"));
        if (pipes_on_) {
            if (!seed.empty())
                load_pipes(seed);
            if (!dir_.empty())
                load_pipes(dir_);
        }
        worker_ = std::thread([this] { run(); });
    }
    ~ShaderWarm() {
        // Run 15 review (P1), run 16 seam: no new background compile may start once destruction begins ...
        async_.close();
        stop_.store(true);
        if (worker_.joinable())
            worker_.join();
        // ... and every one already started is drained: their completions use async_ and queue_ (a block does not
        // retain a C++ object), so this waits, with no timeout, until every admitted completion has finished with
        // them (shader_warm_queue.h ShaderAsyncCompiles; tested with a fake compiler in shader_warm_queue_tests).
        async_.drain();
    }
    static MTLCompileOptions *options() {
        MTLCompileOptions *opts = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *))
            opts.mathMode = MTLMathModeSafe;
        // TEST ONLY: a per-run salt makes every compile miss the system Metal cache (a controlled
        // cold start without deleting any system cache).
        if (const char *salt = recomp_env("D3D9_SHADER_SALT"); salt && *salt)
            opts.preprocessorMacros = @{@"RECOMP_SHADER_SALT" : [NSString stringWithUTF8String:salt]};
        return opts;
    }
    std::string key(const std::string &src) const {
        CC_SHA256_CTX c;
        CC_SHA256_Init(&c);
        CC_SHA256_Update(&c, identity_.data(), (CC_LONG)identity_.size());
        CC_SHA256_Update(&c, "\n", 1);
        CC_SHA256_Update(&c, src.data(), (CC_LONG)src.size());
        unsigned char d[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(d, &c);
        char hex[65];
        for (int i = 0; i < 32; ++i)
            snprintf(hex + 2 * i, 3, "%02x", d[i]);
        return std::string(hex, 64);
    }
    // The finished library for this source, if the warm-up has it (waiting, bounded, when the
    // worker is compiling exactly this source now); nil means compile it here.
    id<MTLLibrary> take(const std::string &k) {
        id<MTLLibrary> lib = nil;
        return queue_.take(k, &lib) ? lib : nil;
    }
    // Run 15 (hitch): the non-blocking form for draws that have a plain fallback (the variant
    // fallback chain draws them as today meanwhile). Returns the library when it is ready; nil with
    // *pending set while a compile of it is in flight (the warm-up's, or one started here in the
    // background now); nil with *pending clear when a background compile of it failed (the caller
    // compiles it in the draw, which reports the error as before).
    // Run 16: the logic (admission, one compile per key, failures, drain) is ShaderAsyncCompiles in
    // shader_warm_queue.h; the Metal call is the compiler given to async_ below.
    id<MTLLibrary> request(const std::string &k, const std::string &src, bool *pending) {
        return async_.request(k, src, pending);
    }
    uint32_t async_started() { return queue_.async_started(); }
    // Called after the draw path compiled a source itself. A new source is recorded; a known one
    // reaches here only when its recorded copy was not usable in time (claimed first, or missing,
    // damaged or altered), so a damaged profile copy is rewritten once rather than kept forever.
    // Entries shipped in the read-only seed are never written.
    void record(const std::string &k, const std::string &src) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        if (dir_.empty() || src.size() > (1u << 20))
            return;
        const std::string path = dir_ + "/" + k + ".msl", tmp = path + ".tmp";
        const bool known = queue_.known(k);
        if (known && (!in_profile(k) || same_file(path, src)))
            return;
        if (!known && queue_.known_count() >= 8192)
            return;
        FILE *f = fopen(tmp.c_str(), "wb");
        if (!f) {
            note_write_failure();
            return;
        }
        const bool ok = fwrite(src.data(), 1, src.size(), f) == src.size();
        if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
            remove(tmp.c_str());
            note_write_failure();
            return;
        }
        if (known) {
            if (++repaired_ == 1)
                fprintf(stderr, "d3d9 metal: shader warm-up rewrote a recorded entry that was missing or "
                                "damaged (%s)\n", k.c_str());
            return;   // already listed in order.txt
        }
        if (FILE *o = fopen((dir_ + "/order.txt").c_str(), "a")) {
            fprintf(o, "%s\n", k.c_str());
            fclose(o);
        }
        queue_.add_known(k);
        profile_keys_.insert(k);
    }
    uint32_t hits() { return queue_.hits(); }
    uint32_t coalesced() { return queue_.coalesced(); }

    // Test70: the finished pipeline recorded with exactly this identity, if the warm-up has it
    // (handed over once); nil means create it here. A recorded identity the worker has not reached
    // is struck from its list, as for libraries. Never waits.
    id<MTLRenderPipelineState> take_pipeline(const std::string &spec) {
        std::lock_guard<std::mutex> lock(pipe_mutex_);
        auto it = pipe_ready_.find(spec);
        if (it == pipe_ready_.end()) {
            if (pipe_known_.count(spec))
                pipe_claimed_.insert(spec);
            return nil;
        }
        id<MTLRenderPipelineState> pso = it->second;
        pipe_ready_.erase(it);
        if (++pipe_hits_ == 1 || pipe_hits_ == 10 || pipe_hits_ == 100 || pipe_hits_ == 1000)
            fprintf(stderr, "d3d9 metal: pipeline warm-up: %u pre-built pipelines used so far (at %.3f s monotonic)\n",
                    pipe_hits_, os_monotonic_ns() / 1e9);
        return pso;
    }
    // Test70: called after the draw path created a pipeline itself; a new identity is appended to
    // the profile's pipelines.txt (at most 8192; the read-only seed is never written).
    void record_pipeline(const std::string &spec) {
        if (dir_.empty() || !pipes_on_)
            return;
        {
            std::lock_guard<std::mutex> lock(pipe_mutex_);
            if (pipe_known_.count(spec) || pipe_known_.size() >= 8192)
                return;
            pipe_known_.insert(spec);
        }
        std::lock_guard<std::mutex> lock(record_mutex_);
        FILE *o = fopen((dir_ + "/pipelines.txt").c_str(), "a");
        if (!o) {
            note_write_failure();
            return;
        }
        const bool ok = fprintf(o, "%s\n", spec.c_str()) > 0;
        if (fclose(o) != 0 || !ok)
            note_write_failure();
    }
    uint32_t pipeline_hits() {
        std::lock_guard<std::mutex> lock(pipe_mutex_);
        return pipe_hits_;
    }
    bool pipelines_on() const { return pipes_on_; }

  private:
    bool in_profile(const std::string &k) const { return profile_keys_.count(k) != 0; }
    static bool same_file(const std::string &path, const std::string &src) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || (uint64_t)st.st_size != src.size())
            return false;
        std::string have;
        if (FILE *f = fopen(path.c_str(), "rb")) {
            have.resize(src.size());
            const size_t n = fread(have.data(), 1, have.size(), f);
            fclose(f);
            have.resize(n);
        }
        return have == src;
    }
    void note_write_failure() {
        // The game is unaffected (the library was compiled and is in use); only the next launch
        // loses this entry's head start. Said once, not per shader.
        if (!write_failure_logged_) {
            write_failure_logged_ = true;
            fprintf(stderr, "d3d9 metal: shader warm-up could not record into %s (%s); continuing "
                            "without recording\n", dir_.c_str(), strerror(errno));
        }
    }
    static bool hex_key(const char *s) {
        for (int i = 0; i < 64; ++i)
            if (!isxdigit((unsigned char)s[i]))
                return false;
        return s[64] == 0 || s[64] == '\n' || s[64] == '\r';
    }
    void load_order(const std::string &from) {
        FILE *o = fopen((from + "/order.txt").c_str(), "r");
        if (!o)
            return;
        char line[128];
        while (fgets(line, sizeof line, o) && order_.size() < 8192) {
            if (!hex_key(line))
                continue;
            std::string k(line, 64);
            if (queue_.add_known(k)) {
                order_.push_back({k, from});
                lib_dir_[k] = from;
                if (from == dir_)
                    profile_keys_.insert(k);
            }
        }
        fclose(o);
    }
    void load_pipes(const std::string &from) {
        FILE *o = fopen((from + "/pipelines.txt").c_str(), "r");
        if (!o)
            return;
        char line[2100];
        while (fgets(line, sizeof line, o) && pipe_order_.size() < 8192) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\n')
                line[--n] = 0;
            PipeSpec p;
            if (!PipeSpec::parse(line, &p) || !warm_formats_ok(p))
                continue;   // damaged, partial or altered: the draw path creates that pipeline itself
            if (pipe_known_.insert(line).second)
                pipe_order_.push_back(line);
        }
        fclose(o);
    }
    static std::string read_source(const std::string &dir, const std::string &k) {
        std::string src;
        if (FILE *f = fopen((dir + "/" + k + ".msl").c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && src.size() <= (1u << 20))
                src.append(buf, n);
            fclose(f);
        }
        return src;
    }
    // Worker only: a recorded library for the pipeline half (the one compiled above, or compiled
    // again from its verified recorded source when the draw path took it first).
    id<MTLLibrary> worker_library(const std::string &k, MTLCompileOptions *opts) {
        auto it = libs_.find(k);
        if (it != libs_.end())
            return it->second;
        id<MTLLibrary> lib = nil;
        if (auto d = lib_dir_.find(k); d != lib_dir_.end()) {
            const std::string src = read_source(d->second, k);
            if (!src.empty() && src.size() <= (1u << 20) && key(src) == k) {
                NSError *error = nil;
                lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opts error:&error];
            }
        }
        libs_[k] = lib;
        return lib;
    }
    void run_pipelines(MTLCompileOptions *opts) {
        const uint64_t t0 = os_monotonic_ns();
        uint32_t made = 0, skipped = 0, taken_over = 0, mismatched = 0;
        for (const std::string &spec : pipe_order_) {
            if (stop_.load())
                break;
            {
                std::lock_guard<std::mutex> lock(pipe_mutex_);
                if (pipe_claimed_.count(spec)) {
                    ++taken_over;
                    continue;
                }
            }
            PipeSpec p;
            if (!PipeSpec::parse(spec, &p)) {
                ++skipped;
                continue;
            }
            @autoreleasepool {
                id<MTLLibrary> vl = worker_library(p.vkey, opts), fl = worker_library(p.fkey, opts);
                id<MTLFunction> vfn = [vl newFunctionWithName:@"vs_main"], ffn = [fl newFunctionWithName:@"ps_main"];
                if (!vfn || !ffn) {
                    ++skipped;
                    continue;
                }
                MTLRenderPipelineDescriptor *pd = warm_descriptor(p, vfn, ffn);
                if (warm_spec_of(pd, p.vkey, p.fkey).text() != spec) {
                    ++mismatched;   // would not be the identity the draw path asks for
                    continue;
                }
                NSError *error = nil;
                id<MTLRenderPipelineState> pso = [dev_ newRenderPipelineStateWithDescriptor:pd error:&error];
                if (!pso) {
                    ++skipped;
                    continue;
                }
                std::lock_guard<std::mutex> lock(pipe_mutex_);
                if (pipe_claimed_.count(spec)) {
                    ++taken_over;
                } else {
                    pipe_ready_[spec] = pso;
                    ++made;
                }
            }
        }
        libs_.clear();
        fprintf(stderr,
                "d3d9 metal: pipeline warm-up created %u recorded pipelines in the background in %.1f ms "
                "(%u skipped, %u mismatched, %u created first by the draw path, %zu recorded%s)\n",
                made, (os_monotonic_ns() - t0) / 1e6, skipped, mismatched, taken_over, pipe_order_.size(),
                stop_.load() ? ", stopped early" : "");
    }
    void run() {
        const uint64_t t0 = os_monotonic_ns();
        __block uint32_t compiled = 0, skipped = 0;
        uint32_t taken_over = 0;
        MTLCompileOptions *opts = options();
        dispatch_semaphore_t slots = dispatch_semaphore_create(kWarmParallel);
        dispatch_group_t group = dispatch_group_create();
        for (const auto &entry : order_) {
            const std::string &k = entry.first;
            if (stop_.load())
                break;
            const std::string src = read_source(entry.second, k);
            if (src.empty() || src.size() > (1u << 20) || key(src) != k) {
                std::lock_guard<std::mutex> lock(libs_mutex_);
                ++skipped;   // missing, oversized, stale or altered: the draw path compiles it itself
                continue;
            }
            if (!queue_.begin(k)) {
                ++taken_over;   // first use got there first and compiled it
                continue;
            }
            // Run 15 (hitch): kWarmParallel compiles in flight (the Metal compiler service runs them
            // in parallel: 45 cold libraries took 6.1 s one at a time, 2.0 s four at a time).
            dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
            dispatch_group_enter(group);
            @autoreleasepool {
                const std::string key_copy = k;
                const bool keep = !pipe_order_.empty();
                [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                   options:opts
                         completionHandler:^(id<MTLLibrary> lib, NSError *) {
                           queue_.finish(key_copy, lib, lib != nil);
                           {
                               std::lock_guard<std::mutex> lock(libs_mutex_);
                               ++(lib ? compiled : skipped);
                               if (lib && keep)
                                   libs_[key_copy] = lib;   // kept for the pipeline half below
                           }
                           dispatch_semaphore_signal(slots);
                           dispatch_group_leave(group);
                         }];
            }
        }
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        fprintf(stderr,
                "d3d9 metal: shader warm-up compiled %u recorded libraries in the background in %.1f ms "
                "(%u skipped, %u compiled first by the draw path, %zu recorded%s)\n",
                compiled, (os_monotonic_ns() - t0) / 1e6, skipped, taken_over, order_.size(),
                stop_.load() ? ", stopped early" : "");
        if (!pipe_order_.empty() && !stop_.load())
            run_pipelines(opts);
    }

    id<MTLDevice> dev_;
    std::string dir_, identity_;
    std::vector<std::pair<std::string, std::string>> order_;   // key, directory (worker only)
    ShaderWarmQueue<id<MTLLibrary>> queue_;
    std::set<std::string> profile_keys_;   // keys listed in the profile's order.txt (writable copies)
    std::mutex record_mutex_;
    uint32_t repaired_ = 0;
    bool write_failure_logged_ = false;
    std::atomic<bool> stop_{false};
    static constexpr long kWarmParallel = 4;   // run 15: worker compiles in flight
    std::mutex libs_mutex_;                    // libs_ and the counters while worker compiles finish
    // Run 15/16: draw-path background compiles (declared after dev_ and queue_, which it uses). The compiler seam:
    // the production compiler is this one Metal call; its completion hands the library (nil on failure) back.
    ShaderAsyncCompiles<id<MTLLibrary>> async_{
        queue_, [this](const std::string &src, ShaderAsyncCompiles<id<MTLLibrary>>::Done done) {
            [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                               options:options()
                     completionHandler:^(id<MTLLibrary> l, NSError *) { done(l); }];
        }};
    // Test70 pipeline half. pipe_order_, lib_dir_ and libs_ are filled before the worker starts or
    // used by the worker only; the rest is shared under pipe_mutex_.
    bool pipes_on_ = true;
    std::vector<std::string> pipe_order_;
    std::unordered_map<std::string, std::string> lib_dir_;
    std::unordered_map<std::string, id<MTLLibrary>> libs_;
    std::mutex pipe_mutex_;
    std::set<std::string> pipe_known_, pipe_claimed_;
    std::unordered_map<std::string, id<MTLRenderPipelineState>> pipe_ready_;
    uint32_t pipe_hits_ = 0;
    std::thread worker_;
};

// Test157b: the number of cached fitted depth buffers, for host_tests (SIZE_MAX until a renderer exists).
std::atomic<size_t> g_fitted_depth_entries{SIZE_MAX};

class Renderer final : public D9Backend {
  public:
    const char *name() const override {
        return "Metal";
    }
    explicit Renderer(gpu::MetalDevice *device)
        : device_(device), mtl_(device->native()), queue_(device->native_queue()) {
        bc_ = false;
        if (@available(macOS 11.0, iOS 16.4, *))
            bc_ = mtl_.supportsBCTextureCompression;
#if TARGET_OS_OSX
        bc_ = true;
#endif
        if (pbw_active_)   // Remaster run 12 pilot: slot rows and the manifest, before the cache starts
            pbw_load();
        rcache_.start(mtl_, bc_); // Remaster A1: workers run only when a pack is configured
        NSError *error = nil;
        utility_ = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:kUtilitySource]
                                      options:nil
                                        error:&error];
        if (!utility_)
            fprintf(stderr, "d3d9 metal: utility shaders failed: %s\n",
                    error.localizedDescription.UTF8String);
        frames_ = dispatch_semaphore_create(3);
        // v4 Metal 4 (docs/METAL4_PLAN.md; RECOMP_METAL4=1, the iOS panel's Diagnostics Renderer switch): the renderer's
        // command buffers go to a Metal 4 queue; anything missing and it runs on classic Metal as before.
        if (const char *e = getenv("RECOMP_METAL4"); e && *e == '1') {
            if (@available(macOS 26.0, iOS 26.0, *)) {
                NSString *why = nil;
                m4_ = [D9M4 contextWithDevice:mtl_ why:&why];
                const bool fx = m4_ && [MTLFXTemporalScalerDescriptor supportsMetal4FX:mtl_];
                fprintf(stderr, "[gpu] renderer: Metal 4%s%s%s\n", m4_ ? (fx ? " (MetalFX 4)" : " (no MetalFX 4: native 3D)") : " NOT available: classic Metal (",
                        m4_ ? "" : why.UTF8String, m4_ ? "" : ")");
                m4_fx_ = fx;
            } else {
                fprintf(stderr, "[gpu] renderer: Metal 4 NOT available on this OS: classic Metal\n");
            }
        } else {
            fprintf(stderr, "[gpu] renderer: classic Metal\n");
        }
        const char *warm = recomp_env("D3D9_SHADER_WARM_DIR"), *seed = recomp_env("D3D9_SHADER_WARM_SEED");
        if ((warm && *warm) || (seed && *seed)) {
            // Namespace of the recorded keys. The compile options themselves are always the current
            // run's (the background and draw paths share ShaderWarm::options()).
            std::string identity = "msl-v1";
            if (@available(macOS 15.0, iOS 18.0, *))
                identity += "-safe";
            warm_ = std::make_unique<ShaderWarm>(mtl_, warm ? warm : "", seed ? seed : "", identity);
        }
        MTLVertexDescriptor *vd = nil;
        (void)vd;
        zero_ = [mtl_ newBufferWithLength:64 options:MTLResourceStorageModeShared];
        memset(zero_.contents, 0, 64);
        const char *trace = recomp_env("D3D9_TRACE");
        if (trace && !strcmp(trace, "1")) {
            capture_mode_ = recomp_env("D3D9_CAPTURE_SESSION") != nullptr;
            if (capture_mode_) {
                trace_ = capture_ring();
                capture_spare_ = capture_ring();
                trace_->retain = false;
            } else
                trace_ = std::make_unique<d9trace::Ring>();
            fprintf(stderr,
                    "d3d9 trace: ten-frame metadata history enabled (%zu bytes); "
                    "probe requests flush history, no texture readback\n",
                    trace_->allocated_bytes());
            if (capture_mode_)
                fprintf(stderr,
                        "capture: DISARMED; F8 arms, wait 2s, F8 triggers; 6144 draws, 1024 "
                        "events, 2Hz full samples, 6s post, 3 captures; buffers=%zu bytes\n",
                        trace_->allocated_bytes() * 2);
        }
    }

    ~Renderer() override {
        if (capture_mode_ && capture_control_.state == capture_state::Capturing)
            capture_save();
        if (capture_job_.valid()) {
            capture_job_.wait();
            capture_poll();
        }
    }

    bool ok() const {
        return utility_ != nil;
    }

    // ---- textures ------------------------------------------------------
    void define(const HostD9TextureDesc &d) override {
        end_pass();
        if (up_.open && (d.id == up_.bb || d.id == up_.ds))
            upscale_abort(d.id); // Remaster run 13: Reset inside a scene (the new texture starts native)
        if (fnd_on_ && fnd_.bb && (d.id == fnd_.bb || d.id == fnd_.ds))
            fnd_drop(); // Remaster run 14 foundation: Reset / resize
        Tex &t = textures_[d.id];
        if (rtex_.on() && d.key && t.desc.key)
            rtex_.redefined(); // ids are never reused and each object is defined once: must stay 0 (C-03 M2)
        if (t.imported)
            device_->destroy(t.imported);
        forget_fitted_depth(t); // a redefinition replaces the textures
        t = Tex{};
        if (trace_)
            t.generation = ++resource_generation_;
        t.desc = d;
        if (rtex_.on())
            rtex_.defined(d);
        const bool volume = d.kind == HOST_D9_TEX_VOLUME;
        // This path admits uncompressed formats only. Volumes are sampled
        // resources, never D3D9 render targets.
        t.info = format_info(d.format, bc_ && !volume);
        if (volume && (!d9volume::extent(d.width, d.height, d.depth, d.levels) ||
                       !d9volume::pixel_bytes(d.format) || d.samples > 1 || d.usage ||
                       t.info.conv == Conv::Unsupported ||
                       t.info.pixel == MTLPixelFormatDepth32Float_Stencil8)) {
            index_texture(d.id, &t);
            fprintf(stderr, "d3d9 metal: invalid/unsupported volume %u\n", d.id);
            return;
        }
        if (t.info.conv == Conv::Unsupported) {
            fprintf(stderr, "d3d9 metal: texture format %08x is drawn as BGRA8\n", d.format);
            t.info.conv = Conv::Direct;
        }
        bool depth =
            (d.usage & HOST_D9_USAGE_DEPTH) || t.info.pixel == MTLPixelFormatDepth32Float_Stencil8;
        if (depth)
            t.info.pixel = MTLPixelFormatDepth32Float_Stencil8;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = volume                       ? MTLTextureType3D
                         : d.kind == HOST_D9_TEX_CUBE ? MTLTextureTypeCube
                                                      : MTLTextureType2D;
        td.pixelFormat = t.info.pixel;
        td.width = d.width ? d.width : 1;
        td.height = d.kind == HOST_D9_TEX_CUBE ? td.width : (d.height ? d.height : 1);
        td.depth = volume ? d.depth : 1;
        // Render targets and depth buffers render at the chosen scale; the
        // guest keeps seeing its own sizes.
        bool target = (d.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) || depth;
        if (target && !scale_chosen_ && d.kind == HOST_D9_TEX_2D) {
            // The first target is the back buffer; its height is what the
            // scale is measured against from then on.
            scale_chosen_ = true;
            base_rows_ = d.height;
            const float want = wanted_scale();
            if (want > 0)
                scale_ = want;
            fprintf(stderr, "d3d9: render targets at %.2fx the game's size\n", scale_);
        }
        if (target && d.kind == HOST_D9_TEX_2D && scale_ != 1.0f) {
            float s = scale_;
            NSUInteger limit = 16384;
            while (s > 1.0f && (td.width * s > limit || td.height * s > limit))
                s -= 0.25f;
            t.scale = std::max(1.0f, s);
            td.width = (NSUInteger)std::lround(td.width * t.scale);
            td.height = (NSUInteger)std::lround(td.height * t.scale);
        }
        uint32_t max_levels = 1;
        for (uint32_t s = (uint32_t)std::max({td.width, td.height, td.depth}); s > 1; s >>= 1)
            ++max_levels;
        td.mipmapLevelCount = std::max<uint32_t>(1, std::min(d.levels ? d.levels : 1, max_levels));
        td.usage = MTLTextureUsageShaderRead;
        if ((d.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) || depth)
            td.usage |= MTLTextureUsageRenderTarget;
        td.storageMode = depth ? MTLStorageModePrivate : MTLStorageModeShared;
        if (t.info.block)
            td.usage = MTLTextureUsageShaderRead;
        t.texture = [mtl_ newTextureWithDescriptor:td];
        note_alloc(AS_TEXTURE, t.texture, td.width, td.height, 1, d.format);
        t.cube = td.textureType == MTLTextureTypeCube;
        t.depth = td.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        {
            // Diagnostic (RECOMP_D3D9_STATS, Test73): every render target at least 1280 wide, with the
            // samples the game asked for, so an anti-aliasing change through a reset can be seen.
            static const bool stats = recomp_env("D3D9_STATS") != nullptr;
            if (stats && target && td.width >= 1280)
                fprintf(stderr, "d3d9 metal: render target %u defined %lux%lu, %u samples requested\n", d.id,
                        (unsigned long)td.width, (unsigned long)td.height, d.samples);
        }
        if (d.samples > 1 && target && d.kind == HOST_D9_TEX_2D && !t.info.block && t.texture) {
            // 4 for 4 or more, else 2; a count the GPU does not support falls to the next lower one (d3d9_msaa.h).
            const NSUInteger n = d9msaa::fit(d.samples, [&](uint32_t k) { return (bool)[mtl_ supportsTextureSampleCount:k]; });
            if (n < (d.samples >= 4 ? 4u : 2u) && !msaa_fallback_logged_) {
                msaa_fallback_logged_ = true;
                fprintf(stderr, "d3d9 metal: this GPU does not support %ux MSAA; render targets use %lux%s\n",
                        d.samples >= 4 ? 4u : 2u, (unsigned long)n, n > 1 ? "" : " (no multisampling)");
            }
            if (n > 1) {
                MTLTextureDescriptor *md = [MTLTextureDescriptor new];
                md.textureType = MTLTextureType2DMultisample;
                md.pixelFormat = td.pixelFormat;
                md.width = td.width;
                md.height = td.height;
                md.sampleCount = n;
                md.usage = MTLTextureUsageRenderTarget;
                if (depth)
                    md.usage |= MTLTextureUsageShaderRead; // Test121: copied by StretchRect
                md.storageMode = MTLStorageModePrivate;
                t.msaa = [mtl_ newTextureWithDescriptor:md];
                note_alloc(AS_MSAA, t.msaa, md.width, md.height, n, d.format);
                t.samples = t.msaa ? (uint32_t)n : 1;
                // Test70 (stable-bar evidence): which targets are really multisampled, once per size.
                if (t.msaa && msaa_logged_.size() < 16 && msaa_logged_.insert((uint64_t)td.width << 32 | td.height << 4 | n).second)
                    fprintf(stderr, "d3d9 metal: render target %lux%lu is multisampled x%lu (%u requested), resolved into its texture\n",
                            (unsigned long)td.width, (unsigned long)td.height, (unsigned long)n, d.samples);
            }
        }
        index_texture(d.id, &t);
        if (trace_)
            if (auto *e = trace_->event("texture_define", trace_context(), trace_texture(d.id))) {
                e->values[0] = d.width;
                e->values[1] = d.height;
                e->values[2] = d.format;
                e->values[3] = d.usage;
                e->values[4] = d.kind;
                e->values[5] = d.levels;
                e->values[6] = d.samples;
                e->values[7] = t.texture != nil;
                e->values[8] = (uint32_t)t.texture.width;
                e->values[9] = (uint32_t)t.texture.height;
                e->values[10] = d.depth;
            }
        if (!t.texture)
            fprintf(stderr, "d3d9 metal: texture %u (%ux%u fmt %08x) was not created\n", d.id,
                    d.width, d.height, d.format);
    }

    void drop(uint32_t id) override {
        auto it = textures_.find(id);
        if (it == textures_.end())
            return;
        end_pass();
        if (up_.open && (id == up_.bb || id == up_.ds))
            upscale_abort(id); // Remaster run 13
        if (fnd_on_ && fnd_.bb && (id == fnd_.bb || id == fnd_.ds))
            fnd_drop(); // Remaster run 14 foundation
        diag_shadow_ids_.erase(id); // the iOS reflection test: an id may come back as another texture
        if (id == up_.last_bb)
            up_.last_bb = 0;
        if (id == up_.last_ds)
            up_.last_ds = 0;
        if (trace_)
            trace_->event("texture_drop", trace_context(), trace_texture(id));
        if (it->second.imported)
            device_->destroy(it->second.imported);
        forget_fitted_depth(it->second);
        index_texture(id, nullptr);
        if (rtex_.on())
            rtex_.dropped(it->second.desc.key);
        textures_.erase(it);
    }

    void tag(uint32_t id, uint64_t key) override {
        auto it = textures_.find(id);
        if (it == textures_.end())
            return;
        it->second.desc.key = key;
        if (rtex_.on())
            rtex_.tagged_late(key, it->second.desc);
    }

    void upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                uint32_t pitch) override {
        auto it = textures_.find(id);
        if (it == textures_.end() || !it->second.texture || !bytes)
            return;
        Tex &t = it->second;
        if (level >= t.texture.mipmapLevelCount)
            return;
        const bool volume = t.desc.kind == HOST_D9_TEX_VOLUME;
        const uint32_t slices = volume ? std::max(t.desc.depth >> level, 1u) : t.cube ? 6u : 1u;
        if (face >= slices)
            return;
        if (t.info.pixel == MTLPixelFormatDepth32Float_Stencil8)
            return;     // the game's view of a depth buffer is not uploaded
        // Draws already encoded read the old contents. v2.4 (the owner's 14 Oct trace: 5 command buffers a frame, the
        // game ~90 fps with the CPU and GPU half idle): a texture still in use by the GPU is no longer waited for
        // (settle commits and blocks on the GPU); a plain 2D upload goes through a staging buffer and a blit queued
        // after those draws (staged_upload below). Volumes and scaled targets still settle. OFF by default (TEST:
        // RECOMP_STAGED_UPLOADS=1): the intro movie uploaded its frames fast enough that the staging buffers grew the
        // device's memory by 1 GB in 0.6 s and iOS killed the game (owner's iPhone, 15 Oct).
        static const bool staged_on = [] { const char *e = getenv("RECOMP_STAGED_UPLOADS"); return e && *e == '1'; }();
        const bool busy = t.used && t.used > completed_.load();
        const bool staged = staged_on && busy && !volume && t.scale == 1.0f && !t.promoted;
        if (!staged)
            settle(t.used);
        if (rtex_.on()) {
            rtex_.upload(t.desc, face, level, bytes, pitch);
            if (t.desc.key && !level && !face && t.desc.kind == HOST_D9_TEX_2D) {
                if (++t.top_uploads == 1) {
                    rcache_.request(t.desc.key, t.desc.width, t.desc.height);
                    if (pbw_active_ && pbw_ecos_.count(t.desc.key))   // run 12 pilot: its normal slot (2x, same aspect)
                        rcache_.request(t.desc.key ^ remaster::kSlotNormal, t.desc.width, t.desc.height);
                }
                else if (!t.no_replace) {
                    t.no_replace = true; // a dynamic texture (a movie): the game's own contents win
                    rtex_.dropped_rewrite();
                }
            }
        }
        if (trace_) {
            ++t.revision;
            if (auto *e = trace_->event("texture_upload", trace_context(), trace_texture(id))) {
                e->values[0] = face;
                e->values[1] = level;
                e->values[2] = pitch;
            }
        }
        uint32_t w = std::max<uint32_t>((uint32_t)t.desc.width >> level, 1);
        uint32_t h = std::max<uint32_t>(
            (uint32_t)(t.desc.kind == HOST_D9_TEX_CUBE ? t.desc.width : t.desc.height) >> level, 1);
        if (volume) {
            // The API supplies one independently pitched Z slice. Conversion
            // and lifetime follow the same settled-resource rules as 2D uploads.
            const uint32_t source_pitch = w * t.info.bytes;
            if (pitch < source_pitch)
                return;
            const uint8_t *src = bytes;
            uint32_t row = pitch;
            if (t.info.conv != Conv::Direct) {
                convert(t.info.conv, bytes, w, h, pitch, scratch_);
                src = scratch_.data();
                row = w * 4;
            }
            [t.texture replaceRegion:MTLRegionMake3D(0, 0, face, w, h, 1)
                         mipmapLevel:level
                               slice:0
                           withBytes:src
                         bytesPerRow:row
                       bytesPerImage:(NSUInteger)row * h];
            return;
        }
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        if (staged) {
            const uint8_t *src = bytes;
            uint32_t row = pitch, rows = t.info.block ? (h + 3) / 4 : h;
            if (!t.info.block && t.info.conv != Conv::Direct) {
                convert(t.info.conv, bytes, w, h, pitch, scratch_);
                src = scratch_.data();
                row = w * 4;
            }
            staged_upload(t, face, level, w, h, src, row, rows);
            return;
        }
        if (t.scale != 1.0f) {
            // The game wrote a scaled target: stretch its pixels to the target's size.
            std::vector<uint8_t> packed;
            const uint8_t *src = bytes;
            uint32_t src_pitch = pitch;
            if (t.info.conv != Conv::Direct) {
                convert(t.info.conv, bytes, w, h, pitch, scratch_);
                src = scratch_.data();
                src_pitch = w * 4;
            }
            uint32_t pw = std::max<uint32_t>((uint32_t)t.texture.width >> level, 1);
            uint32_t ph = std::max<uint32_t>((uint32_t)t.texture.height >> level, 1);
            uint32_t bpp = t.info.conv == Conv::Direct ? t.info.bytes : 4;
            packed.resize((size_t)pw * ph * bpp);
            for (uint32_t y = 0; y < ph; ++y) {
                uint32_t sy = std::min(h - 1, (uint32_t)(y * (uint64_t)h / ph));
                for (uint32_t x = 0; x < pw; ++x) {
                    uint32_t sx = std::min(w - 1, (uint32_t)(x * (uint64_t)w / pw));
                    memcpy(&packed[((size_t)y * pw + x) * bpp],
                           src + (size_t)sy * src_pitch + (size_t)sx * bpp, bpp);
                }
            }
            [t.texture replaceRegion:MTLRegionMake2D(0, 0, pw, ph)
                         mipmapLevel:level
                               slice:face
                           withBytes:packed.data()
                         bytesPerRow:pw * bpp
                       bytesPerImage:0];
            return;
        }
        if (t.info.block) {
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:bytes
                         bytesPerRow:pitch
                       bytesPerImage:0];
        } else if (t.info.conv == Conv::Direct) {
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:bytes
                         bytesPerRow:pitch
                       bytesPerImage:0];
        } else {
            convert(t.info.conv, bytes, w, h, pitch, scratch_);
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:scratch_.data()
                         bytesPerRow:w * 4
                       bytesPerImage:0];
        }
    }

    bool read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes, uint32_t pitch) override {
        auto it = textures_.find(id);
        if (it == textures_.end() || !it->second.texture)
            return false;
        Tex &t = it->second;
        if (!bytes || level >= t.texture.mipmapLevelCount)
            return false;
        const bool volume = t.desc.kind == HOST_D9_TEX_VOLUME;
        if (face >= (volume ? std::max(t.desc.depth >> level, 1u) : t.cube ? 6u : 1u))
            return false;
        if (t.texture.storageMode == MTLStorageModePrivate || t.info.block)
            return false;
        settle(t.used);
        if (t.promoted)   // PLAN_R2 item 10: the float back buffer read as the guest's 8-bit bytes
            return hdr_read(t, level, face, bytes, pitch);
        uint32_t pw = std::max<uint32_t>((uint32_t)t.texture.width >> level, 1);
        uint32_t ph = std::max<uint32_t>((uint32_t)t.texture.height >> level, 1);
        if (volume) {
            // Only direct 32-bit formats have the existing host read contract.
            if (t.info.conv != Conv::Direct || t.info.bytes != 4 || pitch < pw * 4)
                return false;
            [t.texture getBytes:bytes
                    bytesPerRow:pitch
                  bytesPerImage:(NSUInteger)pitch * ph
                     fromRegion:MTLRegionMake3D(0, 0, face, pw, ph, 1)
                    mipmapLevel:level
                          slice:0];
            return true;
        }
        if (t.scale == 1.0f) {
            [t.texture getBytes:bytes
                    bytesPerRow:pitch
                  bytesPerImage:0
                     fromRegion:MTLRegionMake2D(0, 0, pw, ph)
                    mipmapLevel:level
                          slice:face];
            return true;
        }
        // A scaled target reads back at the game's size, one sample per pixel.
        uint32_t w = std::max<uint32_t>(t.desc.width >> level, 1);
        uint32_t h = std::max<uint32_t>(t.desc.height >> level, 1);
        std::vector<uint8_t> full((size_t)pw * ph * 4);
        [t.texture getBytes:full.data()
                bytesPerRow:pw * 4
              bytesPerImage:0
                 fromRegion:MTLRegionMake2D(0, 0, pw, ph)
                mipmapLevel:level
                      slice:face];
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = std::min(ph - 1, (uint32_t)((y + 0.5) * ph / h));
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t sx = std::min(pw - 1, (uint32_t)((x + 0.5) * pw / w));
                memcpy(bytes + (size_t)y * pitch + (size_t)x * 4, &full[((size_t)sy * pw + sx) * 4],
                       4);
            }
        }
        return true;
    }

    // GetRenderTargetData without the wait: a blit into a shared buffer at the end of the work
    // encoded so far; the command buffer's completion hands the image over (one sample per guest
    // pixel, as read() does). Nothing here waits for the GPU.
    void read_async(uint32_t tex_id, uint32_t face, uint32_t level, uint32_t ticket, uint64_t request,
                    ReadDeliver deliver) override {
        auto it = textures_.find(tex_id);
        if (it == textures_.end() || !it->second.texture) {
            deliver(ticket, request, nullptr, 0, 0);
            return;
        }
        Tex &t = it->second;
        if (t.info.block || t.info.conv != Conv::Direct || t.info.bytes != 4 ||
            t.desc.kind == HOST_D9_TEX_VOLUME || level >= t.texture.mipmapLevelCount ||
            face >= (t.cube ? 6u : 1u) || t.texture.sampleCount > 1) {
            deliver(ticket, request, nullptr, 0, 0);
            return;
        }
        const uint32_t pw = std::max<uint32_t>((uint32_t)t.texture.width >> level, 1);
        const uint32_t ph = std::max<uint32_t>((uint32_t)t.texture.height >> level, 1);
        const uint32_t w = std::max<uint32_t>(t.desc.width >> level, 1);
        const uint32_t h = std::max<uint32_t>(t.desc.height >> level, 1);
        const bool fl = t.promoted;   // PLAN_R2 item 10: RGBA16Float texels, converted on completion
        if (fl)
            ++hdr_readbacks_;
        id<MTLBuffer> buf = [mtl_ newBufferWithLength:(NSUInteger)pw * ph * (fl ? 8 : 4)
                                              options:MTLResourceStorageModeShared];
        note_alloc(AS_READBACK, buf, pw, ph, 1, 0);
        if (!buf) {
            deliver(ticket, request, nullptr, 0, 0);
            return;
        }
        end_pass();
        begin_frame();
        D9BlitEnc *blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:t.texture
                         sourceSlice:face
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(pw, ph, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:(NSUInteger)pw * (fl ? 8 : 4)
            destinationBytesPerImage:(NSUInteger)pw * ph * (fl ? 8 : 4)];
        [blit endEncoding];
        t.used = serial_;
        [cmd_ addDone:^(NSError *error, double) {
          if (error) {
              deliver(ticket, request, nullptr, 0, 0);
              return;
          }
          const uint8_t *src = (const uint8_t *)buf.contents;
          std::vector<uint8_t> conv;   // item 10
          if (fl) {
              conv.resize((size_t)pw * ph * 4);
              hdrconv::rgba16f_to_bgra8(src, conv.data(), (size_t)pw * ph);
              src = conv.data();
          }
          if (pw == w && ph == h) {
              deliver(ticket, request, src, w, h);
              return;
          }
          std::vector<uint8_t> out((size_t)w * h * 4);
          for (uint32_t y = 0; y < h; ++y) {
              const uint32_t sy = std::min(ph - 1, (uint32_t)((y + 0.5) * ph / h));
              for (uint32_t x = 0; x < w; ++x) {
                  const uint32_t sx = std::min(pw - 1, (uint32_t)((x + 0.5) * pw / w));
                  memcpy(&out[((size_t)y * w + x) * 4], src + ((size_t)sy * pw + sx) * 4, 4);
              }
          }
          deliver(ticket, request, out.data(), w, h);
        }];
    }

    // ---- buffers --------------------------------------------------------
    // v4: NOOVERWRITE (D3D9: no queued draw reads the bytes written) writes the live buffer: no replacement, no spare.
    uint64_t in_place_uploads_ = 0;
    void buffer_upload_flags(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes, uint32_t size,
                             uint32_t flags) override {
        if (flags & HOST_D9_UPLOAD_NOOVERWRITE) {
            Buf &b = buffers_[id];
            if (b.buffer && b.buffer.length >= total && b.shadow.size() >= total && bytes && offset + size <= total &&
                !trace_) {
                memcpy(b.shadow.data() + offset, bytes, size);
                for (SpareBuf &s : b.spares) {   // the spares are behind by these bytes too
                    s.lo = s.hi > s.lo ? std::min(s.lo, offset) : offset;
                    s.hi = std::max(s.hi, offset + size);
                }
                memcpy((uint8_t *)b.buffer.contents + offset, bytes, size);
                ++in_place_uploads_;
                return;
            }
        }
        upload_discard_ = (flags & HOST_D9_UPLOAD_DISCARD) != 0;
        buffer_upload(id, total, offset, bytes, size);
        upload_discard_ = false;
    }
    bool upload_discard_ = false;   // the upload in progress follows a DISCARD Lock
    void buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                       uint32_t size) override {
        Buf &b = buffers_[id];
        ++b.win_uploads;
        if (offset + size > total || !bytes)
            return;
        if (trace_)
            trace_->event("buffer_before_upload", trace_context(), trace_buffer(id));
        if (b.shadow.size() < total)
            b.shadow.resize(total, 0);
        memcpy(b.shadow.data() + offset, bytes, size);
        const uint32_t end = offset + size;
        for (SpareBuf &s : b.spares) {
            s.lo = s.hi > s.lo ? std::min(s.lo, offset) : offset;
            s.hi = std::max(s.hi, end);
        }
        if (!b.buffer || b.buffer.length < total) {
            b.buffer = [mtl_ newBufferWithBytes:b.shadow.data()
                                         length:std::max<size_t>(b.shadow.size(), 4)
                                        options:MTLResourceStorageModeShared];
            note_alloc(AS_BUFFER, b.buffer, b.shadow.size(), id, 1, 0);
            b.used = 0;
            b.spares.clear();
            trace_buffer_upload(id, b, total, offset, size, true);
            return;
        }
        // A buffer an unfinished frame reads is replaced rather than written:
        // those draws keep the old one, and nothing waits. The replacement is
        // a spare whose frame has finished, brought up to date by copying
        // only what changed since it was set aside.
        const uint64_t done = completed_.load();
        if (b.used > done) {
            SpareBuf retired{b.buffer, b.used, offset, end};
            size_t pick = b.spares.size();
            for (size_t i = 0; i < b.spares.size(); ++i)
                if (b.spares[i].retired <= done && b.spares[i].buffer.length >= total) {
                    pick = i;
                    break;
                }
            ++b.win_replaced;
            if (pick < b.spares.size()) {
                SpareBuf s = b.spares[pick];
                b.spares.erase(b.spares.begin() + (ptrdiff_t)pick);
                // v4: after a DISCARD only the written bytes are defined: copy those, not all since set aside
                const uint32_t lo = upload_discard_ ? offset : s.lo, hi = upload_discard_ ? end : s.hi;
                if (hi > lo) {
                    memcpy((uint8_t *)s.buffer.contents + lo, b.shadow.data() + lo, hi - lo);
                    b.win_copied += hi - lo;
                }
                b.buffer = s.buffer;
            } else {
                b.buffer = [mtl_ newBufferWithBytes:b.shadow.data()
                                             length:std::max<size_t>(b.shadow.size(), 4)
                                            options:MTLResourceStorageModeShared];
                note_alloc(AS_BUFFER_SPARE, b.buffer, b.shadow.size(), id, 1, 0);
                ++b.win_spares;
                b.win_copied += b.shadow.size();
            }
            b.spares.push_back(retired);
            // v4: as many spares as the buffer cycles through (a buffer the game rewrites n times a frame needs about
            // 3n with frames in flight): with 4, such buffers dropped a spare and made a new one every upload, and on
            // Metal 4 each new one joins the residency set (commits ~2.9 ms a frame late in the owner's race). Up to 64
            // (the owner's race: one 96 KB buffer uploaded ~19 times a frame needs ~57), and 16 MB of spares per buffer.
            size_t spare_bytes = 0;
            for (const SpareBuf &s : b.spares)
                spare_bytes += s.buffer.length;
            while (b.spares.size() > 1 && (b.spares.size() > 64 || spare_bytes > (16u << 20))) {
                spare_bytes -= b.spares.front().buffer.length;
                b.spares.erase(b.spares.begin());
            }
            b.used = 0;
            trace_buffer_upload(id, b, total, offset, size, true);
            return;
        }
        memcpy((uint8_t *)b.buffer.contents + offset, bytes, size);
        trace_buffer_upload(id, b, total, offset, size, false);
    }
    void buffer_drop(uint32_t id) override {
        end_pass();
        if (trace_)
            trace_->event("buffer_drop", trace_context(), trace_buffer(id));
        buffers_.erase(id);
    }

    void admission_accounting(const HostD9Accounting &a) override {
        if (trace_)
            trace_->accounting(a);
    }
    void rejected_draw(const HostD9Draw &d) override {
        if (auto *r = trace_draw(d))
            r->result = d9trace::Result::ShimRejected;
    }

    // ---- drawing --------------------------------------------------------
    // Remaster S3 soft-particles trial, step 2a (TEST ONLY, RECOMP_REMASTER_SOFTFX=1; A/B switch softfx): before
    // the first particle draw of a frame (label IDI_PARTICLES_FX/), end the scene pass and copy its depth (smallest
    // sample) into a single-sample R32F texture a soft-particle shader could read; the next draw reopens the pass
    // with its contents loaded. The fade is OFF here: this measures the cost of the break and the copy (stop rule:
    // at most 1.5 ms GPU p90; challenge C-S3).
    // Step 2b (RECOMP_REMASTER_SOFTFX=2): the fade on. Alpha-blended particle draws (SRCALPHA/INVSRCALPHA, the smoke
    // and spray; S3 step 1) in the captured pass get the soft variant: alpha times the depth-proximity fade over
    // RECOMP_REMASTER_SOFTFX_DIST view units (default 1).
    const int softfx_mode_ = [] { const char *e = getenv("RECOMP_REMASTER_SOFTFX"); return e && (*e == '1' || *e == '2') ? *e - '0' : 0; }();
    const bool softfx_on_ = softfx_mode_ > 0;
    const float soft_inv_dist_ = [] { const char *e = getenv("RECOMP_REMASTER_SOFTFX_DIST"); const float v = e ? (float)atof(e) : 1.0f; return v > 0.01f ? 1.0f / v : 1.0f; }();
    bool soft_done_ = false;
    bool soft_ok_ = false; // this frame's copy exists (the fade may use it)
    uint64_t soft_faded_ = 0;
    uint64_t soft_frames_ = 0;
    uint64_t soft_breaks_ = 0;
    uint64_t soft_kind_draws_ = 0;   // run 15 tunnel2: plain fog / steam draws with the near fade (soft 2 / 3)
    const float soft_fog_inv_ = 1.0f / remaster_smoke::kind_defaults(remaster_smoke::KIND_FOG).soft_dist;   // 4 m
    // Remaster run 14 lit smoke (core mod setting lit_smoke; A/B switch litsmk): particle draws the mod marked with a
    // kind (private render state 248, smoke_light.h) get PixelVariant / VertexVariant smoke and D9SmokeParams at
    // fragment buffer(5). No marker (setting off): nothing here runs past the first test.
    uint64_t smoke_draws_ = 0, smoke_fallback_ = 0, smoke_noclip_ = 0, smoke_frames_ = 0, smoke_draws_at_present_ = 0;
    bool smoke_seen_ = false;   // a marked particle draw arrived (either A/B phase): the per-dump line runs
    uint64_t smoke_by_kind_[remaster_smoke::KIND_MAX + 1] = {};
    std::unordered_map<uint64_t, int> smoke_clip_;   // vs key -> clip register (vertex_clip_regs), -1 none
    remaster_smoke::Kind smoke_kind_[remaster_smoke::KIND_MAX + 1];
    bool smoke_kind_init_ = false;
    const double smoke_t0_ = CACurrentMediaTime();
    id<MTLTexture> soft_depth_ = nil;
    void soft_capture(uint32_t depth_id) {
        Tex *st = tex_ptr(depth_id);
        if (!st || !cmd_)
            return;
        id<MTLTexture> d = st->drawn_depth ? st->drawn_depth : st->texture;
        if (!d || !(d.usage & MTLTextureUsageShaderRead))
            return;
        const bool ms = d.textureType == MTLTextureType2DMultisample;
        end_pass();
        if (!soft_depth_ || soft_depth_.width != d.width || soft_depth_.height != d.height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                          width:d.width height:d.height mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            soft_depth_ = [mtl_ newTextureWithDescriptor:td];
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = soft_depth_;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        D9RenderEnc *e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        const uint32_t saved = pass_samples_;
        pass_samples_ = 1;
        MTLPixelFormat cf[4] = {MTLPixelFormatR32Float, MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso = utility_pipeline(mix(ms ? 0xd3c1ull : 0xd3c0ull, MTLPixelFormatR32Float),
                                                          ms ? @"depth_copy_ms_fs" : @"depth_copy_fs", cf, MTLPixelFormatInvalid, true);
        pass_samples_ = saved;
        if (pso) {
            [e setRenderPipelineState:pso];
            [e setCullMode:MTLCullModeNone];
            [e setViewport:(MTLViewport){0, 0, (double)d.width, (double)d.height, 0, 1}];
            QuadIn q{};
            q.rect[0] = -1.0f; q.rect[1] = 1.0f; q.rect[2] = 1.0f; q.rect[3] = -1.0f;
            q.uv[0] = 0.0f; q.uv[1] = 0.0f; q.uv[2] = 1.0f; q.uv[3] = 1.0f;
            [e setVertexBytes:&q length:sizeof q atIndex:0];
            [e setFragmentTexture:d atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            ++soft_breaks_;
            soft_ok_ = true;
        }
        [e endEncoding];
    }
    // ---- Remaster run 14 B1 height fog (review/run14/smoke-realism-design.md 8 (B); maths in atmosphere.h) ----------
    // RECOMP_REMASTER_ATMOS=1 (the mod's setting `atmosphere`); A/B switch heightfog. Per frame the mod sends its
    // packets (host_d9_atmosphere mode 1) at the main view's begin; the WORLD draws into each target give that target's
    // plurality view-projection (foundation.h); at the soft-particle pass break of the main target (its first particle
    // draw: the opaque scene and soft_depth_ are ready) one full-screen pass blends the fog in, inside the scene pass
    // (every MSAA sample blends with its pixel's fog; the depth is the nearest sample). A frame without particles gets
    // it at the scene end (mode 2) instead. Off (unset): none of this runs (no break, no pass, no pipeline).
    // TEST ONLY: RECOMP_REMASTER_ATMOS_HAZE / _FOG = "density,base,scale height" override a layer; _DEBUG=1 shows the
    // fog factor (1 - T) as grey.
    const bool atm_on_ = [] { const char *e = getenv("RECOMP_REMASTER_ATMOS"); return e && *e == '1'; }();
    const bool atm_debug_ = [] { const char *e = getenv("RECOMP_REMASTER_ATMOS_DEBUG"); return e && *e == '1'; }();
    remaster_atm::Params atm_p_{};
    bool atm_fresh_ = false, atm_active_ = false, atm_done_ = false, atm_pending_ = false, atm_lib_tried_ = false;
    struct AtmTarget {
        remaster_fnd::Plurality pl;   // this frame's WORLD clip blocks into the target
        HostD9Target t{};             // the pass it was drawn in (colour and depth), for the scene-end reopen
    };
    std::unordered_map<uint32_t, AtmTarget> atm_plur_;
    std::unordered_map<uint64_t, int32_t> atm_regs_;
    bool atm_cam_have_ = false, atm_have_p_ = false;   // COMB14B fix: the mod's camera (0x200) and the kept P
    remaster_fnd::M4 atm_cam_v_{}, atm_p_kept_{};
    uint64_t atm_vp_by_[4] = {}, atm_reopen_ = 0, atm_passes_at_present_ = 0;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> atm_psos_;
    id<MTLLibrary> atm_lib_ = nil;
    id<MTLDepthStencilState> atm_ds_ = nil;
    uint64_t atm_frames_ = 0, atm_packets_ = 0, atm_passes_ = 0, atm_late_ = 0, atm_skip_[6] = {};
    float atm_last_eye_z_ = 0.0f;
    void atm_override(const char *name, float *dst) {
        const char *e = getenv(name);
        float v[3];
        if (e && sscanf(e, "%f%*[,:]%f%*[,:]%f", &v[0], &v[1], &v[2]) == 3)
            memcpy(dst, v, sizeof v);
    }
    void atmosphere(const float *a, const float *b) override {
        if (!atm_on_)
            return;
        if (b[15] == 1.0f) {   // this frame's parameters (the main view's begin)
            memcpy(atm_p_.a, a, sizeof atm_p_.a);
            memcpy(atm_p_.b, b, sizeof atm_p_.b);
            atm_override("RECOMP_REMASTER_ATMOS_HAZE", &atm_p_.b[0]);
            atm_override("RECOMP_REMASTER_ATMOS_FOG", &atm_p_.b[3]);
            atm_fresh_ = true;
            atm_active_ = remaster_atm::atm_active(atm_p_);
            if (++atm_packets_ == 1)
                fprintf(stderr, "[remaster] atmosphere: on (first packet: haze %.5g/%.4g/%.4g fog %.5g/%.4g/%.4g strength %.3g "
                                "g %.3g debug %d)\n",
                        atm_p_.b[0], atm_p_.b[1], atm_p_.b[2], atm_p_.b[3], atm_p_.b[4], atm_p_.b[5], atm_p_.b[12],
                        atm_p_.a[3], (int)atm_debug_);
            return;
        }
        if (b[15] != 2.0f || !atm_fresh_ || !atm_active_ || atm_done_ || recomp_ab::old(recomp_ab::ATMOS))
            return;
        // The scene end of a frame whose main target drew no particle (or whose break went elsewhere): the pass now.
        // COMB14B: here the open pass is usually no longer the scene's (end-not-scene counted every frame the break
        // had no camera), so the main target's own pass (colour + depth, latched at its WORLD draws) is reopened.
        const uint32_t main = atm_main_target();
        auto mt = main ? atm_plur_.find(main) : atm_plur_.end();
        if (mt == atm_plur_.end() || !mt->second.t.depth.id) {
            ++atm_skip_[4];
            return;
        }
        const HostD9Target t = mt->second.t;
        if (!enc_ || pass_color_id_ != main)
            ++atm_reopen_;
        soft_done_ = true;
        soft_capture(t.depth.id);
        if (!soft_ok_ || !ensure_pass(t)) {
            ++atm_skip_[5];
            return;
        }
        if (atm_draw())
            ++atm_late_;
    }
    uint32_t atm_main_target() const {
        uint32_t best = 0, n = 0;
        for (const auto &kv : atm_plur_)
            if (kv.second.pl.world_draws > n) {
                n = kv.second.pl.world_draws;
                best = kv.first;
            }
        return best;
    }
    bool atm_want(uint32_t target) const {
        return atm_on_ && atm_fresh_ && atm_active_ && !atm_done_ && target && !recomp_ab::old(recomp_ab::ATMOS) &&
               target == atm_main_target();
    }
    // A WORLD draw (before the pass): its clip constant block into its target's plurality.
    void atm_capture(const HostD9Draw &d, uint64_t vs_key, const d9sh::Program &vp) {
        if (!pass_color_id_ || !d.label || strncmp(d.label, "IDI_WORLD_FX/", 13))
            return;
        AtmTarget &at = atm_plur_[pass_color_id_];
        at.t = pass_target_;
        remaster_fnd::Plurality &pl = at.pl;
        pl.world();
        auto it = atm_regs_.find(vs_key);
        if (it == atm_regs_.end()) {
            uint32_t a = 0;
            it = atm_regs_.emplace(vs_key, d9msl::vertex_clip_regs(vp, &a) && a < 252 ? (int32_t)a : -1).first;
        }
        if (it->second >= 0 && d.vconst && (uint32_t)it->second + 4 <= d.vconst_count)
            pl.add(d.vconst + 4 * it->second);
    }
    id<MTLRenderPipelineState> atm_pipeline() {
        if (!atm_lib_tried_) {
            atm_lib_tried_ = true;
            NSError *error = nil;
            atm_lib_ = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:remaster_atm::atm_source()] options:nil
                                            error:&error];
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = NO;
            atm_ds_ = [mtl_ newDepthStencilStateWithDescriptor:dd];
            fprintf(stderr, "[remaster] atmosphere: library %s%s%s\n", atm_lib_ ? "ready" : "FAILED", atm_lib_ ? "" : ": ",
                    atm_lib_ ? "" : (error ? error.localizedDescription.UTF8String : "?"));
        }
        if (!atm_lib_ || !atm_ds_)
            return nil;
        const MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        uint64_t key = pass_samples_ * 0x9e3779b97f4a7c15ull ^ (uint64_t)df;
        for (int i = 0; i < 4; ++i)
            key = key * 1099511628211ull ^ (uint64_t)(pass_color_[i] ? pass_color_[i].pixelFormat : MTLPixelFormatInvalid);
        auto it = atm_psos_.find(key);
        if (it != atm_psos_.end())
            return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.rasterSampleCount = std::max<uint32_t>(pass_samples_, 1);
        pd.vertexFunction = [atm_lib_ newFunctionWithName:@"atm_vs"];
        pd.fragmentFunction = [atm_lib_ newFunctionWithName:@"atm_fs"];
        for (int i = 0; i < 4; ++i) {
            if (!pass_color_[i])
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
            ca.pixelFormat = pass_color_[i].pixelFormat;
            ca.writeMask = i == 0 ? (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue)
                                  : MTLColorWriteMaskNone;
            if (i == 0) {   // col T + L_in (1 - T): src ONE, dst SRC_ALPHA (a = T); the frame's alpha is kept
                ca.blendingEnabled = YES;
                ca.rgbBlendOperation = MTLBlendOperationAdd;
                ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                ca.destinationRGBBlendFactor = MTLBlendFactorSourceAlpha;
                ca.alphaBlendOperation = MTLBlendOperationAdd;
                ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
                ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
            }
        }
        if (df != MTLPixelFormatInvalid) {
            pd.depthAttachmentPixelFormat = df;
            if (df == MTLPixelFormatDepth32Float_Stencil8)
                pd.stencilAttachmentPixelFormat = df;
        }
        NSError *error = nil;
        id<MTLRenderPipelineState> pso = [mtl_ newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pso)
            fprintf(stderr, "[remaster] atmosphere: pipeline FAILED: %s\n", error ? error.localizedDescription.UTF8String : "?");
        atm_psos_[key] = pso;
        return pso;
    }
    // The pass, inside the open scene pass (after a soft break). True when it drew.
    bool atm_draw() {
        auto pit = atm_plur_.find(pass_color_id_);
        static const remaster_fnd::Plurality empty;
        remaster_fnd::M4 vp{};
        const int by = remaster_atm::atm_choose_vp(pit != atm_plur_.end() ? pit->second.pl : empty,
                                                   atm_cam_have_ ? &atm_cam_v_ : nullptr, &atm_p_kept_, &atm_have_p_, &vp);
        ++atm_vp_by_[by];
        if (by == remaster_atm::ATM_VP_NONE) {
            ++atm_skip_[0];
            return false;
        }
        remaster_atm::AtmU u;
        if (!remaster_atm::atm_uniforms(vp.m, atm_p_, atm_debug_, &u)) {
            ++atm_skip_[1];
            return false;
        }
        if (!enc_ || !pass_color_[0] || !soft_depth_ || soft_depth_.width != pass_color_[0].width ||
            soft_depth_.height != pass_color_[0].height) {
            ++atm_skip_[2];
            return false;
        }
        id<MTLRenderPipelineState> pso = atm_pipeline();
        if (!pso) {
            ++atm_skip_[3];
            return false;
        }
        es_ = EncoderState{}; // what follows is not a draw's state
        [enc_ setRenderPipelineState:pso];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setDepthStencilState:atm_ds_];
        const NSUInteger w = pass_color_[0].width, h = pass_color_[0].height;
        [enc_ setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, w, h}];
        [enc_ setFragmentTexture:soft_depth_ atIndex:0];
        [enc_ setFragmentBytes:&u length:sizeof u atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        es_ = EncoderState{};
        atm_done_ = true;
        atm_last_eye_z_ = u.eye[2];
        ++atm_passes_;
        return true;
    }
    void atm_present() {
        if (++atm_frames_ % 600 == 0)
            fprintf(stderr, "[remaster] atmosphere: frame %llu packets %llu passes %llu (at the scene end %llu) skipped: "
                            "no camera %llu bad camera %llu size %llu pipeline %llu end-not-scene %llu end-capture %llu; "
                            "eye z %.1f\n",
                    (unsigned long long)atm_frames_, (unsigned long long)atm_packets_, (unsigned long long)atm_passes_,
                    (unsigned long long)atm_late_, (unsigned long long)atm_skip_[0], (unsigned long long)atm_skip_[1],
                    (unsigned long long)atm_skip_[2], (unsigned long long)atm_skip_[3], (unsigned long long)atm_skip_[4],
                    (unsigned long long)atm_skip_[5], (double)atm_last_eye_z_);
        if (atm_frames_ % 600 == 0)
            fprintf(stderr, "[remaster] atmosphere: frame %llu camera: by the camera's cluster %llu kept P %llu share rule "
                            "%llu none %llu; scene-end reopens %llu\n",
                    (unsigned long long)atm_frames_, (unsigned long long)atm_vp_by_[1], (unsigned long long)atm_vp_by_[2],
                    (unsigned long long)atm_vp_by_[3], (unsigned long long)atm_vp_by_[0], (unsigned long long)atm_reopen_);
        atm_fresh_ = atm_done_ = atm_pending_ = atm_cam_have_ = false;
        for (auto &kv : atm_plur_)
            kv.second.pl.reset();
        if (atm_plur_.size() > 64)
            atm_plur_.clear();
    }
    // Remaster run 12 step 0 (TEST ONLY): classify a draw for the sun-share probe and count it in the census. Sets
    // variant.pbrprobe only for a WORLD lighting pixel shader drawn with a vertex shader whose d1 is the sun.
    // Run 13 census (TEST ONLY, RECOMP_REMASTER_CENSUS=<dir>): once per (label, vs, ps, stage-0 texture key) a log line
    // with the draw's label, program keys, stage-0..3 texture keys/sizes/kinds, target size, samples and blend; and the
    // default translations of both programs as <dir>/<key>.vs.msl / .ps.msl (labelled shader facts for the run 13
    // designs: glass, water, front-end floor, smoke, vehicle flares, camera). Off: one static check per draw.
    void census(const HostD9Draw &d, uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp, const d9sh::Program &pp,
                Tex *const *bound) {
        static const char *dir = [] { const char *e = getenv("RECOMP_REMASTER_CENSUS"); return e && *e ? e : nullptr; }();
        if (!dir)
            return;
        static std::unordered_set<uint64_t> seen, written;
        const uint64_t t0 = bound[0] ? bound[0]->desc.key : 0;
        uint64_t h = vs_key * 0x9e3779b97f4a7c15ull ^ ps_key ^ t0 * 0xc2b2ae3d27d4eb4full;
        for (const char *c = d.label ? d.label : ""; *c; ++c)
            h = (h ^ (uint8_t)*c) * 1099511628211ull;
        if (!seen.insert(h).second || seen.size() > 20000)
            return;
        char tex[256] = "";
        size_t o = 0;
        for (int s = 0; s < 4 && o < sizeof tex; ++s) {
            const Tex *t = bound[s];
            o += snprintf(tex + o, sizeof tex - o, " t%d %016llx %ux%u k%d%s", s, (unsigned long long)(t ? t->desc.key : 0),
                          t ? t->desc.width : 0, t ? t->desc.height : 0, t ? (int)t->desc.kind : -1,
                          t && t->depth ? "d" : "");
        }
        fprintf(stderr, "[remaster] census: %s vs %016llx ps %016llx%s target %u samples %u blend %u src %u dst %u ztest %u "
                        "zwrite %u\n",
                d.label ? d.label : "(no label)", (unsigned long long)vs_key, (unsigned long long)ps_key, tex,
                d.target.color[0].id, pass_samples_, d.render_state[27], d.render_state[19], d.render_state[20],
                d.render_state[7], d.render_state[14]);
        auto dump = [&](uint64_t key, const d9sh::Program &p, bool pixel) {
            if (!written.insert(key * 2 + pixel).second)
                return;
            std::string src, why;
            const bool ok = pixel ? d9msl::pixel_source(p, d9msl::PixelVariant{}, &src, &why)
                                  : d9msl::vertex_source(p, &src, &why);
            char path[1200];
            snprintf(path, sizeof path, "%s/%016llx.%s.msl", dir, (unsigned long long)key, pixel ? "ps" : "vs");
            if (FILE *f = fopen(path, "w")) {
                fputs(ok ? src.c_str() : ("// untranslated: " + why + "\n").c_str(), f);
                fclose(f);
            }
        };
        dump(vs_key, vp, false);
        dump(ps_key, pp, true);
    }
    void pbr_probe(const HostD9Draw &d, uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp,
                   const d9sh::Program &pp, const Tex *t0, d9msl::PixelVariant &variant) {
        auto pi = pbr_ps_.find(ps_key);
        if (pi == pbr_ps_.end())
            pi = pbr_ps_.emplace(ps_key, d9msl::pixel_is_pbrworld(pp)).first;
        if (!pi->second || !(variant.depth_mask & 2u))   // stage 1 must hold the shadow map (depth)
            return;
        auto vi = pbr_vs_.find(vs_key);
        if (vi == pbr_vs_.end()) {
            vi = pbr_vs_.emplace(vs_key, d9msl::vertex_sun_flow(vp)).first;
            std::string cs;
            for (uint32_t c : vi->second.consts)
                cs += " c" + std::to_string(c);
            fprintf(stderr, "[remaster] pbrprobe: vs %016llx (with ps %016llx): %s%s; d1 reads%s\n",
                    (unsigned long long)vs_key, (unsigned long long)ps_key, vi->second.ok ? "sun in d1" : "NOT matched: ",
                    vi->second.why.c_str(), cs.c_str());
        }
        if (!pbr_keys_loaded_) {
            pbr_keys_loaded_ = true;
            if (const char *path = getenv("RECOMP_REMASTER_PBRPROBE_KEYS"))
                if (FILE *f = fopen(path, "r")) {
                    char line[512];
                    while (fgets(line, sizeof line, f))
                        if (strstr(line, "\tbuilt"))
                            pbr_keys_.insert(strtoull(line, nullptr, 16));
                    fclose(f);
                }
            fprintf(stderr, "[remaster] pbrprobe: allow list %zu keys\n", pbr_keys_.size());
        }
        const uint64_t key = t0 ? t0->desc.key : 0;
        const bool allow = key && pbr_keys_.count(key);
        const NSUInteger w = pass_color_[0] ? pass_color_[0].width : 0, h = pass_color_[0] ? pass_color_[0].height : 0;
        pbr_main_w_ = std::max<NSUInteger>(pbr_main_w_, w);
        const bool main = w && w == pbr_main_w_;   // the widest target seen (the scene; mirror and env maps are smaller)
        std::string label = d.label ? d.label : "(no label)";
        if (size_t sl = label.find('/'); sl != std::string::npos)
            label.resize(sl + 1);
        char buf[320];
        snprintf(buf, sizeof buf, "ps %016llx vs %016llx %s %s %lux%lu %s", (unsigned long long)ps_key,
                 (unsigned long long)vs_key, vi->second.ok ? "sun-d1" : "UNMATCHED-VS", label.c_str(), (unsigned long)w,
                 (unsigned long)h, allow ? "allow" : "other");
        ++pbr_census_[buf];
        if (!vi->second.ok)
            return;
        if (allow)
            ++pbr_key_draws_[key];
        if (pbr_log_consts_ && allow && main && d.vconst && d.pconst) {
            pbr_log_consts_ = false;
            std::string cs;
            char cb[96];
            for (uint32_t c : vi->second.consts)
                if (c < d.vconst_count) {
                    snprintf(cb, sizeof cb, " c%u %.3f %.3f %.3f %.3f", c, d.vconst[4 * c], d.vconst[4 * c + 1],
                             d.vconst[4 * c + 2], d.vconst[4 * c + 3]);
                    cs += cb;
                }
            for (uint32_t c = 0; c < 4 && c < d.pconst_count; ++c) {
                snprintf(cb, sizeof cb, " ps.c%u %.3f %.3f %.3f %.3f", c, d.pconst[4 * c], d.pconst[4 * c + 1],
                         d.pconst[4 * c + 2], d.pconst[4 * c + 3]);
                cs += cb;
            }
            fprintf(stderr, "[remaster] pbrprobe: consts key %016llx vs %016llx:%s\n", (unsigned long long)key,
                    (unsigned long long)vs_key, cs.c_str());
        }
        variant.pbrprobe = (uint8_t)(1 + (allow ? 1 : 0) + (main ? 2 : 0));
        ++pbr_draws_;
    }
    void pbr_report() {
        fprintf(stderr, "[remaster] pbrprobe: frame %llu probed draws %llu (fallback without probe %llu)\n",
                (unsigned long long)pbr_frames_, (unsigned long long)pbr_draws_, (unsigned long long)pbr_fallback_);
        for (const auto &kv : pbr_census_)
            fprintf(stderr, "[remaster] pbrprobe: census %s draws %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
        std::vector<std::pair<uint64_t, uint64_t>> top;
        for (const auto &kv : pbr_key_draws_)
            top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        std::string ks;
        char kb[48];
        for (size_t i = 0; i < top.size() && i < 40; ++i) {
            snprintf(kb, sizeof kb, " %016llx:%llu", (unsigned long long)top[i].second, (unsigned long long)top[i].first);
            ks += kb;
        }
        fprintf(stderr, "[remaster] pbrprobe: allow keys drawn %zu:%s\n", top.size(), ks.c_str());
        if (!pbq_)
            return;
        // Windowed (reset after each report). The GPU may still be adding to the buffer: a few counts can fall either side.
        uint32_t *q = (uint32_t *)pbq_.contents;
        static const char *const metric[2] = {"final", "lit"}, *const sub[3] = {"all", "shadow>=0.5", "sunfacing"};
        for (int m = 0; m < 2; ++m)
            for (int c = 0; c < 4; ++c)
                for (int u = 0; u < 3; ++u) {
                    const uint32_t *b = q + ((m * 4 + c) * 3 + u) * 16;
                    uint64_t n = 0;
                    std::string bins;
                    for (int i = 0; i < 16; ++i) {
                        n += b[i];
                        bins += " " + std::to_string(b[i]);
                    }
                    if (n)
                        fprintf(stderr, "[remaster] pbrprobe: hist frame %llu %s %s %s %s n %llu bins%s\n",
                                (unsigned long long)pbr_frames_, metric[m], c & 1 ? "allow" : "other",
                                c & 2 ? "main" : "view", sub[u], (unsigned long long)n, bins.c_str());
                }
        memset(q, 0, d9msl::kPbrProbeCounters * 4);
    }
    void pbw_load() {
        const char *dir = getenv("RECOMP_REMASTER_PBRWORLD_PACK");
        if (!dir || !*dir) {
            fprintf(stderr, "[remaster] pbrworld: no RECOMP_REMASTER_PBRWORLD_PACK: nothing is patched\n");
            return;
        }
        rtex_.add_slots(dir);
        float strength = 1.0f;
        if (const char *v = getenv("RECOMP_REMASTER_PBRWORLD_STRENGTH"); v && *v)
            strength = strtof(v, nullptr);
        const int col = strength >= 3.0f ? 8 : strength >= 1.5f ? 7 : 6;   // ecos4 / ecos2 / ecos1 (manifest columns)
        if (strength != 1.0f && strength != 2.0f && strength != 4.0f)
            fprintf(stderr, "[remaster] pbrworld: strength %.3f has no manifest column; using the nearest\n", strength);
        if (FILE *f = fopen((std::string(dir) + "/manifest.tsv").c_str(), "r")) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                if (!strncmp(line, "key\t", 4))
                    continue;
                std::vector<std::string> cols;
                std::string cur;
                for (const char *c = line; *c && *c != '\n'; ++c)
                    if (*c == '\t') {
                        cols.push_back(cur);
                        cur.clear();
                    } else
                        cur += *c;
                cols.push_back(cur);
                if ((int)cols.size() > col + 3) {   // packed for P.pad1: floor(ecos * 4096) + sig (sig < 1)
                    const float ec = strtof(cols[col].c_str(), nullptr), sg = strtof(cols[col + 3].c_str(), nullptr);
                    pbw_ecos_[strtoull(cols[0].c_str(), nullptr, 16)] =
                        std::floor(std::min(std::max(ec, 0.0f), 1.0f) * 4096.0f) + std::min(std::max(sg, 0.0f), 0.999f);
                }
            }
            fclose(f);
        }
        fprintf(stderr, "[remaster] pbrworld: %zu pilot keys (strength %.3f, ecos column %d) from %s\n", pbw_ecos_.size(),
                strength, col, dir);
    }
    // Remaster run 12 pilot: the selection rules (patch-plan.md 3), before the pipeline. nil = the draw stays as today.
    id<MTLTexture> pbw_select(const HostD9Draw &d, uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp,
                              const d9sh::Program &pp, Tex *t0, const d9msl::PixelVariant &variant, float *ecos) {
        if (recomp_ab::old(recomp_ab::PBRWORLD))
            return nil;
        auto pi = pbr_ps_.find(ps_key);
        if (pi == pbr_ps_.end())
            pi = pbr_ps_.emplace(ps_key, d9msl::pixel_is_pbrworld(pp)).first;
        if (!pi->second || !(variant.depth_mask & 2u) || variant.wetroad || variant.roadsheen || variant.soft || variant.a2c ||
            variant.pbrprobe)
            return nil;
        auto vi = pbw_vs_.find(vs_key);
        if (vi == pbw_vs_.end()) {
            const d9msl::SunFlow f = d9msl::vertex_sun_flow(vp);
            vi = pbw_vs_.emplace(vs_key, f.ok && f.pbr_ok).first;
            fprintf(stderr, "[remaster] pbrworld: vs %016llx: %s%s (normal v%u, position v%u, eye c%u, light c%u)\n",
                    (unsigned long long)vs_key, vi->second ? "eligible" : "NOT eligible: ",
                    vi->second ? "" : (f.ok ? f.pbr_why : f.why).c_str(), f.normal, f.position, f.eye, f.light);
        }
        if (!vi->second || !d.label || strncmp(d.label, "IDI_WORLD_FX/", 13)) {
            ++pbw_skip_[0];
            return nil;
        }
        const NSUInteger w = pass_color_[0] ? pass_color_[0].width : 0;
        pbw_main_w_ = std::max(pbw_main_w_, w);
        if (!w || w != pbw_main_w_) {
            ++pbw_skip_[1];
            return nil;
        }
        if (d.render_state[15]) {
            ++pbw_skip_[2];
            return nil;
        }
        if (!t0 || !t0->desc.key)
            return nil;
        auto ei = pbw_ecos_.find(t0->desc.key);
        if (ei == pbw_ecos_.end())
            return nil;   // not a pilot key (most draws): never patched
        bool usable = t0->texture && !t0->depth && !t0->cube && t0->desc.kind == HOST_D9_TEX_2D && !t0->no_replace &&
                      t0->texture != pass_depth_;
        for (int i = 0; i < 4 && usable; ++i)
            usable = t0->texture != pass_color_[i];
        if (!usable) {
            ++pbw_skip_[3];
            return nil;
        }
        if (!rcache_.active() || !rtex_.swap() || !rcache_.bind(t0->desc.key)) {   // albedo replaced too (no third state)
            ++pbw_skip_[4];
            return nil;
        }
        const uint64_t sk = t0->desc.key ^ remaster::kSlotNormal;
        id<MTLTexture> n = rcache_.bind(sk);
        if (!n) {
            rcache_.request(sk, t0->desc.width, t0->desc.height);   // no-op unless absent from the cache
            ++pbw_skip_[5];
            return nil;
        }
        *ecos = ei->second;
        return n;
    }
    // Remaster run 13 natural water (RECOMP_REMASTER_WATER=1; A/B switch water; review/run13/x360-study-design.md 3(c)):
    // main-view WORLD draws whose stage-0 texture is an ANM_WATERA_ flipbook frame (tag key = catalogue hash << 32; the
    // 14 frames of knowledge/TEXTURE_CATALOGUE.tsv), on a pixel_is_water program with a pbr-eligible vertex shader and
    // no other replacing variant. False = the draw stays as today.
    static bool water_key(uint64_t key) {
        if ((uint32_t)key)
            return false;
        const uint32_t h = (uint32_t)(key >> 32);
        return h == 0x636c197du || (h >= 0xc9e6d6eeu && h <= 0xc9e6d6f6u) || (h >= 0xc9e6d70eu && h <= 0xc9e6d711u);
    }
    // Remaster run 13 grass shells (grass.h; RECOMP_REMASTER_GRASS=<shells>; A/B switch grass): after an encoded
    // main-view grass terrain draw (WORLD / WORLDNORMALMAP, stage-0 key in remaster_grass::kKeys), its shells with the
    // draw's own buffers, constants, textures and samplers: depth test as the draw, depth write on, alpha test (or
    // coverage in an MSAA pass) at 0.5 on the blade noise. The draw itself is never changed; a pair whose shell variant
    // cannot be built is counted and blocked.
    template <class Build>
    void grass_shells(const HostD9Draw &d, const uint32_t *rs, uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp,
                      const d9sh::Program &pp, const Tex *t0, const d9msl::PixelVariant &variant, uint64_t dkey,
                      bool stencil, D9VSParams vparams, D9PSParams pparams, Build &&build) {
        if (!grass_.shells || recomp_ab::old(recomp_ab::GRASS) || !d.label || !t0 || !remaster_grass::key(t0->desc.key) ||
            (strncmp(d.label, "IDI_WORLD_FX/", 13) && strncmp(d.label, "IDI_WORLDNORMALMAP_FX/", 22)))
            return;
        if (!grass_.main_view(pass_color_[0] ? (unsigned long)pass_color_[0].width : 0)) {
            ++grass_.skip[remaster_grass::OTHER_VIEW];
            return;
        }
        // Opaque ground with a depth test and colour writes; no stencil, alpha test or occlusion query.
        if (rs[27] || !rs[7] || !pass_depth_ || !(rs[168] & 7) || stencil || (rs[15] && rs[25] != 8) || query_) {
            ++grass_.skip[remaster_grass::RENDER_STATE];
            return;
        }
        const uint8_t slot = grass_.slot(vs_key, ps_key, vp, pp);
        if (!slot) {
            ++grass_.skip[remaster_grass::PROGRAMS];
            return;
        }
        d9msl::PixelVariant gv;
        gv.cube_mask = variant.cube_mask;
        gv.projected_mask = variant.projected_mask;
        gv.depth_mask = variant.depth_mask;
        gv.shadowpcf = variant.shadowpcf;
        gv.a2c = pass_samples_ > 1 ? (uint8_t)std::min<uint32_t>(pass_samples_, 8) : 0;
        gv.grass = slot;
        d9msl::VertexVariant gvv;
        gvv.grass = slot;
        id<MTLFunction> vf = nil, ff = nil;
        id<MTLRenderPipelineState> pso = build(gv, gvv, &vf, &ff);
        if (!pso) {
            ++grass_.skip[remaster_grass::BUILD];
            fprintf(stderr, "[remaster] grass: vs %016llx ps %016llx: shell variant not built (%s); no shells for it\n",
                    (unsigned long long)vs_key, (unsigned long long)ps_key, !vf ? "vertex" : !ff ? "pixel" : "pipeline");
            grass_.block(vs_key, ps_key);
            return;
        }
        uint32_t srs[256];
        memcpy(srs, rs, sizeof srs);
        srs[14] = 1;   // depth write on (dkey bit 1)
        id<MTLDepthStencilState> dss = depth_state(dkey | 2ull, true, srs, false);
        if (pso != es_.pipeline) {
            [enc_ setRenderPipelineState:pso];
            es_.pipeline = pso;
        }
        if (dss != es_.depth) {
            [enc_ setDepthStencilState:dss];
            es_.depth = dss;
        }
        pparams.alpha_func = 7;   // GREATEREQUAL
        pparams.alpha_ref = 0.5f;
        pparams.pad0 = gv.a2c;
        for (int i = grass_.shells; i >= 1; --i) {   // top shell first: its blades depth-reject the lower shells
            vparams.halfpix[2] = grass_.lift(i);
            pparams.pad1 = grass_.code(i);
            [enc_ setVertexBytes:&vparams length:sizeof vparams atIndex:17];
            [enc_ setFragmentBytes:&pparams length:sizeof pparams atIndex:1];
            grass_.drawn += encode_primitives(d) == d9trace::Result::Encoded;
        }
        ++grass_.draws;
    }
    // Remaster run 14 grass blades (grass.h; RECOMP_REMASTER_GRASS_BLADES=<quality>; A/B switch grass;
    // review/run14/grass-blades-design.md): after an encoded main-view grass terrain draw (the shells' gates), its
    // blades: the chunk's cached roots near the camera (eye = the WORLD VS eye constant, POSITION space), one instanced
    // 5/7-vertex strip draw per arena run (buffer 10, per instance), with the draw's constants, textures and samplers
    // (stage 0 also to the vertex half, for the mask), depth test as the draw, depth write on, cull none (restored), no
    // alpha test. Never instead of the draw; a pair whose blade variant cannot be built is counted and blocked.
    template <class BuildBlade>
    void grass_blades(const HostD9Draw &d, const uint32_t *rs, uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp,
                      const d9sh::Program &pp, const Tex *t0, const d9msl::PixelVariant &variant, uint64_t dkey,
                      bool stencil, D9VSParams vparams, D9PSParams pparams, BuildBlade &&build_blade) {
        if (recomp_ab::old(recomp_ab::GRASS) || !d.label || !t0 || !remaster_grass::key(t0->desc.key) ||
            (strncmp(d.label, "IDI_WORLD_FX/", 13) && strncmp(d.label, "IDI_WORLDNORMALMAP_FX/", 22)))
            return;
        uint64_t *bskip = grass_.bskip;
        if (!grass_.main_view(pass_color_[0] ? (unsigned long)pass_color_[0].width : 0)) {
            ++bskip[remaster_grass::OTHER_VIEW];
            return;
        }
        if (rs[27] || !rs[7] || !pass_depth_ || !(rs[168] & 7) || stencil || (rs[15] && rs[25] != 8) || query_ ||
            !es_.texture[0] || !es_.sampler[0]) {
            ++bskip[remaster_grass::RENDER_STATE];
            return;
        }
        const uint8_t slot = grass_.blade_slot(vs_key, ps_key, vp, pp);
        if (!slot) {
            ++bskip[remaster_grass::PROGRAMS];
            return;
        }
        // COMB14 (golf course: 58,685 draws skipped "eye", one WORLD vs with no eye constant the sun flow finds): the
        // eye (camera position, the same for every world-space draw of a frame) is remembered from this frame's
        // other grass draws and used for a vs without one.
        const int eye = grass_.eye(vs_key, vp);
        float e[3];
        if (eye >= 0 && d.vconst && (uint32_t)eye < d.vconst_count) {
            for (int i = 0; i < 3; ++i)
                e[i] = grass_eye_[i] = d.vconst[eye * 4 + i];
            grass_eye_frame_ = presents_ + 1;
        } else if (grass_eye_frame_ == presents_ + 1) {
            for (int i = 0; i < 3; ++i)
                e[i] = grass_eye_[i];
        } else {
            ++bskip[remaster_grass::EYE];
            return;
        }
        if (d.inline_vertices || d.inline_indices) {
            ++bskip[remaster_grass::INLINE];
            return;
        }
        remaster_grass::Mesh m;
        uint64_t ck = mix(mix((uint64_t)d.index_buffer << 32 | d.start, (uint64_t)d.primitive_count << 32 | d.primitive),
                          mix((uint64_t)d.decl_id << 32 | (uint32_t)d.base_vertex, vs_key));
        for (int i = 0; i < 8; ++i) {
            const HostD9Stream &st = d.stream[i];
            ck = mix(ck, (uint64_t)st.buffer << 32 | st.offset);
            auto b = st.buffer ? buffers_.find(st.buffer) : buffers_.end();
            if (b == buffers_.end() || b->second.shadow.size() <= st.offset)
                continue;
            m.vb[i] = b->second.shadow.data() + st.offset;
            m.vb_bytes[i] = b->second.shadow.size() - st.offset;
            m.stride[i] = st.stride;
        }
        if (d.index_buffer) {
            auto b = buffers_.find(d.index_buffer);
            if (b == buffers_.end()) {
                ++bskip[remaster_grass::INLINE];
                return;
            }
            m.ib = b->second.shadow.data();
            m.ib_bytes = b->second.shadow.size();
        }
        m.index_size = d.index_size == 4 ? 4 : 2;
        m.primitive = d.primitive;
        m.count = d.primitive_count;
        m.start = d.start;
        m.base_vertex = d.base_vertex;
        if (!grass_arena_ && !grass_.b.page.empty()) {
            grass_arena_ = [mtl_ newBufferWithLength:grass_.b.page.size() * remaster_grass::kPage
                                             options:MTLResourceStorageModeShared];
            grass_.b.base = grass_arena_ ? (uint8_t *)grass_arena_.contents : nullptr;
        }
        remaster_grass::Skip why = remaster_grass::FORMAT;
        if (!grass_.b.prepare(ck, m, d.decl, d.decl_size, vp, e, &grass_runs_, &why)) {
            ++bskip[why];
            return;
        }
        if (grass_runs_.empty())
            return;
        d9msl::PixelVariant gv;
        gv.cube_mask = variant.cube_mask;
        gv.projected_mask = variant.projected_mask;
        gv.depth_mask = variant.depth_mask;
        gv.shadowpcf = variant.shadowpcf;
        gv.grass_blade = slot;
        d9msl::VertexVariant gvv;
        gvv.grass_blade = slot;
        id<MTLFunction> vf = nil, ff = nil;
        id<MTLRenderPipelineState> pso = build_blade(gv, gvv, grass_.b.chunks[ck].L, &vf, &ff);
        if (!pso) {
            ++bskip[remaster_grass::BUILD];
            fprintf(stderr, "[remaster] grass: vs %016llx ps %016llx: blade variant not built (%s); no blades for it\n",
                    (unsigned long long)vs_key, (unsigned long long)ps_key, !vf ? "vertex" : !ff ? "pixel" : "pipeline");
            grass_.block_blades(vs_key, ps_key);
            return;
        }
        uint32_t srs[256];
        memcpy(srs, rs, sizeof srs);
        srs[14] = 1;   // depth write on (dkey bit 1)
        id<MTLDepthStencilState> dss = depth_state(dkey | 2ull, true, srs, false);
        if (pso != es_.pipeline) {
            [enc_ setRenderPipelineState:pso];
            es_.pipeline = pso;
        }
        if (dss != es_.depth) {
            [enc_ setDepthStencilState:dss];
            es_.depth = dss;
        }
        const int cull = es_.cull;
        if (cull != (int)MTLCullModeNone)
            [enc_ setCullMode:MTLCullModeNone];
        for (auto &a : vparams.ascale)   // the records hold Direct3D's values
            for (float &x : a)
                x = 1.0f;
        pparams.alpha_func = 8;   // always: opaque blades
        pparams.pad0 = 0;
        pparams.pad1 = 0.0f;
        // metres per pixel at 1 m: 2 tan(fov / 2) / height, fov ~70 degrees (UNV)
        const D9GrassBlade G = grass_.b.params(pass_color_[0] ? 1.4f / (float)pass_color_[0].height : 0.0f);
        [enc_ setVertexBytes:&vparams length:sizeof vparams atIndex:17];
        [enc_ setFragmentBytes:&pparams length:sizeof pparams atIndex:1];
        [enc_ setVertexBytes:&G length:sizeof G atIndex:19];
        [enc_ setVertexTexture:es_.texture[0] atIndex:0];
        [enc_ setVertexSamplerState:es_.sampler[0] atIndex:0];
        for (const remaster_grass::Run &r : grass_runs_) {
            [enc_ setVertexBuffer:grass_arena_ offset:r.offset atIndex:10];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip
                     vertexStart:0
                     vertexCount:(NSUInteger)grass_.b.q.verts
                   instanceCount:r.count];
        }
        if (cull != (int)MTLCullModeNone)
            [enc_ setCullMode:(MTLCullMode)cull];
        ++grass_.bdraws;
    }
    bool water_select(const HostD9Draw &d, uint64_t vs_key, uint64_t ps_key, const d9sh::Program &vp,
                      const d9sh::Program &pp, const Tex *t0, const d9msl::PixelVariant &variant,
                      const d9msl::VertexVariant &vvariant) {
        if (recomp_ab::old(recomp_ab::WATER) || !d.label || strncmp(d.label, "IDI_WORLD_FX/", 13) || !t0 ||
            !water_key(t0->desc.key))
            return false;
        if (variant.pbrworld || variant.wetroad || variant.roadsheen || variant.soft || variant.pbrprobe ||
            variant.glass || variant.pool || vvariant.pbr || vvariant.glass || vvariant.carenv || vvariant.pool) {
            ++water_skip_[0];
            return false;
        }
        const NSUInteger w = pass_color_[0] ? pass_color_[0].width : 0;
        water_main_w_ = std::max(water_main_w_, w);
        if (!w || w != water_main_w_) {
            ++water_skip_[1];
            return false;
        }
        auto pi = water_ps_.find(ps_key);
        if (pi == water_ps_.end()) {
            pi = water_ps_.emplace(ps_key, d9msl::pixel_is_water(pp)).first;
            fprintf(stderr, "[remaster] water: ps %016llx: %s\n", (unsigned long long)ps_key,
                    pi->second ? "water program" : "NOT a water program (drawn as today)");
        }
        auto vi = water_vs_.find(vs_key);
        if (vi == water_vs_.end()) {
            const d9msl::SunFlow f = d9msl::vertex_sun_flow(vp);
            vi = water_vs_.emplace(vs_key, f.ok && f.pbr_ok).first;
            fprintf(stderr, "[remaster] water: vs %016llx: %s%s (normal v%u, position v%u, eye c%u, light c%u)\n",
                    (unsigned long long)vs_key, vi->second ? "eligible" : "NOT eligible: ",
                    vi->second ? "" : (f.ok ? f.pbr_why : f.why).c_str(), f.normal, f.position, f.eye, f.light);
        }
        if (!pi->second || !vi->second) {
            ++water_skip_[2];
            return false;
        }
        return true;
    }
    void pbw_report() {
        fprintf(stderr, "[remaster] pbrworld: frame %llu pilot draws %llu (fallback to plain %llu); stayed as today: vs/label "
                        "%llu, other view %llu, alpha test %llu, stage 0 %llu, albedo not resident %llu, slot not resident %llu\n",
                (unsigned long long)pbw_frames_, (unsigned long long)pbw_draws_, (unsigned long long)pbw_fallback_,
                (unsigned long long)pbw_skip_[0], (unsigned long long)pbw_skip_[1], (unsigned long long)pbw_skip_[2],
                (unsigned long long)pbw_skip_[3], (unsigned long long)pbw_skip_[4], (unsigned long long)pbw_skip_[5]);
        if (!pwq_)
            return;
        uint32_t *q = (uint32_t *)pwq_.contents;   // windowed (reset after each report); see kPbrWorldCounters
        std::string kh, dh;
        for (int i = 0; i < 16; ++i) {
            kh += " " + std::to_string(q[i]);
            dh += " " + std::to_string(q[16 + i]);
        }
        fprintf(stderr, "[remaster] pbrworld: debug frame %llu sunfacing %u k-sum64 %u dY+ %u dY- %u | all %u dY+ %u dY- %u | "
                        "sampled %u degenerate %u sign agree %u disagree %u | ramp %u %u %u %u | k-hist%s | dY-hist%s\n",
                (unsigned long long)pbw_frames_, q[32], q[35], q[33], q[34], q[44], q[45], q[46], q[39], q[38], q[36],
                q[37], q[40], q[41], q[42], q[43], kh.c_str(), dh.c_str());
        memset(q, 0, d9msl::kPbrWorldCounters * 4);
    }
    // Remaster run 13 front-end floor shadows (review/run13/fe-shadows-design.md; RECOMP_REMASTER_FEFLOOR=1, set by the
    // core mod's fe_floor_shadows; A/B switch fefloor). Front end only: the mod sets private render state 254 = 1 in
    // flow 3 (0 in the world; run 14: not 252, which the D3D9 layer takes as the MetalFX scene-end event and never
    // stores). A WORLD receiver drawn there (a vertex shader writing the clip position with WVP and one
    // coordinate with the shadow matrix W x S, d9msl::vertex_clip_shadow, plus a shadow map at a declared stage) gives
    // Mclip = (W x S) x (W x VP)^-1 = S x VP^-1 (W cancels) and the map; the floor (IDI_FE_FX/world/p0, the run 13
    // census programs, depth test on, same colour target) then gets the fefloor variant pair with them, in this frame
    // or the next. Anything missing: the floor is drawn as today.
    // Run 15 (review/run15/fefloor.md): the run-13 floor pair IDI_FE_FX/world/p0 (vs a150954f891b5474, ps
    // f8813735427aff31) is the 2D menu art (ink splats, info panel: FEFLOOR_DEBUG=2 painted only those), not the floor,
    // so nothing under the car was ever darkened. The floor is stage geometry drawn with a WORLD effect that has no
    // shadow lookup of its own (census at the safe-house and main-menu stages): those pairs, IDI_WORLD* labels only.
    // TEST ONLY: RECOMP_REMASTER_FEFLOOR_PROGS=vs:ps[+vs:ps...] (hex keys) replaces the list.
    static std::vector<std::pair<uint64_t, uint64_t>> fef_progs_init() {
        // The safe-house floor (FEFLOOR_DEBUG=2/3, run 15); the main-menu floors use other programs (not covered).
        std::vector<std::pair<uint64_t, uint64_t>> v = {{0x090160e8e9a32890ull, 0x8abb0769bddd4158ull}};
        if (const char *e = getenv("RECOMP_REMASTER_FEFLOOR_PROGS")) {
            v.clear();
            for (const char *c = e; *c;) {
                char *end = nullptr;
                const uint64_t a = strtoull(c, &end, 16);
                if (!end || *end != ':')
                    break;
                const uint64_t b = strtoull(end + 1, &end, 16);
                v.emplace_back(a, b);
                c = *end == '+' ? end + 1 : end;
                if (!*end || *end != '+')
                    break;
            }
        }
        return v;
    }
    // out = B x A^-1 (row-major 4x4; A and B as the vertex constants hold them, one row per register). False when A is
    // (near) singular or the result is not finite.
    static bool fef_mclip(const float *A, const float *B, float *out) {
        double m[4][8], scale = 1.0;
        for (int i = 0; i < 4; ++i) {
            double n = 0.0;
            for (int j = 0; j < 4; ++j) {
                m[i][j] = A[4 * i + j];
                m[i][4 + j] = i == j ? 1.0 : 0.0;
                n += m[i][j] * m[i][j];
            }
            scale *= std::sqrt(n);
        }
        double det = 1.0;
        for (int c = 0; c < 4; ++c) {
            int piv = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(m[r][c]) > std::fabs(m[piv][c]))
                    piv = r;
            if (!(std::fabs(m[piv][c]) > 0.0))
                return false;
            if (piv != c)
                for (int j = 0; j < 8; ++j)
                    std::swap(m[c][j], m[piv][j]);
            det *= m[c][c];
            const double inv = 1.0 / m[c][c];
            for (int j = 0; j < 8; ++j)
                m[c][j] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c && m[r][c] != 0.0) {
                    const double f = m[r][c];
                    for (int j = 0; j < 8; ++j)
                        m[r][j] -= f * m[c][j];
                }
        }
        if (!(scale > 0.0) || !(std::fabs(det) > 1e-9 * scale))
            return false;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double v = 0.0;
                for (int k = 0; k < 4; ++k)
                    v += (double)B[4 * i + k] * m[k][4 + j];
                if (!std::isfinite(v))
                    return false;
                out[4 * i + j] = (float)v;
            }
        return true;
    }
    void fef_capture(const HostD9Draw &d, uint64_t vs_key, const d9sh::Program &vp, const d9sh::Program &pp,
                     Tex *const *bound, uint16_t depth_mask) {
        if (!depth_mask || d.render_state[254] != 1 || !d.label || strncmp(d.label, "IDI_WORLD", 9) || !d.vconst)
            return;
        auto it = fef_recv_.find(vs_key);
        if (it == fef_recv_.end()) {
            uint32_t a = 0, b = 0;
            const bool ok = d9msl::vertex_clip_shadow(vp, &a, &b) && a < 256 && b < 256;
            it = fef_recv_.emplace(vs_key, ok ? (int32_t)(a | b << 8) : -1).first;
            if (ok)
                fprintf(stderr, "[remaster] fefloor: receiver vs %016llx (%s): clip c%u-c%u, shadow c%u-c%u\n",
                        (unsigned long long)vs_key, d.label, a, a + 3, b, b + 3);
        }
        if (it->second < 0)
            return;
        const uint32_t a = (uint32_t)it->second & 255u, b = (uint32_t)it->second >> 8 & 255u;
        if (std::max(a, b) + 4 > d.vconst_count)
            return;
        int s = -1;   // the shadow map: the lowest declared stage bound to a depth texture
        for (const auto &kv : pp.samplers)
            if (kv.first < 16 && (depth_mask >> kv.first & 1) && (s < 0 || (int)kv.first < s))
                s = (int)kv.first;
        if (s < 0 || !bound[s] || !bound[s]->texture)
            return;
        float m[16];
        if (!fef_mclip(d.vconst + 4 * a, d.vconst + 4 * b, m)) {
            ++fef_singular_;
            return;
        }
        if (fef_cap_frame_ == fef_frame_ + 1) {   // another capture this frame: count it as another view if it differs
            float dm = 0.0f, mm = 0.0f;
            for (int i = 0; i < 16; ++i) {
                dm = std::max(dm, std::fabs(m[i] - fef_m_[i]));
                mm = std::max(mm, std::fabs(m[i]));
            }
            if (dm > 1e-3f * std::max(mm, 1e-6f) || fef_target_ != d.target.color[0].id || fef_map_ != bound[s]->texture)
                ++fef_views_;
        }
        memcpy(fef_m_, m, sizeof m);
        fef_map_ = bound[s]->texture;
        fef_target_ = d.target.color[0].id;
        fef_cap_frame_ = fef_frame_ + 1;
        ++fef_captures_;
    }
    // The floor gate; true = draw it with the fefloor pair (fef_m_, fef_map_).
    bool fef_select(const HostD9Draw &d, uint64_t vs_key, uint64_t ps_key) {
        bool prog = false;   // run 15: the stage's floor programs (fef_progs_), not the menu art
        for (const auto &pr : fef_progs_)
            prog |= pr.first == vs_key && pr.second == ps_key;
        if (!prog)
            return false;
        // Run 14 (CONF14: the floor programs drew, the gate took none): which condition refused it, counted.
        // COMB14: every floor draw had the depth test OFF (ztest 422,295 refusals), so it is no longer required (the
        // floor is the stage's ground, drawn first; the receiver pair samples the shadow map, not the depth).
        const int why = d.render_state[254] != 1 ? 0 : !d.label || strncmp(d.label, "IDI_WORLD", 9) ? 1
                        : !pass_depth_ ? 3 : d.pool_fuse ? 4 : recomp_ab::old(recomp_ab::FEFLOOR) ? 5 : -1;
        if (why >= 0) {
            ++fef_gate_[why];
            return false;
        }
        ++fef_floor_draws_;
        const bool recent = fef_cap_frame_ && fef_frame_ + 1 - fef_cap_frame_ <= 1;
        if (!recent || !fef_map_ || fef_target_ != d.target.color[0].id) {
            ++fef_fallback_;
            ++fef_no_recv_;
            return false;
        }
        bool clash = fef_map_ == pass_depth_;
        for (int i = 0; i < 4; ++i)
            clash |= fef_map_ == pass_color_[i];
        if (clash) {
            ++fef_fallback_;
            return false;
        }
        return true;
    }
    void fef_present() {
        if (++fef_frame_ == 1)
            fprintf(stderr, "[remaster] fefloor: on (K %.2f, debug %d)\n", fef_k_, fef_debug_);
        if (fef_frame_ % 600 == 0) {
            size_t n = 0;
            for (const auto &kv : fef_recv_)
                n += kv.second >= 0;
            fprintf(stderr, "[remaster] fefloor: frame %llu floor draws %llu shadowed %llu fallbacks %llu (no receiver %llu) "
                            "captures %llu views %llu singular %llu build %llu receivers %zu of %zu checked\n",
                    (unsigned long long)fef_frame_, (unsigned long long)fef_floor_draws_,
                    (unsigned long long)fef_shadowed_, (unsigned long long)fef_fallback_,
                    (unsigned long long)fef_no_recv_, (unsigned long long)fef_captures_, (unsigned long long)fef_views_,
                    (unsigned long long)fef_singular_, (unsigned long long)fef_build_fail_, n, fef_recv_.size());
            fprintf(stderr, "[remaster] fefloor: frame %llu floor programs refused: marker %llu label %llu ztest %llu "
                            "no depth %llu fused %llu A/B off %llu\n",
                    (unsigned long long)fef_frame_, (unsigned long long)fef_gate_[0], (unsigned long long)fef_gate_[1],
                    (unsigned long long)fef_gate_[2], (unsigned long long)fef_gate_[3], (unsigned long long)fef_gate_[4],
                    (unsigned long long)fef_gate_[5]);
        }
    }
    void draw(const HostD9Draw &d) override {
        ++stat_calls_;
        if (perf_mark_ != presents_) {   // v4 step 0: this (the encoding) thread's CPU time, sampled once a frame
            perf_mark_ = presents_;
            perf_enc_cpu_ns_ = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
            perf_sample_ = presents_ % 8 == 0;   // the timed frames: one in 8
            perf_sampled_ += perf_sample_;
            if (m4_)
                if (@available(macOS 26.0, iOS 26.0, *))
                    ((D9M4 *)m4_).st->sampling = perf_sample_;
        }
        struct DrawTimer {   // the whole draw (both renderers), on the timed frames
            uint64_t *acc;
            uint64_t t0;
            ~DrawTimer() {
                if (t0)
                    *acc += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
            }
        } draw_timer_{&perf_draw_ns_, perf_sample_ ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0};
        if (test_skip_label_ && d.label && !strncmp(d.label, test_skip_label_, strlen(test_skip_label_)))
            return;   // run 16 TEST ONLY (RECOMP_TEST_SKIP_LABEL=<label prefix>): which draws are which on screen
        if (diag_cap_list_)
            diag_record(d);   // the iOS reflection test's frame capture: every draw, held or not
        if (g_d9_diag.load(std::memory_order_relaxed) && diag_hold(d.target, false))
            return;   // the iOS reflection test (g_d9_diag)
        if (softfx_on_ && !soft_done_ && d.label && !strncmp(d.label, "IDI_PARTICLES_FX/", 17) && enc_ &&
            !recomp_ab::old(recomp_ab::SOFTFX)) {
            soft_done_ = true;
            soft_capture(d.target.depth.id);
            atm_pending_ = atm_on_ && atm_want(d.target.color[0].id);   // run 14 B1: the fog at this break
        } else if (atm_on_ && !soft_done_ && enc_ && d.label && !strncmp(d.label, "IDI_PARTICLES_FX/", 17) &&
                   atm_want(d.target.color[0].id)) {
            soft_done_ = true;   // run 14 B1 without soft particles: the same break, for the fog only
            soft_capture(d.target.depth.id);
            atm_pending_ = true;
        }
        const bool atm_now = atm_pending_;
        atm_pending_ = false;
        d9trace::Draw *tr = trace_draw(d);
        if (!ensure_pass(d.target)) {
            if (tr)
                tr->result = d9trace::Result::NoPass;
            skip("no render pass");
            return;
        }
        if (atm_now && soft_ok_)
            atm_draw();   // run 14 B1 height fog: before the first particle draw, in the reopened scene pass
        if (tr) {
            tr->context = trace_context();
            tr->fitted_depth = pass_fitted_depth_;
            tr->samples = pass_samples_;
        }
        // Remaster S3 step 1 (TEST ONLY, RECOMP_REMASTER_FXLOG=1): where particle draws land (target, samples, fitted
        // depth, blend), the facts a soft-particle pass break needs (study S3 and its challenge).
        static const bool fxlog = [] { const char *e = getenv("RECOMP_REMASTER_FXLOG"); return e && *e == '1'; }();
        if (fxlog && d.label && !strncmp(d.label, "IDI_PARTICLES_FX/", 17)) {
            static uint64_t n = 0;
            if (++n <= 40 || (n & (n - 1)) == 0)
                fprintf(stderr, "[remaster] fx draw %llu: %s target color0 %u depth %u samples %u fitted %d blend %u src %u dst %u "
                                "zwrite %u ztest %u\n",
                        (unsigned long long)n, d.label, d.target.color[0].id, d.target.depth.id, pass_samples_,
                        (int)pass_fitted_depth_, d.render_state[27], d.render_state[19], d.render_state[20],
                        d.render_state[14], d.render_state[7]);
        }
        count_query();
        uint64_t vs_key = d.vs_key ? d.vs_key : d9sh::code_key(d.vs, d.vs_size);
        uint64_t ps_key = d.ps_key ? d.ps_key : d9sh::code_key(d.ps, d.ps_size);
        const d9sh::Program &vp = d9sh::program_for_key(vs_key, d.vs, d.vs_size);
        const d9sh::Program &pp = d9sh::program_for_key(ps_key, d.ps, d.ps_size);
        if (!vp.ok || !pp.ok) {
            if (tr)
                tr->result = d9trace::Result::UndecodedShader;
            skip("undecoded shader");
            const std::string why =
                std::string(vp.ok ? "pixel shader: " + pp.why : "vertex shader: " + vp.why);
            if (undecoded_.insert(why + (d.label ? d.label : "")).second)
                fprintf(stderr, "d3d9 metal: skipping draws: %s (%s)\n", why.c_str(),
                        d.label ? d.label : "no label");
            return;
        }
        d9msl::PixelVariant variant;
        Tex *bound[16];
        for (uint32_t s = 0; s < 16; ++s) {
            Tex *t = d.sampler_texture[s] ? tex_ptr(d.sampler_texture[s]) : nullptr;
            bound[s] = t;
            auto decl = pp.samplers.find(s);
            const bool declared2d = decl == pp.samplers.end() || decl->second == d9sh::S_2D;
            if (declared2d && t && t->texture && t->depth && t->desc.kind == HOST_D9_TEX_2D) {
                variant.depth_mask |= (uint16_t)(1u << s);
                if (g_d9_diag.load(std::memory_order_relaxed) && t->desc.width == t->desc.height)
                    diag_shadow_ids_.insert(d.sampler_texture[s]);   // a shadow map (the iOS reflection test)
            }
        }
        census(d, vs_key, ps_key, vp, pp, bound);   // run 13 (TEST ONLY, RECOMP_REMASTER_CENSUS=<dir>)
        if (fef_on_)   // run 13 front-end floor shadows: a WORLD receiver's clip-to-shadow matrix and map
            fef_capture(d, vs_key, vp, pp, bound, variant.depth_mask);
        if (fnd_on_ && fnd_.open)   // run 14 foundation F1: the camera VP probe (K)
            fnd_capture(d, vs_key, vp);
        if (atm_on_ && atm_fresh_ && !atm_done_)   // run 14 B1 height fog: the WORLD view-projection per target
            atm_capture(d, vs_key, vp);
        if (pp.major < 2) {
            for (int s = 0; s < 8; ++s)
                if (bound[s] && bound[s]->desc.kind == HOST_D9_TEX_CUBE)
                    variant.cube_mask |= (uint16_t)(1u << s);
            variant.projected_mask = (uint16_t)d.projected_mask;
        }
        // Remaster A3 step A (RECOMP_REMASTER_A2C=1): an alpha test (GREATER or GREATEREQUAL) on an unblended draw
        // into an MSAA pass becomes sample coverage, so cut-outs (foliage, fences, wires) are antialiased too.
        // A/B switch a2c: OLD periods draw without it (GPU-cost measurement).
        if (a2c_on_ && !recomp_ab::old(recomp_ab::A2C) && pass_samples_ > 1 && d.render_state[15] && !d.render_state[27] &&
            (d.render_state[25] == 5 || d.render_state[25] == 7)) {
            variant.a2c = (uint8_t)std::min<uint32_t>(pass_samples_, 8);
            ++a2c_draws_;
        }
        if (softfx_mode_ == 2 && soft_ok_ && d.label && !strncmp(d.label, "IDI_PARTICLES_FX/", 17) &&
            !recomp_ab::old(recomp_ab::SOFTFX) && d.render_state[27] && d.render_state[19] == 5 &&
            d.render_state[20] == 6 && pass_color_[0] && soft_depth_.width == pass_color_[0].width &&
            soft_depth_.height == pass_color_[0].height) {
            variant.soft = 1;
            ++soft_faded_;
        }
        // Remaster run 14 lit smoke: the kind the core mod set around this emitter's draw (smoke_light.h), on the
        // alpha-blended particle draws (SRCALPHA/INVSRCALPHA, as the soft fade); the camera from the draw's clip rows.
        D9SmokeParams smk{};
        float smk_soft = 0.0f;
        if (d.render_state[remaster_smoke::RS_KIND])
            smoke_seen_ = true;
        if (d.render_state[remaster_smoke::RS_KIND] && d.label && !strncmp(d.label, "IDI_PARTICLES_FX/", 17) &&
            d.render_state[27] && d.render_state[19] == 5 && d.render_state[20] == 6 &&
            d.render_state[remaster_smoke::RS_KIND] <= remaster_smoke::KIND_MAX && !recomp_ab::old(recomp_ab::LITSMOKE)) {
            const uint32_t kind = d.render_state[remaster_smoke::RS_KIND];
            if (!smoke_kind_init_) {
                smoke_kind_init_ = true;
                for (uint32_t k = 0; k <= remaster_smoke::KIND_MAX; ++k)
                    smoke_kind_[k] = remaster_smoke::kind_params(k);
            }
            auto sc = smoke_clip_.find(vs_key);
            if (sc == smoke_clip_.end()) {
                uint32_t clip = 0;
                sc = smoke_clip_.emplace(vs_key, d9msl::vertex_clip_regs(vp, &clip) ? (int)clip : -1).first;
                fprintf(stderr, "[remaster] lit smoke: vs %016llx: %s (clip c%d)\n", (unsigned long long)vs_key,
                        sc->second >= 0 ? "eligible" : "NOT eligible (no clip rows)", sc->second);
            }
            const remaster_smoke::Kind &k = smoke_kind_[kind];
            if (sc->second >= 0 && d.vconst && d.vconst_count >= (uint32_t)sc->second + 4 &&
                remaster_smoke::fill(d.render_state, d.vconst + 4 * sc->second,
                                     (float)fmod(CACurrentMediaTime() - smoke_t0_, 1000.0), k, &smk)) {
                variant.smoke = (uint8_t)kind;
                if (variant.soft && k.soft_dist > 0.01f)
                    smk_soft = 1.0f / k.soft_dist;   // fog: a wide soft fade (3-5 m) into the road and walls
                ++smoke_draws_;
                ++smoke_by_kind_[kind];
            } else {
                ++smoke_noclip_;
            }
        }
        // Run 15 tunnel2: fog (soft 2) and vent steam (soft 3) drawn plain (lit smoke off, or its fallback): the core
        // mod's soft-kind marker (private render state 241, smoke_light.h) gives them the lit path's near-camera fade
        // and fog's wide soft fade, so ground fog has no hard sprite edge on the road and no near-plane cut.
        if (variant.soft && !variant.smoke && (d.render_state[241] == remaster_smoke::KIND_FOG ||
                                               d.render_state[241] == remaster_smoke::KIND_STEAM)) {
            variant.soft = d.render_state[241] == remaster_smoke::KIND_FOG ? 2 : 3;
            ++soft_kind_draws_;
        }
        // Run 16 smokeperf: tyre smoke the core mod spawns fewer of (smoke.h) gets the alpha exponent it marks (private
        // render state 240 = k x 16) on its alpha-blended particle draws, so the cloud keeps its density at less fill.
        if (d.render_state[240] && d.label && !strncmp(d.label, "IDI_PARTICLES_FX/", 17) && d.render_state[27] &&
            d.render_state[19] == 5 && d.render_state[20] == 6)
            variant.alphak = (uint8_t)std::min<uint32_t>(d.render_state[240], 255u);
        // Remaster R2 wet roads (TEST ONLY, RECOMP_REMASTER_WETROAD=1; A/B switch wetroad): road pixel shaders (the PC
        // road effect's reflection lines, detected once per program) get the native wet-road shading variant.
        if (wetroad_on_ && !recomp_ab::old(recomp_ab::WETROAD)) {
            auto wr = wetroad_ps_.find(ps_key);
            if (wr == wetroad_ps_.end())
                wr = wetroad_ps_.emplace(ps_key, d9msl::pixel_is_wetroad(pp)).first;
            if (wr->second) {
                variant.wetroad = wetroad_mode_;
                ++wetroad_draws_;
                if (wetroad_log_ && d.pconst && d.pconst_count >= 2) { // challenge C-WR1 4: what c1 holds, dry vs wet
                    wetroad_log_ = false;
                    fprintf(stderr, "[remaster] wetroad: road draw c0 %.3f %.3f %.3f %.3f c1 %.3f %.3f %.3f %.3f\n", d.pconst[0],
                            d.pconst[1], d.pconst[2], d.pconst[3], d.pconst[4], d.pconst[5], d.pconst[6], d.pconst[7]);
                    if (d.pconst_count >= 4)   // run 9 item C: SpecularPower (c2.x) and Brightness (c3.y), DER names
                        fprintf(stderr, "[remaster] wetroad: road draw c2 %.3f %.3f %.3f %.3f c3 %.3f %.3f %.3f %.3f\n",
                                d.pconst[8], d.pconst[9], d.pconst[10], d.pconst[11], d.pconst[12], d.pconst[13],
                                d.pconst[14], d.pconst[15]);
                }
            }
        }
        // Remaster run 13 road sheen (RECOMP_REMASTER_ROADSHEEN=1; A/B switch roadsheen): the road effect's sun
        // highlight becomes the measured GGX sheen, with or without the wet-road shading.
        if (roadsheen_on_ && !recomp_ab::old(recomp_ab::ROADSHEEN)) {
            auto rs = roadsheen_ps_.find(ps_key);
            if (rs == roadsheen_ps_.end())
                rs = roadsheen_ps_.emplace(ps_key, d9msl::pixel_is_roadsheen(pp)).first;
            if (rs->second) {
                variant.roadsheen = 1;
                ++roadsheen_draws_;
            }
        }
        if (pbrprobe_on_)   // Remaster run 12 step 0 (TEST ONLY): the sun-share probe
            pbr_probe(d, vs_key, ps_key, vp, pp, bound[0], variant);
        // Remaster R2 softer sun shadows (RECOMP_REMASTER_SHADOWPCF=1; A/B switch shadowpcf): any draw that samples a
        // shadow map gets the 12-tap filter variant.
        if (shadowpcf_on_ && variant.depth_mask && !recomp_ab::old(recomp_ab::SHADOWPCF)) {
            variant.shadowpcf = 1;
            ++shadowpcf_draws_;
        }
        const uint32_t *rs = d.render_state;
        // Test86 HDR: after the core mod's pre-HUD scene marker (private render state 250 = 1; the mod has
        // set the backbuffer's alpha to 1 over the scene), every draw into the backbuffer - the HUD, the
        // mirror, menus - also clears alpha where it covers the scene (alpha blend ZERO, INVSRCALPHA) with its
        // colour unchanged, so the presenter expands highlights only where the scene still shows. Test109: it was
        // ZERO, ZERO, which cleared the whole quad - the minimap's square backing quad (transparent corners) left
        // a square of unexpanded road around the map in bright HDR scenes (owner's RC2 report).
        hdr_mirror_keep_ = false;   // run 15: set below for a mirror draw on the float frame only
        if (rs[250] == 1 && pass_color_id_ && pass_color_id_ == last_presented_ && (rs[168] & 7)) {
            // Run 15 (owner: cop light bars in the mirror faint with HDR on): on the float frame a mirror draw that
            // comes before the split keeps the scene mark like Test133 - the split then moves the mirror's own excess
            // (its target is promoted) and the add-back returns it, so the presenter's shoulder lights it like the
            // main view. A mirror after the split stays HUD (SDR white): the scene excess behind it would come back.
            hdr_mirror_keep_ = hdr_float_ && d.hdr_scene_texture && !hdr_split_tried_;
            if (hdr_float_ && !hdr_split_tried_ && !hdr_mirror_keep_)   // PLAN_R2 item 10: the HUD composites over the SDR-clipped scene
                hdr_split(pass_color_id_);
            memcpy(hud_rs_, rs, sizeof hud_rs_);
            if (!hud_rs_[27]) { // no blending: colour ONE, ZERO, ADD
                hud_rs_[27] = 1;
                hud_rs_[19] = 2;
                hud_rs_[20] = 1;
                hud_rs_[171] = 1;
            }
            hud_rs_[206] = 1; // separate alpha: ZERO, INVSRCALPHA, ADD (dest alpha x (1 - HUD coverage))
            hud_rs_[207] = 1;
            hud_rs_[208] = 6;
            hud_rs_[209] = 1;
            // Test133 (owner, 2 Oct 2026: police lights in the mirror faint in HDR, right with HDR off): a draw of
            // the texture the mod marked as scene content (the rear-view mirror) leaves the scene marker as it is
            // (ZERO, ONE), so the presenter expands its highlights like the main view's.
            if (d.hdr_scene_texture)   // PLAN_R2 item 10: promoted with the back buffer (run 15: also when HDR starts on)
                hdr_mirror_id_ = d.sampler_texture[0];
            if (d.hdr_scene_texture && hdr_float_)
                ++hdr_mirror_draws_[hdr_mirror_keep_ ? 0 : 1];
            if (d.hdr_scene_texture && (!hdr_float_ || hdr_mirror_keep_)) {   // run 15: float frame too (before the split)
                hud_rs_[208] = 2;
                if (++hdr_scene_draws_ == 1 || hdr_scene_draws_ % 36000 == 0) {
                    fprintf(stderr, "d3d9 metal: HDR scene texture under the HUD (the mirror) kept its highlight expansion: %llu draws (Test133)\n",
                            (unsigned long long)hdr_scene_draws_);
                }
            }
            hud_rs_[168] |= 8;
            rs = hud_rs_;
            hdr_mask_frame_ = true;
        }

        // Vertex layout.
        float ascale[16][4];
        for (auto &a : ascale)
            for (float &x : a)
                x = 1.0f;
        uint64_t vkey =
            mix(mix((uint64_t)d.decl_id << 32 | d.decl_size, vs_key), fnv(d.decl, d.decl_size));
        {
            uint64_t strides = 0;
            for (int i = 0; i < 8; ++i)
                strides = strides * 1099511628211ull + d.stream[i].stride;
            vkey = mix(vkey, strides);
        }
        MTLVertexDescriptor *vdesc = vertex_descriptor(d, vp, vkey, ascale);

        // Pipeline state (everything but the shaders).
        MTLPixelFormat cf[4] = {MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        for (int i = 0; i < 4; ++i)
            if (id<MTLTexture> t = pass_color_[i])
                cf[i] = t.pixelFormat;
        hdr_cls_ = hdr_float_ ? hdr_class(d, rs, ps_key, pp) : 0;   // PLAN_R2 item 10 (function() applies it)
        MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        bool blend = rs[27] != 0;
        uint32_t cw[4] = {rs[168], rs[190], rs[191], rs[192]};
        // v2.5 MetalFX temporal: in a motion pass every unblended draw writes its motion (colour 1, RG); the rest
        // leave it (a blended draw shows what is behind it, whose motion is already there).
        // The car body and its windows draw blended (paint, glass) but write depth: they are surfaces, so they write
        // their own motion too (unblended, pipeline()); a blend without depth writes (smoke, glow) leaves it.
        const bool want_motion = pass_motion_ && rs[7] && (!blend || rs[14]);
        if (pass_motion_)
            cw[1] = want_motion ? 3u : 0u;
        uint64_t bkey =
            blend ? (1ull | (uint64_t)rs[19] << 1 | (uint64_t)rs[20] << 5 | (uint64_t)rs[171] << 9 |
                     (uint64_t)(rs[206] != 0) << 13 | (uint64_t)rs[207] << 14 |
                     (uint64_t)rs[208] << 18 | (uint64_t)rs[209] << 22)
                  : 0;
        // The shaders of one variant and their pipeline (nil when either shader or the pipeline is unavailable).
        auto build = [&](const d9msl::PixelVariant &pv, const d9msl::VertexVariant &vv, id<MTLFunction> *vf,
                         id<MTLFunction> *ff) -> id<MTLRenderPipelineState> {
            *vf = function(vs_key, vp, pv, false, vv);
            *ff = function(ps_key, pp, pv, true);
            if (!*vf || !*ff)
                return nil;
            uint64_t key = mix(mix(vs_key, ps_key), pv.key());
            key = mix(key, vkey);
            for (int i = 0; i < 4; ++i)
                if (cf[i] != MTLPixelFormatInvalid)
                    key = mix(key, (uint64_t)cf[i] + 1000 * (uint64_t)i);
            key = mix(key, df);
            key = mix(key, bkey);
            key = mix(key, (uint64_t)cw[0] | (uint64_t)cw[1] << 4 | (uint64_t)cw[2] << 8 | (uint64_t)cw[3] << 12);
            return pipeline(key, *vf, *ff, vdesc, cf, df, blend, rs, cw);
        };
        // Remaster run 14 grass blades: as build, with the root-record vertex layout (grass.h Layout: every input at
        // buffer 10 per instance, float4 or half4, the seed / birth time at attribute 16).
        auto build_blade = [&](const d9msl::PixelVariant &pv, const d9msl::VertexVariant &vv,
                               const remaster_grass::Layout &gl, id<MTLFunction> *vf,
                               id<MTLFunction> *ff) -> id<MTLRenderPipelineState> {
            *vf = function(vs_key, vp, pv, false, vv);
            *ff = function(ps_key, pp, pv, true);
            if (!*vf || !*ff)
                return nil;
            MTLVertexDescriptor *bd = grass_vdescs_[gl.hash];
            if (!bd) {
                bd = [MTLVertexDescriptor vertexDescriptor];
                for (int k = 0; k < gl.n; ++k) {
                    bd.attributes[gl.in[k].reg].format = gl.in[k].wide ? MTLVertexFormatFloat4 : MTLVertexFormatHalf4;
                    bd.attributes[gl.in[k].reg].offset = gl.in[k].field;
                    bd.attributes[gl.in[k].reg].bufferIndex = 10;
                }
                bd.attributes[16].format = MTLVertexFormatFloat2;
                bd.attributes[16].offset = gl.extra;
                bd.attributes[16].bufferIndex = 10;
                bd.layouts[10].stride = gl.stride;
                bd.layouts[10].stepFunction = MTLVertexStepFunctionPerInstance;
                bd.layouts[10].stepRate = 1;
                grass_vdescs_[gl.hash] = bd;
            }
            uint64_t key = mix(mix(mix(vs_key, ps_key), pv.key()), gl.hash ^ 0x6772617373626c64ull);
            for (int i = 0; i < 4; ++i)
                if (cf[i] != MTLPixelFormatInvalid)
                    key = mix(key, (uint64_t)cf[i] + 1000 * (uint64_t)i);
            key = mix(key, df);
            key = mix(key, bkey);
            key = mix(key, (uint64_t)cw[0] | (uint64_t)cw[1] << 4 | (uint64_t)cw[2] << 8 | (uint64_t)cw[3] << 12);
            return pipeline(key, *vf, *ff, bd, cf, df, blend, rs, cw);
        };
        // Remaster run 10 fused pool shader, run 11 safety (review/run11/design-A.md). The game thread fuses a
        // (programs, form) only after a probe here built it (host_d9_pool_state), within its supported set. The pool
        // forms sample stages 0/1 as plain 2D (no depth, cube) and never combine with a replacing variant (wet road,
        // soft particle: dropped for a pool draw). A fused draw whose own variant still fails here retries the minimal
        // pool variant (no A2C / shadow filter: the shaders the probe built), so the light stays; only if that fails
        // too is the base drawn alone (pool_late_lost_, expected 0) and the key blocked (two-pass from then on).
        // The readiness key's depth stages: declared 2D stages whose bound texture holds depth (as d3d9.cpp).
        auto pool_depth_stages = [&]() {
            uint32_t m = 0;
            for (const auto &kv : pp.samplers)
                if (kv.first < 16 && kv.second == d9sh::S_2D && bound[kv.first] && bound[kv.first]->depth &&
                    bound[kv.first]->desc.kind == HOST_D9_TEX_2D)
                    m |= 1u << kv.first;
            return m;
        };
        d9msl::VertexVariant vvariant;
        vvariant.smoke = variant.smoke ? 1 : 0;   // run 14 lit smoke: both halves or neither (t7 = world position)
        // Remaster run 12 PBR step 2 pilot: both flags or neither; the slot texture and the key's ecos for later.
        id<MTLTexture> pbw_tex = nil;
        float pbw_ecos = 1.0f;
        if (pbw_active_ && (pbw_tex = pbw_select(d, vs_key, ps_key, vp, pp, bound[0], variant, &pbw_ecos))) {
            variant.pbrworld = 1;
            vvariant.pbr = 1;
        }
        // Remaster run 13 natural water (RECOMP_REMASTER_WATER=1; A/B switch water): both halves or neither (the pbr
        // vertex half writes t4..t6); exclusive with the pilot and every other replacing variant (water_select).
        if (water_on_ && water_select(d, vs_key, ps_key, vp, pp, bound[0], variant, vvariant)) {
            variant.water = water_mode_;
            vvariant.pbr = 1;
        }
        // Remaster run 13 wall parallax (RECOMP_REMASTER_PARALLAX=1; A/B switch parallax): WORLDNORMALMAP draws whose
        // pixel shader has the t[0] samples and tangent-space V (pixel_parallax_stage) and whose bound normal map stores
        // alpha (DXT3, DXT5, A8R8G8B8: a DXT1 or X8 map would read a constant height and slide the texture).
        if (parallax_on_ && d.label && !strncmp(d.label, "IDI_WORLDNORMALMAP_FX/", 22) &&
            !recomp_ab::old(recomp_ab::PARALLAX) && !variant.pbrworld && !variant.pool) {
            auto px = parallax_ps_.find(ps_key);
            if (px == parallax_ps_.end())
                px = parallax_ps_.emplace(ps_key, d9msl::pixel_parallax_stage(pp)).first;
            const Tex *nt = px->second >= 0 && px->second < 16 ? bound[px->second] : nullptr;
            const uint32_t nf = nt ? nt->desc.format : 0;
            if (nt && (nf == 0x33545844u || nf == 0x35545844u || nf == 21u)) {   // DXT3, DXT5, A8R8G8B8
                variant.parallax = 1;
                ++parallax_draws_;
            } else if (px->second >= 0) {
                ++parallax_noalpha_;
            }
        }
        // Remaster run 13 natural glass (RECOMP_REMASTER_GLASS=1; A/B switch glass): building-glass draws whose pixel
        // shader has the reflection add and whose vertex shader has one NORMAL input and one eye term.
        if (glass_on_ && d.label && !strncmp(d.label, "IDI_GLOSSYWINDOW_FX/", 20) && !recomp_ab::old(recomp_ab::GLASS)) {
            auto gp = glass_ps_.find(ps_key);
            if (gp == glass_ps_.end())
                gp = glass_ps_.emplace(ps_key, [&] {
                         // Run 16 glass pop: also the ps_1_x window program (RECOMP_TEST_GLASS1=0: TEST ONLY, as before).
                         const bool g2 = d9msl::pixel_is_glass(pp), g1 = !g2 && glass1_on_ && d9msl::pixel_is_glass1(pp);
                         fprintf(stderr, "[remaster] glass: ps %016llx (ps %d.%d) %s\n", (unsigned long long)ps_key,
                                 (int)pp.major, (int)pp.minor, g2 ? "glass" : g1 ? "glass ps_1_x" : "not glass");
                         return g2 || g1;
                     }()).first;
            auto gv = glass_vs_.find(vs_key);
            if (gv == glass_vs_.end())
                gv = glass_vs_.emplace(vs_key, [&] {
                         const d9msl::GlassFlow f = d9msl::glass_vs_flow(vp);
                         return f.ok && (glass_mode_ != 2 || f.light >= 0);
                     }()).first;
            if (gp->second && gv->second && !variant.pbrworld && !vvariant.pbr) {
                variant.glass = glass_mode_;
                vvariant.glass = glass_mode_;
                ++glass_draws_;
                if (pp.major == 1)
                    ++glass1_draws_;
            }
        }
        // Remaster run 13 Xbox 360 sky, ours (RECOMP_REMASTER_SKY=1; A/B switch sky): IDI_SKYBOX_FX draws whose pixel
        // shader is the PC pass 0 combine and/or whose vertex shader is the in-scatter (once per program each).
        if (sky_on_ && d.label && !strncmp(d.label, "IDI_SKYBOX_FX/", 14)) {
            ++sky_all_;
            if (!recomp_ab::old(recomp_ab::SKY)) {
                auto sp = sky_ps_.find(ps_key);
                if (sp == sky_ps_.end())
                    sp = sky_ps_.emplace(ps_key, d9msl::pixel_is_sky(pp)).first;
                auto sv = sky_vs_.find(vs_key);
                if (sv == sky_vs_.end())
                    sv = sky_vs_.emplace(vs_key, d9msl::vertex_is_sky(vp)).first;
                if (sp->second) {
                    variant.sky = 1;
                    ++sky_draws_;
                }
                if (sv->second) {
                    vvariant.sky = 1;
                    ++sky_vs_draws_;
                }
            }
        }
        // Remaster run 13 car reflections (RECOMP_REMASTER_CARENV=1; A/B switch carenv): car effect vertex shaders
        // with the environment strength lines (vertex_is_carenv, once per program).
        if (carenv_on_ && d.label && !strncmp(d.label, "IDI_CAR_FX/", 11) && !recomp_ab::old(recomp_ab::CARENV)) {
            auto ce = carenv_vs_.find(vs_key);
            if (ce == carenv_vs_.end())
                ce = carenv_vs_.emplace(vs_key, d9msl::vertex_is_carenv(vp)).first;
            if (ce->second && carenv_mode_ == 2) {   // run 13 Xbox 360 car paint: VS t6 + PS flake/hot spot/bloom
                auto cp = carpaint_ps_.find(ps_key);
                if (cp == carpaint_ps_.end())
                    cp = carpaint_ps_.emplace(ps_key, d9msl::pixel_is_carpaint(pp)).first;
                auto cf = carflow_vs_.find(vs_key);
                if (cf == carflow_vs_.end()) {
                    uint32_t cpos = 0, ceye = 0;
                    cf = carflow_vs_.emplace(vs_key, d9msl::car_vs_flow(vp, &cpos, &ceye)).first;
                }
                if (cp->second && cf->second) {
                    vvariant.carenv = 2;
                    variant.carpaint = 1;
                    ++carenv_draws_;
                }
            } else if (ce->second) {
                vvariant.carenv = 1;
                ++carenv_draws_;
            }
        }
        // Remaster run 13 front-end floor shadows (RECOMP_REMASTER_FEFLOOR=1; A/B switch fefloor): the floor pair, with
        // the receivers' shadow filter.
        if (fef_on_ && fef_select(d, vs_key, ps_key)) {
            variant.fefloor = fef_debug_ == 1 ? 2 : fef_debug_ == 2 ? 3 : fef_debug_ == 3 ? 4 : 1;
            vvariant.fefloor = 1;
            if (shadowpcf_on_ && !recomp_ab::old(recomp_ab::SHADOWPCF))
                variant.shadowpcf = 1;
        }
        if (want_motion) {   // v2.5 MetalFX temporal: both halves (the vertex half finds its own clip rows)
            variant.motion = 1;
            vvariant.motion = 1;
        }
        auto pool_variant = [&](uint32_t form, bool minimal, d9msl::PixelVariant &pv, d9msl::VertexVariant &vv) {
            const uint16_t st = form == 1 ? 3u : 1u;   // the stages the pool form samples
            // v4: pixel shaders 1.x too (their stage kinds come from the bound textures); the wet road stays (its
            // lines and the pool term share nothing: the wet base plus the pool light, as the native two passes)
            if (form < 1 || form > 2 || (variant.depth_mask & st) || (variant.cube_mask & st))
                return false;
            pv = variant;
            pv.parallax = 0;
            pv.roadsheen = 0;
            pv.soft = 0;
            if (minimal) {
                pv.a2c = 0;
                pv.shadowpcf = 0;
                pv.pbrprobe = 0;
                pv.pbrworld = 0;
                pv.water = 0;   // run 13 natural water: the minimal form is the probe's (built without it)
            }
            pv.pool = (uint8_t)form;
            vv = d9msl::VertexVariant{};
            vv.pool = (uint8_t)form;
            vv.motion = pv.motion;   // v2.5 MetalFX temporal: both halves or neither
            vv.pbr = pv.pbrworld || pv.water;   // run 12/13: the pilot's (or water's) vertex half goes with its pixel half
            return true;
        };
        id<MTLFunction> vfn = nil, ffn = nil;
        id<MTLRenderPipelineState> pso = nil;
        if (d.pool_fuse && d.pool_block) {
            const int force = pool_force();
            const uint64_t pkey = host_d9_pool_key(vs_key, ps_key, d.pool_fuse | pool_depth_stages() << 8);
            d9msl::PixelVariant pv;
            d9msl::VertexVariant pvv;
            if (force != POOL_FORCE_LATE && force != POOL_FORCE_LATEALL && pool_variant(d.pool_fuse, false, pv, pvv))
                pso = build(pv, pvv, &vfn, &ffn);
            if (!pso && variant.motion) {   // v2.5 MetalFX temporal: the pool light before the motion
                ++tmp_.plain;
                variant.motion = 0;
                vvariant.motion = 0;
                cw[1] = 0;
                if (force != POOL_FORCE_LATE && force != POOL_FORCE_LATEALL && pool_variant(d.pool_fuse, false, pv, pvv))
                    pso = build(pv, pvv, &vfn, &ffn);
            }
            if (!pso && variant.pbrworld && force == POOL_FORCE_NONE) {   // run 12: full without the pilot before minimal
                ++pbw_fallback_;
                variant.pbrworld = 0;
                if (pool_variant(d.pool_fuse, false, pv, pvv))
                    pso = build(pv, pvv, &vfn, &ffn);
            }
            if (!pso && variant.water && force == POOL_FORCE_NONE) {   // run 13 natural water: full without it before minimal
                ++water_fallback_;
                variant.water = 0;
                vvariant.pbr = variant.pbrworld;
                if (pool_variant(d.pool_fuse, false, pv, pvv))
                    pso = build(pv, pvv, &vfn, &ffn);
            }
            if (!pso && force != POOL_FORCE_LATEALL && pool_variant(d.pool_fuse, true, pv, pvv)) {
                pso = build(pv, pvv, &vfn, &ffn);
                if (pso)
                    ++pool_minimal_;
            }
            if (pso) {
                variant = pv;
                vvariant = pvv;
                ++pool_draws_;
            } else {
                if (!pool_late_lost_++)
                    fprintf(stderr, "[remaster] pool fusion: DEFECT: a ready fused draw could not be built (vs %016llx "
                                    "ps %016llx form %u%s); drawn without its pool light, now two-pass\n",
                            (unsigned long long)vs_key, (unsigned long long)ps_key, d.pool_fuse,
                            force ? ", forced by the test switch" : "");
                host_d9_pool_mark(pkey, HOST_D9_POOL_BLOCKED);
            }
        }
        if (!pso) {
            defer_ok_ = true;   // run 15 (hitch): the chain below draws a not-yet-compiled variant plain
            pso = build(variant, vvariant, &vfn, &ffn);
            if ((!vfn || !ffn || !pso) && variant.motion) {   // v2.5 MetalFX temporal: drawn plain (camera motion)
                ++tmp_.plain;
                variant.motion = 0;
                vvariant.motion = 0;
                cw[1] = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.pbrprobe) {   // run 12 probe (TEST ONLY): never lose the draw
                ++pbr_fallback_;
                variant.pbrprobe = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.smoke) {   // run 14 lit smoke: drawn as today, both halves dropped
                if (++smoke_fallback_ <= 4)
                    fprintf(stderr, "[remaster] lit smoke: vs %016llx ps %016llx kind %u: variant failed, drawn plain\n",
                            (unsigned long long)vs_key, (unsigned long long)ps_key, (unsigned)variant.smoke);
                variant.smoke = 0;
                vvariant.smoke = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.parallax) {   // run 13 wall parallax: drawn as today
                ++parallax_fallback_;
                variant.parallax = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.glass) {   // run 13 natural glass: drawn as today, both halves dropped
                ++glass_fallback_;
                variant.glass = 0;
                vvariant.glass = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && (variant.sky || vvariant.sky)) {   // run 13 sky (ours): drawn as the PC program
                ++sky_fallback_;
                variant.sky = 0;
                vvariant.sky = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && vvariant.carenv) {   // run 13 car reflections: drawn as today
                ++carenv_fallback_;
                vvariant.carenv = 0;
                variant.carpaint = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.fefloor) {   // run 13 front-end floor shadows: drawn as today
                ++fef_fallback_;
                ++fef_build_fail_;
                variant.fefloor = 0;
                vvariant.fefloor = 0;
                if (!variant.depth_mask)
                    variant.shadowpcf = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.roadsheen) {   // run 13 road sheen: drawn as today
                ++roadsheen_fallback_;
                variant.roadsheen = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.water) {   // run 13 natural water: drawn as today, both halves dropped
                ++water_fallback_;
                variant.water = 0;
                vvariant.pbr = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            if ((!vfn || !ffn || !pso) && variant.pbrworld) {   // run 12 pilot: drawn as today, both halves dropped
                ++pbw_fallback_;
                variant.pbrworld = 0;
                vvariant.pbr = 0;
                pso = build(variant, vvariant, &vfn, &ffn);
            }
            defer_ok_ = false;
            if (!vfn || !ffn) {
                if (tr)
                    tr->result = d9trace::Result::UntranslatedShader;
                skip("untranslated shader");
                return;
            }
            if (!pso) {
                if (tr)
                    tr->result = d9trace::Result::NoPipeline;
                skip("no pipeline");
                return;
            }
            // Run 11 probe: the base pass of a two-pass pool draw (drawn plain here, the game thread adds the pool
            // pass) whose (programs, form) is unknown: build its fused variant and the minimal one now.
            if (d.pool_probe) {
                const uint64_t pkey = host_d9_pool_key(vs_key, ps_key, d.pool_probe | pool_depth_stages() << 8);
                if (host_d9_pool_state(pkey) == HOST_D9_POOL_UNKNOWN) {
                    const int force = pool_force();
                    int fail = force >= POOL_FORCE_REFUSE && force <= POOL_FORCE_PSO ? force : 0;
                    // v4: a wet-road program fuses with its wet shading kept (pool_variant); RECOMP_REMASTER_POOL_WET=0
                    // (TEST ONLY) refuses it as before.
                    static const bool wet_refuse = [] {
                        const char *e = getenv("RECOMP_REMASTER_POOL_WET");
                        return e && *e == '0';
                    }();
                    if (!fail && wetroad_on_ && wet_refuse) {
                        auto wr = wetroad_ps_.find(ps_key);
                        if (wr == wetroad_ps_.end())
                            wr = wetroad_ps_.emplace(ps_key, d9msl::pixel_is_wetroad(pp)).first;
                        if (wr->second)
                            fail = POOL_FORCE_REFUSE;
                    }
                    if (!fail && roadsheen_on_) {   // run 13: nor does a sheen road program
                        auto rs = roadsheen_ps_.find(ps_key);
                        if (rs == roadsheen_ps_.end())
                            rs = roadsheen_ps_.emplace(ps_key, d9msl::pixel_is_roadsheen(pp)).first;
                        if (rs->second)
                            fail = POOL_FORCE_REFUSE;
                    }
                    for (int m = 0; m < 2 && !fail; ++m) {
                        d9msl::PixelVariant pv;
                        d9msl::VertexVariant pvv;
                        if (!pool_variant(d.pool_probe, m == 1, pv, pvv)) {
                            fail = POOL_FORCE_REFUSE;
                            break;
                        }
                        pv.pbrworld = 0;   // run 12: readiness is judged without the pilot (never blocks fusion)
                        pv.water = 0;      // run 13: nor natural water
                        pvv.pbr = 0;
                        id<MTLFunction> a = nil, b = nil;
                        id<MTLRenderPipelineState> p = build(pv, pvv, &a, &b);
                        fail = !a ? POOL_FORCE_VS : !b ? POOL_FORCE_PS : !p ? POOL_FORCE_PSO : 0;
                    }
                    host_d9_pool_mark(pkey, fail ? HOST_D9_POOL_BLOCKED : HOST_D9_POOL_READY);
                    ++pool_probe_[fail];
                    if (fail || pool_probe_[0] <= 8)
                        fprintf(stderr, "[remaster] pool fusion: probe vs %016llx ps %016llx form %u: %s%s\n",
                                (unsigned long long)vs_key, (unsigned long long)ps_key, d.pool_probe,
                                kPoolProbe[fail], force ? " (forced by the test switch)" : "");
                }
            }
        }
        if (pso != es_.pipeline) {
            [enc_ setRenderPipelineState:pso];
            es_.pipeline = pso;
            ++perf_pso_changes_;
        }

        // Depth and stencil.
        bool zon = rs[7] != 0 && pass_depth_;
        uint64_t dkey = (uint64_t)zon | (uint64_t)(rs[14] != 0) << 1 | (uint64_t)rs[23] << 2 |
                        (uint64_t)(rs[52] != 0) << 6;
        bool stencil = rs[52] != 0 && pass_depth_;
        if (stencil)
            dkey = mix(dkey, fnv(&rs[53], 7 * sizeof(uint32_t)) ^ ((uint64_t)rs[185] << 1) ^
                                 fnv(&rs[186], 4 * sizeof(uint32_t)));
        {
            id<MTLDepthStencilState> dss = depth_state(dkey, zon, rs, stencil);
            if (dss != es_.depth) {
                [enc_ setDepthStencilState:dss];
                es_.depth = dss;
            }
        }
        if (stencil && es_.stencil_ref != (rs[57] & 0xff)) {
            es_.stencil_ref = rs[57] & 0xff;
            [enc_ setStencilReferenceValue:es_.stencil_ref];
        }

        // Rasterizer.
        {
            int cull = rs[22] == 2   ? (int)MTLCullModeFront
                       : rs[22] == 3 ? (int)MTLCullModeBack
                                     : (int)MTLCullModeNone;
            if (cull != es_.cull) {
                [enc_ setCullMode:(MTLCullMode)cull];
                es_.cull = cull;
            }
            int fill = rs[8] == 2 ? (int)MTLTriangleFillModeLines : (int)MTLTriangleFillModeFill;
            if (fill != es_.fill) {
                [enc_ setTriangleFillMode:(MTLTriangleFillMode)fill];
                es_.fill = fill;
            }
        }
        float bias, slope;
        memcpy(&bias, &rs[195], 4);
        memcpy(&slope, &rs[175], 4);
        // D3D's DEPTHBIAS is in depth units; Metal's in units of the minimum resolvable
        // difference, which for a float depth buffer is tiny, so scale it up.
        if (!(bias == es_.bias && slope == es_.slope)) {
            [enc_ setDepthBias:bias * 16777215.0f slopeScale:slope clamp:0.0f];
            es_.bias = bias;
            es_.slope = slope;
        }
        const double s = pass_scale_;
        MTLViewport vpm = {d.viewport[0] * s, d.viewport[1] * s, d.viewport[2] * s,
                           d.viewport[3] * s, d.depth_range[0],  d.depth_range[1]};
        if (!es_.viewport_valid || memcmp(&vpm, &es_.viewport, sizeof vpm) != 0) {
            [enc_ setViewport:vpm];
            es_.viewport = vpm;
            es_.viewport_valid = true;
        }
        set_scissor(rs[174] != 0, d.scissor);
        if (blend && (rs[19] == 14 || rs[19] == 15 || rs[20] == 14 || rs[20] == 15 ||
                      rs[207] == 14 || rs[208] == 14)) {
            uint32_t f = rs[193];
            [enc_ setBlendColorRed:((f >> 16) & 255) / 255.0f
                             green:((f >> 8) & 255) / 255.0f
                              blue:(f & 255) / 255.0f
                             alpha:((f >> 24) & 255) / 255.0f];
        }

        // Constants and parameters.
        D9VSParams vparams{};
        memcpy(vparams.bconst, d.vbool, sizeof vparams.bconst);
        // Direct3D 9 samples a pixel at its integer corner, here at its centre:
        // geometry moves half a pixel right and down to match. 63/64 of it, as
        // Wine does, so an edge never lands exactly on a pixel centre.
        // Remaster run 13 MetalFX: in the scene backing (below the game's size) half a rendered pixel; 1 elsewhere,
        // which leaves every other pass exactly as it was.
        const float hs = upscale_factor_ > 0.0f ? upscale_draw() : 1.0f;
        vparams.halfpix[0] = d.viewport[2] ? (63.0f / 64.0f) / (d.viewport[2] * hs) : 0.0f;
        vparams.halfpix[1] = d.viewport[3] ? -(63.0f / 64.0f) / (d.viewport[3] * hs) : 0.0f;
        // run 14 foundation F3: the scene passes' jitter (RECOMP_REMASTER_FOUNDATION_JITTER). v2.5 MetalFX temporal:
        // only the 3D geometry (depth-tested draws). A full-screen 2D pass inside the scene (the PC game copies the
        // picture and draws it back for its effects) jittered again would move the picture by another jitter step
        // each time, more than the scaler is told: the picture shakes.
        if (fnd_on_ && fnd_.open && (!tmp_on_ || rs[7]))
            fnd_halfpix(d, vparams.halfpix);
        if (tmp_.frame && pass_motion_ && !rs[7])
            tmp_note_flat(d);
        memcpy(vparams.ascale, ascale, sizeof ascale);
        [enc_ setVertexBytes:&vparams length:sizeof vparams atIndex:17];
        set_constants(true, d.vconst, d.vconst_count, vp);
        if (vvariant.motion)   // v2.5 MetalFX temporal: this draw's clip rows of the previous scene (vertex buffer 20)
            tmp_bind(d, vs_key, vp);
        D9PSParams pparams{};
        memcpy(pparams.bconst, d.pbool, sizeof pparams.bconst);
        pparams.alpha_func = rs[15] ? (int32_t)rs[25] : 8;
        pparams.alpha_ref = (rs[24] & 0xff) / 255.0f;
        pparams.pad0 = variant.a2c;
        if (!variant.pbrworld)   // run 12 pilot: dropped by a fallback (or never chosen)
            pbw_tex = nil;
        else
            pparams.pad1 = pbw_ecos;   // the key's packed manifest values (unused by the shader since the per-pixel tilt cosine)
        if (variant.water) {   // run 13 natural water: seconds for the ripples (wraps at 1000 s; the rates are n/1000 Hz)
            pparams.pad1 = (float)fmod(CACurrentMediaTime() - water_t0_, 1000.0);
            ++water_draws_;
        }
        if (variant.wetroad) // Remaster R2 wet roads C1c: seconds for the rain disturbance (wraps at 1000 s)
            pparams.pad1 = (float)fmod(CACurrentMediaTime() - wetroad_t0_, 1000.0);
        if (variant.soft) {
            pparams.pad1 = variant.smoke && smk_soft > 0.0f ? smk_soft : variant.soft == 2 ? soft_fog_inv_ : soft_inv_dist_;   // run 14: fog's wider fade (run 15: also plain)
            [enc_ setFragmentTexture:soft_depth_ atIndex:16];
        }
        if (variant.smoke)   // run 14 lit smoke: the kind's parameters, sun and camera (fragment buffer 5)
            [enc_ setFragmentBytes:&smk length:sizeof smk atIndex:5];
        if (variant.fefloor)   // run 13 front-end floor shadows: the darkness K
            pparams.pad1 = fef_k_;
        if (rs[28]) {
            uint32_t table = rs[35];
            pparams.fog_mode = table == 3 ? 2 : table == 1 ? 3 : table == 2 ? 4 : 1;
            uint32_t fc = rs[34];
            pparams.fog_color[0] = ((fc >> 16) & 255) / 255.0f;
            pparams.fog_color[1] = ((fc >> 8) & 255) / 255.0f;
            pparams.fog_color[2] = (fc & 255) / 255.0f;
            pparams.fog_color[3] = 1.0f;
            memcpy(&pparams.fog_start, &rs[36], 4);
            memcpy(&pparams.fog_end, &rs[37], 4);
            memcpy(&pparams.fog_density, &rs[38], 4);
        }
        [enc_ setFragmentBytes:&pparams length:sizeof pparams atIndex:1];
        set_constants(false, d.pconst, d.pconst_count, pp);
        if (variant.pool)   // Remaster run 10: the pool block (20 float4)
            [enc_ setFragmentBytes:d.pool_block length:20 * 16 atIndex:2];
        if (variant.fefloor) {   // run 13 front-end floor shadows: Mclip rows and the map (index 18 is not in the state cache)
            [enc_ setVertexBytes:fef_m_ length:sizeof fef_m_ atIndex:18];
            [enc_ setFragmentTexture:fef_map_ atIndex:18];
            ++fef_shadowed_;
        }
        if (variant.pbrworld) {   // Remaster run 12 pilot: the slot normal map (index 17 is not in the state cache)
            [enc_ setFragmentTexture:pbw_tex atIndex:17];
            ++pbw_draws_;
            if (pbw_debug_) {
                if (!pwq_) {
                    pwq_ = [mtl_ newBufferWithLength:d9msl::kPbrWorldCounters * 4 options:MTLResourceStorageModeShared];
                    memset(pwq_.contents, 0, d9msl::kPbrWorldCounters * 4);
                }
                [enc_ setFragmentBuffer:pwq_ offset:0 atIndex:3];
            }
        }
        if (variant.pbrprobe) {   // Remaster run 12 step 0 (TEST ONLY): the probe's histogram counters
            if (!pbq_) {
                pbq_ = [mtl_ newBufferWithLength:d9msl::kPbrProbeCounters * 4 options:MTLResourceStorageModeShared];
                memset(pbq_.contents, 0, d9msl::kPbrProbeCounters * 4);
            }
            [enc_ setFragmentBuffer:pbq_ offset:0 atIndex:3];
        }

        // Textures and samplers.
        if (pp.sampler_mask < 0) {
            uint32_t cubes = 0;
            for (const auto &kv : pp.samplers)
                if (kv.first < 16 && kv.second == d9sh::S_CUBE)
                    cubes |= 1u << kv.first;
            pp.cube_samplers = cubes;
            pp.sampler_mask = d9msl::pixel_sampler_mask(pp);
        }
        uint32_t used = (uint32_t)pp.sampler_mask;
        if (variant.pool)   // Remaster run 10: the pool term samples stage 0 (and 1 for the normal-mapped form)
            used |= variant.pool == 1 ? 3u : 1u;
        if (tr)
            tr->used_samplers = used;
        for (uint32_t s = 0; s < 16; ++s) {
            if (!(used >> s & 1))
                continue;
            Tex *t = bound[s];
            id<MTLTexture> tex = t ? t->texture : nil;
            if (t && rtex_.on())
                rtex_.bind(t->desc.key);
            if (tr && !tex)
                tr->missing_textures |= 1u << s;
            bool cube = pp.major >= 2 ? (pp.cube_samplers >> s & 1) : (variant.cube_mask >> s & 1);
            auto declaration = pp.samplers.find(s);
            const bool volume = pp.major >= 2 && declaration != pp.samplers.end() &&
                                declaration->second == d9sh::S_VOLUME;
            if (tex && (t->cube != cube || (t->desc.kind == HOST_D9_TEX_VOLUME) != volume)) {
                if (tr)
                    tr->incompatible_textures |= 1u << s;
                tex = nil;
            }
            const bool depth = variant.depth_mask >> s & 1;
            if (tex && t->depth != depth) {
                if (tr)
                    tr->incompatible_textures |= 1u << s;
                tex = nil;
            }
            // Nor can the depth buffer this pass writes.
            if (tex && depth && tex == pass_depth_) {
                if (tr)
                    tr->feedback_textures |= 1u << s;
                tex = nil;
            }
            // A texture that is also this pass's target cannot be read from.
            for (int i = 0; i < 4 && tex; ++i)
                if (tex == pass_color_[i]) {
                    if (tr)
                        tr->feedback_textures |= 1u << s;
                    tex = nil;
                }
            if (tex)
                t->used = serial_;
            if (tex && t->desc.key && !t->no_replace && rcache_.active()) {
                // after every check above, which judge the game's own texture
                id<MTLTexture> r = rcache_.bind(t->desc.key);
                if (r && rtex_.swap()) {
                    tex = r;
                    rtex_.replaced_bind();
                }
            }
            id<MTLTexture> use = tex     ? tex
                                 : depth ? placeholder_depth()
                                         : placeholder(cube, volume);
            if (use != es_.texture[s]) {
                [enc_ setFragmentTexture:use atIndex:s];
                es_.texture[s] = use;
            }
            id<MTLSamplerState> smp = sampler(d.sampler_state + s * 14);
            if (smp != es_.sampler[s]) {
                [enc_ setFragmentSamplerState:smp atIndex:s];
                es_.sampler[s] = smp;
            }
        }

        // Geometry.
        if (tr)
            tr->vertex_stream_mask = stream_mask_;
        for (int i = 0; i < 8; ++i) {
            if (!(stream_mask_ >> i & 1))
                continue;
            const HostD9Stream &st = d.stream[i];
            if (st.buffer) {
                auto b = buffers_.find(st.buffer);
                if (b == buffers_.end() || !b->second.buffer) {
                    if (tr)
                        tr->result = d9trace::Result::MissingVertexBuffer;
                    skip("missing vertex buffer");
                    return;
                }
                b->second.used = serial_;
                [enc_ setVertexBuffer:b->second.buffer offset:st.offset atIndex:i];
            } else if (d.inline_vertices) {
                bind_inline(i, d.inline_vertices, d.inline_bytes);
            } else {
                [enc_ setVertexBuffer:zero_ offset:0 atIndex:i];
            }
        }
        if (vdesc_needs_zero_)
            [enc_ setVertexBuffer:zero_ offset:0 atIndex:8];
        const d9trace::Result encoded = encode_primitives(d);
        if (pass_motion_) {   // v2.5 MetalFX temporal: grass shells and blades draw without motion (the ground's stays)
            cw[1] = 0;
            variant.motion = 0;
        }
        if (grass_.blades_on() && encoded == d9trace::Result::Encoded)   // run 14 grass blades: after the draw, never instead
            grass_blades(d, rs, vs_key, ps_key, vp, pp, bound[0], variant, dkey, stencil, vparams, pparams, build_blade);
        if (grass_.on() && encoded == d9trace::Result::Encoded)   // run 13 grass shells: after the draw, never instead
            grass_shells(d, rs, vs_key, ps_key, vp, pp, bound[0], variant, dkey, stencil, vparams, pparams, build);
        if (tr)
            tr->result = encoded;
        if (trace_ && encoded == d9trace::Result::Encoded) {
            for (int i = 0; i < 4; ++i)
                if (pass_color_[i] && cw[i])
                    textures_[d.target.color[i].id].written = serial_;
            if (pass_depth_ && !pass_fitted_depth_ && ((zon && rs[14]) || stencil))
                textures_[d.target.depth.id].written = serial_;
        }
        ++draws_;
        if (probing()) {
            char vs_hash[20], ps_hash[20];
            snprintf(vs_hash, sizeof vs_hash, "%016llx", (unsigned long long)vs_key);
            snprintf(ps_hash, sizeof ps_hash, "%016llx", (unsigned long long)ps_key);
            float c0[4] = {0, 0, 0, 0};
            if (d.vconst && d.vconst_count)
                memcpy(c0, d.vconst, sizeof c0);
            fprintf(
                stderr,
                "probe draw %llu: rt %u/%u/%u depth %u vp %d,%d %dx%d prim %u x%u start %u base %d "
                "ib %u stream0 %u+%u/%u inline %u vs %s ps %s z %u/%u/%u cull %u blend %u %u/%u "
                "cw %x atest %u/%u fog %u tex0 %u c0 %g %g %g %g %s\n",
                (unsigned long long)draws_, d.target.color[0].id, d.target.color[0].face,
                d.target.color[0].level, d.target.depth.id, d.viewport[0], d.viewport[1],
                d.viewport[2], d.viewport[3], d.primitive, d.primitive_count, d.start,
                d.base_vertex, d.index_buffer, d.stream[0].buffer, d.stream[0].offset,
                d.stream[0].stride, d.inline_bytes, vs_hash, ps_hash, rs[7], rs[14], rs[23], rs[22],
                rs[27], rs[19], rs[20], rs[168], rs[15], rs[25], rs[28], d.sampler_texture[0],
                c0[0], c0[1], c0[2], c0[3], d.label ? d.label : "-");
        }
    }

    // ---- occlusion queries ----------------------------------------------
    // Every command buffer with a query in it has a buffer of 64-bit sample
    // counts. Each render pass drawn under a query gets a slot of its own; the
    // query's count is the sum of its slots once the command buffer has run.
    static constexpr NSUInteger kVisSlots = 1024;
    struct Query {
        struct Slot {
            id<MTLBuffer> buffer;
            uint32_t index;
            uint64_t serial;
        };
        std::vector<Slot> slots; // not yet counted
        uint64_t counted = 0;
        bool overflow = false;
        bool ended = false;
    };
    void query_begin(uint32_t id) override {
        if (trace_)
            trace_->event("query_begin", trace_context(), {id});
        Query &q = queries_[id];
        q = Query{};
        query_ = id;
        pass_slot_ = -1;
        if (enc_ && !pass_vis_)
            end_pass(); // the next draw opens a pass that can count
    }
    void query_end(uint32_t id) override {
        if (trace_)
            trace_->event("query_end", trace_context(), {id});
        if (query_ != id)
            return;
        if (enc_ && pass_slot_ >= 0)
            [enc_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        pass_slot_ = -1;
        query_ = 0;
        queries_[id].ended = true;
    }
    int query_result(uint32_t id, uint32_t *count) override {
        auto it = queries_.find(id);
        if (it == queries_.end() || !it->second.ended)
            return -1;
        Query &q = it->second;
        if (q.overflow) {
            *count = 1;
            return 1;
        }
        if (!fold(q))
            return 0; // its frame has not finished on the GPU
        *count = (uint32_t)std::min<uint64_t>(q.counted, 0xffffffffu);
        if (trace_)
            if (auto *e = trace_->event("query_result", trace_context(), {id}))
                e->values[0] = *count;
        return 1;
    }
    // Adds the slots whose command buffers have run; true when none is left.
    bool fold(Query &q) {
        uint64_t done = completed_.load();
        size_t keep = 0;
        for (auto &s : q.slots) {
            if (s.serial <= done)
                q.counted += ((const uint64_t *)s.buffer.contents)[s.index];
            else
                q.slots[keep++] = s;
        }
        q.slots.resize(keep);
        return keep == 0;
    }
    void query_drop(uint32_t id) override {
        if (query_ == id)
            query_end(id);
        queries_.erase(id);
    }
    void count_query() {
        if (!query_ || pass_slot_ >= 0)
            return;
        Query &q = queries_[query_];
        if (!pass_vis_ || vis_used_ >= kVisSlots) {
            q.overflow = true;
            return;
        }
        pass_slot_ = (int)vis_used_++;
        ((uint64_t *)vis_.contents)[pass_slot_] = 0;
        [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting
                               offset:(NSUInteger)pass_slot_ * 8];
        q.slots.push_back({vis_, (uint32_t)pass_slot_, serial_});
    }
    id<MTLBuffer> vis_buffer() {
        if (vis_)
            return vis_;
        if (!vis_pool_.empty() && vis_pool_.front().second <= completed_.load())
            for (auto &[id, q] : queries_)
                fold(q);
        for (size_t i = 0; i < vis_pool_.size(); ++i) {
            if (vis_pool_[i].second <= completed_.load()) {
                vis_ = vis_pool_[i].first;
                vis_pool_.erase(vis_pool_.begin() + i);
                break;
            }
        }
        if (!vis_)
            vis_ = [mtl_ newBufferWithLength:kVisSlots * 8 options:MTLResourceStorageModeShared];
        vis_used_ = 0;
        return vis_;
    }

    bool probing() const {
        return probe_frame_ && presents_ + 1 == probe_frame_;
    }
    d9trace::Context trace_context() const {
        return {serial_, submitted_, completed_.load(), pass_serial_};
    }
    d9trace::Resource trace_texture(uint32_t id) {
        const Tex *t = tex_ptr(id);
        return t ? d9trace::Resource{id, t->generation, t->revision, t->used, t->written}
                 : d9trace::Resource{id};
    }
    void trace_texture_desc(uint32_t id, uint32_t *desc) {
        const Tex *t = tex_ptr(id);
        if (!t)
            return;
        desc[0] = t->desc.width;
        desc[1] = t->desc.height;
        desc[2] = t->desc.format;
        desc[3] = t->desc.kind;
    }
    d9trace::Resource trace_buffer(uint32_t id) const {
        const auto it = buffers_.find(id);
        if (it == buffers_.end())
            return {id};
        const Buf &b = it->second;
        return {id, b.generation, b.revision, b.used, 0};
    }
    void trace_buffer_upload(uint32_t id, Buf &b, uint32_t total, uint32_t offset, uint32_t size,
                             bool replaced) {
        if (!trace_)
            return;
        if (replaced)
            b.generation = ++resource_generation_;
        ++b.revision;
        if (auto *e = trace_->event("buffer_upload", trace_context(), trace_buffer(id))) {
            e->values[0] = total;
            e->values[1] = offset;
            e->values[2] = size;
            e->values[3] = replaced;
            e->values[4] = b.buffer != nil;
        }
    }
    d9trace::Draw *trace_draw(const HostD9Draw &d) {
        if (!trace_)
            return nullptr;
        auto *r = trace_->record(d);
        if (!r)
            return nullptr;
        r->context = trace_context();
        r->vs = d.vs_key ? d.vs_key : (d.vs ? d9sh::code_key(d.vs, d.vs_size) : 0);
        r->ps = d.ps_key ? d.ps_key : (d.ps ? d9sh::code_key(d.ps, d.ps_size) : 0);
        for (int i = 0; i < 16; ++i) {
            r->textures[i] = trace_texture(d.sampler_texture[i]);
            trace_texture_desc(d.sampler_texture[i], r->texture_desc[i]);
        }
        for (int i = 0; i < 8; ++i)
            r->buffers[i] = trace_buffer(d.stream[i].buffer);
        r->buffers[8] = trace_buffer(d.index_buffer);
        for (int i = 0; i < 4; ++i) {
            r->attachments[i] = trace_texture(d.target.color[i].id);
            trace_texture_desc(d.target.color[i].id, r->attachment_desc[i]);
        }
        r->attachments[4] = trace_texture(d.target.depth.id);
        trace_texture_desc(d.target.depth.id, r->attachment_desc[4]);
        return r;
    }
    // The render queue orders this request after preceding commands. It copies
    // no live resource bytes and does not wait for the GPU. File I/O is confined
    // to this explicit trigger; normal frames only overwrite bounded metadata.
    bool trace_flush(const char *tag) {
        char path[1024];
        os_mkdir(host_dump_dir());
        snprintf(path, sizeof path, "%s/trace-%llu-%u.jsonl", host_dump_dir(),
                 (unsigned long long)os_wall_time_us(), ++trace_flushes_);
        int fd = os_fd_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL);
        FILE *file = fd < 0 ? nullptr : static_cast<FILE *>(os_fdopen(fd, "wb"));
        if (!file) {
            if (fd >= 0)
                os_fd_close(fd);
            fprintf(stderr, "d3d9 trace: could not create %s\n", path);
            return false;
        }
        bool ok = trace_->write(file, tag, trace_context());
        long bytes = ftell(file);
        if (bytes > 0)
            trace_bytes_ += (uint64_t)bytes;
        ok = fclose(file) == 0 && bytes >= 0 && ok;
        fprintf(stderr, "d3d9 trace: %s %s\n", ok ? "wrote" : "write failed", path);
        return ok;
    }
    struct CaptureSave {
        std::unique_ptr<d9trace::Ring> ring;
        bool ok = false, complete = false;
    };
    static std::unique_ptr<d9trace::Ring> capture_ring() {
        return std::make_unique<d9trace::Ring>(d9trace::CaptureControl::draw_capacity,
                                               d9trace::CaptureControl::event_capacity,
                                               d9trace::CaptureControl::history);
    }
    void capture_poll() {
        if (capture_job_.valid() &&
            capture_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto saved = capture_job_.get();
            capture_spare_ = std::move(saved.ring);
            capture_control_.saved(saved.ok, saved.complete);
            fprintf(stderr, "capture: C%llu %s reason=%s\n",
                    (unsigned long long)capture_control_.id,
                    capture_state::name(capture_control_.state),
                    capture_state::reason(capture_control_.reason));
        }
    }
    void capture_save() {
        // Move preallocated storage, never copy draw arrays or wait for disk/GPU.
        // The worker owns all frozen values; it cannot touch live renderer state.
        auto frozen = std::move(trace_);
        trace_ = std::move(capture_spare_);
        trace_->reset(frozen->current_frame_number());
        trace_->retain = false;
        const auto control = capture_control_;
        const auto summaries = capture_summaries_;
        const auto summary_count = capture_summary_count_;
        const std::string dir = host_dump_dir();
        const auto epoch = capture_epoch_;
        capture_job_ = std::async(std::launch::async, [frozen = std::move(frozen), control,
                                                       summaries, summary_count, dir,
                                                       epoch]() mutable {
            CaptureSave result;
            bool records_complete = true;
            uint64_t first = 0, last = 0, first_ns = 0, last_ns = 0;
            uint32_t samples = 0;
            frozen->visit([&](const d9trace::Frame &f) {
                if (!first) {
                    first = f.number;
                    first_ns = f.monotonic_ns;
                }
                last = f.number;
                last_ns = f.monotonic_ns;
                ++samples;
                records_complete &= f.complete && !f.draw_overflow && !f.event_overflow;
            });
            char path[1024];
            snprintf(path, sizeof path, "%s/capture-%llu-%03llu.jsonl", dir.c_str(),
                     (unsigned long long)epoch, (unsigned long long)control.id);
            os_mkdir(dir.c_str());
            int fd = os_fd_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL);
            FILE *file = fd < 0 ? nullptr : static_cast<FILE *>(os_fdopen(fd, "wb"));
            if (!file && fd >= 0)
                os_fd_close(fd);
            bool ok = file != nullptr;
            uint64_t timeline_first = 0, timeline_last = 0, rows = 0;
            if (file) {
                setvbuf(file, nullptr, _IOFBF, 65536);
                ok = frozen->write(file, "sampled-capture", {});
                const size_t count = std::min<uint64_t>(summary_count, summaries.size());
                for (size_t i = 0; i < count; ++i) {
                    const auto &r = summaries[(summary_count - count + i) % summaries.size()];
                    if (r.frame < first || r.frame > last)
                        continue;
                    if (!timeline_first)
                        timeline_first = r.frame;
                    timeline_last = r.frame;
                    ++rows;
                    fprintf(
                        file,
                        "{\"kind\":\"frame_counts\",\"frame\":%llu,\"monotonic_ns\":%llu,\"wall_"
                        "us\":%llu,\"detailed\":%s,\"draws_attempted\":%llu,\"draws_retained\":%"
                        "llu,\"draws_dropped\":%llu,\"events_attempted\":%llu,\"events_retained\":%"
                        "llu,\"events_dropped\":%llu,\"context\":",
                        (unsigned long long)r.frame, (unsigned long long)r.ns,
                        (unsigned long long)r.wall_us, r.detailed ? "true" : "false",
                        (unsigned long long)r.draws_attempted, (unsigned long long)r.draws_retained,
                        (unsigned long long)r.draws_dropped, (unsigned long long)r.events_attempted,
                        (unsigned long long)r.events_retained,
                        (unsigned long long)r.events_dropped);
                    d9trace::context_json(file, r.context);
                    d9trace::accounting_json(file, r.accounting);
                    fprintf(file, "}\n");
                }
                result.complete = records_complete && samples > 1 &&
                                  first_ns < control.trigger_ns && last_ns >= control.deadline &&
                                  timeline_first == first && timeline_last == last &&
                                  rows == last - first + 1;
                fprintf(file,
                        "{\"kind\":\"capture_manifest\",\"capture_id\":%llu,\"status\":\"%s\","
                        "\"scope\":\"unfiltered full frames sampled at 2Hz; other frames counts "
                        "only; no "
                        "pixels\",\"history_samples\":16,\"post_ns\":6000000000,\"trigger_ns\":%"
                        "llu,\"trigger_render_frame\":%llu,\"first_ns\":%llu,\"last_ns\":%llu,"
                        "\"first_frame\":%llu,\"last_frame\":%llu,\"samples\":%u,\"timeline_rows\":"
                        "%llu,\"display_identity\":\"join frame_timings render_frame; presenter "
                        "frame_id differs; display_ack records actual acknowledgement\"}\n",
                        (unsigned long long)control.id, result.complete ? "COMPLETE" : "INCOMPLETE",
                        (unsigned long long)control.trigger_ns,
                        (unsigned long long)control.trigger_frame, (unsigned long long)first_ns,
                        (unsigned long long)last_ns, (unsigned long long)first,
                        (unsigned long long)last, samples, (unsigned long long)rows);
                ok = !ferror(file) && ok;
                ok = fclose(file) == 0 && ok;
            }
            result.ok = ok;
            fprintf(stderr, "capture: saved C%llu %s %s\n", (unsigned long long)control.id,
                    ok && result.complete ? "COMPLETE" : "INCOMPLETE", path);
            result.ring = std::move(frozen);
            return result;
        });
    }
    CaptureMarker capture_present(uint64_t frame, uint64_t ns, uint64_t wall,
                                  d9trace::Context context) {
        capture_poll();
        const auto &f = trace_->current();
        const bool detail = trace_->retain;
        auto &r = capture_summaries_[capture_summary_count_++ % capture_summaries_.size()];
        r = {f.accounting,
             frame,
             ns,
             wall,
             f.draws_attempted,
             f.events_attempted,
             f.draws,
             f.events,
             f.draw_overflow,
             f.event_overflow,
             context,
             detail};
        trace_->present(context, wall, ns);
        bool save = capture_control_.frame(ns, detail);
        const auto marker = capture_control_.marker(frame, ns, detail);
        if (save)
            capture_save();
        trace_->retain = capture_control_.next_detail;
        return marker;
    }
    void probe_next(const char *tag) override {
        if (capture_mode_) {
            capture_poll();
            const uint64_t now = os_monotonic_ns();
            bool accepted = capture_control_.request(now, trace_->current_frame_number());
            fprintf(stderr, "capture: request %s C%llu %s reason=%s mono_ns=%llu R=%llu\n",
                    accepted ? "accepted" : "REJECTED", (unsigned long long)capture_control_.id,
                    capture_state::name(capture_control_.state),
                    capture_state::reason(capture_control_.reason), (unsigned long long)now,
                    (unsigned long long)trace_->current_frame_number());
            return;
        }
        if (trace_) {
            // One explicitly armed, bounded forward capture. The ordinary ten-frame
            // ring cannot retain a fleeting fault until a human reacts to it.
            if (const char *burst = recomp_env("D3D9_TRACE_BURST")) {
                char *end = nullptr;
                unsigned long count = strtoul(burst, &end, 10);
                if (!*burst || *end || count < 1 || count > 240 || burst_used_) {
                    fprintf(stderr, "d3d9 burst: rejected (invalid count or already used)\n");
                    return;
                }
                burst_used_ = true;
                burst_start_ = presents_ + 2; // omit the partially drawn frame
                burst_left_ = (uint32_t)count;
                burst_trace_start_ = trace_bytes_;
                fprintf(stderr, "d3d9 burst: armed %lu complete frames; no pre-trigger pixels\n",
                        count);
            }
            if (!trace_flush(tag))
                burst_left_ = 0;
            return;
        }
        probe_frame_ = presents_ + 2; // the frame after the one in progress, whole
        probe_tag_ = tag ? tag : "";
        fprintf(stderr, "probe %s: frame %llu\n", probe_tag_.c_str(),
                (unsigned long long)probe_frame_);
    }
    void probe_dump() {
        settle(serial_);
        for (auto &kv : textures_) {
            Tex &t = kv.second;
            if (!t.texture || t.info.pixel != MTLPixelFormatBGRA8Unorm ||
                t.desc.kind != HOST_D9_TEX_2D)
                continue;
            uint32_t w = (uint32_t)t.texture.width, h = (uint32_t)t.texture.height;
            std::vector<uint8_t> bgra((size_t)w * h * 4), rgb((size_t)w * h * 3);
            [t.texture getBytes:bgra.data()
                    bytesPerRow:w * 4
                     fromRegion:MTLRegionMake2D(0, 0, w, h)
                    mipmapLevel:0];
            size_t lit = 0;
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
                rgb[i * 3] = bgra[i * 4 + 2];
                rgb[i * 3 + 1] = bgra[i * 4 + 1];
                rgb[i * 3 + 2] = bgra[i * 4];
                lit += (bgra[i * 4] | bgra[i * 4 + 1] | bgra[i * 4 + 2]) > 8;
            }
            char path[1024];
            snprintf(path, sizeof path, "%s/probe%s%s_%u_%ux%u_%s.ppm", host_dump_dir(),
                     probe_tag_.empty() ? "" : "_", probe_tag_.c_str(), kv.first, w, h,
                     (t.desc.usage & HOST_D9_USAGE_RENDERTARGET) ? "rt" : "tex");
            host_write_ppm(path, rgb.data(), (int)w, (int)h);
            fprintf(stderr, "probe texture %u %ux%u fmt %08x usage %u: %zu of %zu lit\n", kv.first,
                    w, h, t.desc.format, t.desc.usage, lit, (size_t)w * h);
        }
    }

    void skip(const char *why) {
        ++skips_[why];
    }
    void report() {
        if (!recomp_env("D3D9_STATS"))
            return;
        double now = CACurrentMediaTime();
        double fps = report_time_ > 0 ? (presents_ - report_frames_) / (now - report_time_) : 0.0;
        double dpf = presents_ > report_frames_
                         ? double(draws_ - report_draws_) / double(presents_ - report_frames_)
                         : 0.0;
        const uint64_t frames = presents_ > report_frames_ ? presents_ - report_frames_ : 1;
        const uint64_t gpu_us = gpu_us_.exchange(0);
        // The thread presenting is the game's: its CPU time is the frame's cost.
        const uint64_t cpu_ns = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        const double cpu_ms = report_cpu_ns_ ? (cpu_ns - report_cpu_ns_) / 1e6 / frames : 0.0;
        report_cpu_ns_ = cpu_ns;
        // Instructions are what a frame costs whichever core ran it.
        double minstr = 0.0;
#if TARGET_OS_OSX
        rusage_info_v4 ru{};
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ru) == 0) {
            if (report_instructions_)
                minstr = (ru.ri_instructions - report_instructions_) / 1e6 / frames;
            report_instructions_ = ru.ri_instructions;
        }
#endif
        const double waited = gpu_wait_s_;
        gpu_wait_s_ = 0;
        report_time_ = now;
        report_frames_ = presents_;
        report_draws_ = draws_;
        fprintf(stderr,
                "d3d9 metal: frame %llu: %.1f fps, %.0f draws/frame, %llu draw calls, %llu "
                "encoded, %zu textures, %zu pipelines, %.1fM instructions/frame, game thread %.2f "
                "ms/frame, gpu %.2f ms/frame, waited %.2f ms/frame",
                (unsigned long long)presents_, fps, dpf, (unsigned long long)stat_calls_,
                (unsigned long long)draws_, textures_.size(), pipelines_.size(), minstr, cpu_ms,
                gpu_us / 1000.0 / frames, waited * 1000.0 / frames);
        for (auto &s : skips_)
            fprintf(stderr, ", %s %llu", s.first.c_str(), (unsigned long long)s.second);
        fprintf(stderr, "\n");
    }

    void clear(const HostD9Target &target, const int32_t vp[4], uint32_t count,
               const int32_t *rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        if (g_d9_diag.load(std::memory_order_relaxed) && diag_hold(target, true))
            return;   // the iOS reflection test (g_d9_diag)
        if (trace_)
            if (auto *e = trace_->event("clear_request", trace_context(),
                                        trace_texture(target.color[0].id))) {
                e->values[0] = target.depth.id;
                e->values[1] = flags;
                e->values[2] = color;
                memcpy(&e->values[3], &z, 4);
                e->values[4] = stencil;
                e->values[5] = count;
                memcpy(&e->values[6], vp, 16);
            }
        if (probing())
            fprintf(
                stderr,
                "probe clear: rt %u depth %u flags %x colour %08x z %g rects %u vp %d,%d %dx%d\n",
                target.color[0].id, target.depth.id, flags, color, z, count, vp[0], vp[1], vp[2],
                vp[3]);
        id<MTLTexture> c0 = texture_for(target.color[0]);
        id<MTLTexture> dz = texture_for(target.depth);
        if (!c0)
            return;
        const Tex &ct = textures_[target.color[0].id];
        int32_t lw = (int32_t)std::max<uint32_t>(ct.desc.width >> target.color[0].level, 1);
        int32_t lh = (int32_t)std::max<uint32_t>(ct.desc.height >> target.color[0].level, 1);
        bool whole = count == 0 && vp[0] <= 0 && vp[1] <= 0 && vp[2] >= lw && vp[3] >= lh;
        bool want_color = flags & 1, want_depth = (flags & 2) && dz,
             want_stencil = (flags & 4) && dz;
        float rgba[4] = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                         (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f};
        if (upscale_factor_ > 0.0f)
            upscale_note_clear(target, whole, want_color, rgba, want_depth, z, want_stencil, stencil);
        if (whole) {
            // Folded into the pass's load actions: a new pass on this target.
            end_pass();
            begin_pass(target, want_color, rgba, want_depth, z, want_stencil, stencil);
            return;
        }
        if (!ensure_pass(target))
            return;
        // A partial clear is a quad per rectangle, written unconditionally.
        MTLPixelFormat cf[4];
        for (int i = 0; i < 4; ++i)
            cf[i] = pass_color_[i] ? pass_color_[i].pixelFormat : MTLPixelFormatInvalid;
        MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        uint64_t key = mix(mix(0xc1ea5ull, (uint64_t)want_color), df);
        for (int i = 0; i < 4; ++i)
            key = mix(key, cf[i]);
        id<MTLRenderPipelineState> pso = utility_pipeline(key, @"clear_fs", cf, df, want_color);
        if (!pso)
            return;
        es_ = EncoderState{}; // what follows is not a draw's state
        [enc_ setRenderPipelineState:pso];
        uint64_t dkey = 0xc1ea5000ull | (uint64_t)want_depth | (uint64_t)want_stencil << 1;
        id<MTLDepthStencilState> ds = clear_depth_state(want_depth, want_stencil, dkey);
        [enc_ setDepthStencilState:ds];
        [enc_ setStencilReferenceValue:stencil & 0xff];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
        [enc_ setDepthBias:0 slopeScale:0 clamp:0];
        int32_t tw = lw, th = lh; // quads are placed in guest pixels; the viewport scales them
        [enc_ setViewport:(MTLViewport){
                              0, 0,
                              (double)std::max<NSUInteger>(c0.width >> target.color[0].level, 1),
                              (double)std::max<NSUInteger>(c0.height >> target.color[0].level, 1),
                              0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){
                                 0, 0, std::max<NSUInteger>(c0.width >> target.color[0].level, 1),
                                 std::max<NSUInteger>(c0.height >> target.color[0].level, 1)}];
        std::vector<int32_t> list;
        if (count && rects)
            list.assign(rects, rects + 4 * count);
        else
            list = {vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3]};
        for (size_t i = 0; i + 4 <= list.size(); i += 4) {
            int32_t x0 = std::max(list[i], std::max(vp[0], 0));
            int32_t y0 = std::max(list[i + 1], std::max(vp[1], 0));
            int32_t x1 = std::min(list[i + 2], std::min(vp[0] + vp[2], tw));
            int32_t y1 = std::min(list[i + 3], std::min(vp[1] + vp[3], th));
            if (x1 <= x0 || y1 <= y0)
                continue;
            QuadIn q{};
            q.rect[0] = 2.0f * x0 / tw - 1.0f;
            q.rect[1] = 1.0f - 2.0f * y0 / th;
            q.rect[2] = 2.0f * x1 / tw - 1.0f;
            q.rect[3] = 1.0f - 2.0f * y1 / th;
            q.z = z;
            memcpy(q.color, rgba, sizeof rgba);
            [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
            [enc_ setFragmentBytes:&q length:sizeof q atIndex:0];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            if (trace_) {
                if (want_color)
                    textures_[target.color[0].id].written = serial_;
                if (pass_depth_ && !pass_fitted_depth_ && (want_depth || want_stencil))
                    textures_[target.depth.id].written = serial_;
            }
        }
    }

    void stretch(HostD9Surface src, const int32_t sr[4], HostD9Surface dst, const int32_t dr[4],
                 uint32_t filter) override {
        if (trace_)
            if (auto *e =
                    trace_->event("stretch_request", trace_context(), trace_texture(dst.id))) {
                e->values[0] = src.id;
                e->values[1] = src.face;
                e->values[2] = src.level;
                e->values[3] = dst.face;
                e->values[4] = dst.level;
                e->values[5] = filter;
                memcpy(&e->values[6], sr, 16);
                memcpy(&e->values[10], dr, 16);
            }
        if (probing())
            fprintf(stderr,
                    "probe stretch: %u/%u/%u (%d,%d,%d,%d) -> %u/%u/%u (%d,%d,%d,%d) filter %u\n",
                    src.id, src.face, src.level, sr[0], sr[1], sr[2], sr[3], dst.id, dst.face,
                    dst.level, dr[0], dr[1], dr[2], dr[3], filter);
        id<MTLTexture> s = view_for(src);
        id<MTLTexture> t = texture_for(dst);
        if (!s || !t)
            return;
        if (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            stretch_depth(src, dst, t);
            return;
        }
        end_pass();
        uint32_t tw = std::max<uint32_t>((uint32_t)t.width >> dst.level, 1);
        uint32_t th = std::max<uint32_t>((uint32_t)t.height >> dst.level, 1);
        const Tex &st_ = textures_[src.id];
        const Tex &dt_ = textures_[dst.id];
        // Rectangles are in each surface's guest pixels.
        const float ssx =
            (float)s.width /
            std::max<uint32_t>(1, st_.desc.kind == HOST_D9_TEX_CUBE
                                      ? st_.desc.width >> src.level
                                      : std::max<uint32_t>(st_.desc.width >> src.level, 1));
        const float ssy =
            (float)s.height /
            std::max<uint32_t>(1, st_.desc.kind == HOST_D9_TEX_CUBE
                                      ? st_.desc.width >> src.level
                                      : std::max<uint32_t>(st_.desc.height >> src.level, 1));
        const float dsx = (float)tw / std::max<uint32_t>(dt_.desc.width >> dst.level, 1);
        const float dsy = (float)th / std::max<uint32_t>(dt_.desc.height >> dst.level, 1);
        uint32_t sw = (uint32_t)s.width, sh = (uint32_t)s.height;
        HostD9Target target{};
        target.color[0] = dst;
        begin_pass(target, false, nullptr, false, 0, false, 0);
        MTLPixelFormat cf[4] = {t.pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso = utility_pipeline(mix(0xb117ull, t.pixelFormat), @"copy_fs",
                                                          cf, MTLPixelFormatInvalid, true);
        if (!pso)
            return;
        es_ = EncoderState{}; // what follows is not a draw's state
        [enc_ setRenderPipelineState:pso];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setViewport:(MTLViewport){0, 0, (double)tw, (double)th, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, tw, th}];
        QuadIn q{};
        q.rect[0] = 2.0f * dr[0] * dsx / tw - 1.0f;
        q.rect[1] = 1.0f - 2.0f * dr[1] * dsy / th;
        q.rect[2] = 2.0f * dr[2] * dsx / tw - 1.0f;
        q.rect[3] = 1.0f - 2.0f * dr[3] * dsy / th;
        q.uv[0] = sr[0] * ssx / sw;
        q.uv[1] = sr[1] * ssy / sh;
        q.uv[2] = sr[2] * ssx / sw;
        q.uv[3] = sr[3] * ssy / sh;
        [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
        textures_[src.id].used = serial_;
        [enc_ setFragmentTexture:s atIndex:0];
        uint32_t st[14] = {};
        st[1] = st[2] = st[3] = 3; // clamp
        st[5] = st[6] = filter >= 2 ? 2 : 1;
        [enc_ setFragmentSamplerState:sampler(st) atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (trace_)
            textures_[dst.id].written = serial_;
        end_pass();
    }

    // Test121: StretchRect from a depth surface into a colour target (R32F for the core mod's
    // motion blur and depth of field): the whole of the depth its last pass drew, stretched to
    // the whole target, nearest texel; a multisampled depth buffer gives its smallest sample. Rectangles are
    // ignored (the one use copies whole surfaces). Not a D3D9 behaviour; the guest only gets it
    // by asking for it, and a colour source keeps the ordinary path.
    void stretch_depth(HostD9Surface src, HostD9Surface dst, id<MTLTexture> t) {
        Tex *st = tex_ptr(src.id);
        {   // Ledger L1: before Test121 this request was dropped; say when anything first asks for it
            static unsigned long long n = 0;
            if (n++ == 0)
                fprintf(stderr, "d3d9 metal: depth StretchRect into colour target %u (%ux%u px; first request; Test121)\n",
                        dst.id, (unsigned)t.width, (unsigned)t.height);
        }
        if (!st || src.level || t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
            return;
        id<MTLTexture> d = st->drawn_depth ? st->drawn_depth : st->texture;
        if (!d || !(d.usage & MTLTextureUsageShaderRead))
            return;
        const bool ms = d.textureType == MTLTextureType2DMultisample;
        end_pass();
        uint32_t tw = std::max<uint32_t>((uint32_t)t.width >> dst.level, 1);
        uint32_t th = std::max<uint32_t>((uint32_t)t.height >> dst.level, 1);
        HostD9Target target{};
        target.color[0] = dst;
        begin_pass(target, false, nullptr, false, 0, false, 0);
        if (!enc_)
            return;
        MTLPixelFormat cf[4] = {t.pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso =
            utility_pipeline(mix(ms ? 0xd3c1ull : 0xd3c0ull, t.pixelFormat),
                             ms ? @"depth_copy_ms_fs" : @"depth_copy_fs", cf, MTLPixelFormatInvalid, true);
        if (!pso) {
            end_pass();
            return;
        }
        es_ = EncoderState{};
        [enc_ setRenderPipelineState:pso];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setViewport:(MTLViewport){0, 0, (double)tw, (double)th, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, tw, th}];
        QuadIn q{};
        q.rect[0] = -1.0f;
        q.rect[1] = 1.0f;
        q.rect[2] = 1.0f;
        q.rect[3] = -1.0f;
        q.uv[0] = 0.0f;
        q.uv[1] = 0.0f;
        q.uv[2] = 1.0f;
        q.uv[3] = 1.0f;
        [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
        st->used = serial_;
        [enc_ setFragmentTexture:d atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (trace_)
            textures_[dst.id].written = serial_;
        end_pass();
    }

    // Shader work done synchronously inside a frame (MSL compile, pipeline creation). A cold
    // system Metal cache (the first run of a new bundle) makes this long; the report says where a
    // no-new-frame gap came from. Bounded: frames holding 50 ms or more, at most 200 lines.
    void note_shader_work(uint64_t ns, bool library) {
        shader_frame_ns_ += ns;
        shader_total_ns_ += ns;
        (library ? shader_frame_lib_ns_ : shader_frame_pso_ns_) += ns;
        if (ns > shader_frame_max_ns_) shader_frame_max_ns_ = ns;
        ++(library ? shader_frame_libs_ : shader_frame_psos_);
    }
    void report_shader_work() {
        if (shader_frame_ns_ >= 50000000ull && shader_reports_ < 200) {
            ++shader_reports_;
            fprintf(stderr,
                    "d3d9 metal: shader compilation held this frame %.1f ms (%u libraries %.1f ms, %u pipelines %.1f ms, "
                    "longest %.1f ms; %.0f ms in all so far; %u pre-compiled used, %u waits on the warm-up, %u pre-built pipelines used; at %.3f s monotonic)\n",
                    shader_frame_ns_ / 1e6, shader_frame_libs_, shader_frame_lib_ns_ / 1e6, shader_frame_psos_,
                    shader_frame_pso_ns_ / 1e6, shader_frame_max_ns_ / 1e6,
                    shader_total_ns_ / 1e6, warm_ ? warm_->hits() : 0u, warm_ ? warm_->coalesced() : 0u,
                    warm_ ? warm_->pipeline_hits() : 0u,
                    os_monotonic_ns() / 1e9);
        }
        shader_frame_ns_ = shader_frame_max_ns_ = shader_frame_lib_ns_ = shader_frame_pso_ns_ = 0;
        shader_frame_libs_ = shader_frame_psos_ = 0;
        // Run 15 (hitch): warm-cache totals, at most every 20 s and only when they changed (seed hit rate =
        // pre-compiled used / (pre-compiled used + compiled in the draw + compiled in the background)).
        if (warm_) {
            const uint64_t now = os_monotonic_ns();
            const uint32_t used = warm_->hits(), bg = warm_->async_started();
            if (now - warm_report_ns_ >= 20000000000ull &&
                (used != warm_report_used_ || draw_compiles_ != warm_report_draw_ || bg != warm_report_bg_)) {
                warm_report_ns_ = now;
                warm_report_used_ = used;
                warm_report_draw_ = draw_compiles_;
                warm_report_bg_ = bg;
                fprintf(stderr, "d3d9 metal: shader warm totals: %u pre-compiled used, %u compiled in the draw (%u over 50 ms), "
                                "%u compiled in the background for deferred draws; at %.3f s monotonic\n",
                        used, draw_compiles_, draw_compiles_slow_, bg, now / 1e9);
            }
        }
    }
    void present(uint32_t backbuffer, uint32_t w, uint32_t h) override {
        const uint32_t guest_w = w, guest_h = h;
        diag_frame();
        if (fnd_on_)
            fnd_present(); // Remaster run 14 foundation: before the upscale's (the motion pass runs at the scene size)
        if (upscale_factor_ > 0.0f)
            upscale_present(); // Remaster run 13: closes a scene left open; the periodic report
        if (rtex_.on())
            rtex_.frame();
        rcache_.present();
        soft_done_ = false; // Remaster S3 trial: one capture per frame at most
        if (smoke_draws_ && ++smoke_frames_ % 600 == 0)   // run 14 lit smoke: once per 600 frames while in use
            fprintf(stderr, "[remaster] lit smoke: frame %llu lit draws %llu (tyre %llu flare %llu steam stack %llu soot %llu "
                            "fog %llu steam %llu green %llu) no camera %llu fallbacks %llu\n",
                    (unsigned long long)smoke_frames_, (unsigned long long)smoke_draws_,
                    (unsigned long long)smoke_by_kind_[1], (unsigned long long)smoke_by_kind_[2],
                    (unsigned long long)smoke_by_kind_[3], (unsigned long long)smoke_by_kind_[4],
                    (unsigned long long)smoke_by_kind_[5], (unsigned long long)smoke_by_kind_[6],
                    (unsigned long long)smoke_by_kind_[7], (unsigned long long)smoke_noclip_,
                    (unsigned long long)smoke_fallback_);
        soft_ok_ = false;
        if (atm_on_)
            atm_present(); // Remaster run 14 B1 height fog
        if (softfx_on_ && ++soft_frames_ % 600 == 0)
            fprintf(stderr, "[remaster] softfx: frame %llu depth captures %llu faded draws %llu (plain fog/steam %llu)\n",
                    (unsigned long long)soft_frames_, (unsigned long long)soft_breaks_, (unsigned long long)soft_faded_,
                    (unsigned long long)soft_kind_draws_);
        if (++pool_frames_ % 600 == 0 && (pool_draws_ || pool_minimal_ || pool_late_lost_ || pool_probe_[0] ||
                                          pool_probe_[1] || pool_probe_[2] || pool_probe_[3] || pool_probe_[4])) {
            // Remaster run 10/11: cumulative for the whole run (never reset)
            fprintf(stderr, "[remaster] pool fusion totals: frame %llu fused draws %llu (minimal variant %llu), light "
                            "lost %llu, probes ready %llu blocked %llu (refused %llu, vs %llu, ps %llu, pipeline %llu)\n",
                    (unsigned long long)pool_frames_, (unsigned long long)pool_draws_,
                    (unsigned long long)pool_minimal_, (unsigned long long)pool_late_lost_,
                    (unsigned long long)pool_probe_[0],
                    (unsigned long long)(pool_probe_[1] + pool_probe_[2] + pool_probe_[3] + pool_probe_[4]),
                    (unsigned long long)pool_probe_[1], (unsigned long long)pool_probe_[2],
                    (unsigned long long)pool_probe_[3], (unsigned long long)pool_probe_[4]);
        }
        if (shadowpcf_on_ && ++shadowpcf_frames_ % 600 == 0)
            fprintf(stderr, "[remaster] shadowpcf: frame %llu filtered shadow draws %llu\n",
                    (unsigned long long)shadowpcf_frames_, (unsigned long long)shadowpcf_draws_);
        grass_.frame();   // run 13 grass shells (grass.h)
        if (water_on_ && ++water_frames_ % 600 == 0) {   // run 13 natural water
            size_t n = 0;
            for (const auto &kv : water_ps_)
                n += kv.second;
            fprintf(stderr, "[remaster] water: frame %llu water draws %llu (water programs %zu of %zu checked) fallbacks %llu\n",
                    (unsigned long long)water_frames_, (unsigned long long)water_draws_, n, water_ps_.size(),
                    (unsigned long long)water_fallback_);
            fprintf(stderr, "[remaster] water: frame %llu stayed as today: other variant %llu, not main view %llu, "
                            "program/vs not eligible %llu\n",
                    (unsigned long long)water_frames_, (unsigned long long)water_skip_[0],
                    (unsigned long long)water_skip_[1], (unsigned long long)water_skip_[2]);
        }
        if (parallax_on_ && ++parallax_frames_ % 600 == 0)
            fprintf(stderr, "[remaster] parallax: frame %llu parallax draws %llu no-alpha draws %llu fallbacks %llu\n",
                    (unsigned long long)parallax_frames_, (unsigned long long)parallax_draws_,
                    (unsigned long long)parallax_noalpha_, (unsigned long long)parallax_fallback_);
        if (glass_on_ && ++glass_frames_ % 600 == 0) {
            size_t n = 0;
            for (const auto &kv : glass_ps_)
                n += kv.second;
            fprintf(stderr, "[remaster] glass: frame %llu glass draws %llu (glass programs %zu of %zu checked) fallbacks %llu mode %d\n",
                    (unsigned long long)glass_frames_, (unsigned long long)glass_draws_, n, glass_ps_.size(),
                    (unsigned long long)glass_fallback_, (int)glass_mode_);
        }
        if (sky_on_ && ++sky_frames_ % 600 == 0) {
            size_t np = 0, nv = 0;
            for (const auto &kv : sky_ps_)
                np += kv.second;
            for (const auto &kv : sky_vs_)
                nv += kv.second;
            fprintf(stderr, "[remaster] sky: frame %llu sky draws %llu fallbacks %llu (in-scatter draws %llu, skybox draws %llu; "
                            "sky programs ps %zu of %zu, vs %zu of %zu)\n",
                    (unsigned long long)sky_frames_, (unsigned long long)sky_draws_, (unsigned long long)sky_fallback_,
                    (unsigned long long)sky_vs_draws_, (unsigned long long)sky_all_, np, sky_ps_.size(), nv, sky_vs_.size());
        }
        if (carenv_on_ && ++carenv_frames_ % 600 == 0) {
            size_t n = 0;
            for (const auto &kv : carenv_vs_)
                n += kv.second;
            fprintf(stderr, "[remaster] carenv: frame %llu car draws %llu (car programs %zu of %zu checked) fallbacks %llu mode %d\n",
                    (unsigned long long)carenv_frames_, (unsigned long long)carenv_draws_, n, carenv_vs_.size(),
                    (unsigned long long)carenv_fallback_, (int)carenv_mode_);
        }
        if (roadsheen_on_ && ++roadsheen_frames_ % 600 == 0) {
            size_t n = 0;
            for (const auto &kv : roadsheen_ps_)
                n += kv.second;
            fprintf(stderr, "[remaster] roadsheen: frame %llu sheen draws %llu (sheen programs %zu of %zu checked) "
                            "fallbacks %llu\n",
                    (unsigned long long)roadsheen_frames_, (unsigned long long)roadsheen_draws_, n, roadsheen_ps_.size(),
                    (unsigned long long)roadsheen_fallback_);
        }
        if (fef_on_)   // run 13 front-end floor shadows: frame count and the periodic report
            fef_present();
        if (wetroad_on_ && ++wetroad_frames_ % 600 == 0) {
            wetroad_log_ = true;
            size_t n = 0;
            for (const auto &kv : wetroad_ps_)
                n += kv.second;
            fprintf(stderr, "[remaster] wetroad: frame %llu road draws %llu (road shaders %zu of %zu checked)%s\n",
                    (unsigned long long)wetroad_frames_, (unsigned long long)wetroad_draws_, n, wetroad_ps_.size(),
                    wetroad_mode_ == 2 ? " mode 2" : "");
        }
        if (pbrprobe_on_ && ++pbr_frames_ % 600 == 0)
            pbr_report();
        if (pbw_active_ && ++pbw_frames_ % 600 == 0)
            pbw_report();
        if (a2c_on_ && ++a2c_frames_ % 600 == 0)
            fprintf(stderr, "[remaster] a2c: frame %llu alpha-to-coverage draws %llu\n", (unsigned long long)a2c_frames_,
                    (unsigned long long)a2c_draws_);
        alloc_watch();
        report_shader_work();
        if (hdr_split_done_)   // run 15 (cost): the add-back as one more draw in the open HUD pass, not a pass of its own
            hdr_addback_open(backbuffer);
        end_pass();
        auto it = textures_.find(backbuffer);
        if (it == textures_.end() || !it->second.texture) {
            flush();
            if (trace_) {
                if (burst_left_) {
                    fprintf(stderr, "d3d9 burst: aborted: missing presented target\n");
                    burst_left_ = 0;
                }
                trace_->event("present_missing", trace_context(), {backbuffer});
                if (capture_mode_)
                    capture_present(trace_->current_frame_number(), os_monotonic_ns(),
                                    os_wall_time_us(), trace_context());
                else
                    trace_->present(trace_context(), os_wall_time_us());
            }
            return;
        }
        Tex &t = it->second;
        base_rows_ = t.desc.height;
        w = (uint32_t)t.texture.width;
        h = (uint32_t)t.texture.height;
        if (hdr_split_done_)   // PLAN_R2 item 10: the scene's excess back where the scene still shows
            hdr_addback(t);
        keep_presented(t, w, h);
        flush();
        uint64_t trace_frame = trace_ ? trace_->current_frame_number() : 0;
        uint64_t present_wall_us = os_wall_time_us();
        CaptureMarker capture_marker;
        if (trace_) {
            if (auto *e = trace_->event("present", trace_context(), trace_texture(backbuffer))) {
                e->values[0] = guest_w;
                e->values[1] = guest_h;
                e->values[2] = w;
                e->values[3] = h;
            }
            if (capture_mode_)
                capture_marker = capture_present(trace_frame, os_monotonic_ns(), present_wall_us,
                                                 trace_context());
            else
                trace_->present(trace_context(), present_wall_us);
        }
        if (burst_left_ && presents_ + 1 >= burst_start_) {
            // Reuse the existing exact backbuffer dump and metadata ring. I/O and
            // GPU waits perturb timing; this is evidence capture, not benchmarking.
            const uint64_t bytes = (uint64_t)w * h * 3;
            if (t.info.pixel != MTLPixelFormatBGRA8Unorm || !w || !h ||
                (uint64_t)w * h > 1920u * 1080u ||
                burst_pixel_bytes_ + trace_bytes_ - burst_trace_start_ + bytes > (2ull << 30)) {
                fprintf(stderr, "d3d9 burst: aborted: format/resolution/2GiB budget limit\n");
                trace_flush("burst-limit");
                burst_left_ = 0;
            } else {
                dump(t, w, h);
                burst_pixel_bytes_ += bytes;
                --burst_left_;
                ++burst_captured_;
                fprintf(stderr,
                        "d3d9 burst: present=%llu trace_frame=%llu wall_us=%llu remaining=%u\n",
                        (unsigned long long)(presents_ + 1), (unsigned long long)trace_frame,
                        (unsigned long long)present_wall_us, burst_left_);
                // Flush every ten completed frames, before the ring can overwrite
                // any burst frame. The final partial group is flushed as well.
                if (burst_captured_ % 10 == 0 || !burst_left_)
                    if (!trace_flush("burst"))
                        burst_left_ = 0;
                if (!burst_left_)
                    fprintf(stderr, "d3d9 burst: finished %u frames\n", burst_captured_);
            }
        }
        if (probing())
            probe_dump();
        if (diag_cap_pending_ || diag_cap_list_)
            diag_capture(t, w, h);   // the iOS reflection test's frame capture (g_d9_diag 8)
        uint32_t every = host_dump_every();
        if (every && ((presents_ + 1) % every) == 0) {
            dump(t, w, h);
            if (pbw_active_)   // run 12 pilot: which dumps show it (an in-run A/B pairs on and off frames)
                fprintf(stderr, "[remaster] pbrworld: dump present_%05u pilot draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(pbw_draws_ - pbw_draws_at_present_));
            if (glass_on_)
                fprintf(stderr, "[remaster] glass: dump present_%05u glass draws %llu (ps_1_x %llu)\n",
                        (unsigned)(presents_ + 1), (unsigned long long)(glass_draws_ - glass_draws_at_present_),
                        (unsigned long long)(glass1_draws_ - glass1_draws_at_present_));
            if (sky_on_)   // run 13 sky (ours): in-run A/B pairs (A/B switch sky)
                fprintf(stderr, "[remaster] sky: dump present_%05u sky draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(sky_draws_ - sky_draws_at_present_));
            if (carenv_on_)
                fprintf(stderr, "[remaster] carenv: dump present_%05u car draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(carenv_draws_ - carenv_draws_at_present_));
            if (roadsheen_on_)   // run 13: the same for the road sheen (in-run A/B pairs; pbw_report.py --switch)
                fprintf(stderr, "[remaster] roadsheen: dump present_%05u sheen draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(roadsheen_draws_ - roadsheen_draws_at_present_));
            if (water_on_)   // run 13 natural water: the same (in-run A/B pairs)
                fprintf(stderr, "[remaster] water: dump present_%05u water draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(water_draws_ - water_draws_at_present_));
            if (parallax_on_)   // run 14: the same for wall parallax (ALL14 had no pairs for it)
                fprintf(stderr, "[remaster] parallax: dump present_%05u parallax draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(parallax_draws_ - parallax_draws_at_present_));
            if (smoke_seen_)   // run 14 lit smoke: the same (in-run A/B pairs, pbw_report.py --switch litsmk)
                fprintf(stderr, "[remaster] litsmk: dump present_%05u lit draws %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(smoke_draws_ - smoke_draws_at_present_));
            if (atm_on_)   // run 14 B1 height fog: the same (in-run A/B pairs, pbw_report.py --switch heightfog)
                fprintf(stderr, "[remaster] heightfog: dump present_%05u fog passes %llu\n", (unsigned)(presents_ + 1),
                        (unsigned long long)(atm_passes_ - atm_passes_at_present_));
            grass_.dump((unsigned)(presents_ + 1));   // run 13 grass shells: the same (in-run A/B pairs)
        }
        if (t.promoted && hdr_dump_every_ && (presents_ + 1) % hdr_dump_every_ == 0 && hdr_dump_in_window())   // item 10: RECOMP_DUMP_FLOAT_FRAMES
            hdr_dump_float(t, w, h);
        pbw_draws_at_present_ = pbw_draws_;
        roadsheen_draws_at_present_ = roadsheen_draws_;
        water_draws_at_present_ = water_draws_;
        parallax_draws_at_present_ = parallax_draws_;
        smoke_draws_at_present_ = smoke_draws_;
        atm_passes_at_present_ = atm_passes_;
        grass_.mark();
        carenv_draws_at_present_ = carenv_draws_;
        glass_draws_at_present_ = glass_draws_;
        glass1_draws_at_present_ = glass1_draws_;
        sky_draws_at_present_ = sky_draws_;
        if (!t.imported)
            t.imported = device_->import_texture(t.texture);
        g_host_hdr_scene_mask.store(hdr_mask_frame_, std::memory_order_relaxed); // Test86
        hdr_mask_frame_ = false;
        if (t.info.pixel == MTLPixelFormatBGRA8Unorm || t.promoted) {   // item 10: the float frame too (mode 3)
            gpu::CommandBuffer cb = device_->begin();
            D9M4 *m4 = nil;
            if (m4_)
                if (@available(macOS 26.0, iOS 26.0, *))
                    m4 = (D9M4 *)m4_;
            if (m4 && last_)   // v4 Metal 4: the presenter's copy waits for this frame (it shared our queue before)
                device_->encode_wait(cb, m4.done, last_.value);
            if (host_present_stage_texture(t.imported, (int)w, (int)h, (int)guest_w, (int)guest_h,
                                           cb, capture_marker.enabled ? &capture_marker : nullptr))
                host_present_track_command(cb);
            if (m4) {   // ... and the next frame's drawing into the back buffer waits for the copy
                const uint64_t v = [m4 nextPresentedValue];
                device_->encode_signal(cb, m4.presented, v);
                [m4 waitPresented:v];
            }
            device_->commit(cb);
        }
        ++presents_;
        if (presents_ % 600 == 0 && presents_)
            perf_report();
        if (m4_ && presents_ % 600 == 0 && presents_)   // v4 Metal 4 pipelines: compiled bases vs cheap specializations
            fprintf(stderr, "[gpu] Metal 4 pipelines: %llu compiled (%.0f ms), %llu specialized (%.0f ms), %llu failed\n",
                    (unsigned long long)m4_base_n_, m4_base_ns_ / 1e6, (unsigned long long)m4_spec_n_, m4_spec_ns_ / 1e6,
                    (unsigned long long)m4_fail_n_);
        if (presents_ % 600 == 0)   // v2.4: GPU waits left vs uploads that no longer wait (staged_upload)
            fprintf(stderr, "[gpu] frame %llu: %llu GPU waits, %llu staged texture uploads so far\n",
                    (unsigned long long)presents_, (unsigned long long)settle_waits_,
                    (unsigned long long)staged_uploads_);
        g_host_game_gpu_frames.fetch_add(1, std::memory_order_relaxed);
        hdr_latch(backbuffer);   // PLAN_R2 item 10: the next frame's format
        if (presents_ % 300 == 1)
            report();
        follow_drawable();
        last_presented_ = backbuffer;
        last_w_ = w;
        last_h_ = h;
    }

    // ---- PLAN_R2 item 10 real HDR (review/run14/hdr-design.md s2) ------------------------------------------------
    // At Present the presenter's wish (g_host_hdr_float) is latched: the back buffer (and the mirror) get an
    // RGBA16Float backing, or their own back. host_d9_hdr_float tells the guest (GetRenderState 249). Off: nothing
    // here does anything (no promotion; hdr_cls_ stays 0, so every shader source and pipeline is as before).
    void hdr_latch(uint32_t bb) {
        // Run 14 (COMB14 K8: no float frame at all): the presenter's poll needs a swapchain, which the harness's
        // pop_smoke runs do not have, so the TEST switch RECOMP_TEST_HDR_FLOAT=1 is also honoured here.
        static const bool test_force = [] {
            const char *e = recomp_env("TEST_HDR_FLOAT");
            return e && std::atoi(e) != 0;
        }();
        hdr_split_tried_ = hdr_split_done_ = false;
        const bool want = test_force || g_host_hdr_float.load(std::memory_order_relaxed);
        if (want != hdr_float_) {
            hdr_float_ = want;
            fprintf(stderr, "[remaster] hdr: float frame %s at present %llu\n", want ? "on" : "off",
                    (unsigned long long)presents_);
        }
        if (want) {
            hdr_promote(bb, true);
            if (hdr_mirror_id_)
                hdr_promote(hdr_mirror_id_, true);
        } else if (hdr_live_) {
            for (auto &kv : textures_)
                if (kv.second.promoted)
                    hdr_promote(kv.first, false);
            hdr_live_ = 0;
            hdr_tex_rebuild();
        }
        auto it = textures_.find(bb);
        const uint32_t on = want && it != textures_.end() && it->second.promoted ? 1u : 0u;
        __atomic_store_n(&host_d9_hdr_float, on, __ATOMIC_RELAXED);
        if ((hdr_float_ || hdr_promotions_) && presents_ % 600 == 0)
            fprintf(stderr, "[remaster] hdr: present %llu float %d promotions %llu demotions %llu live %u readback "
                            "conversions %llu clamped draws: ceiling 1 %llu, ceiling 64 %llu (chain %llu additive %llu "
                            "mirror %llu family %llu), families %zu keys, float dumps %llu, HUD splits %llu (add-back in pass %llu), mirror marked %llu late %llu%s\n",
                    (unsigned long long)presents_, (int)on, (unsigned long long)hdr_promotions_,
                    (unsigned long long)hdr_demotions_, hdr_live_, (unsigned long long)hdr_readbacks_,
                    (unsigned long long)hdr_draws_[0], (unsigned long long)hdr_draws_[1],
                    (unsigned long long)hdr_why_[0], (unsigned long long)hdr_why_[1], (unsigned long long)hdr_why_[2],
                    (unsigned long long)hdr_why_[3], hdr_family_.size(), (unsigned long long)hdr_dumps_,
                    (unsigned long long)hdr_splits_, (unsigned long long)hdr_addback_open_, (unsigned long long)hdr_mirror_draws_[0],
                    (unsigned long long)hdr_mirror_draws_[1], hdr_split_off_ ? " (split unavailable)" : "");
    }
    void hdr_promote(uint32_t tid, bool on) {
        auto it = textures_.find(tid);
        if (it == textures_.end() || !it->second.texture)
            return;
        Tex &t = it->second;
        if (t.promoted == on || t.aliased || t.depth || t.cube || t.desc.kind != HOST_D9_TEX_2D ||
            !(t.desc.usage & HOST_D9_USAGE_RENDERTARGET))
            return;
        if (on && (t.info.pixel != MTLPixelFormatBGRA8Unorm || t.texture.mipmapLevelCount != 1))
            return;
        end_pass();
        if (on) {
            const NSUInteger w = t.texture.width, h = t.texture.height;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                          width:w
                                                                                         height:h
                                                                                      mipmapped:NO];
            td.usage = t.texture.usage;
            td.storageMode = t.texture.storageMode;
            id<MTLTexture> f = [mtl_ newTextureWithDescriptor:td], fm = nil;
            if (t.msaa) {
                MTLTextureDescriptor *md = [MTLTextureDescriptor new];
                md.textureType = MTLTextureType2DMultisample;
                md.pixelFormat = MTLPixelFormatRGBA16Float;
                md.width = w;
                md.height = h;
                md.sampleCount = t.msaa.sampleCount;
                md.usage = t.msaa.usage;
                md.storageMode = MTLStorageModePrivate;
                fm = [mtl_ newTextureWithDescriptor:md];
            }
            if (!f || (t.msaa && !fm)) {
                fprintf(stderr, "[remaster] hdr: target %u: no float backing (%lux%lu)\n", tid, (unsigned long)w,
                        (unsigned long)h);
                return;
            }
            note_alloc(AS_TEXTURE, f, w, h, 1, t.desc.format);
            if (fm)
                note_alloc(AS_MSAA, fm, w, h, (uint32_t)fm.sampleCount, t.desc.format);
            begin_frame();   // black, not whatever the new memory held
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = f;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[cmd_ renderCommandEncoderWithDescriptor:rp] endEncoding];
            t.sdr_texture = t.texture;
            t.sdr_msaa = t.msaa;
            t.texture = f;
            if (fm)
                t.msaa = fm;
            t.info.pixel = MTLPixelFormatRGBA16Float;
            ++hdr_promotions_;
            ++hdr_live_;
        } else {
            if (!t.sdr_texture)
                return;
            t.texture = t.sdr_texture;
            if (t.sdr_msaa)
                t.msaa = t.sdr_msaa;
            t.sdr_texture = t.sdr_msaa = nil;
            t.info.pixel = MTLPixelFormatBGRA8Unorm;
            ++hdr_demotions_;
            if (hdr_live_)
                --hdr_live_;
        }
        t.promoted = on;
        if (t.imported)
            device_->destroy(t.imported);
        t.imported = {};
        t.alt_texture = t.alt_msaa = t.alt_drawn = nil;   // MetalFX makes its scene backing again, in the new format
        fprintf(stderr, "[remaster] hdr: target %u (%lux%lu%s) %s\n", tid, (unsigned long)t.texture.width,
                (unsigned long)t.texture.height, t.msaa ? ", multisampled" : "",
                on ? "promoted to RGBA16Float" : "back to its 8-bit backing");
        hdr_tex_rebuild();
    }
    // ---- PLAN_R2 item 10: the HUD over the SDR picture -------------------------------------------------------------
    // The HUD blends over the scene. On the 8-bit frame it blended over values clipped at 1; on the float frame a
    // translucent panel over the sun carried the sun's excess (CONF14B: values up to 4.2 outside the scene mark; a dark
    // panel turned white). At the first HUD draw of a frame one full-screen draw in the open pass (framebuffer fetch,
    // per sample) moves the excess above 1 into hdr_excess_ and leaves min(rgb, 1); at Present the excess goes back
    // where the scene mark is still > 0.5, scaled by it (the scene's remaining share). Unmarked pixels (HUD, mirror,
    // menus) are then exactly the SDR blend. Without framebuffer fetch (not an Apple GPU) nothing changes.
    static constexpr const char *kHdrSplitSource = R"MSL(#include <metal_stdlib>
using namespace metal;
struct HV { float4 pos [[position]]; };
vertex HV hdr_quad_vs(uint vid [[vertex_id]]) {
    const float2 c = float2(float((vid << 1) & 2), float(vid & 2));
    HV o;
    o.pos = float4(c * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
fragment float4 hdr_split_fs(HV i [[stage_in]], float4 dst [[color(0)]], uint sid [[sample_id]],
                             texture2d<float, access::write> e [[texture(0)]]) {
    if (sid == 0)
        e.write(float4(max(dst.rgb - 1.0, 0.0), 0.0), uint2(i.pos.xy));
    return float4(min(dst.rgb, 1.0), dst.a);
}
fragment float4 hdr_addback_fs(HV i [[stage_in]], float4 dst [[color(0)]], texture2d<float> e [[texture(0)]]) {
    const float w = dst.a > 0.5 ? dst.a : 0.0;
    return float4(dst.rgb + w * e.read(uint2(i.pos.xy)).rgb, dst.a);
}
)MSL";
    bool hdr_split_ready() {
        if (hdr_split_off_)
            return false;
        if (hdr_lib_)
            return true;
        hdr_split_off_ = true;
        if (@available(macOS 10.15, iOS 13.0, *)) {
            if (![mtl_ supportsFamily:MTLGPUFamilyApple1])   // framebuffer fetch: Apple GPUs only
                return false;
        } else {
            return false;
        }
        NSError *error = nil;
        hdr_lib_ = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:kHdrSplitSource] options:nil error:&error];
        if (!hdr_lib_) {
            fprintf(stderr, "[remaster] hdr: HUD split unavailable: %s\n", error.localizedDescription.UTF8String);
            return false;
        }
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = NO;
        hdr_nodepth_ = [mtl_ newDepthStencilStateWithDescriptor:dd];
        hdr_split_off_ = false;
        return true;
    }
    id<MTLRenderPipelineState> hdr_pso(bool split, uint32_t samples, const MTLPixelFormat cf[4], MTLPixelFormat df) {
        uint64_t key = (uint64_t)split << 63 | (uint64_t)samples << 48 | (uint64_t)df << 32;
        for (int i = 0; i < 4; ++i)
            key = mix(key, (uint64_t)cf[i] + 1000 * (uint64_t)i);
        auto it = hdr_psos_.find(key);
        if (it != hdr_psos_.end())
            return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.rasterSampleCount = std::max<uint32_t>(samples, 1);
        pd.vertexFunction = [hdr_lib_ newFunctionWithName:@"hdr_quad_vs"];
        pd.fragmentFunction = [hdr_lib_ newFunctionWithName:split ? @"hdr_split_fs" : @"hdr_addback_fs"];
        for (int i = 0; i < 4; ++i)
            if (cf[i] != MTLPixelFormatInvalid) {
                pd.colorAttachments[i].pixelFormat = cf[i];
                pd.colorAttachments[i].writeMask = i ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
            }
        if (df != MTLPixelFormatInvalid) {
            pd.depthAttachmentPixelFormat = df;
            if (df == MTLPixelFormatDepth32Float_Stencil8)
                pd.stencilAttachmentPixelFormat = df;
        }
        NSError *error = nil;
        id<MTLRenderPipelineState> pso = [mtl_ newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pso) {
            fprintf(stderr, "[remaster] hdr: %s pipeline failed: %s\n", split ? "split" : "add-back",
                    error.localizedDescription.UTF8String);
            hdr_split_off_ = true;
        }
        hdr_psos_[key] = pso;
        return pso;
    }
    void hdr_split(uint32_t bb) {
        hdr_split_tried_ = true;
        auto it = textures_.find(bb);
        if (it == textures_.end() || !it->second.promoted || !enc_ || !pass_color_[0] ||
            pass_color_[0] != it->second.texture || !hdr_split_ready())
            return;
        const NSUInteger w = pass_color_[0].width, h = pass_color_[0].height;
        if (!hdr_excess_ || hdr_excess_.width != w || hdr_excess_.height != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                          width:w
                                                                                         height:h
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            td.storageMode = MTLStorageModePrivate;
            hdr_excess_ = [mtl_ newTextureWithDescriptor:td];
            if (!hdr_excess_)
                return;
            note_alloc(AS_TEXTURE, hdr_excess_, w, h, 1, 113);
        }
        MTLPixelFormat cf[4];
        for (int i = 0; i < 4; ++i)
            cf[i] = pass_color_[i] ? pass_color_[i].pixelFormat : MTLPixelFormatInvalid;
        const MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        id<MTLRenderPipelineState> pso = hdr_pso(true, pass_samples_, cf, df);
        if (!pso)
            return;
        [enc_ setRenderPipelineState:pso];
        [enc_ setDepthStencilState:hdr_nodepth_];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
        [enc_ setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, w, h}];
        [enc_ setFragmentTexture:hdr_excess_ atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        es_ = EncoderState{};   // what follows is the HUD draw's own state
        hdr_split_done_ = true;
        ++hdr_splits_;
    }
    // Run 15 (findings 22/25: +1.6-2.9 ms GPU with the float frame): at Present the HUD pass on the back buffer is
    // normally still open, so the add-back is one full-screen draw in it (framebuffer fetch, like the split) instead
    // of ending the pass and loading/storing the 4K float frame once more. Any other state: hdr_addback below.
    void hdr_addback_open(uint32_t bb) {
        auto it = textures_.find(bb);
        if (it == textures_.end() || !it->second.promoted || !enc_ || !pass_color_[0] ||
            pass_color_[0] != it->second.texture || !hdr_excess_ || hdr_excess_.width != pass_color_[0].width ||
            hdr_excess_.height != pass_color_[0].height)
            return;
        MTLPixelFormat cf[4];
        for (int i = 0; i < 4; ++i)
            cf[i] = pass_color_[i] ? pass_color_[i].pixelFormat : MTLPixelFormatInvalid;
        const MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        id<MTLRenderPipelineState> pso = hdr_pso(false, pass_samples_, cf, df);
        if (!pso)
            return;
        const NSUInteger w = pass_color_[0].width, h = pass_color_[0].height;
        [enc_ setRenderPipelineState:pso];
        [enc_ setDepthStencilState:hdr_nodepth_];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
        [enc_ setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, w, h}];
        [enc_ setFragmentTexture:hdr_excess_ atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        es_ = EncoderState{};
        hdr_split_done_ = false;
        ++hdr_addback_open_;
    }
    void hdr_addback(Tex &t) {
        hdr_split_done_ = false;
        if (!t.promoted || !hdr_excess_ || hdr_excess_.width != t.texture.width ||
            hdr_excess_.height != t.texture.height)
            return;
        const MTLPixelFormat cf[4] = {t.texture.pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                      MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso = hdr_pso(false, 1, cf, MTLPixelFormatInvalid);
        if (!pso)
            return;
        end_pass();
        begin_frame();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t.texture;   // the resolved frame (its samples are drawn anew next frame)
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        D9RenderEnc *enc = [cmd_ renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        [enc setFragmentTexture:hdr_excess_ atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        t.used = serial_;
    }
    void hdr_tex_rebuild() {
        hdr_tex_.clear();
        for (auto &kv : textures_) {
            const Tex &t = kv.second;
            if (!t.promoted)
                continue;
            for (id<MTLTexture> x : {t.texture, t.msaa, t.alt_texture, t.alt_msaa})
                if (x && x.pixelFormat == MTLPixelFormatRGBA16Float)
                    hdr_tex_.insert((__bridge const void *)x);
        }
    }
    // The output clamp class of a draw into a promoted target: 2 (ceiling 64) for light (the core mod's chain passes,
    // additive blends, the mirror under the HUD, the sky / car paint / glass / road programs), 1 (SDR white) otherwise.
    uint8_t hdr_class(const HostD9Draw &d, const uint32_t *rs, uint64_t ps_key, const d9sh::Program &pp) {
        id<MTLTexture> c0 = pass_color_[0];
        if (!c0 || !hdr_tex_.count((__bridge const void *)c0))
            return 0;
        if (rs[250] == 1 && !hdr_mirror_keep_) {   // after the scene mark: HUD, menus at SDR white (design s3), additive or not
            ++hdr_draws_[0];
            return 1;
        }
        int why = rs[HOST_D9_RS_HDR_FLOAT] == 1 ? 0 : (rs[27] && rs[20] == 2) ? 1 : d.hdr_scene_texture ? 2 : -1;
        if (why < 0) {
            auto it = hdr_family_.find(ps_key);
            if (it == hdr_family_.end()) {
                const uint8_t fam = !pp.ok || !pp.pixel                ? 0
                                    : d9msl::pixel_is_sky(pp)          ? 1
                                    : d9msl::pixel_is_carpaint(pp)     ? 2
                                    : d9msl::pixel_is_glass(pp)        ? 3
                                    : d9msl::pixel_is_roadsheen(pp)    ? 4
                                                                       : 0;
                it = hdr_family_.emplace(ps_key, fam).first;
                if (fam)
                    fprintf(stderr, "[remaster] hdr: ps %016llx family %s: ceiling 64\n", (unsigned long long)ps_key,
                            fam == 1 ? "sky" : fam == 2 ? "car paint" : fam == 3 ? "glass" : "road");
            }
            if (it->second)
                why = 3;
        }
        if (why >= 0)
            ++hdr_why_[why];
        ++hdr_draws_[why >= 0 ? 1 : 0];
        return why >= 0 ? 2 : 1;
    }
    bool hdr_read(Tex &t, uint32_t level, uint32_t face, uint8_t *bytes, uint32_t pitch) {
        if (level || face)
            return false;
        const uint32_t pw = (uint32_t)t.texture.width, ph = (uint32_t)t.texture.height;
        const uint32_t w = std::max<uint32_t>(t.desc.width, 1), h = std::max<uint32_t>(t.desc.height, 1);
        if (pitch < w * 4)
            return false;
        std::vector<uint16_t> half((size_t)pw * ph * 4);
        [t.texture getBytes:half.data() bytesPerRow:pw * 8 fromRegion:MTLRegionMake2D(0, 0, pw, ph) mipmapLevel:0];
        std::vector<uint8_t> full((size_t)pw * ph * 4);
        hdrconv::rgba16f_to_bgra8(half.data(), full.data(), (size_t)pw * ph);
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t sy = std::min(ph - 1, (uint32_t)((y + 0.5) * ph / h));
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t sx = std::min(pw - 1, (uint32_t)((x + 0.5) * pw / w));
                memcpy(bytes + (size_t)y * pitch + (size_t)x * 4, &full[((size_t)sy * pw + sx) * 4], 4);
            }
        }
        ++hdr_readbacks_;
        return true;
    }
    // RECOMP_DUMP_FLOAT_FRAMES=N (TEST): every Nth float frame as raw RGBA half (alpha = the scene mark) + a json the
    // converter (tools/hdr/measure.py) reads; H = the presenter's headroom, or 4 (synthetic) on an SDR swapchain.
    void hdr_dump_float(Tex &t, uint32_t w, uint32_t h) {
        settle(t.used);
        std::vector<uint16_t> half((size_t)w * h * 4);
        [t.texture getBytes:half.data() bytesPerRow:w * 8 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        size_t over = 0, mask = 0;
        float peak = 0.0f;
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            const float a = hdrconv::half_to_float(half[i * 4 + 3]);
            float m = 0.0f;
            for (int k = 0; k < 3; ++k)
                m = std::max(m, hdrconv::half_to_float(half[i * 4 + k]));
            if (a > 0.5f) {
                ++mask;
                over += m > 1.0f;
                peak = std::max(peak, m);
            }
        }
        os_mkdir(host_dump_dir());
        const unsigned n = (unsigned)(presents_ + 1);
        const float H = g_host_hdr_headroom.load(std::memory_order_relaxed);
        char path[1024];
        snprintf(path, sizeof path, "%s/float_%05u.rgba16f", host_dump_dir(), n);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(half.data(), 2, half.size(), f);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/float_%05u.json", host_dump_dir(), n);
        if (FILE *f = fopen(path, "w")) {
            fprintf(f, "{\"present\": %u, \"width\": %u, \"height\": %u, \"format\": \"rgba16f\", "
                       "\"encoding\": \"extended gamma (sRGB curve), alpha > 0.5 = scene mark\", \"headroom\": %.4f, "
                       "\"headroom_synthetic\": %s, \"mask_pixels\": %zu, \"mask_over_1\": %zu, \"mask_peak\": %.4f}\n",
                    n, w, h, H > 1.001f ? H : 4.0f, H > 1.001f ? "false" : "true", mask, over, (double)peak);
            fclose(f);
        }
        ++hdr_dumps_;
        fprintf(stderr, "[remaster] hdr: float dump present_%05u: scene pixels %zu, above 1: %zu (%.3f %%), peak %.3f\n", n,
                mask, over, mask ? 100.0 * over / mask : 0.0, (double)peak);
    }

    // The frame as presented, copied on the GPU when it was presented: the
    // back buffer itself already holds the start of the next frame.
    void keep_presented(Tex &t, uint32_t w, uint32_t h) {
        if ((t.info.pixel != MTLPixelFormatBGRA8Unorm && !t.promoted) || !t.texture)
            return;
        // Run 15 (HDR cost): only the harness's frame dumps (pop_smoke write_dump, RECOMP_HOST_DUMP_DIR) read the kept
        // frame, so a float frame (twice the bytes of the 8-bit one: 66 MB a frame at 4K) is not copied without them.
        static const bool dumps = recomp_env("HOST_DUMP_DIR") != nullptr;
        if (t.promoted && !dumps)
            return;
        std::lock_guard<std::mutex> lock(kept_mutex_);
        // PLAN_R2 item 10: a float frame is kept as float (read_presented converts).
        if (!kept_ || kept_.width != w || kept_.height != h || kept_.pixelFormat != t.texture.pixelFormat) {
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.texture.pixelFormat
                                                                   width:w
                                                                  height:h
                                                               mipmapped:NO];
            td.storageMode = MTLStorageModeShared;
            kept_ = [mtl_ newTextureWithDescriptor:td];
            note_alloc(AS_KEPT, kept_, w, h, 1, 0);
        }
        begin_frame();
        D9BlitEnc *blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:t.texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:kept_
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        kept_cmd_ = cmd_;
        kept_w_ = w;
        kept_h_ = h;
    }

    // The scale that fills the drawable's height with the back buffer (at
    // least RECOMP_D3D9_MIN_ROWS rows, for supersampling), or 0 when it is
    // fixed by RECOMP_D3D9_SCALE or unknown.
    float wanted_scale() const {
        if (recomp_env("D3D9_SCALE") || !base_rows_)
            return 0.0f;
        int dw = 0, dh = 0;
        if (!host_present_drawable(&dw, &dh) || dh <= 0)
            return 0.0f;
        int rows = dh;
        if (const char *min = recomp_env("D3D9_MIN_ROWS"))
            rows = std::max(rows, atoi(min));
        float s = std::floor(8.0f * rows / base_rows_ + 0.5f) / 8.0f;
        return std::max(1.0f, std::min(8.0f, s));
    }
    // Follow the drawable: once a different scale has been wanted for a
    // quarter of a second's worth of frames, every scaled target is made
    // again at the new size, between frames. Their contents are redrawn by
    // the game; a target it only draws now and then shows black once.
    void follow_drawable() {
        const float want = wanted_scale();
        if (want <= 0 || std::fabs(want - scale_) < 0.01f) {
            rescale_frames_ = 0;
            return;
        }
        if (++rescale_frames_ < 15)
            return;
        rescale_frames_ = 0;
        end_pass();
        flush();
        wait();
        scale_ = want;
        fitted_depth_.clear();
        g_fitted_depth_entries = 0;
        std::vector<HostD9TextureDesc> again;
        for (auto &kv : textures_)
            if (kv.second.scale != 1.0f ||
                ((kv.second.desc.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) &&
                 kv.second.desc.kind == HOST_D9_TEX_2D))
                again.push_back(kv.second.desc);
        for (const HostD9TextureDesc &d : again)
            define(d);
        fprintf(stderr, "d3d9: render targets now at %.2fx the game's size\n", scale_);
    }

    bool read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) override {
        id<MTLTexture> kept;
        D9Cmd *cmd;
        {
            std::lock_guard<std::mutex> lock(kept_mutex_);
            kept = kept_;
            cmd = kept_cmd_;
            *w = kept_w_;
            *h = kept_h_;
        }
        if (!kept)
            return false;
        uint32_t fw = *w, fh = *h;
        if (!rgb || cap < fw * fh * 3)
            return false;
        [cmd waitUntilCompleted];
        std::vector<uint8_t> bgra((size_t)fw * fh * 4);
        std::lock_guard<std::mutex> lock(kept_mutex_);
        if (kept.pixelFormat == MTLPixelFormatRGBA16Float) {   // PLAN_R2 item 10: the kept float frame, clamped
            std::vector<uint16_t> half((size_t)fw * fh * 4);
            [kept getBytes:half.data() bytesPerRow:fw * 8 fromRegion:MTLRegionMake2D(0, 0, fw, fh) mipmapLevel:0];
            hdrconv::rgba16f_to_bgra8(half.data(), bgra.data(), (size_t)fw * fh);
        } else
        [kept getBytes:bgra.data()
            bytesPerRow:fw * 4
             fromRegion:MTLRegionMake2D(0, 0, fw, fh)
            mipmapLevel:0];
        size_t n = (size_t)fw * fh;
        for (size_t i = 0; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        return true;
    }

  private:
    // ---- passes ---------------------------------------------------------
    // ---- Remaster run 13 MetalFX upscaling (review/run13/metalfx-design.md, v1 spatial) --------------------
    // The core mod marks the 3D scene (private render state 253 at the main view's colour pass, 252 before the
    // post chain and the HUD). Between the markers the back buffer and its depth draw into scene backings at
    // 1/factor of their size; at the end MTLFXSpatialScaler scales the scene up and one pass writes it, with the
    // scene depth, into the native backings. Everything outside the markers (front end, HUD, menus, PC look, the
    // post chain) draws native as before. Upscaling off (RECOMP_REMASTER_UPSCALE unset): none of this runs.
    // Remaster run 14 foundation F2: with RECOMP_REMASTER_FOUNDATION=1 the same markers open the foundation's window
    // too (after the upscale begin, before the upscale end), with or without upscaling.
    void scene_split(uint32_t phase, uint32_t bb, uint32_t ds) override {
        if (upscale_factor_ <= 0.0f && !fnd_on_)
            return;
        if (phase == HOST_D9_SCENE_BEGIN) {
            if (upscale_factor_ > 0.0f)
                upscale_begin(bb, ds);
            if (fnd_on_)
                fnd_begin(bb, ds);
        } else if (phase == HOST_D9_SCENE_END) {
            if (fnd_on_)
                fnd_end();
            if (upscale_factor_ > 0.0f)
                upscale_end(true);
        }
    }
    // v4: a MetalFX scaler's work into the open command buffer: classic, or (Metal 4) after a barrier on the scene,
    // with its textures kept resident and alive by the command buffer.
    void fx_encode(id sc, std::initializer_list<id<MTLTexture>> textures) {
        if (!m4_) {
            [(id<MTLFXSpatialScaler>)sc encodeToCommandBuffer:cmd_.raw];   // both classic scalers answer it
            return;
        }
        if (@available(macOS 26.0, iOS 26.0, *)) {
            [cmd_ barrierBeforeForeignWork:textures];
            [(id<MTL4FXSpatialScaler>)sc encodeToCommandBuffer:(id<MTL4CommandBuffer>)cmd_.m4];
        }
    }
    bool upscale_available() {
        if (up_.checked)
            return up_.supported;
        up_.checked = true;
        if (@available(macOS 13.0, iOS 16.0, *)) {
            Class c = NSClassFromString(@"MTLFXSpatialScalerDescriptor");
            up_.supported = c != nil && [MTLFXSpatialScalerDescriptor supportsDevice:mtl_];
        }
        fprintf(stderr, "[remaster] upscale: MetalFX spatial %s, factor %.2f (%s)%s\n",
                up_.supported ? "available" : "NOT available: the scene stays native", upscale_factor_,
                upscale_factor_ <= 1.5f ? "Quality" : "Performance",
                upscale_temporal_ ? (tmp_on_ ? "; temporal (experimental) on" : "; temporal asked for without the foundation and jitter: spatial") : "");
        return up_.supported;
    }
    static void swap_backing(Tex &t) {
        std::swap(t.texture, t.alt_texture);
        std::swap(t.msaa, t.alt_msaa);
        std::swap(t.drawn_depth, t.alt_drawn);
        std::swap(t.scale, t.alt_scale);
        std::swap(t.samples, t.alt_samples);
        t.aliased = !t.aliased;
    }
    // The scene backing of a native target at `s` physical pixels per guest pixel: same format and samples.
    bool ensure_backing(Tex &t, float s) {
        const NSUInteger w = (NSUInteger)std::max<long>(1, std::lround(t.desc.width * s)),
                         h = (NSUInteger)std::max<long>(1, std::lround(t.desc.height * s));
        if (t.alt_texture && t.alt_texture.width == w && t.alt_texture.height == h && (!t.msaa || t.alt_msaa))
            return true;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.texture.pixelFormat
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = t.depth ? MTLStorageModePrivate : t.texture.storageMode;
        t.alt_texture = [mtl_ newTextureWithDescriptor:td];
        note_alloc(AS_TEXTURE, t.alt_texture, w, h, 1, t.desc.format);
        t.alt_msaa = nil;
        t.alt_drawn = nil;
        if (t.msaa && t.alt_texture) {
            MTLTextureDescriptor *md = [MTLTextureDescriptor new];
            md.textureType = MTLTextureType2DMultisample;
            md.pixelFormat = td.pixelFormat;
            md.width = w;
            md.height = h;
            md.sampleCount = t.samples;
            md.usage = MTLTextureUsageRenderTarget | (t.depth ? MTLTextureUsageShaderRead : 0);
            md.storageMode = MTLStorageModePrivate;
            t.alt_msaa = [mtl_ newTextureWithDescriptor:md];
            note_alloc(AS_MSAA, t.alt_msaa, w, h, t.samples, t.desc.format);
        }
        t.alt_scale = s;
        t.alt_samples = t.alt_msaa ? t.samples : 1;
        ++up_.allocs;
        if (t.promoted)
            hdr_tex_rebuild();   // PLAN_R2 item 10: the float scene backing gets the output clamp too
        return t.alt_texture && (!t.msaa || t.alt_msaa);
    }
    void upscale_begin(uint32_t bb, uint32_t ds) {
        if (up_.open) {
            ++up_.nested;
            return;
        }
        if (!upscale_available())
            return;
        Tex *bt = tex_ptr(bb);
        if (!bt || !bt->texture || bt->depth || bt->aliased || bt->desc.kind != HOST_D9_TEX_2D || bt->info.block) {
            ++up_.refused;
            return;
        }
        Tex *dt = ds ? tex_ptr(ds) : nullptr;
        if (dt && (!dt->texture || !dt->depth || dt->aliased || dt->desc.kind != HOST_D9_TEX_2D ||
                   dt->desc.width != bt->desc.width || dt->desc.height != bt->desc.height ||
                   std::fabs(dt->scale - bt->scale) > 0.001f)) {
            ++up_.depth_mismatch; // a depth the scene backing cannot pair with: this frame stays native
            return;
        }
        up_.last_bb = bb;
        up_.last_ds = dt ? ds : 0;
        // The back buffer was drawn before the scene began this frame: its picture and depth are copied down into
        // the scene backings below (counted; expected 0 for this game).
        const bool copy_down = up_.pre_frame != 0;
        if (copy_down)
            ++up_.bb_before;
        up_.pre_frame = 0;
        float s = bt->scale / upscale_factor_;
        // A host's scene height (upscale_control.h) replaces the launch factor: the backing is that many physical
        // rows, at most 4x below the back buffer; at or above the back buffer's own height the scene draws native.
        // Above 5/6 of it the scene is drawn at 5/6 (v2.4; v2.3 drew it native, which turned MetalFX off at most
        // players' steps): the spatial scaler at a ratio that close to 1 (800 rows onto 900: 9/8) is sharp where
        // output and scene pixels line up and soft between, a grid of lines every 1/(ratio-1) scene pixels (8 there)
        // on the whole picture. The iOS panel's steps are 5/6 and 3/4 of the output (v2.2's 600p and 540p at 720p).
        if (int rows = g_upscale_rows.load(std::memory_order_relaxed); rows >= 0) {
            const float native_rows = bt->desc.height * bt->scale;
            if (rows == 0 || rows >= native_rows - 0.5f) {
                ++up_.native;
                g_upscale_in.store(0);
                g_upscale_out.store(0);
                return;
            }
            rows = std::min(rows, (int)(native_rows * 5.0f / 6.0f));
            s = std::max((float)rows, native_rows / 4.0f) / (float)bt->desc.height;
        }
        if (!ensure_backing(*bt, s) || (dt && !ensure_backing(*dt, s))) {
            ++up_.failures;
            return;
        }
        end_pass();
        swap_backing(*bt);
        if (dt)
            swap_backing(*dt);
        if (up_follow_)
            follow_begin(*bt, bb, up_.ds, s);
        up_.open = true;
        up_.begun_frame = true;
        up_.bb = bb;
        up_.ds = dt ? ds : 0;
        up_.draws = 0;
        up_.in_w = (uint32_t)bt->texture.width;
        up_.in_h = (uint32_t)bt->texture.height;
        tmp_begin(*bt);
        if (copy_down) {
            upscale_compose(bt->alt_texture, dt ? (dt->alt_drawn ? dt->alt_drawn : dt->alt_texture) : nil, bb, up_.ds);
            return;
        }
        // The native backings were cleared earlier this frame: the scene backings get the same clear.
        const bool cc = up_.clr_color, cz = up_.clr_depth && up_.ds, cs = up_.clr_stencil && up_.ds;
        if (cc || cz || cs) {
            HostD9Target t{};
            t.color[0].id = bb;
            t.depth.id = up_.ds;
            begin_pass(t, cc, up_.clr_rgba, cz, up_.clr_z, cs, up_.clr_stencil_value);
            end_pass();
            ++up_.replayed;
        }
    }
    // Ends the scene: scale it up into the native back buffer and switch both ids back. `marker`: the mod's 252
    // (false: Present closed a scene the mod left open).
    void upscale_end(bool marker) {
        if (!up_.open) {
            if (marker)
                ++up_.stray_end;
            return;
        }
        end_pass();
        up_.open = false;
        follow_end();
        Tex *bt = tex_ptr(up_.bb);
        Tex *dt = up_.ds ? tex_ptr(up_.ds) : nullptr;
        if (!bt || !bt->aliased) {
            ++up_.failures;
            if (dt && dt->aliased)
                swap_backing(*dt);
            return;
        }
        id<MTLTexture> scene = bt->texture;
        id<MTLTexture> scene_z = nil;
        if (dt && dt->aliased) {
            scene_z = dt->drawn_depth ? dt->drawn_depth : dt->texture;
            swap_backing(*dt);
        }
        swap_backing(*bt);
        if (!up_.draws) {
            ++up_.skipped; // nothing drew the scene: the native back buffer is the frame (H1)
            return;
        }
        id<MTLTexture> out = tmp_.frame ? tmp_encode(scene, scene_z, bt->texture) : nil;
        const bool temporal_out = out != nil;
        if (tmp_.frame) {   // this scene's rows are the next one's previous rows
            tmp_.prev.swap(tmp_.cur);
            tmp_.frame = false;
            g_temporal_active.store(out ? 1 : 0, std::memory_order_relaxed);
        }
        if (!out)
            out = upscale_encode(scene, bt->texture);
        if (!out) {
            ++up_.failures;
            out = scene; // still a picture: the compose pass stretches it bilinearly
        }
        upscale_compose(out, scene_z, up_.bb, up_.ds, temporal_out ? (float)fnd_.jx : 0.0f,
                        temporal_out ? (float)fnd_.jy : 0.0f);
        ++up_.scaled;
        g_upscale_in.store(pack_size(scene.width, scene.height));
        g_upscale_out.store(pack_size(bt->texture.width, bt->texture.height));
    }
    // MTLFXSpatialScaler: scene -> a private output at the native size. Null when it could not run.
    id<MTLTexture> upscale_encode(id<MTLTexture> scene, id<MTLTexture> native) {
        if (@available(macOS 13.0, iOS 16.0, *)) {
            const NSUInteger iw = scene.width, ih = scene.height, ow = native.width, oh = native.height;
            if (!up_.scaler || up_.sc_iw != iw || up_.sc_ih != ih || up_.sc_ow != ow || up_.sc_oh != oh ||
                up_.sc_fmt != scene.pixelFormat) {
                m4_keep(up_.scaler);   // v4 Metal 4: the GPU may still be using it
                up_.scaler = nil;
                up_.out = nil;
                up_.sc_iw = iw, up_.sc_ih = ih, up_.sc_ow = ow, up_.sc_oh = oh, up_.sc_fmt = scene.pixelFormat;
                MTLFXSpatialScalerDescriptor *sd = [MTLFXSpatialScalerDescriptor new];
                sd.inputWidth = iw;
                sd.inputHeight = ih;
                sd.outputWidth = ow;
                sd.outputHeight = oh;
                sd.colorTextureFormat = scene.pixelFormat;
                sd.outputTextureFormat = scene.pixelFormat;
                sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
                id<MTLFXSpatialScaler> sc = nil;   // v4: on Metal 4 an MTL4FXSpatialScaler (same properties)
                if (m4_) {
                    if (@available(macOS 26.0, iOS 26.0, *))
                        if (m4_fx_)
                            sc = (id<MTLFXSpatialScaler>)[sd newSpatialScalerWithDevice:mtl_ compiler:((D9M4 *)m4_).compiler];
                } else {
                    sc = [sd newSpatialScalerWithDevice:mtl_];
                }
                if (sc) {
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:scene.pixelFormat
                                                                                                  width:ow
                                                                                                 height:oh
                                                                                              mipmapped:NO];
                    td.usage = sc.outputTextureUsage | MTLTextureUsageShaderRead;
                    td.storageMode = MTLStorageModePrivate;
                    up_.out = [mtl_ newTextureWithDescriptor:td];
                    note_alloc(AS_TEXTURE, up_.out, ow, oh, 1, 0);
                    up_.scaler = sc;
                    ++up_.allocs;
                }
                fprintf(stderr, "[remaster] upscale: spatial scaler %lux%lu -> %lux%lu format %lu %s\n",
                        (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh,
                        (unsigned long)scene.pixelFormat, up_.scaler && up_.out ? "ready" : "FAILED");
            }
            id<MTLFXSpatialScaler> sc = up_.scaler;
            if (!sc || !up_.out || (scene.usage & sc.colorTextureUsage) != sc.colorTextureUsage)
                return nil;
            begin_frame();
            sc.colorTexture = scene;
            sc.outputTexture = up_.out;
            sc.inputContentWidth = iw;
            sc.inputContentHeight = ih;
            fx_encode(sc, {scene, up_.out});
            return up_.out;
        }
        return nil;
    }
    // One pass into the native back buffer (and its depth, when the scene had one): every pixel and sample.
    void upscale_compose(id<MTLTexture> color, id<MTLTexture> scene_z, uint32_t bb, uint32_t ds, float zx = 0.0f,
                         float zy = 0.0f) {
        HostD9Target target{};
        target.color[0].id = bb;
        target.depth.id = scene_z ? ds : 0;
        if (!begin_pass(target, false, nullptr, false, 0, false, 0) || !enc_) {
            end_pass();
            ++up_.failures;
            return;
        }
        const bool z = pass_depth_ != nil && scene_z != nil;
        const bool zms = z && scene_z.textureType == MTLTextureType2DMultisample;
        MTLPixelFormat cf[4] = {pass_color_[0].pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        const MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        id<MTLRenderPipelineState> pso = utility_pipeline(
            mix(mix(z ? (zms ? 0x05ca1e2ull : 0x05ca1e1ull) : 0x05ca1e0ull, cf[0]), df),
            z ? (zms ? @"upscale_compose_ms_fs" : @"upscale_compose_fs") : @"copy_fs", cf, df, true);
        if (!pso) {
            end_pass();
            ++up_.failures;
            return;
        }
        if (!up_.zwrite) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            up_.zwrite = [mtl_ newDepthStencilStateWithDescriptor:dd];
            MTLDepthStencilDescriptor *dn = [MTLDepthStencilDescriptor new];
            dn.depthCompareFunction = MTLCompareFunctionAlways;
            dn.depthWriteEnabled = NO;
            up_.znone = [mtl_ newDepthStencilStateWithDescriptor:dn];
        }
        const NSUInteger tw = pass_color_[0].width, th = pass_color_[0].height;
        es_ = EncoderState{}; // what follows is not a draw's state
        [enc_ setRenderPipelineState:pso];
        [enc_ setDepthStencilState:z ? up_.zwrite : up_.znone];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setViewport:(MTLViewport){0, 0, (double)tw, (double)th, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, tw, th}];
        QuadIn q{};
        q.rect[0] = -1.0f;
        q.rect[1] = 1.0f;
        q.rect[2] = 1.0f;
        q.rect[3] = -1.0f;
        q.uv[2] = 1.0f;
        q.uv[3] = 1.0f;
        [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
        [enc_ setFragmentTexture:color atIndex:0];
        if (z) {
            [enc_ setFragmentTexture:scene_z atIndex:1];
            const float zoff[2] = {zx, zy};
            [enc_ setFragmentBytes:zoff length:sizeof zoff atIndex:0];
        }
        uint32_t st[14] = {};
        st[1] = st[2] = st[3] = 3; // clamp
        st[5] = st[6] = 2;         // linear (exact at the same size; a fallback stretch otherwise)
        [enc_ setFragmentSamplerState:sampler(st) atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (Tex *bt = tex_ptr(bb))
            bt->used = serial_;
        if (z)
            if (Tex *dt = tex_ptr(ds)) {
                dt->used = serial_;
                dt->drawn_depth = pass_depth_;
            }
        end_pass();
    }
    // A draw in the scene backing counts toward the scene and gets half a rendered pixel (the backing's scale);
    // one into the back buffer before the scene began this frame is counted (must stay 0).
    float upscale_draw() {
        if (up_.open && pass_color_id_ == up_.bb && pass_color_id_) {
            ++up_.draws;
            return pass_scale_;
        }
        if (!up_.open && up_.last_bb && pass_color_id_ == up_.last_bb && !up_.begun_frame)
            ++up_.pre_frame;
        return 1.0f;
    }
    // A clear of the native back buffer or scene depth before this frame's scene: replayed on the scene backings.
    void upscale_note_clear(const HostD9Target &t, bool whole, bool color, const float *rgba, bool depth, float z,
                            bool stencil, uint32_t sv) {
        if (up_.open || up_.begun_frame)
            return;
        if (up_.last_bb && t.color[0].id == up_.last_bb && color) {
            if (whole) {
                up_.clr_color = true;
                memcpy(up_.clr_rgba, rgba, sizeof up_.clr_rgba);
            } else
                ++up_.pre_frame;
        }
        if (up_.last_ds && t.depth.id == up_.last_ds && (depth || stencil) && whole) {
            if (depth) {
                up_.clr_depth = true;
                up_.clr_z = z;
            }
            if (stencil) {
                up_.clr_stencil = true;
                up_.clr_stencil_value = sv;
            }
        }
    }
    // Present closes a scene the mod left open; the per-frame state resets; the TEST ONLY report every 600 frames.
    void upscale_present() {
        if (up_.open) {
            ++up_.no_end;
            upscale_end(false);
        }
        if (tmp_on_ && (up_.frames + 1) % 600 == 0)
            fprintf(stderr,
                    "[remaster] temporal: scenes %llu scaled %llu spatial %llu resets %llu | motion draws %llu: previous "
                    "constants %llu, new %llu, drawn plain %llu, flat %llu | jitter sign %+.0f\n",
                    (unsigned long long)tmp_.scenes, (unsigned long long)tmp_.scaled, (unsigned long long)tmp_.spatial,
                    (unsigned long long)tmp_.resets, (unsigned long long)tmp_.draws, (unsigned long long)tmp_.known,
                    (unsigned long long)tmp_.fresh, (unsigned long long)tmp_.plain, (unsigned long long)tmp_.flat,
                    g_temporal_flip.load(std::memory_order_relaxed) ? -tmp_jsign_ : tmp_jsign_);
        up_.pre_frame = 0;
        up_.begun_frame = false;
        up_.clr_color = up_.clr_depth = up_.clr_stencil = false;
        if (++up_.frames % 600 == 0)
            fprintf(stderr,
                    "[remaster] upscale: frames %llu scaled %llu skipped %llu mode %d in %ux%u out %lux%lu "
                    "(factor %.2f, temporal %s) | must be 0: bb_before %llu fitted %llu failures %llu nested %llu "
                    "refused %llu | info: stray_end %llu no_end %llu depth_mismatch %llu replayed %llu allocs %llu "
                    "aborts %llu | effect targets at the scene size %.1f/scene%s\n",
                    (unsigned long long)up_.frames, (unsigned long long)up_.scaled,
                    (unsigned long long)up_.skipped, upscale_factor_ <= 1.5f ? 1 : 2, up_.in_w, up_.in_h,
                    (unsigned long)up_.sc_ow, (unsigned long)up_.sc_oh, upscale_factor_,
                    tmp_on_ ? (tmp_.failed ? "FAILED, spatial" : "on (experimental)") : "off",
                    (unsigned long long)up_.bb_before, (unsigned long long)up_.fitted,
                    (unsigned long long)up_.failures, (unsigned long long)up_.nested,
                    (unsigned long long)up_.refused, (unsigned long long)up_.stray_end,
                    (unsigned long long)up_.no_end, (unsigned long long)up_.depth_mismatch,
                    (unsigned long long)up_.replayed, (unsigned long long)up_.allocs,
                    (unsigned long long)up_.aborts,
                    up_.follow_frames ? (double)up_.follow_swaps / (double)up_.follow_frames : 0.0,
                    up_follow_ ? "" : " (off)");
    }
    // The back buffer or scene depth is redefined or dropped inside the scene: the other one goes back to native
    // and this frame's scene is lost (counted).
    // The scene-following effect targets (up_.followers): every 2D colour render target the size of the frame, a
    // half or a quarter of it (rounded up, as the clean chain sizes them), not the frame or its depth, not
    // multisampled, float or already aliased, and drawn only inside scenes for the last 120 frames: a target drawn
    // before the scene and read in it (a reflection the game renders first) keeps its own backing. Each one gets a
    // backing at the scene's scale `s` and is swapped in.
    void follow_begin(const Tex &bt, uint32_t bb, uint32_t ds, float s) {
        up_.followers.clear();
        ++up_.follow_frames;
        const uint32_t fw = bt.desc.width, fh = bt.desc.height;
        for (auto &kv : textures_) {
            Tex &t = kv.second;
            if (kv.first == bb || kv.first == ds || !t.texture || t.depth || t.cube || t.aliased || t.msaa || t.promoted ||
                t.desc.kind != HOST_D9_TEX_2D || !(t.desc.usage & HOST_D9_USAGE_RENDERTARGET) || t.info.block ||
                t.scale != bt.alt_scale ||   // the native scale the frame had: a target made like the frame
                (t.outside_frame && presents_ + 1 - t.outside_frame <= 120))
                continue;
            bool follows = false;
            for (uint32_t div : {1u, 2u, 4u})
                if (t.desc.width == (fw + div - 1) / div && t.desc.height == (fh + div - 1) / div)
                    follows = true;
            if (!follows || !ensure_backing(t, s))
                continue;
            swap_backing(t);
            up_.followers.push_back(kv.first);
        }
        up_.follow_swaps += up_.followers.size();
    }
    void follow_end() {
        for (uint32_t id : up_.followers)
            if (Tex *t = tex_ptr(id))
                if (t->aliased)
                    swap_backing(*t);
        up_.followers.clear();
    }
    void upscale_abort(uint32_t id) {
        up_.open = false;
        ++up_.aborts;
        up_.followers.erase(std::remove(up_.followers.begin(), up_.followers.end(), id), up_.followers.end());
        follow_end();
        for (uint32_t other : {up_.bb, up_.ds})
            if (other && other != id)
                if (Tex *t = tex_ptr(other))
                    if (t->aliased)
                        swap_backing(*t);
    }

    // ---- v2.5 MetalFX temporal (experimental; state and switches at tmp_) ------------------------------------------
    // The scene began (its backings are in place): the motion target at the scene's size, and a gap in the scenes
    // (menus, a load, a cut) starts the history again.
    void tmp_begin(const Tex &bt) {
        tmp_.frame = false;
        if (!tmp_on_ || tmp_.failed || bt.msaa || !bt.texture)
            return;
        if (presents_ > tmp_.last_present + 1) {
            tmp_.reset_next = true;
            tmp_.prev.clear();
        }
        tmp_.last_present = presents_;
        const NSUInteger w = bt.texture.width, h = bt.texture.height;
        if (!tmp_.om || tmp_.om.width != w || tmp_.om.height != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                                                          width:w
                                                                                         height:h
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            tmp_.om = [mtl_ newTextureWithDescriptor:td];
            note_alloc(AS_TEXTURE, tmp_.om, w, h, 1, 0);
            tmp_.reset_next = true;
            tmp_.prev.clear();
        }
        tmp_.frame = tmp_.om != nil;
        tmp_.cleared = tmp_.wrote = tmp_.motion_ok = false;
        tmp_.cur.clear();
        tmp_.occ.clear();
        ++tmp_.scenes;
    }
    // A motion draw: its identity (programs' vertex half, buffers, range and how many draws of the same before it this
    // scene) finds its vertex constants of the last scene (vertex buffer 20, flag at 21); none (a new object): the
    // flag is 0 and the camera pass fills it. The motion vertex half runs its body again on those constants.
    void tmp_bind(const HostD9Draw &d, uint64_t vs_key, const d9sh::Program &) {
        ++tmp_.draws;
        // The constants exactly as this draw binds them (set_constants just built them: padded to what the program
        // reads, its own defs in place), so the second run never reads past the end. v4: on Metal 4 a read past the
        // end found the next upload in the shared chunk (another draw's data): wild motion, MetalFX smears.
        const size_t floats = std::min<size_t>(constants_.size(), 1024u);   // setVertexBytes: 4 KB at most
        uint64_t id = mix(vs_key, (uint64_t)d.stream[0].buffer << 32 | d.stream[0].offset);
        id = mix(id, (uint64_t)d.index_buffer << 32 | d.start);
        id = mix(id, (uint64_t)(uint32_t)d.base_vertex << 32 | d.primitive_count);
        id = mix(id, (uint64_t)d.primitive << 32 | (d.stream[0].buffer ? 0u : d.inline_bytes));
        id = mix(id, tmp_.occ[id]++);
        float known[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float *prev = constants_.data();   // unknown: this frame's (the flag keeps them unused)
        auto pv = tmp_.prev.find(id);
        if (floats && pv != tmp_.prev.end() && pv->second.c.size() == floats) {
            prev = pv->second.c.data();
            known[0] = 1.0f;
            ++tmp_.known;
        } else {
            ++tmp_.fresh;
        }
        [enc_ setVertexBytes:prev length:floats * sizeof(float) atIndex:20];
        [enc_ setVertexBytes:known length:sizeof known atIndex:21];
        if (floats)
            tmp_.cur[id].c.assign(constants_.data(), constants_.data() + floats);
    }
    // TEST: what draws flat (no depth test) inside the scene, by label (the first 16 kinds, logged once each).
    void tmp_note_flat(const HostD9Draw &d) {
        ++tmp_.flat;
        if (tmp_.flat_labels.size() < 16 && tmp_.flat_labels.insert(d.label ? d.label : "(no label)").second)
            fprintf(stderr, "[remaster] temporal: flat draw in the scene (not jittered, no motion): %s, %ux%u viewport, "
                            "blend %u\n",
                    d.label ? d.label : "(no label)", (unsigned)d.viewport[2], (unsigned)d.viewport[3],
                    d.render_state[27]);
    }
    // The scene depth as a plain Depth32Float (only when the scaler would not take the scene's own format).
    id<MTLTexture> tmp_zcopy(id<MTLTexture> z) {
        if (!fnd_library())
            return nil;
        if (!tmp_.z32 || tmp_.z32.width != z.width || tmp_.z32.height != z.height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:z.width
                                                                                         height:z.height
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            tmp_.z32 = [mtl_ newTextureWithDescriptor:td];
            note_alloc(AS_TEXTURE, tmp_.z32, z.width, z.height, 1, 0);
        }
        if (!tmp_.zpso) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [fnd_.lib newFunctionWithName:@"tmp_z_vs"];
            pd.fragmentFunction = [fnd_.lib newFunctionWithName:@"tmp_z_fs"];
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            NSError *error = nil;
            tmp_.zpso = [mtl_ newRenderPipelineStateWithDescriptor:pd error:&error];
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            tmp_.zdss = [mtl_ newDepthStencilStateWithDescriptor:dd];
            if (!tmp_.zpso)
                fprintf(stderr, "[remaster] temporal: depth copy pipeline FAILED: %s\n",
                        error ? error.localizedDescription.UTF8String : "?");
        }
        if (!tmp_.z32 || !tmp_.zpso)
            return nil;
        end_pass();
        begin_frame();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = tmp_.z32;
        rp.depthAttachment.loadAction = MTLLoadActionDontCare;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        D9RenderEnc *e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        if (!e)
            return nil;
        [e setRenderPipelineState:tmp_.zpso];
        [e setDepthStencilState:tmp_.zdss];
        [e setFragmentTexture:z atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        return tmp_.z32;
    }
    // MTLFXTemporalScaler: scene (+ depth, the foundation's vectors, the jitter) -> a private output at the native
    // size. Nil (that frame scaled spatially, the history reset) when a part is missing; a scaler MetalFX will not
    // make turns temporal off for the run.
    id<MTLTexture> tmp_encode(id<MTLTexture> scene, id<MTLTexture> z, id<MTLTexture> native) {
        if (@available(macOS 13.0, iOS 16.0, *)) {
            id<MTLTexture> mv = fnd_.motion_tex;
            if (!tmp_.motion_ok || !mv || !z || z.textureType != MTLTextureType2D || z.width != scene.width ||
                z.height != scene.height || mv.width != scene.width || mv.height != scene.height) {
                ++tmp_.spatial;
                tmp_.reset_next = true;
                return nil;
            }
            const NSUInteger iw = scene.width, ih = scene.height, ow = native.width, oh = native.height;
            if (!tmp_.scaler || tmp_.iw != iw || tmp_.ih != ih || tmp_.ow != ow || tmp_.oh != oh ||
                tmp_.cf != scene.pixelFormat || tmp_.df != z.pixelFormat) {
                m4_keep(tmp_.scaler);   // v4 Metal 4: the GPU may still be using it
                tmp_.scaler = nil;
                tmp_.out = nil;
                tmp_.iw = iw, tmp_.ih = ih, tmp_.ow = ow, tmp_.oh = oh, tmp_.cf = scene.pixelFormat, tmp_.df = z.pixelFormat;
                for (int attempt = 0; attempt < 2 && !tmp_.scaler; ++attempt) {
                    MTLFXTemporalScalerDescriptor *td = [MTLFXTemporalScalerDescriptor new];
                    td.inputWidth = iw;
                    td.inputHeight = ih;
                    td.outputWidth = ow;
                    td.outputHeight = oh;
                    td.colorTextureFormat = scene.pixelFormat;
                    td.depthTextureFormat = attempt ? MTLPixelFormatDepth32Float : z.pixelFormat;
                    td.motionTextureFormat = MTLPixelFormatRG16Float;
                    td.outputTextureFormat = scene.pixelFormat;
                    td.autoExposureEnabled = NO;
                    if (m4_) {   // v4: an MTL4FXTemporalScaler (same properties)
                        if (@available(macOS 26.0, iOS 26.0, *))
                            if (m4_fx_)
                                tmp_.scaler = [td newTemporalScalerWithDevice:mtl_ compiler:((D9M4 *)m4_).compiler];
                    } else {
                        tmp_.scaler = [td newTemporalScalerWithDevice:mtl_];
                    }
                    tmp_.zcopy = attempt == 1;
                }
                id<MTLFXTemporalScaler> sc = tmp_.scaler;
                if (sc) {
                    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:scene.pixelFormat
                                                                                                  width:ow
                                                                                                 height:oh
                                                                                              mipmapped:NO];
                    od.usage = sc.outputTextureUsage | MTLTextureUsageShaderRead;
                    od.storageMode = MTLStorageModePrivate;
                    tmp_.out = [mtl_ newTextureWithDescriptor:od];
                    note_alloc(AS_TEXTURE, tmp_.out, ow, oh, 1, 0);
                }
                fprintf(stderr, "[remaster] temporal: scaler %lux%lu -> %lux%lu colour %lu depth %lu%s %s\n",
                        (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh,
                        (unsigned long)scene.pixelFormat, (unsigned long)z.pixelFormat,
                        tmp_.zcopy ? " (copied to Depth32Float)" : "",
                        tmp_.scaler && tmp_.out ? "ready" : "FAILED: spatial for the rest of the run");
                if (!tmp_.scaler || !tmp_.out) {
                    tmp_.failed = true;
                    tmp_.scaler = nil;
                    return nil;
                }
                tmp_.reset_next = true;
            }
            id<MTLFXTemporalScaler> sc = tmp_.scaler;
            id<MTLTexture> zt = tmp_.zcopy ? tmp_zcopy(z) : z;
            if (!zt || (scene.usage & sc.colorTextureUsage) != sc.colorTextureUsage ||
                (zt.usage & sc.depthTextureUsage) != sc.depthTextureUsage ||
                (mv.usage & sc.motionTextureUsage) != sc.motionTextureUsage) {
                if (!tmp_.warned) {
                    tmp_.warned = true;
                    fprintf(stderr, "[remaster] temporal: an input lacks the usage the scaler asks for (colour %d depth %d "
                                    "motion %d): spatial\n",
                            (int)((scene.usage & sc.colorTextureUsage) == sc.colorTextureUsage),
                            (int)(zt && (zt.usage & sc.depthTextureUsage) == sc.depthTextureUsage),
                            (int)((mv.usage & sc.motionTextureUsage) == sc.motionTextureUsage));
                }
                ++tmp_.spatial;
                tmp_.reset_next = true;
                return nil;
            }
            end_pass();
            begin_frame();
            sc.colorTexture = scene;
            sc.depthTexture = zt;
            sc.motionTexture = mv;
            sc.outputTexture = tmp_.out;
            sc.inputContentWidth = iw;
            sc.inputContentHeight = ih;
            const float sign = g_temporal_flip.load(std::memory_order_relaxed) ? -tmp_jsign_ : tmp_jsign_;
            sc.jitterOffsetX = sign * (float)fnd_.jx;
            sc.jitterOffsetY = sign * (float)fnd_.jy;
            sc.motionVectorScaleX = 1.0f;
            sc.motionVectorScaleY = 1.0f;
            sc.depthReversed = NO;
            // Only a real break in the picture resets the history: the G/K camera check's disagreement (the front end
            // fails it every frame) and a missing VP_k are the camera-motion path's, not the scaler's.
            using namespace remaster_fnd;
            const bool reset = tmp_.reset_next || (fnd_.reset & (RESET_CUT | RESET_JUMP | RESET_FOV | RESET_GAP | RESET_RESIZE)) != 0;
            sc.reset = reset;
            tmp_.resets += reset;
            tmp_.reset_next = false;
            fx_encode(sc, {scene, zt, mv, tmp_.out});
            ++tmp_.scaled;
            return tmp_.out;
        }
        return nil;
    }

    // ---- Remaster run 14 foundation F1-F3 (review/run14/foundation-design.md; state and envs at fnd_) -------------
    // F2 window: 253 opens it (after upscale_begin, so the bb id is the scene backing when upscaling), 252 closes it
    // (before upscale_end: the motion pass runs at the scene size). Without upscaling nothing is swapped.
    void fnd_begin(uint32_t bb, uint32_t ds) {
        if (fnd_.open) {
            ++fnd_.nested;
            return;
        }
        fnd_.open = true;
        fnd_.bb = bb;
        fnd_.ds = ds;
        fnd_.plur.reset();
        fnd_library();   // CONF14B: at the first window, so "motion kernels ready/FAILED" is in every run's log
        fnd_.cam = remaster_fnd::Camera{};
        ++fnd_.windows;
        fnd_.jx = fnd_.jy = 0.0;
        if (fnd_jitter_) {   // F3: Halton(2,3), 8 x f^2 phases; a fixed phase with _JITTER_PHASE
            // v2.5: the scene's real ratio (its backing is in place: scale is the scene's, alt_scale the native),
            // not the launch factor: 1.2x is 12 phases, not 32 (a shorter cycle settles sooner)
            double ratio = upscale_factor_ > 0.0f ? upscale_factor_ : 1.0;
            if (const Tex *t = tex_ptr(bb); t && t->aliased && t->scale > 0.0f && t->alt_scale > t->scale)
                ratio = (double)t->alt_scale / t->scale;
            fnd_.phases = remaster_fnd::jitter_phases(ratio);
            fnd_.phase = fnd_jitter_ >= 1000 ? (uint32_t)(fnd_jitter_ - 1000) % fnd_.phases
                                             : (uint32_t)(fnd_.windows % fnd_.phases);
            remaster_fnd::jitter(fnd_.phase, &fnd_.jx, &fnd_.jy);
        }
    }
    void scene_camera(const float *view, const float *info) override {
        if (atm_on_) {   // run 14 B1 height fog (COMB14B fix): the camera for this frame's view-projection pick
            atm_cam_v_ = remaster_fnd::from_rows(view);
            atm_cam_have_ = true;
        }
        if (!fnd_on_)
            return;
        if (!fnd_.open) {
            ++fnd_.stray_camera;
            return;
        }
        remaster_fnd::Camera &c = fnd_.cam;
        c.have = true;
        c.V = remaster_fnd::from_rows(view);
        c.fov = info[0];
        c.near = info[1];
        c.far = info[2];
        memcpy(&c.flags, &info[3], 4);
        memcpy(&c.mover, &info[4], 4);
        memcpy(&c.frame, &info[5], 4);
    }
    // F1 K: a WORLD draw into the scene back buffer inside the window gives its clip constant block.
    void fnd_capture(const HostD9Draw &d, uint64_t vs_key, const d9sh::Program &vp) {
        if (!pass_color_id_ || pass_color_id_ != fnd_.bb || !d.label || strncmp(d.label, "IDI_WORLD_FX/", 13))
            return;
        fnd_.plur.world();
        ++fnd_.world_draws;
        auto it = fnd_.regs.find(vs_key);
        if (it == fnd_.regs.end()) {
            uint32_t a = 0;
            it = fnd_.regs.emplace(vs_key, d9msl::vertex_clip_regs(vp, &a) && a < 252 ? (int32_t)a : -1).first;
        }
        if (it->second < 0 || !d.vconst || (uint32_t)it->second + 4 > d.vconst_count) {
            ++fnd_.world_noregs;
            return;
        }
        ++fnd_.world_regs;
        fnd_.plur.add(d.vconst + 4 * it->second);
    }
    // F3 jitter: only for draws into the scene back buffer inside the window (shadow, mirror, reflections, car env,
    // front end and the post chain never get it). One render pixel is 2 / (viewport x pass scale) in NDC.
    void fnd_halfpix(const HostD9Draw &d, float *halfpix) {
        if (!fnd_jitter_ || !pass_color_id_ || pass_color_id_ != fnd_.bb || (tmp_on_ && tmp_.failed))
            return;   // v2.5: no jitter once temporal fell back to spatial for the run (it would shimmer)
        float dx, dy;
        remaster_fnd::jitter_halfpix(fnd_.jx, fnd_.jy, d.viewport[2] * (double)pass_scale_, d.viewport[3] * (double)pass_scale_,
                                     &dx, &dy);
        halfpix[0] += dx;
        halfpix[1] += dy;
        ++fnd_.jittered;
    }
    bool fnd_library() {
        if (fnd_.lib_tried)
            return fnd_.cs[0] != nil;
        fnd_.lib_tried = true;
        NSError *error = nil;
        fnd_.lib = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:kFndSource] options:nil error:&error];
        for (int i = 0; i < 2 && fnd_.lib; ++i) {
            id<MTLFunction> f = [fnd_.lib newFunctionWithName:i ? @"fnd_motion_ms_cs" : @"fnd_motion_cs"];
            fnd_.cs[i] = f ? [mtl_ newComputePipelineStateWithFunction:f error:&error] : nil;
        }
        fnd_.q = [mtl_ newBufferWithLength:9 * 32 * 4 options:MTLResourceStorageModeShared];   // slot 8: discarded
        if (fnd_.q)
            memset(fnd_.q.contents, 0, 9 * 32 * 4);
        fprintf(stderr, "[remaster] foundation: motion kernels %s%s%s\n", fnd_.cs[0] && fnd_.cs[1] ? "ready" : "FAILED",
                error ? ": " : "", error ? error.localizedDescription.UTF8String : "");
        return fnd_.cs[0] != nil && fnd_.cs[1] != nil && fnd_.q != nil;
    }
    id<MTLTexture> fnd_texture(id<MTLTexture> t, MTLPixelFormat f, NSUInteger w, NSUInteger h) {
        if (t && t.width == w && t.height == h)
            return t;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> n = [mtl_ newTextureWithDescriptor:td];
        note_alloc(AS_TEXTURE, n, w, h, 1, 0);
        return n;
    }
    // GPU counters of completed frames into the window sums (and the per-frame TEST line with _LOG).
    void fnd_harvest() {
        if (!fnd_.q)
            return;
        const uint64_t done = completed_.load();
        for (int i = 0; i < 8; ++i) {
            FndSlot &sl = fnd_.slot[i];
            if (!sl.pending || sl.serial > done)
                continue;
            sl.pending = false;
            uint32_t *q = (uint32_t *)fnd_.q.contents + 32 * i;
            const uint64_t sum = (uint64_t)q[3] | (uint64_t)q[4] << 32;
            for (int k = 0; k < 17; ++k)
                if (k != 3 && k != 4 && k != 5)
                    fnd_.q_sum[k] += q[k];
            fnd_.q_sum[3] += sum;
            fnd_.q_max = std::max<uint64_t>(fnd_.q_max, q[5]);
            ++fnd_.q_frames;
            if (fnd_log_ && sl.frame % (uint64_t)fnd_log_ == 0) {
                uint64_t he = 0;
                for (int k = 6; k <= 12; ++k)
                    he += q[k];
                fprintf(stderr,
                        "[remaster] foundation frame %llu: px %u sky %u behind %u mv mean %.5f max %.5f px | reproj n %llu "
                        "<0.1%% %u <0.25%% %u <0.5%% %u <1%% %u <2%% %u <5%% %u >=5%% %u disocc %u off %u edge %u locked %u\n",
                        (unsigned long long)sl.frame, q[0], q[1], q[2], q[0] ? sum / 65536.0 / q[0] : 0.0, q[5] / 65536.0,
                        (unsigned long long)he, q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15], q[16]);
            }
            memset(q, 0, 32 * 4);
        }
    }
    // F1/F3 at the scene end: VP_k, the G/K check, the resets, the motion pass, the debug view, the previous VP.
    void fnd_end() {
        if (!fnd_.open) {
            ++fnd_.stray_end;
            return;
        }
        fnd_.open = false;
        ++fnd_.frames;
        using namespace remaster_fnd;
        Tex *bt = tex_ptr(fnd_.bb);
        // K with G (CONF14B fix): clusters of equal blocks; with the camera, the largest one that is its projection
        M4 vinv{};
        double trans = 0.0;
        const bool have_g = fnd_.cam.have && inverse(fnd_.cam.V, &vinv);
        if (have_g) {
            double c[3];
            camera_position(fnd_.cam.V, c);
            trans = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        }
        const Pick pk = pick_clusters(fnd_.plur, 8, 0.25, have_g ? &vinv : nullptr, trans);
        fnd_.ties += pk.tie;
        fnd_.by_camera += pk.by_camera;
        fnd_.not_largest += pk.by_camera && pk.rank;
        fnd_.clusters_sum += pk.clusters;
        fnd_.distinct_sum += pk.distinct;
        fnd_.share_sum += pk.share;
        if (!pk.valid && fnd_.invalid_logged < 3 && pk.world) {
            ++fnd_.invalid_logged;
            fprintf(stderr, "[remaster] foundation: no VP_k (frame %llu): WORLD draws %u, distinct blocks %u, clusters %u, "
                            "top %u %u %u, camera %d, any cluster with the projection form %d\n",
                    (unsigned long long)(fnd_.frames), pk.world, pk.distinct, pk.clusters, pk.top[0], pk.top[1], pk.top[2],
                    (int)have_g, (int)pk.form_any);
        }
        uint32_t reset = camera_resets(fnd_.cam_prev, fnd_.cam);
        if (!fnd_.had_window)
            reset |= RESET_GAP;
        if (fnd_.resized)
            reset |= RESET_RESIZE;
        fnd_.resized = false;
        M4 vp{}, vp_inv{};
        bool valid = pk.valid && pk.block;
        if (valid) {
            vp = from_const_block(pk.block);
            valid = inverse(vp, &vp_inv);
        }
        // G/K: P = V_g^-1 x VP_k has the projection form, near and the depth direction (FOV / aspect / far: info)
        bool agree = false;
        M4 P{};
        if (valid && have_g && bt && bt->desc.height) {
            {
                P = mul(vinv, vp);
                const ProjCheck pc = check_projection(P, fnd_.cam.fov, (double)bt->desc.width / bt->desc.height,
                                                      fnd_.cam.near, fnd_.cam.far, trans);
                ++fnd_.gk_checked;
                agree = pc.agree();
                if (agree) {   // the measured widescreen scales and far plane (info; CONF14B: 1.2122, 1.6926, ~14980)
                    const double kv = std::fabs(pc.ys) * std::tan(0.5 * fnd_.cam.fov), ar = std::fabs(pc.ys / pc.xs);
                    const bool first = fnd_.gk_agree == 0 || fnd_.kv[0] == 0.0;
                    fnd_.kv[0] = first ? kv : std::min(fnd_.kv[0], kv);
                    fnd_.kv[1] = first ? kv : std::max(fnd_.kv[1], kv);
                    fnd_.ar[0] = first ? ar : std::min(fnd_.ar[0], ar);
                    fnd_.ar[1] = first ? ar : std::max(fnd_.ar[1], ar);
                    fnd_.pfar[0] = first ? pc.far : std::min(fnd_.pfar[0], pc.far);
                    fnd_.pfar[1] = first ? pc.far : std::max(fnd_.pfar[1], pc.far);
                }
                fnd_.off_max = std::max(fnd_.off_max, pc.max_off * pc.tol_off);
                const bool fails[6] = {!pc.form, !pc.fov_ok, !pc.aspect_ok, !pc.near_ok, !pc.far_ok, pc.reversed};
                for (int i = 0; i < 6; ++i)
                    fnd_.gk_fail[i] += fails[i];
                if (agree) {
                    ++fnd_.gk_agree;
                    fnd_.p_last = P;
                    fnd_.have_p = true;
                    fnd_.fov_last = fnd_.cam.fov;
                }
                if (!fnd_.logged_p || (!agree && fnd_.gk_checked - fnd_.gk_agree <= 3)) {
                    fnd_.logged_p = true;
                    fprintf(stderr,
                            "[remaster] foundation: P = V_g^-1 x VP_k (frame %llu, %s): [%.6g %.3g %.3g %.3g | %.3g %.6g %.3g %.3g | "
                            "%.3g %.3g %.8g %.6g | %.3g %.3g %.6g %.3g] fov %.4f (camera %.4f) y %s near %.4g far %.6g "
                            "(camera %.4g %.6g) aspect %.4f; fails form %d fov %d aspect %d near %d far %d reversed %d; "
                            "off-diagonal %.3g of tol %.3g\n",
                            (unsigned long long)fnd_.frames, agree ? "agree" : "DISAGREE", P.m[0][0], P.m[0][1], P.m[0][2],
                            P.m[0][3], P.m[1][0], P.m[1][1], P.m[1][2], P.m[1][3], P.m[2][0], P.m[2][1], P.m[2][2], P.m[2][3],
                            P.m[3][0], P.m[3][1], P.m[3][2], P.m[3][3], pc.fov, fnd_.cam.fov, pc.y_down ? "down" : "up",
                            pc.near, pc.far, fnd_.cam.near, fnd_.cam.far, (double)bt->desc.width / bt->desc.height,
                            fails[0], fails[1], fails[2], fails[3], fails[4], fails[5], pc.max_off * pc.tol_off, pc.tol_off);
                }
            }
        }
        if (valid) {
            ++fnd_.vp_valid;
            fnd_.world_identity += pk.count;   // the WORLD draws whose block is VP_k within float rounding (W = I)
            const double st = self_test(vp);
            fnd_.selftest_max = std::max(fnd_.selftest_max, std::isfinite(st) ? st : 1e30);
            if (!(st < 1e-5))
                ++fnd_.selftest_fail;
            if (fnd_.cam.have && !agree)
                reset |= RESET_DISAGREE;
        } else if (fnd_.cam.have && fnd_.have_p && std::fabs(fnd_.cam.fov - fnd_.fov_last) < 1e-6) {
            vp = mul(fnd_.cam.V, fnd_.p_last);   // design 1 fallback: V_g x P_last at an unchanged FOV
            valid = inverse(vp, &vp_inv);
            fnd_.vp_fallback += valid;
            if (!valid)
                reset |= RESET_VP_INVALID;
        } else
            reset |= RESET_VP_INVALID;
        fnd_.reset = reset;
        if (reset) {
            ++fnd_.resets;
            for (uint32_t i = 0; i < RESET_COUNT; ++i)
                fnd_.reset_why[i] += (reset >> i) & 1u;
        }
        // v2.5 MetalFX temporal: the camera vectors only from a VP_k the camera agrees with (a wrong pick is noise
        // on every pixel without its own vector: the sky, particles)
        fnd_motion(bt, valid && (!tmp_.frame || agree), vp, agree ? &P : nullptr);
        // the previous VP: only at the end of a valid frame (main view only)
        fnd_.prev_valid = valid;
        if (valid)
            fnd_.vp_prev = vp;
        fnd_.prev_cam_valid = agree;
        if (agree)
            fnd_.p_prev = P;
        fnd_.cam_prev = fnd_.cam;
        fnd_.had_window = true;
        fnd_.jx_prev = fnd_.jx;
        fnd_.jy_prev = fnd_.jy;
    }
    // The scene back buffer or depth is redefined or dropped: the window closes without vectors, the history resets.
    void fnd_drop() {
        tmp_.reset_next = true;   // v2.5 MetalFX temporal: no history across a reset / resize
        tmp_.prev.clear();
        fnd_.open = false;
        fnd_.resized = true;
        fnd_.prev_valid = fnd_.prevz_valid = false;
    }
    // F3 the motion pass: camera-only vectors from the scene depth and R = VP_k^-1 x VP_prev (double, CPU).
    void fnd_motion(Tex *bt, bool valid, const remaster_fnd::M4 &vp, const remaster_fnd::M4 *P) {
        using namespace remaster_fnd;
        fnd_harvest();
        Tex *dt = fnd_.ds ? tex_ptr(fnd_.ds) : nullptr;
        id<MTLTexture> z = dt ? (dt->drawn_depth ? dt->drawn_depth : dt->texture) : nil;
        // v2.5 MetalFX temporal: without the camera (the front end's garage, a first frame) the pass still runs
        // with no camera motion, so the draws' own vectors reach the scaler and the frame stays temporal.
        bool cam = true;
        if (!valid || !fnd_.prev_valid) {
            ++fnd_.motion_skip[0];
            fnd_.prevz_valid = false;
            if (!tmp_.frame)
                return;
            cam = false;
        }
        if (!bt || !bt->texture || !z || !fnd_library()) {
            ++fnd_.motion_skip[1];
            fnd_.prevz_valid = false;
            return;
        }
        const NSUInteger w = bt->texture.width, h = bt->texture.height;
        if (z.width != w || z.height != h) {
            ++fnd_.motion_skip[2];   // a fitted depth of another size: no vectors this frame
            fnd_.prevz_valid = false;
            return;
        }
        M4 R = identity(), Rsky;
        if (cam && !reprojection(vp, fnd_.vp_prev, &R)) {
            ++fnd_.motion_skip[3];
            fnd_.prevz_valid = false;
            if (!tmp_.frame)
                return;
            cam = false;
            R = identity();
        }
        Rsky = R;
        if (cam && P && fnd_.prev_cam_valid && fnd_.cam.have && fnd_.cam_prev.have) {   // the sky: rotation only
            M4 rs;
            if (reprojection(mul(rotation_only(fnd_.cam.V), *P), mul(rotation_only(fnd_.cam_prev.V), fnd_.p_prev), &rs))
                Rsky = rs;
        }
        fnd_.motion_tex = fnd_texture(fnd_.motion_tex, MTLPixelFormatRG16Float, w, h);
        // TEST measurement (the per-frame counters and the previous depth): _LOG or debug 8 only (CONF14B: +0.61 ms
        // with them always on at 3840x2160 4x MSAA)
        // v2.5: the panel's live "Show motion vectors" (g_temporal_debug) is debug view 6
        const int dmode = fnd_debug_ ? fnd_debug_ : g_temporal_debug.load(std::memory_order_relaxed) ? 6 : 0;
        const bool measure = fnd_log_ > 0 || dmode == 8;
        for (int i = 0; i < 2 && measure; ++i) {
            id<MTLTexture> o = fnd_.prevz[i];
            fnd_.prevz[i] = fnd_texture(o, MTLPixelFormatR32Float, w, h);
            if (fnd_.prevz[i] != o)
                fnd_.prevz_valid = false;
        }
        const bool dbg = dmode == 6 || dmode == 8;
        if (dbg)
            fnd_.dbg = fnd_texture(fnd_.dbg, MTLPixelFormatRGBA16Float, w, h);
        if (!fnd_.motion_tex || (measure && (!fnd_.prevz[0] || !fnd_.prevz[1])) || (dbg && !fnd_.dbg)) {
            ++fnd_.motion_skip[1];
            return;
        }
        FndMv u{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {   // MSL columns: u.R[4 j + i] = R[i][j]
                u.R[4 * j + i] = (float)R.m[i][j];
                u.Rsky[4 * j + i] = (float)Rsky.m[i][j];
            }
        const bool upscaled = upscale_factor_ > 0.0f && bt->aliased;
        const double hs = upscaled ? bt->scale : 1.0;   // the draw's halfpix (d3d9_metal.mm draw)
        u.size[0] = (float)w;
        u.size[1] = (float)h;
        u.h[0] = (float)((63.0 / 64.0) / (bt->desc.width * hs));
        u.h[1] = (float)(-(63.0 / 64.0) / (bt->desc.height * hs));
        u.jc[0] = (float)fnd_.jx;
        u.jc[1] = (float)fnd_.jy;
        u.jp[0] = (float)fnd_.jx_prev;
        u.jp[1] = (float)fnd_.jy_prev;
        // near/far of the projection the GPU used (COMB14B: P's far is ~15000, camera +0xc0 says 10000)
        const double pn = P && P->m[2][2] > 0.0 ? -P->m[3][2] / P->m[2][2] : 0.0,
                     pf = P && P->m[2][2] > 0.0 && P->m[2][2] < 1.0 ? P->m[3][2] / (1.0 - P->m[2][2]) : 0.0;
        u.zn = pn > 0.0 ? (float)pn : fnd_.cam.have && fnd_.cam.near > 0.0 ? (float)fnd_.cam.near : 0.5f;
        u.zf = pf > u.zn ? (float)pf : fnd_.cam.have && fnd_.cam.far > u.zn ? (float)fnd_.cam.far : 15000.0f;
        u.flags = (measure && fnd_.prevz_valid ? 1u : 0u) | (dmode == 6 ? 2u : 0u) | (dmode == 8 ? 4u : 0u) |
                  (measure ? 8u : 0u) | 16u;   // 16: MSAA depth sample 0 (COMB14B cost: 0.19 vs 0.30 ms at 4K 4x)
        const bool om = tmp_.frame && tmp_.wrote && tmp_.om && tmp_.om.width == w && tmp_.om.height == h;
        if (om)
            u.flags |= 32u;   // v2.5 MetalFX temporal: the draws' own motion where they wrote it
        const int si = (int)(fnd_.frames & 7);
        FndSlot &sl = fnd_.slot[si];
        const bool count = measure && !sl.pending;   // a slot still in flight: this frame goes uncounted
        end_pass();
        begin_frame();
        D9ComputeEnc *ce = [cmd_ computeCommandEncoder];
        const bool ms = z.textureType == MTLTextureType2DMultisample;
        [ce setComputePipelineState:fnd_.cs[ms ? 1 : 0]];
        [ce setTexture:z atIndex:0];
        [ce setTexture:fnd_.motion_tex atIndex:1];
        [ce setTexture:fnd_.prevz[fnd_.prevz_i] atIndex:2];
        [ce setTexture:fnd_.prevz[fnd_.prevz_i ^ 1] atIndex:3];
        [ce setTexture:dbg ? fnd_.dbg : fnd_.motion_tex atIndex:4];
        [ce setBytes:&u length:sizeof u atIndex:0];
        [ce setBuffer:fnd_.q offset:(NSUInteger)(count ? si : 8) * 32 * 4 atIndex:1];
        [ce setTexture:om ? tmp_.om : nil atIndex:5];
        [ce dispatchThreadgroups:MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce endEncoding];
        if (count) {
            sl.pending = true;
            sl.serial = serial_;
            sl.frame = fnd_.frames;
        }
        fnd_.prevz_i ^= 1;
        fnd_.prevz_valid = measure;
        ++fnd_.motion;
        tmp_.motion_ok = true;
        if (dbg)
            fnd_debug_view();
    }
    // TEST ONLY: the debug picture over the scene back buffer (copy_fs, the whole target).
    void fnd_debug_view() {
        HostD9Target target{};
        target.color[0].id = fnd_.bb;
        if (!begin_pass(target, false, nullptr, false, 0, false, 0) || !enc_) {
            end_pass();
            return;
        }
        MTLPixelFormat cf[4] = {pass_color_[0].pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso = utility_pipeline(mix(0xf0d0ull, cf[0]), @"copy_fs", cf, MTLPixelFormatInvalid, true);
        if (pso) {
            const NSUInteger tw = pass_color_[0].width, th = pass_color_[0].height;
            es_ = EncoderState{};
            [enc_ setRenderPipelineState:pso];
            [enc_ setCullMode:MTLCullModeNone];
            [enc_ setViewport:(MTLViewport){0, 0, (double)tw, (double)th, 0, 1}];
            [enc_ setScissorRect:(MTLScissorRect){0, 0, tw, th}];
            QuadIn q{};
            q.rect[0] = -1.0f;
            q.rect[1] = 1.0f;
            q.rect[2] = 1.0f;
            q.rect[3] = -1.0f;
            q.uv[2] = 1.0f;
            q.uv[3] = 1.0f;
            [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
            [enc_ setFragmentTexture:fnd_.dbg atIndex:0];
            uint32_t st[14] = {};
            st[1] = st[2] = st[3] = 3;
            st[5] = st[6] = 1;   // point
            [enc_ setFragmentSamplerState:sampler(st) atIndex:0];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        end_pass();
    }
    // Present: a window left open is closed (counted); a frame without a window breaks the history; the TEST report.
    void fnd_present() {
        if (fnd_.open) {
            ++fnd_.no_end;
            fnd_end();
        }
        if (fnd_.frames == fnd_.last_frames) {   // this frame had no scene window: the next one resets
            fnd_.had_window = false;
            fnd_.prev_valid = fnd_.prevz_valid = false;
        }
        fnd_.last_frames = fnd_.frames;
        const uint64_t presents = ++fnd_.presents;
        if (presents % 600)
            return;
        fnd_harvest();
        using namespace remaster_fnd;
        const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; };
        uint64_t he = 0, h05 = 0;
        for (int k = 6; k <= 12; ++k)
            he += fnd_.q_sum[k];
        for (int k = 6; k <= 8; ++k)
            h05 += fnd_.q_sum[k];
        fprintf(stderr,
                "[remaster] foundation: presents %llu windows %llu frames %llu | VP valid %llu (%.2f%%) fallback %llu ties %llu "
                "self-test fail %llu max %.3g | pick by camera %llu (not the largest cluster %llu), per frame clusters %.1f distinct "
                "%.1f VP share %.1f%% | G/K checked %llu agree %llu (%.2f%%) fail form %llu near %llu reversed %llu; info: fov "
                "%llu aspect %llu far %llu, |P11| tan(fov/2) %.4f-%.4f, |P11/P00| %.4f-%.4f, P far %.0f-%.0f | off-diag max "
                "%.3g | WORLD draws %llu analysed %llu not %llu, W=I %llu (%.2f%%) | "
                "resets %llu: cut %llu jump %llu fov %llu vp %llu disagree %llu gap %llu pause %llu resize %llu nocam %llu | "
                "motion %llu skipped: invalid/no prev %llu no depth %llu size %llu singular %llu | counted frames %llu: px %llu "
                "mv mean %.5f max %.5f px, reproj <0.5%% %.2f%% of %llu (disocc %llu off %llu edge %llu; moves with the camera %llu "
                "= %.2f%% of px) | jitter %s phases %u "
                "draws %llu | must be 0: nested %llu stray_end %llu stray_camera %llu no_end %llu\n",
                (unsigned long long)presents, (unsigned long long)fnd_.windows, (unsigned long long)fnd_.frames,
                (unsigned long long)fnd_.vp_valid, pct(fnd_.vp_valid, fnd_.frames), (unsigned long long)fnd_.vp_fallback,
                (unsigned long long)fnd_.ties, (unsigned long long)fnd_.selftest_fail, fnd_.selftest_max,
                (unsigned long long)fnd_.by_camera, (unsigned long long)fnd_.not_largest,
                fnd_.frames ? (double)fnd_.clusters_sum / fnd_.frames : 0.0,
                fnd_.frames ? (double)fnd_.distinct_sum / fnd_.frames : 0.0,
                fnd_.frames ? 100.0 * fnd_.share_sum / fnd_.frames : 0.0, (unsigned long long)fnd_.gk_checked,
                (unsigned long long)fnd_.gk_agree, pct(fnd_.gk_agree, fnd_.gk_checked), (unsigned long long)fnd_.gk_fail[0],
                (unsigned long long)fnd_.gk_fail[3], (unsigned long long)fnd_.gk_fail[5], (unsigned long long)fnd_.gk_fail[1],
                (unsigned long long)fnd_.gk_fail[2], (unsigned long long)fnd_.gk_fail[4], fnd_.kv[0], fnd_.kv[1], fnd_.ar[0],
                fnd_.ar[1], fnd_.pfar[0], fnd_.pfar[1], fnd_.off_max, (unsigned long long)fnd_.world_draws, (unsigned long long)fnd_.world_regs,
                (unsigned long long)fnd_.world_noregs, (unsigned long long)fnd_.world_identity,
                pct(fnd_.world_identity, fnd_.world_draws), (unsigned long long)fnd_.resets,
                (unsigned long long)fnd_.reset_why[0], (unsigned long long)fnd_.reset_why[1],
                (unsigned long long)fnd_.reset_why[2], (unsigned long long)fnd_.reset_why[3],
                (unsigned long long)fnd_.reset_why[4], (unsigned long long)fnd_.reset_why[5],
                (unsigned long long)fnd_.reset_why[6], (unsigned long long)fnd_.reset_why[7],
                (unsigned long long)fnd_.reset_why[8], (unsigned long long)fnd_.motion,
                (unsigned long long)fnd_.motion_skip[0], (unsigned long long)fnd_.motion_skip[1],
                (unsigned long long)fnd_.motion_skip[2], (unsigned long long)fnd_.motion_skip[3],
                (unsigned long long)fnd_.q_frames, (unsigned long long)fnd_.q_sum[0],
                fnd_.q_sum[0] ? fnd_.q_sum[3] / 65536.0 / (double)fnd_.q_sum[0] : 0.0, fnd_.q_max / 65536.0,
                pct(h05, he), (unsigned long long)he, (unsigned long long)fnd_.q_sum[13], (unsigned long long)fnd_.q_sum[14],
                (unsigned long long)fnd_.q_sum[15], (unsigned long long)fnd_.q_sum[16], pct(fnd_.q_sum[16], fnd_.q_sum[0]),
                !fnd_jitter_ ? "off" : fnd_jitter_ >= 1000 ? "fixed" : "Halton(2,3)", fnd_.phases,
                (unsigned long long)fnd_.jittered, (unsigned long long)fnd_.nested, (unsigned long long)fnd_.stray_end,
                (unsigned long long)fnd_.stray_camera, (unsigned long long)fnd_.no_end);
        // the GPU counters are per report window; the rest is cumulative
        memset(fnd_.q_sum, 0, sizeof fnd_.q_sum);
        fnd_.q_frames = fnd_.q_max = 0;
    }

    // Textures by id without hashing: ids are small, and map nodes stay put.
    void index_texture(uint32_t id, Tex *t) {
        if (id >= (1u << 20))
            return;
        if (tex_index_.size() <= id)
            tex_index_.resize(id + 1024, nullptr);
        tex_index_[id] = t;
    }
    Tex *tex_ptr(uint32_t id) {
        if (id < tex_index_.size())
            return tex_index_[id];
        if (!id)
            return nullptr;
        auto it = textures_.find(id);
        return it != textures_.end() ? &it->second : nullptr;
    }

    id<MTLTexture> texture_for(const HostD9Surface &s) {
        if (!s.id)
            return nil;
        Tex *t = tex_ptr(s.id);
        return t ? t->texture : nil;
    }
    // A 2D view of one face and level, for sampling a surface as a texture.
    id<MTLTexture> view_for(const HostD9Surface &s) {
        id<MTLTexture> t = texture_for(s);
        if (!t || t.textureType == MTLTextureType3D)
            return nil;
        if (t.textureType == MTLTextureType2D && s.level == 0 && t.mipmapLevelCount == 1)
            return t;
        return [t newTextureViewWithPixelFormat:t.pixelFormat
                                    textureType:MTLTextureType2D
                                         levels:NSMakeRange(s.level, 1)
                                         slices:NSMakeRange(s.face, 1)];
    }

    bool same_target(const HostD9Target &t) const {
        return enc_ && memcmp(&t, &pass_target_, sizeof t) == 0;
    }
    bool ensure_pass(const HostD9Target &t) {
        if (same_target(t))
            return true;
        end_pass();
        return begin_pass(t, false, nullptr, false, 0, false, 0);
    }
    bool begin_pass(const HostD9Target &t, bool clear_color, const float *rgba, bool clear_depth,
                    float z, bool clear_stencil, uint32_t stencil) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        bool any = false;
        pass_samples_ = 0;
        pass_fitted_depth_ = false;
        for (int i = 0; i < 4; ++i) {
            pass_color_[i] = nil;
            Tex *ct = t.color[i].id ? tex_ptr(t.color[i].id) : nullptr;
            id<MTLTexture> tex = ct ? ct->texture : nil;
            if (!tex || ct->depth || ct->desc.kind == HOST_D9_TEX_VOLUME)
                continue;
            // Multisampled only as a whole surface; every attachment of a
            // pass has one sample count, the first one's.
            const bool ms = ct->msaa && t.color[i].level == 0 && t.color[i].face == 0;
            const uint32_t samples = ms ? ct->samples : 1;
            if (pass_samples_ && samples != pass_samples_)
                continue;
            pass_samples_ = samples;
            pass_color_[i] = tex;
            ct->used = serial_ + (cmd_ ? 0 : 1);
            if (!up_.open)
                ct->outside_frame = presents_ + 1;
            if (ms) {
                rp.colorAttachments[i].texture = ct->msaa;
                rp.colorAttachments[i].resolveTexture = tex;
                rp.colorAttachments[i].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            } else {
                rp.colorAttachments[i].texture = tex;
                rp.colorAttachments[i].level = t.color[i].level;
                rp.colorAttachments[i].slice = t.color[i].face;
                rp.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
            if (i == 0 && clear_color) {
                rp.colorAttachments[i].loadAction = MTLLoadActionClear;
                rp.colorAttachments[i].clearColor =
                    MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
            } else {
                rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
            }
            any = true;
        }
        if (!pass_samples_)
            pass_samples_ = 1;
        // v2.5 MetalFX temporal: a pass on the scene back buffer gets the motion target as colour 1 (cleared to
        // "no motion" at the scene's first pass and with any whole clear of the frame; loaded after).
        pass_motion_ = false;
        if (tmp_.frame && up_.open && t.color[0].id == up_.bb && !t.color[1].id && t.color[0].level == 0 &&
            pass_samples_ == 1 && pass_color_[0] && tmp_.om && tmp_.om.width == pass_color_[0].width &&
            tmp_.om.height == pass_color_[0].height) {
            rp.colorAttachments[1].texture = tmp_.om;
            rp.colorAttachments[1].loadAction = tmp_.cleared && !clear_color ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.colorAttachments[1].clearColor = MTLClearColorMake(60000.0, 0.0, 0.0, 0.0);
            rp.colorAttachments[1].storeAction = MTLStoreActionStore;
            pass_color_[1] = tmp_.om;
            tmp_.cleared = tmp_.wrote = true;
            pass_motion_ = true;
        }
        pass_color_id_ = pass_color_[0] ? t.color[0].id : 0; // Test86
        pass_depth_ = nil;
        Tex *dt = t.depth.id ? tex_ptr(t.depth.id) : nullptr;
        id<MTLTexture> dz = dt ? dt->texture : nil;
        if (dz && pass_samples_ > 1)
            dz = (dt->msaa && dt->samples == pass_samples_) ? dt->msaa : nil;
        bool depth_unmatched = false;
        if (!dz && dt && dt->texture && pass_samples_ > 1 && pass_color_[0]) {
            pass_fitted_depth_ = true;
            // A single-sampled depth buffer under a multisampled target: the
            // pass gets a depth buffer of its own that matches.
            dz = depth_for_size(dt->texture, pass_color_[0], 0, pass_samples_);
            depth_unmatched = true;
        }
        if (dz && dz.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && pass_color_[0] &&
            dz.width >= pass_color_[0].width >> t.color[0].level &&
            dz.height >= pass_color_[0].height >> t.color[0].level) {
            pass_depth_ = dz;
            if (dt)
                dt->used = serial_ + (cmd_ ? 0 : 1);
            (void)depth_unmatched;
            rp.depthAttachment.texture = dz;
            rp.depthAttachment.loadAction = clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = z;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = dz;
            rp.stencilAttachment.loadAction =
                clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = stencil & 0xff;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!any)
            return false;
        // Metal requires every attachment to be the same size.
        if (pass_depth_ &&
            (pass_depth_.width !=
                 std::max<NSUInteger>(pass_color_[0].width >> t.color[0].level, 1) ||
             pass_depth_.height !=
                 std::max<NSUInteger>(pass_color_[0].height >> t.color[0].level, 1))) {
            id<MTLTexture> fit =
                depth_for_size(pass_depth_, pass_color_[0], t.color[0].level, pass_samples_);
            pass_depth_ = fit;
            pass_fitted_depth_ = true;
            rp.depthAttachment.texture = fit;
            rp.stencilAttachment.texture = fit;
        }
        if (pass_depth_ && dt)
            dt->drawn_depth = pass_depth_; // Test121
        if (pass_fitted_depth_ && up_.open && ((up_.ds && t.depth.id == up_.ds) || t.color[0].id == up_.bb))
            ++up_.fitted; // Remaster run 13: a scene backing paired with a target of another size (must stay 0)
        begin_frame();
        pass_vis_ = false;
        pass_slot_ = -1;
        if (query_) {
            rp.visibilityResultBuffer = vis_buffer();
            pass_vis_ = true;
        }
        enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
        ++perf_passes_;
        if (trace_) {
            ++pass_serial_;
            if (enc_) {
                if (clear_color && pass_color_[0])
                    textures_[t.color[0].id].written = serial_;
                if ((clear_depth || clear_stencil) && pass_depth_ && !pass_fitted_depth_)
                    textures_[t.depth.id].written = serial_;
            }
            if (auto *e = trace_->event("pass", trace_context())) {
                for (int i = 0; i < 4; ++i)
                    e->values[i] = t.color[i].id;
                e->values[4] = t.depth.id;
                e->values[5] = pass_fitted_depth_;
                e->values[6] = pass_samples_;
                e->values[7] = clear_color;
                e->values[8] = clear_depth;
                e->values[9] = clear_stencil;
                e->values[10] = enc_ != nil;
            }
        }
        es_ = EncoderState{};
        [enc_ setFrontFacingWinding:MTLWindingClockwise];
        pass_target_ = t;
        pass_scale_ = textures_[t.color[0].id].scale;
        return enc_ != nil;
    }
    void end_pass() {
        if (enc_) {
            [enc_ endEncoding];
            enc_ = nil;
        }
        pass_vis_ = false;
        pass_slot_ = -1;
        memset(&pass_target_, 0, sizeof pass_target_);
    }
    // Direct3D allows a depth buffer larger than the colour target; Metal does
    // not. A draw into a smaller target gets a depth buffer of its own size.
    // Test155 (owner sessions 3 Oct 19:54, 20:49, 21:14): at rain onset the game's footprint grew ~13 GB/s (about
    // 265 MB a frame at ~50 fps, 3840x2160 with 4x MSAA) until macOS killed it; all four draw calls were guarded and
    // refused nothing. Every Metal resource this backend creates is counted by call site; one of 64 MB or more is
    // logged, and when the device's allocated size grows by more than 1 GB within two seconds the log gets the
    // growth by site with each site's last descriptor and the pass it was made for. A counter read and an add per
    // allocation; one property read per presented frame.
    enum AllocSite { AS_TEXTURE, AS_MSAA, AS_READBACK, AS_BUFFER, AS_BUFFER_SPARE, AS_KEPT, AS_FITTED_DEPTH,
                     AS_TRANSIENT, AS_COUNT };
    struct AllocStat {
        uint64_t bytes = 0, count = 0, seen_bytes = 0, seen_count = 0;
        char last[160] = {};
    };
    static const char *alloc_site_name(int s) {
        static const char *n[] = {"texture",       "msaa colour", "readback buffer", "buffer",
                                  "buffer spare",  "kept frame",  "fitted depth",    "transient upload"};
        return s >= 0 && s < AS_COUNT ? n[s] : "?";
    }
    void note_alloc(AllocSite site, id<MTLResource> r, uint64_t w, uint64_t h, uint64_t samples, uint64_t format) {
        if (!r)
            return;
        const uint64_t n = (uint64_t)r.allocatedSize;
        AllocStat &a = alloc_[site];
        a.bytes += n;
        ++a.count;
        snprintf(a.last, sizeof a.last,
                 "%llux%llu format %llu x%llu samples, %llu MB, for pass target %u (frame %llu)",
                 (unsigned long long)w, (unsigned long long)h, (unsigned long long)format,
                 (unsigned long long)samples, (unsigned long long)(n >> 20), pass_color_id_,
                 (unsigned long long)serial_);
        if (n >= (64ull << 20)) {
            ++alloc_big_;
            if (alloc_big_ <= 16 || (alloc_big_ & (alloc_big_ - 1)) == 0)
                fprintf(stderr, "d3d9 metal: large allocation (%s): %s (%llu large so far)\n", alloc_site_name(site),
                        a.last, (unsigned long long)alloc_big_);
        }
    }
    void alloc_watch() {
        const uint64_t cur = (uint64_t)[mtl_ currentAllocatedSize];
        const double now = CACurrentMediaTime();
        // Test157c diagnostics (RECOMP_D3D9_STATS): every 5 s, the renderer's live resources, so a growing count can
        // be followed to its owner (the review's 20-reset discriminator).
        static const bool stats = recomp_env("D3D9_STATS") != nullptr;
        static double stats_t = 0;
        if (stats && now - stats_t >= 5.0) {
            stats_t = now;
            fprintf(stderr, "d3d9 metal stats: device allocated %llu MB, textures %zu, buffers %zu, fitted depth %zu "
                            "(frame %llu)\n",
                    (unsigned long long)(cur >> 20), textures_.size(), buffers_.size(), fitted_depth_.size(),
                    (unsigned long long)serial_);
        }
        if (alloc_t0_ == 0 || now - alloc_t0_ > 2.0) {
            if (alloc_t0_ == 0)
                fprintf(stderr, "d3d9 metal: allocation watch on (device allocated %llu MB)\n",
                        (unsigned long long)(cur >> 20));
            alloc_t0_ = now;
            alloc_base_ = cur;
            for (AllocStat &a : alloc_) {
                a.seen_bytes = a.bytes;
                a.seen_count = a.count;
            }
            return;
        }
        if (cur < alloc_base_ + (1ull << 30))
            return;
        if (++alloc_lines_ <= 48) {
            fprintf(stderr, "d3d9 metal: ALLOCATION GROWTH device allocated %llu MB, +%llu MB in %.2f s (frame %llu)\n",
                    (unsigned long long)(cur >> 20), (unsigned long long)((cur - alloc_base_) >> 20), now - alloc_t0_,
                    (unsigned long long)serial_);
            for (int i = 0; i < AS_COUNT; ++i) {
                const AllocStat &a = alloc_[i];
                if (a.count > a.seen_count)
                    fprintf(stderr, "d3d9 metal:   %s: +%llu MB in %llu allocations; last %s\n", alloc_site_name(i),
                            (unsigned long long)((a.bytes - a.seen_bytes) >> 20),
                            (unsigned long long)(a.count - a.seen_count), a.last);
            }
            fprintf(stderr, "d3d9 metal:   fitted depth cache %zu entries, textures %zu, buffers %zu\n",
                    fitted_depth_.size(), textures_.size(), buffers_.size());
        }
        alloc_t0_ = now;
        alloc_base_ = cur;
        for (AllocStat &a : alloc_) {
            a.seen_bytes = a.bytes;
            a.seen_count = a.count;
        }
    }

    // Test157b (review after the rain incident): fitted depth buffers were never evicted, so a source depth
    // texture that was dropped or redefined left its fitted buffer behind for the rest of the session (and its
    // pointer could be reused by a new texture and hit the stale entry). They now go with their source.
    void forget_fitted_depth(const Tex &t) {
        if (fitted_depth_.empty())
            return;
        const uintptr_t a = (uintptr_t)(__bridge void *)t.texture, b = (uintptr_t)(__bridge void *)t.msaa;
        for (auto it = fitted_depth_.begin(); it != fitted_depth_.end();)
            if (it->second.source && (it->second.source == a || it->second.source == b))
                it = fitted_depth_.erase(it);
            else
                ++it;
        g_fitted_depth_entries = fitted_depth_.size();
    }

    id<MTLTexture> depth_for_size(id<MTLTexture> depth, id<MTLTexture> color, uint32_t level,
                                  uint32_t samples = 1) {
        NSUInteger w = std::max<NSUInteger>(color.width >> level, 1),
                   h = std::max<NSUInteger>(color.height >> level, 1);
        uint64_t key = mix(mix(mix((uint64_t)(__bridge void *)depth, w), h), samples);
        auto it = fitted_depth_.find(key);
        if (it != fitted_depth_.end())
            return it->second.texture;
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth.pixelFormat
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        if (samples > 1) {
            td.textureType = MTLTextureType2DMultisample;
            td.sampleCount = samples;
        }
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; // Test121
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [mtl_ newTextureWithDescriptor:td];
        note_alloc(AS_FITTED_DEPTH, t, w, h, samples, (uint64_t)depth.pixelFormat);
        fitted_depth_[key] = FittedDepth{(uintptr_t)(__bridge void *)depth, t};
        g_fitted_depth_entries = fitted_depth_.size();
        return t;
    }

    // v2.4 (upload above): the new contents of one level/face of a texture the GPU still reads, copied into a
    // staging buffer and blitted in the current command buffer, after the draws that read the old contents (the
    // queue keeps encoders and command buffers in order, and the texture's hazard tracking orders the write after
    // those reads). No CPU wait, no extra commit. `row` bytes per row (per block row for compressed), `rows` rows.
    uint64_t staged_uploads_ = 0, settle_waits_ = 0;
    void staged_upload(Tex &t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, const uint8_t *src,
                       uint32_t row, uint32_t rows) {
        id<MTLBuffer> staging = [mtl_ newBufferWithBytes:src length:(NSUInteger)row * rows
                                                 options:MTLResourceStorageModeShared |
                                                         MTLResourceCPUCacheModeWriteCombined];
        if (!staging) {   // out of memory: the old way
            settle(t.used);
            [t.texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:face withBytes:src
                         bytesPerRow:row bytesPerImage:0];
            return;
        }
        end_pass();
        begin_frame();
        D9BlitEnc *blit = [cmd_ blitCommandEncoder];
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:row
            sourceBytesPerImage:(NSUInteger)row * rows
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:t.texture
               destinationSlice:face
               destinationLevel:level
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        t.used = serial_;
        ++staged_uploads_;
        // The command buffer keeps its staging buffers until it completes, and a movie uploads a frame after frame with
        // no Present to commit them (the owner's first run: 3.4 GB at the intro). Past 32 MB the buffer is committed
        // (no wait); the next draw opens a new one.
        staged_bytes_ += (uint64_t)row * rows;
        if (staged_bytes_ > (32ull << 20))
            flush();
    }
    uint64_t staged_bytes_ = 0;   // staging bytes held by the open command buffer (flush resets it)

    void begin_frame() {
        if (cmd_)
            return;
        {
            const double t0 = CACurrentMediaTime();
            dispatch_semaphore_wait(frames_, DISPATCH_TIME_FOREVER);
            gpu_wait_s_ += CACurrentMediaTime() - t0;
        }
        cmd_ = new_cmd();
        ++serial_;
        dispatch_semaphore_t sem = frames_;
        uint64_t serial = serial_;
        std::atomic<uint64_t> *done = &completed_;
        std::atomic<uint64_t> *gpu_us = &gpu_us_;
        std::atomic<uint64_t> *perf_gpu = &perf_gpu_us_;
        [cmd_ addDone:^(NSError *error, double gpu_s) {
          if (gpu_s > 0.0) {
              const uint64_t us = (uint64_t)(gpu_s * 1e6);
              gpu_us->fetch_add(us);
              perf_gpu->fetch_add(us, std::memory_order_relaxed);
              g_host_game_gpu_us.fetch_add(us, std::memory_order_relaxed); // Test83: the overlay's GPU
          }
          if (error)
              fprintf(stderr, "d3d9 metal: command buffer failed: %s\n",
                      error.localizedDescription.UTF8String);
          done->store(serial);
          dispatch_semaphore_signal(sem);
        }];
    }
    void flush() {
        if (!cmd_)
            return;
        end_pass();
        if (vis_) {
            vis_pool_.emplace_back(vis_, serial_);
            vis_ = nil;
        }
        [cmd_ commit];
        staged_bytes_ = 0;
        if (trace_) {
            submitted_ = serial_;
            trace_->event("submit", trace_context());
        }
        last_ = cmd_;
        cmd_ = nil;
        in_flight_ = true;
    }
    void wait() {
        if (last_) {
            const double t0 = CACurrentMediaTime();
            [last_ waitUntilCompleted];
            gpu_wait_s_ += CACurrentMediaTime() - t0;
            last_ = nil;
        }
        in_flight_ = false;
    }
    // Everything up to frame `used` has run, so the resource can be touched.
    void settle(uint64_t used) {
        if (!used || used <= completed_.load())
            return;
        ++settle_waits_;   // v2.4: reported with the staged uploads (present)
        end_pass();
        if (cmd_ && used >= serial_)
            flush();
        wait();
    }

    void set_scissor(bool on, const int32_t sc[4]) {
        NSUInteger tw =
            pass_color_[0]
                ? std::max<NSUInteger>(pass_color_[0].width >> pass_target_.color[0].level, 1)
                : 1;
        NSUInteger th =
            pass_color_[0]
                ? std::max<NSUInteger>(pass_color_[0].height >> pass_target_.color[0].level, 1)
                : 1;
        MTLScissorRect r = {0, 0, tw, th};
        if (on) {
            const float s = pass_scale_;
            int32_t x0 = std::max(0, (int32_t)std::lround(sc[0] * s)),
                    y0 = std::max(0, (int32_t)std::lround(sc[1] * s));
            int32_t x1 = std::min((int32_t)tw, (int32_t)std::lround(sc[2] * s));
            int32_t y1 = std::min((int32_t)th, (int32_t)std::lround(sc[3] * s));
            if (x1 <= x0 || y1 <= y0) {
                x0 = y0 = 0;
                x1 = y1 = 0;
            }
            r = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)std::max(0, x1 - x0),
                 (NSUInteger)std::max(0, y1 - y0)};
        }
        if (!es_.scissor_valid || memcmp(&r, &es_.scissor, sizeof r) != 0) {
            [enc_ setScissorRect:r];
            es_.scissor = r;
            es_.scissor_valid = true;
        }
    }

    // ---- shaders --------------------------------------------------------
    // Run 15 (hitch): variants the main draw chain can draw plain when their shader is not ready
    // (each listed field has a fallback there that drops it). Pools, grass and the rest never defer.
    static bool pixel_deferrable(const d9msl::PixelVariant &v) {
        return (v.smoke || v.parallax || v.glass || v.sky || v.fefloor || v.roadsheen || v.water || v.pbrworld ||
                v.pbrprobe || v.motion) &&
               !v.pool && !v.grass && !v.grass_blade && !v.carpaint;
    }
    static bool vertex_deferrable(const d9msl::PixelVariant &v, const d9msl::VertexVariant &vv) {
        return ((vv.smoke && v.smoke) || (vv.glass && v.glass) || vv.sky || vv.carenv || (vv.fefloor && v.fefloor) ||
                (vv.pbr && (v.water || v.pbrworld)) || vv.motion) &&
               !vv.pool && !vv.grass && !vv.grass_blade;
    }
    id<MTLFunction> function(uint64_t code_key, const d9sh::Program &p,
                             const d9msl::PixelVariant &v, bool pixel,
                             const d9msl::VertexVariant &vv = d9msl::VertexVariant{}) {
        d9msl::PixelVariant vh;   // PLAN_R2 item 10: this draw's output clamp on the float frame (hdr_cls_)
        const bool hx = pixel && hdr_cls_ && !v.hdr;
        if (hx) {
            vh = v;
            vh.hdr = hdr_cls_;
        }
        const d9msl::PixelVariant &vx = hx ? vh : v;
        uint64_t key = mix(code_key, pixel ? vx.key() + 1 : vv.key());   // Remaster run 10: vertex variants keyed
        auto it = functions_.find(key);
        if (it != functions_.end())
            return it->second;
        std::string src, why;
        // Run 15 (hitch): a variant that has a plain fallback in the draw chain (defer_ok_) and is not
        // compiled yet is compiled in the background; this draw gets nil (drawn as today) and the
        // source is kept here until the library is ready, so it is translated once.
        const bool deferrable = defer_ok_ && warm_ && (pixel ? pixel_deferrable(vx) : vertex_deferrable(v, vv));
        auto pend = deferrable ? deferred_src_.find(key) : deferred_src_.end();
        const bool from_pending = pend != deferred_src_.end();
        bool ok = from_pending ? true
                : pixel ? d9msl::pixel_source(p, vx, &src, &why) : d9msl::vertex_source(p, vv, &src, &why);
        if (from_pending)
            src = pend->second;
        if (const char *dir = recomp_env("D3D9_SHADER_DUMP"); dir && ok && !from_pending) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%016llx.msl", dir, (unsigned long long)key);
            if (FILE *f = fopen(path, "w")) {
                fputs(src.c_str(), f);
                fclose(f);
            }
        }
        id<MTLFunction> fn = nil;
        if (ok) {
            NSError *error = nil;
            std::string warm_key;
            id<MTLLibrary> lib = nil;
            bool from_background = false;
            if (warm_) {
                warm_key = warm_->key(src);
                // Run 15 (hitch): one source can reach here under several function keys (variant fields the
                // program does not use, e.g. masks of unsampled stages); a library built once is reused.
                if (auto same = libs_by_src_.find(warm_key); same != libs_by_src_.end())
                    lib = same->second;
            }
            if (warm_ && !lib) {
                if (deferrable) {
                    bool pending = false;
                    lib = warm_->request(warm_key, src, &pending);
                    if (pending) {
                        if (!from_pending) {
                            deferred_src_[key] = src;
                            if (++deferred_logged_ <= 64)
                                fprintf(stderr, "d3d9 metal: shader deferred: %s %016llx (warm %.12s, %zu bytes) compiles in the "
                                                "background, drawn plain meanwhile (%u so far)\n",
                                        pixel ? "ps" : "vs", (unsigned long long)key, warm_key.c_str(), src.size(),
                                        deferred_logged_);
                        }
                        return nil;   // not cached: asked again next draw
                    }
                    from_background = lib != nil;
                    deferred_src_.erase(key);
                } else {
                    const uint64_t take_t0 = os_monotonic_ns();
                    lib = warm_->take(warm_key);
                    // Only a wait on the warm-up's in-flight compile of this source takes measurable time.
                    if (const uint64_t waited = os_monotonic_ns() - take_t0; waited > 500000ull)
                        note_shader_work(waited, true);
                }
            }
            if (!lib) {
                MTLCompileOptions *opts = ShaderWarm::options();
                const uint64_t compile_t0 = os_monotonic_ns();
                lib = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                         options:opts
                                           error:&error];
                const uint64_t compile_ns = os_monotonic_ns() - compile_t0;
                note_shader_work(compile_ns, true);
                ++draw_compiles_;
                if (compile_ns >= 50000000ull)
                    ++draw_compiles_slow_;
                // Run 15 (hitch): which sources stall a frame (the seed must hold them).
                if (compile_ns >= 50000000ull && ++slow_logged_ <= 64)
                    fprintf(stderr, "d3d9 metal: slow shader compile %.1f ms: %s %016llx (warm %.12s, %zu bytes; variant "
                                    "%016llx vertex %016llx)\n",
                            compile_ns / 1e6, pixel ? "ps" : "vs", (unsigned long long)key,
                            warm_key.empty() ? "-" : warm_key.c_str(), src.size(),
                            (unsigned long long)vx.key(), (unsigned long long)vv.key());
                // Run 15 (hitch): wall relief and natural water are default-on now (no longer pilots): their sources are
                // recorded too (they were the last in-play stalls with a full seed: ~210 ms each).
                if (lib && warm_)
                    warm_->record(warm_key, src);
            } else if (from_background) {
                warm_->record(warm_key, src);   // run 15: compiled in the background, recorded as a draw compile is
            }
            if (lib && !warm_key.empty())
                libs_by_src_.emplace(warm_key, lib);
            if (lib)
                fn = [lib newFunctionWithName:pixel ? @"ps_main" : @"vs_main"];
            if (fn)
                fn_libs_[(__bridge const void *)fn] = lib;   // v4 Metal 4: pipelines name the library and function
            if (fn && !warm_key.empty())
                fn_warm_keys_[(__bridge const void *)fn] = warm_key;   // Test70: names the pipeline identity
            else if (!fn) // Test73: a compiled shader without a warm-up cache is not a failure
                fprintf(stderr, "d3d9 metal: a %s shader failed to compile: %s\n",
                        pixel ? "pixel" : "vertex", error.localizedDescription.UTF8String);
        } else {
            char key_text[64];
            snprintf(key_text, sizeof key_text, "d3d9.metal.untranslated.%llx",
                     (unsigned long long)key);
            log_once(key_text, "d3d9 metal: a %s shader is not translated: %s",
                     pixel ? "pixel" : "vertex", why.c_str());
        }
        functions_[key] = fn;
        return fn;
    }

    MTLVertexDescriptor *vertex_descriptor(const HostD9Draw &d, const d9sh::Program &vp,
                                           uint64_t key, float ascale[16][4]) {
        auto it = vdescs_.find(key);
        if (it != vdescs_.end()) {
            memcpy(ascale, it->second.ascale, sizeof it->second.ascale);
            stream_mask_ = it->second.streams;
            vdesc_needs_zero_ = it->second.zero;
            return it->second.desc;
        }
        VDesc v;
        MTLVertexDescriptor *desc = [MTLVertexDescriptor vertexDescriptor];
        uint32_t streams = 0;
        uint32_t stream_end[8] = {};
        bool zero = false;
        for (const d9sh::DclIn &in : vp.inputs) {
            if (in.type != d9sh::R_INPUT)
                continue;
            uint32_t reg = in.index & 15;
            bool found = false;
            for (uint32_t i = 0; i + 8 <= d.decl_size; i += 8) {
                const uint8_t *e = d.decl + i;
                uint16_t stream = (uint16_t)(e[0] | e[1] << 8);
                if (stream == 0xff)
                    break;
                uint16_t offset = (uint16_t)(e[2] | e[3] << 8);
                uint8_t type = e[4], usage = e[6], index = e[7];
                if (usage != in.usage || index != in.usage_index || stream >= 8)
                    continue;
                MTLVertexFormat fmt;
                float scale[4];
                if (!vertex_format(type, &fmt, scale))
                    continue;
                desc.attributes[reg].format = fmt;
                desc.attributes[reg].offset = offset;
                desc.attributes[reg].bufferIndex = stream;
                memcpy(v.ascale[reg], scale, sizeof scale);
                streams |= 1u << stream;
                stream_end[stream] =
                    std::max<uint32_t>(stream_end[stream], offset + vertex_format_bytes(type));
                found = true;
                break;
            }
            if (!found) {
                desc.attributes[reg].format = MTLVertexFormatFloat4;
                desc.attributes[reg].offset = 0;
                desc.attributes[reg].bufferIndex = 8;
                zero = true;
            }
        }
        for (uint32_t s = 0; s < 8; ++s) {
            if (!(streams >> s & 1))
                continue;
            uint32_t stride = d.stream[s].stride ? d.stream[s].stride : stream_end[s];
            desc.layouts[s].stride = std::max(stride, stream_end[s]);
            desc.layouts[s].stepFunction = MTLVertexStepFunctionPerVertex;
        }
        if (zero) {
            desc.layouts[8].stride = 16;
            desc.layouts[8].stepFunction = MTLVertexStepFunctionConstant;
            desc.layouts[8].stepRate = 0;
        }
        v.desc = desc;
        v.streams = streams;
        v.zero = zero;
        for (int r = 0; r < 16; ++r)
            for (int k = 0; k < 4; ++k)
                if (v.ascale[r][k] == 0.0f)
                    v.ascale[r][k] = 1.0f;
        memcpy(ascale, v.ascale, sizeof v.ascale);
        stream_mask_ = streams;
        vdesc_needs_zero_ = zero;
        vdescs_[key] = v;
        return desc;
    }

    id<MTLRenderPipelineState> pipeline(uint64_t key, id<MTLFunction> vfn, id<MTLFunction> ffn,
                                        MTLVertexDescriptor *vdesc, const MTLPixelFormat cf[4],
                                        MTLPixelFormat df, bool blend, const uint32_t *rs,
                                        const uint32_t cw[4]) {
        key = mix(key, 0x5a3f0000ull + pass_samples_);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end())
            return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.rasterSampleCount = std::max<uint32_t>(pass_samples_, 1);
        pd.vertexFunction = vfn;
        pd.fragmentFunction = ffn;
        pd.vertexDescriptor = vdesc;
        for (int i = 0; i < 4; ++i) {
            if (cf[i] == MTLPixelFormatInvalid)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
            a.pixelFormat = cf[i];
            MTLColorWriteMask m = MTLColorWriteMaskNone;
            if (cw[i] & 1)
                m |= MTLColorWriteMaskRed;
            if (cw[i] & 2)
                m |= MTLColorWriteMaskGreen;
            if (cw[i] & 4)
                m |= MTLColorWriteMaskBlue;
            if (cw[i] & 8)
                m |= MTLColorWriteMaskAlpha;
            a.writeMask = m;
            if (blend && !(pass_motion_ && i == 1)) {   // v2.5: the motion target is never blended
                a.blendingEnabled = YES;
                a.sourceRGBBlendFactor = blend_factor(rs[19], false);
                a.destinationRGBBlendFactor = blend_factor(rs[20], false);
                // BOTHSRCALPHA / BOTHINVSRCALPHA override the destination too.
                if (rs[19] == 12)
                    a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                if (rs[19] == 13)
                    a.destinationRGBBlendFactor = MTLBlendFactorSourceAlpha;
                a.rgbBlendOperation = blend_op(rs[171]);
                if (rs[206]) {
                    a.sourceAlphaBlendFactor = blend_factor(rs[207], true);
                    a.destinationAlphaBlendFactor = blend_factor(rs[208], true);
                    a.alphaBlendOperation = blend_op(rs[209]);
                } else {
                    a.sourceAlphaBlendFactor = blend_factor(rs[19], true);
                    a.destinationAlphaBlendFactor = blend_factor(rs[20], true);
                    if (rs[19] == 12)
                        a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    if (rs[19] == 13)
                        a.destinationAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                    a.alphaBlendOperation = blend_op(rs[171]);
                }
            }
        }
        if (df != MTLPixelFormatInvalid) {
            pd.depthAttachmentPixelFormat = df;
            pd.stencilAttachmentPixelFormat = df;
        }
        // Test70: a pipeline the warm-up created from an identical recorded identity is used as is.
        std::string spec;
        if (warm_ && warm_->pipelines_on()) {
            auto vk = fn_warm_keys_.find((__bridge const void *)vfn), fk = fn_warm_keys_.find((__bridge const void *)ffn);
            if (vk != fn_warm_keys_.end() && fk != fn_warm_keys_.end()) {
                const PipeSpec ps = warm_spec_of(pd, vk->second, fk->second);
                if (ps.valid() && warm_formats_ok(ps))
                    spec = ps.text();
            }
            if (!spec.empty())
                if (id<MTLRenderPipelineState> ready = warm_->take_pipeline(spec)) {
                    pipelines_[key] = ready;
                    return ready;
                }
        }
        if (m4_)   // v4 Metal 4: a cheap specialization of the shader pair's one compiled pipeline
            if (@available(macOS 26.0, iOS 26.0, *))
                if (id<MTLRenderPipelineState> m4 = m4_pipeline(pd)) {
                    if (!spec.empty())
                        warm_->record_pipeline(spec);
                    pipelines_[key] = m4;
                    return m4;
                }
        NSError *error = nil;
        const uint64_t pso_t0 = os_monotonic_ns();
        id<MTLRenderPipelineState> pso = [mtl_ newRenderPipelineStateWithDescriptor:pd
                                                                              error:&error];
        note_shader_work(os_monotonic_ns() - pso_t0, false);
        if (!pso)
            fprintf(stderr, "d3d9 metal: pipeline failed: %s\n",
                    error.localizedDescription.UTF8String);
        else if (!spec.empty())
            warm_->record_pipeline(spec);
        pipelines_[key] = pso;
        return pso;
    }

    // v4 Metal 4 flexible pipelines: the classic descriptor `pd` as an MTL4 one. The shader pair is compiled once with
    // every colour attachment's format, blend state, factors, operations and write mask left unspecialized; each real
    // combination is then a specialization of that base (no shader compile). Nil: the caller builds it classically.
    id<MTLRenderPipelineState> m4_pipeline(MTLRenderPipelineDescriptor *pd) API_AVAILABLE(macos(26.0), ios(26.0)) {
        D9M4 *m4 = (D9M4 *)m4_;
        auto fdesc = [&](id<MTLFunction> f) -> MTL4LibraryFunctionDescriptor * {
            auto it = f ? fn_libs_.find((__bridge const void *)f) : fn_libs_.end();
            if (it == fn_libs_.end())
                return nil;
            MTL4LibraryFunctionDescriptor *d = [MTL4LibraryFunctionDescriptor new];
            d.library = it->second;
            d.name = f.name;
            return d;
        };
        MTL4LibraryFunctionDescriptor *vd = fdesc(pd.vertexFunction), *fd = fdesc(pd.fragmentFunction);
        if (!vd || !fd || !m4.compiler)
            return nil;
        uint32_t cmask = 0;
        for (int i = 0; i < 4; ++i)
            if (pd.colorAttachments[i].pixelFormat != MTLPixelFormatInvalid)
                cmask |= 1u << i;
        auto descriptor = [&](bool specialized) {
            MTL4RenderPipelineDescriptor *d = [MTL4RenderPipelineDescriptor new];
            d.vertexFunctionDescriptor = vd;
            d.fragmentFunctionDescriptor = fd;
            d.vertexDescriptor = pd.vertexDescriptor;
            d.rasterSampleCount = pd.rasterSampleCount;
            for (int i = 0; i < 4; ++i) {
                if (!(cmask >> i & 1))
                    continue;
                MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[i];
                MTL4RenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
                if (!specialized) {
                    a.pixelFormat = MTLPixelFormatUnspecialized;
                    a.writeMask = MTLColorWriteMaskUnspecialized;
                    a.blendingState = MTL4BlendStateUnspecialized;
                    a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = MTLBlendFactorUnspecialized;
                    a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
                    a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized;
                    continue;
                }
                a.pixelFormat = c.pixelFormat;
                a.writeMask = c.writeMask;
                a.blendingState = c.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
                a.sourceRGBBlendFactor = c.sourceRGBBlendFactor;
                a.destinationRGBBlendFactor = c.destinationRGBBlendFactor;
                a.sourceAlphaBlendFactor = c.sourceAlphaBlendFactor;
                a.destinationAlphaBlendFactor = c.destinationAlphaBlendFactor;
                a.rgbBlendOperation = c.rgbBlendOperation;
                a.alphaBlendOperation = c.alphaBlendOperation;
            }
            return d;
        };
        // The vertex layout by its contents: pd.vertexDescriptor is pd's own copy, freed with pd, so its address came
        // back for other layouts and a draw got a base reading its vertices wrongly (v4: the "lasers from the sky").
        uint64_t vkey = 0;
        if (MTLVertexDescriptor *v = pd.vertexDescriptor) {
            for (NSUInteger i = 0; i < 16; ++i) {
                MTLVertexAttributeDescriptor *a = v.attributes[i];
                vkey = mix(vkey, (uint64_t)a.format << 48 | (uint64_t)a.bufferIndex << 32 | a.offset);
            }
            for (NSUInteger i = 0; i < 9; ++i) {
                MTLVertexBufferLayoutDescriptor *l = v.layouts[i];
                vkey = mix(vkey, (uint64_t)l.stepFunction << 56 | (uint64_t)l.stepRate << 32 | l.stride);
            }
        }
        uint64_t bkey = mix(mix((uint64_t)(uintptr_t)(__bridge const void *)pd.vertexFunction,
                                (uint64_t)(uintptr_t)(__bridge const void *)pd.fragmentFunction),
                            vkey);
        bkey = mix(mix(bkey, pd.rasterSampleCount), cmask);
        NSError *error = nil;
        auto bit = m4_bases_.find(bkey);
        if (bit == m4_bases_.end()) {
            const uint64_t t0 = os_monotonic_ns();
            id<MTLRenderPipelineState> base = [m4.compiler newRenderPipelineStateWithDescriptor:descriptor(false)
                                                                            compilerTaskOptions:nil
                                                                                          error:&error];
            const uint64_t ns = os_monotonic_ns() - t0;
            note_shader_work(ns, false);
            m4_base_ns_ += ns;
            ++m4_base_n_;
            if (!base && ++m4_fail_n_ <= 4)
                fprintf(stderr, "[gpu] Metal 4 pipeline base failed (classic pipeline instead): %s\n",
                        error.localizedDescription.UTF8String);
            bit = m4_bases_.emplace(bkey, base).first;
        }
        if (!bit->second)
            return nil;
        const uint64_t t0 = os_monotonic_ns();
        id<MTLRenderPipelineState> p = [m4.compiler newRenderPipelineStateBySpecializationWithDescriptor:descriptor(true)
                                                                                                  pipeline:bit->second
                                                                                                     error:&error];
        const uint64_t ns = os_monotonic_ns() - t0;
        note_shader_work(ns, false);
        m4_spec_ns_ += ns;
        ++m4_spec_n_;
        if (!p && ++m4_fail_n_ <= 4)
            fprintf(stderr, "[gpu] Metal 4 pipeline specialization failed (classic pipeline instead): %s\n",
                    error.localizedDescription.UTF8String);
        return p;
    }
    id<MTLRenderPipelineState> utility_pipeline(uint64_t key, NSString *fragment,
                                                const MTLPixelFormat cf[4], MTLPixelFormat df,
                                                bool write_color) {
        key = mix(key, 0x5a3f0000ull + pass_samples_);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end())
            return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.rasterSampleCount = std::max<uint32_t>(pass_samples_, 1);
        pd.vertexFunction = [utility_ newFunctionWithName:@"quad_vs"];
        pd.fragmentFunction = [utility_ newFunctionWithName:fragment];
        for (int i = 0; i < 4; ++i)
            if (cf[i] != MTLPixelFormatInvalid) {
                pd.colorAttachments[i].pixelFormat = cf[i];
                pd.colorAttachments[i].writeMask =
                    (i == 0 && write_color) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            }
        if (df != MTLPixelFormatInvalid) {
            pd.depthAttachmentPixelFormat = df;
            pd.stencilAttachmentPixelFormat = df;
        }
        NSError *error = nil;
        id<MTLRenderPipelineState> pso = [mtl_ newRenderPipelineStateWithDescriptor:pd
                                                                              error:&error];
        if (!pso)
            fprintf(stderr, "d3d9 metal: utility pipeline failed: %s\n",
                    error.localizedDescription.UTF8String);
        pipelines_[key] = pso;
        return pso;
    }

    id<MTLDepthStencilState> depth_state(uint64_t key, bool zon, const uint32_t *rs, bool stencil) {
        auto it = depth_states_.find(key);
        if (it != depth_states_.end())
            return it->second;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = zon ? compare_fn(rs[23]) : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = zon && rs[14] != 0;
        if (stencil) {
            MTLStencilDescriptor *front = [MTLStencilDescriptor new];
            front.stencilFailureOperation = stencil_op(rs[53]);
            front.depthFailureOperation = stencil_op(rs[54]);
            front.depthStencilPassOperation = stencil_op(rs[55]);
            front.stencilCompareFunction = compare_fn(rs[56]);
            front.readMask = rs[58] & 0xff;
            front.writeMask = rs[59] & 0xff;
            dd.frontFaceStencil = front;
            if (rs[185]) {
                MTLStencilDescriptor *back = [MTLStencilDescriptor new];
                back.stencilFailureOperation = stencil_op(rs[186]);
                back.depthFailureOperation = stencil_op(rs[187]);
                back.depthStencilPassOperation = stencil_op(rs[188]);
                back.stencilCompareFunction = compare_fn(rs[189]);
                back.readMask = rs[58] & 0xff;
                back.writeMask = rs[59] & 0xff;
                dd.backFaceStencil = back;
            } else {
                dd.backFaceStencil = front;
            }
        }
        id<MTLDepthStencilState> s = [mtl_ newDepthStencilStateWithDescriptor:dd];
        depth_states_[key] = s;
        return s;
    }
    id<MTLDepthStencilState> clear_depth_state(bool depth, bool stencil, uint64_t key) {
        auto it = depth_states_.find(key);
        if (it != depth_states_.end())
            return it->second;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = depth;
        if (stencil) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = MTLCompareFunctionAlways;
            s.depthStencilPassOperation = MTLStencilOperationReplace;
            dd.frontFaceStencil = s;
            dd.backFaceStencil = s;
        }
        id<MTLDepthStencilState> st = [mtl_ newDepthStencilStateWithDescriptor:dd];
        depth_states_[key] = st;
        return st;
    }

    // World filtering (host setting, Test72/73): the anisotropy of the game's WORLD texture samplers.
    // Those are the mipmapped (linear mip), linearly or anisotropically filtered samplers with wrapped
    // U and V addressing - roads, walls, terrain. Clamped samplers (render targets, shadow maps,
    // post-process, fonts, HUD, movies), point and unmipmapped samplers keep the game's state.
    // Measured (Test73 mn1, the game's own "anisotropic" texture filtering): the game itself already
    // asks 16x on some world samplers and leaves others trilinear with MAXANISOTROPY 16.
    // 0 = the game's own state; 1 = trilinear (no anisotropy anywhere); 2/3/4 = 4x/8x/16x on every
    // world sampler. Read per lookup (an atomic), so a change takes effect at the next draw with no
    // rebuild; the cache holds at most five variants of each game sampler state.
    static bool world_sampler(const uint32_t *st) {
        const bool filtered = st[6] >= 2 && st[6] <= 3 && st[5] >= 2 && st[5] <= 3;
        return filtered && st[7] == 2 && st[1] == 1 && st[2] == 1;
    }
    static int world_anisotropy(const uint32_t *st, int policy) {
        const int game = (st[6] == 3 || st[5] == 3) ? int(std::max<uint32_t>(1, std::min<uint32_t>(16, st[10]))) : 1;
        if (!policy || !world_sampler(st))
            return game;
        switch (policy) {
        case 1:
            return 1;
        case 2:
            return 4;
        case 3:
            return 8;
        default:
            return 16;
        }
    }
    id<MTLSamplerState> sampler(const uint32_t *st) {
        const int policy = std::clamp(mods_display_filtering(), 0, 4);
        uint64_t key = fnv(st, 14 * sizeof(uint32_t));
        if (policy && world_sampler(st))
            key ^= 0x9e3779b97f4a7c15ull * uint64_t(policy);
        auto it = samplers_.find(key);
        if (it != samplers_.end())
            return it->second;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;   // v4 Metal 4: bound by gpuResourceID in an argument table
        sd.sAddressMode = address_mode(st[1]);
        sd.tAddressMode = address_mode(st[2]);
        sd.rAddressMode = address_mode(st[3]);
        sd.magFilter = st[5] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.minFilter = st[6] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = st[7] == 0   ? MTLSamplerMipFilterNotMipmapped
                       : st[7] == 1 ? MTLSamplerMipFilterNearest
                                    : MTLSamplerMipFilterLinear;
        const int anisotropy = world_anisotropy(st, policy);
        if (anisotropy > 1)
            sd.maxAnisotropy = NSUInteger(anisotropy);
        {
            // Diagnostic (RECOMP_D3D9_STATS): each new sampler state the game asked for, bounded.
            static const bool stats = recomp_env("D3D9_STATS") != nullptr;
            static int logged = 0;
            if (stats && logged < 64) {
                ++logged;
                fprintf(stderr, "d3d9 metal: sampler min %u mag %u mip %u maxaniso %u -> anisotropy %d (world filtering %d) address %u/%u/%u lodmin %u\n",
                        st[6], st[5], st[7], st[10], anisotropy, policy, st[1], st[2], st[3], st[9]);
            }
        }
        sd.lodMinClamp = (float)st[9];
        uint32_t border = st[4];
        sd.borderColor = (border >> 24) < 128  ? MTLSamplerBorderColorTransparentBlack
                         : (border & 0xffffff) ? MTLSamplerBorderColorOpaqueWhite
                                               : MTLSamplerBorderColorOpaqueBlack;
        id<MTLSamplerState> s = [mtl_ newSamplerStateWithDescriptor:sd];
        samplers_[key] = s;
        return s;
    }

    id<MTLTexture> placeholder(bool cube, bool volume = false) {
        __strong id<MTLTexture> &t = volume ? white_volume_ : cube ? white_cube_ : white_;
        if (t)
            return t;
        MTLTextureDescriptor *td =
            cube ? [MTLTextureDescriptor
                       textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                       size:1
                                                  mipmapped:NO]
                 : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                      width:1
                                                                     height:1
                                                                  mipmapped:NO];
        if (volume)
            td.textureType = MTLTextureType3D;
        t = [mtl_ newTextureWithDescriptor:td];
        const uint8_t black[4] = {0, 0, 0, 255};
        for (NSUInteger s = 0; s < (cube ? 6u : 1u); ++s)
            [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                  mipmapLevel:0
                        slice:s
                    withBytes:black
                  bytesPerRow:4
                bytesPerImage:volume ? 4 : 0];
        return t;
    }

    // A depth texture at the far plane: every shadow lookup passes.
    id<MTLTexture> placeholder_depth() {
        if (white_depth_)
            return white_depth_;
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                         width:1
                                        height:1
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        white_depth_ = [mtl_ newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = white_depth_;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        D9Cmd *cb = new_cmd();
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb commit];
        return white_depth_;
    }

    void set_constants(bool vertex, const float *c, uint32_t count, const d9sh::Program &p) {
        // As many as the program reads, so its own defs always land and a
        // read past what the game set is zero rather than past the buffer.
        uint32_t n = std::max<uint32_t>(std::max<uint32_t>(count, p.max_const), 1);
        constants_.assign(n * 4, 0.0f);
        if (c && count)
            memcpy(constants_.data(), c, count * 4 * sizeof(float));
        for (const auto &def : p.defs)
            if (def.first < n)
                memcpy(&constants_[def.first * 4], def.second.data(), 4 * sizeof(float));
        if (vertex)
            [enc_ setVertexBytes:constants_.data()
                          length:constants_.size() * sizeof(float)
                         atIndex:16];
        else
            [enc_ setFragmentBytes:constants_.data()
                            length:constants_.size() * sizeof(float)
                           atIndex:0];
    }

    void bind_inline(int slot, const uint8_t *bytes, uint32_t size) {
        if (size <= 4000) {
            [enc_ setVertexBytes:bytes length:size atIndex:slot];
            return;
        }
        id<MTLBuffer> b = transient(size);
        memcpy((uint8_t *)b.contents + transient_used_ - size, bytes, size);
        [enc_ setVertexBuffer:b offset:transient_used_ - size atIndex:slot];
    }
    // Space in this frame's upload buffer; returns the buffer, with the space
    // ending at transient_used_.
    id<MTLBuffer> transient(uint32_t size) {
        uint32_t aligned = (size + 15) & ~15u;
        if (!transient_ || transient_used_ + aligned > transient_.length) {
            // v4: full ones wait for their frame and come back (a new 4 MB each time was a new resident on Metal 4)
            if (transient_ && transient_.length == (4u << 20) && transient_pool_.size() < 8)
                transient_pool_.push_back({transient_, serial_});
            transient_ = nil;
            const uint64_t done = completed_.load();
            if (aligned <= (4u << 20))
                for (size_t i = 0; i < transient_pool_.size(); ++i)
                    if (transient_pool_[i].retired <= done) {
                        transient_ = transient_pool_[i].buffer;
                        transient_pool_.erase(transient_pool_.begin() + (ptrdiff_t)i);
                        break;
                    }
            if (!transient_) {
                NSUInteger len = std::max<NSUInteger>(4u << 20, aligned);
                transient_ = [mtl_ newBufferWithLength:len options:MTLResourceStorageModeShared];
                note_alloc(AS_TRANSIENT, transient_, len, 1, 1, 0);
            }
            transient_used_ = 0;
        }
        transient_used_ += aligned;
        return transient_;
    }

    d9trace::Result encode_primitives(const HostD9Draw &d) {
        MTLPrimitiveType type;
        uint32_t count = d.primitive_count;
        bool fan = false;
        switch (d.primitive) {
        case 1:
            type = MTLPrimitiveTypePoint;
            break;
        case 2:
            type = MTLPrimitiveTypeLine;
            count *= 2;
            break;
        case 3:
            type = MTLPrimitiveTypeLineStrip;
            count += 1;
            break;
        case 4:
            type = MTLPrimitiveTypeTriangle;
            count *= 3;
            break;
        case 5:
            type = MTLPrimitiveTypeTriangleStrip;
            count += 2;
            break;
        case 6:
            type = MTLPrimitiveTypeTriangle;
            fan = true;
            break;
        default:
            return d9trace::Result::UnsupportedPrimitive;
        }
        if (!d.primitive_count)
            return d9trace::Result::Empty;
        bool indexed = d.index_buffer || d.inline_indices;
        if (!indexed && !fan) {
            [enc_ drawPrimitives:type vertexStart:d.start vertexCount:count];
            return d9trace::Result::Encoded;
        }
        MTLIndexType itype = d.index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
        uint32_t isize = d.index_size == 4 ? 4 : 2;
        if (!fan) {
            if (d.index_buffer) {
                auto b = buffers_.find(d.index_buffer);
                if (b == buffers_.end() || !b->second.buffer)
                    return d9trace::Result::MissingIndexBuffer;
                if ((uint64_t)(d.start + count) * isize > b->second.buffer.length)
                    return d9trace::Result::IndexRange;
                b->second.used = serial_;
                [enc_ drawIndexedPrimitives:type
                                 indexCount:count
                                  indexType:itype
                                indexBuffer:b->second.buffer
                          indexBufferOffset:d.start * isize
                              instanceCount:1
                                 baseVertex:d.base_vertex
                               baseInstance:0];
            } else {
                id<MTLBuffer> b = transient(count * isize);
                memcpy((uint8_t *)b.contents + transient_used_ - ((count * isize + 15) & ~15u),
                       d.inline_indices, count * isize);
                [enc_ drawIndexedPrimitives:type
                                 indexCount:count
                                  indexType:itype
                                indexBuffer:b
                          indexBufferOffset:transient_used_ - ((count * isize + 15) & ~15u)];
            }
            return d9trace::Result::Encoded;
        }
        // A fan becomes a triangle list over the same vertices.
        std::vector<uint32_t> list;
        list.reserve(count * 3);
        auto source = [&](uint32_t i) -> uint32_t {
            if (!indexed)
                return d.start + i;
            const uint8_t *p = nullptr;
            if (d.inline_indices) {
                p = d.inline_indices + (size_t)i * isize;
            } else {
                auto b = buffers_.find(d.index_buffer);
                size_t at = (size_t)(d.start + i) * isize;
                if (b == buffers_.end() || at + isize > b->second.shadow.size())
                    return 0;
                p = b->second.shadow.data() + at;
            }
            uint32_t v = isize == 4
                             ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24)
                             : (uint32_t)(p[0] | p[1] << 8);
            return v + (uint32_t)(d.inline_indices ? 0 : d.base_vertex);
        };
        for (uint32_t i = 0; i < count; ++i) {
            list.push_back(source(0));
            list.push_back(source(i + 1));
            list.push_back(source(i + 2));
        }
        uint32_t bytes = (uint32_t)list.size() * 4;
        id<MTLBuffer> b = transient(bytes);
        NSUInteger off = transient_used_ - ((bytes + 15) & ~15u);
        memcpy((uint8_t *)b.contents + off, list.data(), bytes);
        [enc_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:list.size()
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:b
                  indexBufferOffset:off];
        return d9trace::Result::Encoded;
    }

    void dump(Tex &t, uint32_t w, uint32_t h) {
        if (t.info.pixel != MTLPixelFormatBGRA8Unorm && !t.promoted)
            return;
        settle(t.used);
        std::vector<uint8_t> bgra((size_t)w * h * 4);
        if (t.promoted) {   // PLAN_R2 item 10: the float frame's 8-bit picture (clamped), comparable with an 8-bit run
            std::vector<uint16_t> half((size_t)w * h * 4);
            [t.texture getBytes:half.data() bytesPerRow:w * 8 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            hdrconv::rgba16f_to_bgra8(half.data(), bgra.data(), (size_t)w * h);
        } else
        [t.texture getBytes:bgra.data()
                bytesPerRow:w * 4
                 fromRegion:MTLRegionMake2D(0, 0, w, h)
                mipmapLevel:0];
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/present_%05u.ppm", host_dump_dir(),
                 (unsigned)(presents_ + 1));
        host_write_ppm(path, rgb.data(), (int)w, (int)h);
    }

    struct VDesc {
        MTLVertexDescriptor *desc = nil;
        float ascale[16][4] = {};
        uint32_t streams = 0;
        bool zero = false;
    };

    gpu::MetalDevice *device_;
    id<MTLDevice> mtl_;
    id<MTLCommandQueue> queue_;
    id<MTLLibrary> utility_;
    bool bc_ = false;
    dispatch_semaphore_t frames_;
    D9Cmd *cmd_ = nil, *last_ = nil;
    id m4_ = nil;          // v4: D9M4 (Metal 4) or nil (classic Metal)
    bool m4_fx_ = false;   // v4: MetalFX 4 scalers available (Metal 4 without them: the 3D scene draws native)
    // v4 step 0: per frame over the last 600: the encoding thread's CPU, GPU time, draws, passes, pipeline changes,
    // command buffers and (Metal 4) upload bytes; real time per frame for the frame rate.
    void perf_report() {
        const uint64_t now = os_monotonic_ns();
        const double frames = 600.0;
        const double cpu = perf_last_cpu_ns_ && perf_enc_cpu_ns_ > perf_last_cpu_ns_
                               ? (perf_enc_cpu_ns_ - perf_last_cpu_ns_) / 1e6 / frames : 0.0;
        const double wall = perf_last_t_ ? (now - perf_last_t_) / 1e6 / frames : 0.0;
        uint64_t up = 0, bars = 0, reused = 0, sets = 0, skips = 0, held = 0;
        if (m4_)
            if (@available(macOS 26.0, iOS 26.0, *)) {
                D9M4 *m = (D9M4 *)m4_;
                up = [m takeUploadedBytes];
                bars = [m takeBarriers];
                D9M4State *st = m.st;
                reused = st->reused, sets = st->table_sets, skips = st->table_skips, held = st->held();
                st->reused = st->table_sets = st->table_skips = 0;
                const double sf = perf_sampled_ ? (double)perf_sampled_ : 1.0;
                fprintf(stderr, "[gpu] perf Metal 4 time (ms per frame, timed frames): binds %.2f, uploads %.2f, draws %.2f, "
                                "passes %.2f, commits %.2f | residency per frame: uses %.0f, added %.1f, removed %.1f, "
                                "set commits %.2f\n",
                        st->t_ns[0] / 1e6 / sf, st->t_ns[1] / 1e6 / sf, st->t_ns[2] / 1e6 / sf, st->t_ns[3] / 1e6 / sf,
                        st->t_ns[4] / 1e6 / sf, st->uses / frames, st->res_adds / frames, st->res_removes / frames,
                        st->res_commits / frames);
                memset(st->t_ns, 0, sizeof st->t_ns);
                st->uses = st->res_adds = st->res_removes = st->res_commits = 0;
            }
        fprintf(stderr, "[gpu] perf (%s): draw() %.2f ms per frame (timed frames)\n", m4_ ? "Metal 4" : "Metal",
                perf_sampled_ ? perf_draw_ns_ / 1e6 / perf_sampled_ : 0.0);
        perf_draw_ns_ = 0;
        perf_sampled_ = 0;
        fprintf(stderr, "[gpu] perf (%s, per frame, last 600): encode CPU %.2f ms, GPU %.2f ms, frame %.2f ms | draws %.0f, "
                        "passes %.0f, pipeline changes %.0f, command buffers %.1f, uploads %.0f KB\n",
                m4_ ? "Metal 4" : "Metal", cpu, perf_gpu_us_.exchange(0) / 1e3 / frames, wall,
                (draws_ - perf_last_draws_) / frames, perf_passes_ / frames, perf_pso_changes_ / frames, perf_cmds_ / frames,
                up / 1024.0 / frames);
        if (m4_)
            fprintf(stderr, "[gpu] perf Metal 4 (per frame): barriers %.1f (%s), uploads reused %.0f, table entries set %.0f "
                            "skipped %.0f, resident %llu\n",
                    bars / frames, getenv("RECOMP_METAL4_BARRIERS") && !strcmp(getenv("RECOMP_METAL4_BARRIERS"), "safe")
                                       ? "safe" : "precise",
                    reused / frames, sets / frames, skips / frames, (unsigned long long)held);
        {   // v4: new GPU resources per frame by site (each one is a residency add on Metal 4)
            double n[AS_COUNT];
            for (int i = 0; i < AS_COUNT; ++i) {
                n[i] = (alloc_[i].count - perf_alloc_prev_[i]) / frames;
                perf_alloc_prev_[i] = alloc_[i].count;
            }
            fprintf(stderr, "[gpu] perf new resources per frame: buffers %.2f, spares %.2f, transient %.2f, textures %.2f, "
                            "other %.2f | buffer writes in place (NOOVERWRITE) %.1f\n",
                    n[AS_BUFFER], n[AS_BUFFER_SPARE], n[AS_TRANSIENT], n[AS_TEXTURE],
                    n[AS_MSAA] + n[AS_READBACK] + n[AS_KEPT] + n[AS_FITTED_DEPTH], in_place_uploads_ / frames);
            in_place_uploads_ = 0;
        }
        {   // v4: the buffers that made the most new spares (size, uploads, replacements, new spares, bytes copied)
            std::vector<std::pair<uint32_t, Buf *>> top;
            for (auto &kv : buffers_) {
                if (kv.second.win_spares)
                    top.push_back({kv.first, &kv.second});
            }
            std::partial_sort(top.begin(), top.begin() + std::min<size_t>(3, top.size()), top.end(),
                              [](const auto &a, const auto &b) { return a.second->win_spares > b.second->win_spares; });
            std::string line;
            for (size_t i = 0; i < std::min<size_t>(3, top.size()); ++i) {
                const Buf &b = *top[i].second;
                char t[200];
                snprintf(t, sizeof t, "%s#%u %u KB: uploads %.1f, replaced %.1f, new spares %.1f, copied %.0f KB, held %zu",
                         i ? "; " : "", top[i].first, (unsigned)(b.shadow.size() >> 10), b.win_uploads / frames,
                         b.win_replaced / frames, b.win_spares / frames, b.win_copied / 1024.0 / frames, b.spares.size());
                line += t;
            }
            if (!line.empty())
                fprintf(stderr, "[gpu] perf buffers making spares (per frame): %s\n", line.c_str());
            for (auto &kv : buffers_)
                kv.second.win_uploads = kv.second.win_spares = kv.second.win_replaced = 0, kv.second.win_copied = 0;
        }
        perf_last_cpu_ns_ = perf_enc_cpu_ns_;
        perf_last_draws_ = draws_;
        perf_last_t_ = now;
        perf_passes_ = perf_pso_changes_ = perf_cmds_ = 0;
    }
    void m4_keep(id object) {
        if (m4_ && object)
            if (@available(macOS 26.0, iOS 26.0, *))
                [(D9M4 *)m4_ keepAlive:object];
    }
    D9Cmd *new_cmd() {
        ++perf_cmds_;
        if (m4_)
            if (@available(macOS 26.0, iOS 26.0, *))
                return [[D9Cmd alloc] initWithM4:(D9M4 *)m4_];
        return [[D9Cmd alloc] initWithQueue:queue_];
    }
    uint64_t serial_ = 0;
    uint64_t submitted_ = 0, pass_serial_ = 0;
    uint32_t resource_generation_ = 0, trace_flushes_ = 0;
    uint64_t trace_bytes_ = 0, burst_trace_start_ = 0, burst_pixel_bytes_ = 0, burst_start_ = 0;
    uint32_t burst_left_ = 0, burst_captured_ = 0;
    bool burst_used_ = false;
    std::unique_ptr<d9trace::Ring> trace_;
    bool capture_mode_ = false;
    d9trace::CaptureControl capture_control_;
    uint64_t capture_epoch_ = os_wall_time_us(), capture_summary_count_ = 0;
    std::array<d9trace::CaptureSummary, d9trace::CaptureControl::timeline_capacity>
        capture_summaries_{};
    std::unique_ptr<d9trace::Ring> capture_spare_;
    std::future<CaptureSave> capture_job_;
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> gpu_us_{0}; // GPU time of finished command buffers since the last report
    // v4 step 0 (docs/METAL4_PLAN.md): the per-frame cost line, every 600 frames, both renderers.
    std::atomic<uint64_t> perf_gpu_us_{0};
    uint64_t perf_mark_ = ~0ull, perf_enc_cpu_ns_ = 0, perf_last_cpu_ns_ = 0, perf_last_draws_ = 0, perf_pso_changes_ = 0,
             perf_passes_ = 0, perf_cmds_ = 0, perf_last_t_ = 0, perf_draw_ns_ = 0, perf_sampled_ = 0;
    bool perf_sample_ = false;
    double gpu_wait_s_ = 0;           // time the game thread spent waiting for the GPU
    uint64_t report_cpu_ns_ = 0, report_instructions_ = 0;
    bool in_flight_ = false;
    D9RenderEnc *enc_ = nil;
    std::unordered_map<uint32_t, Query> queries_;
    uint32_t query_ = 0;
    id<MTLBuffer> vis_ = nil;
    NSUInteger vis_used_ = 0;
    std::vector<std::pair<id<MTLBuffer>, uint64_t>> vis_pool_;
    bool pass_vis_ = false;
    int pass_slot_ = -1;
    HostD9Target pass_target_{};
    float pass_scale_ = 1.0f;
    uint32_t pass_samples_ = 1; // sample count of the open pass's attachments
    bool pass_fitted_depth_ = false;
    bool pass_motion_ = false;   // v2.5 MetalFX temporal: colour 1 of this pass is tmp_.om
    // Test155 allocation watch (see note_alloc / alloc_watch).
    AllocStat alloc_[AS_COUNT];
    uint64_t alloc_base_ = 0, alloc_lines_ = 0, alloc_big_ = 0;
    double alloc_t0_ = 0;
    bool scale_chosen_ = false;
    uint32_t base_rows_ = 0;
    int rescale_frames_ = 0;
    // Render targets are this many times the guest's size (RECOMP_D3D9_SCALE).
    float scale_ = recomp_env("D3D9_SCALE")
                       ? std::max(1.0f, std::min(8.0f, (float)atof(recomp_env("D3D9_SCALE"))))
                       : 1.0f;
    id<MTLTexture> pass_color_[4] = {nil, nil, nil, nil};
    // Test86 HDR: the guest id of the pass's first colour target, and whether this frame had the core
    // mod's pre-HUD scene marker (private render state 250, reset by Present in d3d9.cpp).
    uint32_t pass_color_id_ = 0;
    bool hdr_mask_frame_ = false;
    uint32_t hud_rs_[256] = {};
    uint64_t hdr_scene_draws_ = 0; // Test133: mirror draws that kept the HDR expansion
    // PLAN_R2 item 10 real HDR (review/run14/hdr-design.md s2): the float frame, latched at Present.
    bool hdr_float_ = false;
    uint8_t hdr_cls_ = 0;          // this draw's output clamp (d9msl::PixelVariant::hdr), 0 = none
    uint32_t hdr_mirror_id_ = 0;   // the texture the mod marks as scene content under the HUD (the mirror)
    bool hdr_mirror_keep_ = false; // run 15: this HUD draw is the mirror and keeps the scene mark on the float frame
    uint64_t hdr_mirror_draws_[2] = {};   // run 15: float-frame mirror draws: kept the mark (before the split), late (HUD)
    unsigned hdr_live_ = 0;        // promoted targets
    std::unordered_set<const void *> hdr_tex_;           // their Metal textures (MSAA twins, MetalFX backings too)
    std::unordered_map<uint64_t, uint8_t> hdr_family_;   // ps key -> 1 sky, 2 car paint, 3 glass, 4 road, 0 none
    uint64_t hdr_promotions_ = 0, hdr_demotions_ = 0, hdr_readbacks_ = 0, hdr_draws_[2] = {}, hdr_dumps_ = 0;
    uint64_t hdr_why_[4] = {};     // ceiling-64 draws by reason: chain (RS 249), additive, mirror, family
    bool hdr_split_tried_ = false, hdr_split_done_ = false, hdr_split_off_ = false;   // the HUD split (hdr_split)
    uint64_t hdr_splits_ = 0, hdr_addback_open_ = 0;   // run 15: add-backs drawn in the open pass
    id<MTLLibrary> hdr_lib_ = nil;
    id<MTLTexture> hdr_excess_ = nil;
    id<MTLDepthStencilState> hdr_nodepth_ = nil;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> hdr_psos_;
    uint32_t hdr_dump_every_ = [] {
        const char *e = recomp_env("DUMP_FLOAT_FRAMES");
        return e ? (uint32_t)std::max(0, atoi(e)) : 0u;
    }();
    // D-110 TEST (RECOMP_DUMP_FLOAT_WINDOW=<start s>:<seconds>): float dumps only inside that window of seconds after the
    // first float frame (a frame-by-frame window for the HDRX flicker gate without dumping the whole run).
    double hdr_dump_t0_ = 0;
    bool hdr_dump_in_window() {
        static const double w0 = [] { const char *e = recomp_env("DUMP_FLOAT_WINDOW"); return e ? atof(e) : -1.0; }();
        static const double wl = [] { const char *e = recomp_env("DUMP_FLOAT_WINDOW"); const char *c = e ? strchr(e, ':') : nullptr; return c ? atof(c + 1) : 0.0; }();
        if (w0 < 0)
            return true;
        const double now = CACurrentMediaTime();
        if (hdr_dump_t0_ == 0)
            hdr_dump_t0_ = now;
        return now - hdr_dump_t0_ >= w0 && now - hdr_dump_t0_ <= w0 + wl;
    }
    id<MTLTexture> pass_depth_ = nil;
    uint32_t stream_mask_ = 0;
    bool vdesc_needs_zero_ = false;
    id<MTLBuffer> zero_ = nil;
    id<MTLBuffer> transient_ = nil;
    NSUInteger transient_used_ = 0;
    std::vector<SpareBuf> transient_pool_;      // v4: full transient buffers, reused once their frame is done
    uint64_t perf_alloc_prev_[AS_COUNT] = {};   // alloc_[site].count at the last perf line
    id<MTLTexture> white_ = nil, white_cube_ = nil, white_depth_ = nil, white_volume_ = nil;
    std::vector<uint8_t> scratch_;
    std::vector<float> constants_;
    std::unordered_map<uint32_t, Tex> textures_;
    remaster::TexKeys rtex_; // Remaster A1 step 1: texture keys, dumps, counters (off unless asked)
    remaster::ReplacementCache rcache_{rtex_}; // Remaster A1 step 3: per-key replacements (pack only)
    const bool a2c_on_ = [] { const char *e = getenv("RECOMP_REMASTER_A2C"); return e && *e == '1'; }();
    const bool wetroad_on_ = [] { const char *e = getenv("RECOMP_REMASTER_WETROAD"); return e && (*e == '1' || *e == '2'); }();
    // Remaster run 13 Xbox 360 wet road (RECOMP_REMASTER_WETROAD=2): the Natural wet road + the 360 rain-splash layer.
    const uint8_t wetroad_mode_ = [] { const char *e = getenv("RECOMP_REMASTER_WETROAD"); return uint8_t(e && *e == '2' ? 2 : 1); }();
    const bool shadowpcf_on_ = [] { const char *e = getenv("RECOMP_REMASTER_SHADOWPCF"); return e && *e == '1'; }();
    uint64_t shadowpcf_draws_ = 0, shadowpcf_frames_ = 0;
    // Remaster run 10/11 fused pools (cumulative): fused draws, of them with the minimal variant, ready draws drawn
    // without their light (a defect), probes by outcome (0 ready, then POOL_FORCE_REFUSE..PSO = blocked reasons).
    uint64_t pool_draws_ = 0, pool_minimal_ = 0, pool_late_lost_ = 0, pool_probe_[5] = {}, pool_frames_ = 0;
    // TEST ONLY (RECOMP_REMASTER_POOL_FORCE, run 11 fixtures): refuse / vsfail / psfail / psofail fail every probe
    // with that reason (the key is blocked: two-pass); late fails a ready draw's own variant (the minimal retry
    // draws it); lateall fails both (the base is drawn alone once, then the key is blocked).
    enum { POOL_FORCE_NONE, POOL_FORCE_REFUSE, POOL_FORCE_VS, POOL_FORCE_PS, POOL_FORCE_PSO, POOL_FORCE_LATE,
           POOL_FORCE_LATEALL };
    static int pool_force() {
        static const int f = [] {
            const char *e = getenv("RECOMP_REMASTER_POOL_FORCE");
            if (!e)
                return 0;
            static const char *const names[] = {"", "refuse", "vsfail", "psfail", "psofail", "late", "lateall"};
            for (int i = 1; i < 7; ++i)
                if (!strcmp(e, names[i]))
                    return i;
            return 0;
        }();
        return f;
    }
    static constexpr const char *kPoolProbe[5] = {"ready", "BLOCKED (refused)", "BLOCKED (vertex shader)",
                                                  "BLOCKED (pixel shader)", "BLOCKED (pipeline)"};
    std::unordered_map<uint64_t, bool> wetroad_ps_;
    uint64_t wetroad_draws_ = 0, wetroad_frames_ = 0;
    // Remaster run 13 road sheen (RECOMP_REMASTER_ROADSHEEN=1; A/B switch roadsheen): road programs whose highlight
    // lines the patch replaces (pixel_is_roadsheen, once per program), and the draws that got the variant.
    const bool roadsheen_on_ = [] { const char *e = getenv("RECOMP_REMASTER_ROADSHEEN"); return e && *e == '1'; }();
    std::unordered_map<uint64_t, bool> roadsheen_ps_;
    uint64_t roadsheen_draws_ = 0, roadsheen_frames_ = 0, roadsheen_fallback_ = 0, roadsheen_draws_at_present_ = 0;
    // Remaster run 13 MetalFX upscaling (RECOMP_REMASTER_UPSCALE=1 Quality 1.5x | 2 Performance 2x, or a factor
    // 1.1-3; the core mod sets it from the `upscale` setting). 0: off, and nothing below is ever touched.
    const float upscale_factor_ = [] {
        const char *e = getenv("RECOMP_REMASTER_UPSCALE");
        double f = e ? atof(e) : 0.0;
        if (f == 1.0)
            f = 1.5;
        return (f >= 1.1 && f <= 3.0) ? (float)f : 0.0f;
    }();
    const bool upscale_temporal_ = [] { const char *e = getenv("RECOMP_REMASTER_UPSCALE_TEMPORAL"); return e && *e == '1'; }();
    // v2.5 MetalFX temporal (experimental; the iOS panel's Upscaler "Temporal" sets RECOMP_REMASTER_UPSCALE_TEMPORAL,
    // the foundation and its jitter at launch). Each unblended scene draw also writes its own motion (the motion shader
    // variants: the same vertex through its clip rows of the previous frame, matched by draw identity) into `om`; the
    // foundation's camera pass fills every other pixel from the depth; MTLFXTemporalScaler then takes the scene, its
    // depth, those vectors and the jitter. Anything missing on a frame: that frame is scaled spatially, history reset.
    // From the environment itself: fnd_on_ / fnd_jitter_ are declared (so initialised) after this member.
    const bool tmp_on_ = [] {
        const char *t = getenv("RECOMP_REMASTER_UPSCALE_TEMPORAL"), *f = getenv("RECOMP_REMASTER_FOUNDATION"),
                   *j = getenv("RECOMP_REMASTER_FOUNDATION_JITTER"), *k = getenv("RECOMP_REMASTER_FOUNDATION_JITTER_PHASE");
        return t && *t == '1' && f && *f == '1' && j && *j == '1' && !(k && *k);
    }();
    // MetalFX's jitter is the sample offset: the geometry moved by (jx, jy) pixels samples at minus that. 1 flips it.
    // (owner test 16 Oct: the flipped sign, +1, was the steadier one; the scaler takes the shift as drawn)
    const float tmp_jsign_ = [] { const char *e = getenv("RECOMP_TEMPORAL_JITTER_SIGN"); return e && *e == '0' ? -1.0f : 1.0f; }();
    struct TmpRows {
        std::vector<float> c;   // the draw's vertex constants (4 floats each)
    };
    struct {
        bool frame = false, cleared = false, wrote = false, motion_ok = false, reset_next = true, failed = false;
        bool zcopy = false, warned = false;
        id<MTLTexture> om = nil, z32 = nil, out = nil;
        id scaler = nil;   // id<MTLFXTemporalScaler>
        id<MTLRenderPipelineState> zpso = nil;
        id<MTLDepthStencilState> zdss = nil;
        NSUInteger iw = 0, ih = 0, ow = 0, oh = 0;
        MTLPixelFormat cf = MTLPixelFormatInvalid, df = MTLPixelFormatInvalid;
        std::unordered_map<uint64_t, TmpRows> cur, prev;   // draw identity -> its clip rows (this / the last scene)
        std::unordered_map<uint64_t, uint32_t> occ;        // identity -> draws so far this scene (repeats of one mesh)
        std::unordered_map<uint64_t, int32_t> clip;        // vs key -> clip register (-1: no 4-dp4 oPos)
        uint64_t last_present = 0;
        uint64_t scenes = 0, scaled = 0, spatial = 0, resets = 0, draws = 0, known = 0, fresh = 0, noform = 0, plain = 0;
        uint64_t flat = 0;              // draws in the scene without a depth test (2D passes): not jittered, no motion
        std::set<std::string> flat_labels;
    } tmp_;
    struct {
        bool checked = false, supported = false;
        bool open = false, begun_frame = false;
        uint32_t bb = 0, ds = 0, last_bb = 0, last_ds = 0, in_w = 0, in_h = 0;
        uint64_t draws = 0, pre_frame = 0;
        bool clr_color = false, clr_depth = false, clr_stencil = false;
        float clr_rgba[4] = {}, clr_z = 0.0f;
        uint32_t clr_stencil_value = 0;
        uint64_t frames = 0, scaled = 0, skipped = 0, bb_before = 0, fitted = 0, failures = 0, nested = 0,
                 stray_end = 0, refused = 0, no_end = 0, depth_mismatch = 0, replayed = 0, allocs = 0, aborts = 0,
                 native = 0; // scenes a host's height left at the back buffer's size
        id scaler = nil; // id<MTLFXSpatialScaler>
        id<MTLTexture> out = nil;
        NSUInteger sc_iw = 0, sc_ih = 0, sc_ow = 0, sc_oh = 0;
        MTLPixelFormat sc_fmt = MTLPixelFormatInvalid;
        id<MTLDepthStencilState> zwrite = nil, znone = nil;
        // v2.4 (RECOMP_UPSCALE_EFFECTS_INTERNAL=1): the effect targets that follow the frame's size (the frame, a
        // half, a quarter of it: the clean chain's copies, AO, bloom, DoF, sun) get a scene-scaled backing while the
        // scene is open, as the back buffer does, so the effects that already work from the scene's pixels stop
        // drawing 1/(scale^2) more of them. `followers` are the ones swapped this scene; `fsize` caches the scan.
        std::vector<uint32_t> followers;
        uint64_t follow_frames = 0, follow_swaps = 0;
    } up_;
    const bool up_follow_ = [] {
        const char *e = getenv("RECOMP_UPSCALE_EFFECTS_INTERNAL");
        const char *off = getenv("RECOMP_UPSCALE_FOLLOW");
        return e && *e == '1' && !(off && *off == '0');
    }();
    // Remaster run 14 foundation F1-F3 (review/run14/foundation-design.md 1-3; TEST ONLY until a consumer passes its
    // gates). RECOMP_REMASTER_FOUNDATION=1 (the core mod then sends the scene markers also without upscaling, and the
    // camera): the scene window, the VP probe, the motion vectors and the history resets. Unset: nothing below runs
    // (no library, no pass, no halfpix change, no counters). RECOMP_REMASTER_FOUNDATION_JITTER=1: Halton(2,3) jitter on
    // the in-window scene passes (off by default even with the foundation: config F must look identical);
    // _JITTER_PHASE=<k>: a fixed phase k (the gate 5.4 sign/size pair). _DEBUG=6 motion (hue = direction), 8 the
    // reprojected-depth error (green < 0.5 %, red above, blue disoccluded, purple depth edge), drawn over the scene
    // before 252. _LOG=<n>: one line per n window frames with that frame's GPU counters (the combined test's gates).
    const bool fnd_on_ = [] { const char *e = getenv("RECOMP_REMASTER_FOUNDATION"); return e && *e == '1'; }();
    const int fnd_jitter_ = [] {
        const char *e = getenv("RECOMP_REMASTER_FOUNDATION_JITTER"), *k = getenv("RECOMP_REMASTER_FOUNDATION_JITTER_PHASE");
        if (k && *k)
            return 1000 + std::max(0, atoi(k));   // 1000 + a fixed phase
        return e && *e == '1' ? 1 : 0;
    }();
    const int fnd_debug_ = [] { const char *e = getenv("RECOMP_REMASTER_FOUNDATION_DEBUG"); return e ? atoi(e) : 0; }();
    const int fnd_log_ = [] { const char *e = getenv("RECOMP_REMASTER_FOUNDATION_LOG"); return e ? std::max(0, atoi(e)) : 0; }();
    struct FndSlot {
        uint64_t serial = 0, frame = 0;
        bool pending = false;
    };
    struct {
        bool open = false, had_window = false, prev_valid = false, prev_cam_valid = false, resized = false;
        bool have_p = false, logged_p = false, prevz_valid = false, lib_tried = false;
        uint32_t bb = 0, ds = 0;
        remaster_fnd::Plurality plur;
        std::unordered_map<uint64_t, int32_t> regs;   // vs key -> clip register (-1: no 4-dp4 oPos)
        remaster_fnd::Camera cam, cam_prev;
        remaster_fnd::M4 vp_prev{}, p_last{}, p_prev{};
        double fov_last = 0.0, jx = 0.0, jy = 0.0, jx_prev = 0.0, jy_prev = 0.0;
        uint32_t phases = 8, phase = 0, prevz_i = 0;
        uint32_t reset = 0;   // the last window's history-reset reasons (remaster_fnd::RESET_*; for the consumers)
        uint64_t by_camera = 0, not_largest = 0, clusters_sum = 0, distinct_sum = 0, invalid_logged = 0;
        double share_sum = 0.0, kv[2] = {0, 0}, ar[2] = {0, 0}, pfar[2] = {0, 0};
        uint64_t frames = 0, windows = 0, nested = 0, stray_end = 0, stray_camera = 0, no_end = 0;
        uint64_t world_draws = 0, world_regs = 0, world_noregs = 0, world_identity = 0, ties = 0, vp_valid = 0,
                 vp_fallback = 0, gk_checked = 0, gk_agree = 0, gk_fail[6] = {}, selftest_fail = 0, motion = 0,
                 motion_skip[4] = {}, resets = 0, reset_why[remaster_fnd::RESET_COUNT] = {}, jittered = 0;
        double selftest_max = 0.0, off_max = 0.0;
        uint64_t q_sum[17] = {};   // harvested GPU counters since the last report (q[3]/[4] as one 64-bit sum)
        uint64_t q_frames = 0, q_max = 0, presents = 0, last_frames = 0;
        id<MTLLibrary> lib = nil;
        id<MTLComputePipelineState> cs[2] = {nil, nil};
        id<MTLTexture> motion_tex = nil, prevz[2] = {nil, nil}, dbg = nil;
        id<MTLBuffer> q = nil;   // 8 slots x 32 words, shared
        FndSlot slot[8];
    } fnd_;
    // Remaster run 13 Xbox 360 sky, our own version (RECOMP_REMASTER_SKY=1, set by core.nfsmw x360_sky 2; A/B switch
    // sky): the PC sky pass 0 combine in the 360 order and the 360 horizon distance (pixel_is_sky / vertex_is_sky).
    const bool sky_on_ = [] { const char *e = getenv("RECOMP_REMASTER_SKY"); return e && *e == '1'; }();
    std::unordered_map<uint64_t, bool> sky_ps_, sky_vs_;
    uint64_t sky_draws_ = 0, sky_vs_draws_ = 0, sky_all_ = 0, sky_frames_ = 0, sky_fallback_ = 0, sky_draws_at_present_ = 0;
    // Remaster run 13 natural glass (RECOMP_REMASTER_GLASS=1; A/B switch glass).
    const bool glass_on_ = [] { const char *e = getenv("RECOMP_REMASTER_GLASS"); return e && (*e == '1' || *e == '2'); }();
    // Remaster run 13 wall parallax (RECOMP_REMASTER_PARALLAX=1; A/B switch parallax).
    const bool parallax_on_ = [] { const char *e = getenv("RECOMP_REMASTER_PARALLAX"); return e && *e == '1'; }();
    std::unordered_map<uint64_t, int> parallax_ps_;
    uint64_t parallax_draws_ = 0, parallax_noalpha_ = 0, parallax_frames_ = 0, parallax_fallback_ = 0,
             parallax_draws_at_present_ = 0;
    // Remaster run 13 Xbox 360 glass mode (RECOMP_REMASTER_GLASS=2): the 360 window formula per pixel (glass_patch_x360).
    const uint8_t glass_mode_ = [] { const char *e = getenv("RECOMP_REMASTER_GLASS"); return uint8_t(e && *e == '2' ? 2 : 1); }();
    std::unordered_map<uint64_t, bool> glass_ps_, glass_vs_;
    uint64_t glass_draws_ = 0, glass_frames_ = 0, glass_fallback_ = 0, glass_draws_at_present_ = 0;
    // Remaster run 16 glass pop: the ps_1_x window program is patched too (RECOMP_TEST_GLASS1=0, TEST ONLY: not).
    const bool glass1_on_ = [] { const char *e = getenv("RECOMP_TEST_GLASS1"); return !(e && *e == '0'); }();
    uint64_t glass1_draws_ = 0, glass1_draws_at_present_ = 0;
    const char *const test_skip_label_ = [] { const char *e = getenv("RECOMP_TEST_SKIP_LABEL"); return e && *e ? e : nullptr; }();
    // Remaster run 13 natural water (RECOMP_REMASTER_WATER=1; A/B switch water).
    // 1 = Definitive, 2 = Xbox 360 (the patch's mode, PixelVariant.water); anything else = off.
    const uint8_t water_mode_ = [] {
        const char *e = getenv("RECOMP_REMASTER_WATER");
        return (uint8_t)(e && (*e == '1' || *e == '2') && !e[1] ? *e - '0' : 0);
    }();
    const bool water_on_ = water_mode_ != 0;
    std::unordered_map<uint64_t, bool> water_ps_, water_vs_;
    remaster_grass::State grass_;   // run 13 grass shells (grass.h)
    float grass_eye_[3] = {};          // run 14: this frame's eye from a grass draw that has one (see grass_blades)
    uint64_t grass_eye_frame_ = 0;
    id<MTLBuffer> grass_arena_ = nil;   // run 14 grass blades: the root records (shared storage, grass_.b pages)
    std::unordered_map<uint64_t, MTLVertexDescriptor *> grass_vdescs_;   // run 14: blade layouts by Layout hash
    std::vector<remaster_grass::Run> grass_runs_;
    NSUInteger water_main_w_ = 0;
    const double water_t0_ = CACurrentMediaTime();
    uint64_t water_draws_ = 0, water_frames_ = 0, water_fallback_ = 0, water_draws_at_present_ = 0, water_skip_[3] = {};
    // Remaster run 13 car reflections (RECOMP_REMASTER_CARENV=1; A/B switch carenv).
    const bool carenv_on_ = [] { const char *e = getenv("RECOMP_REMASTER_CARENV"); return e && (*e == '1' || *e == '2'); }();
    // Remaster run 13 Xbox 360 car paint (RECOMP_REMASTER_CARENV=2): the program's env strength + carpaint_patch.
    const uint8_t carenv_mode_ = [] { const char *e = getenv("RECOMP_REMASTER_CARENV"); return uint8_t(e && *e == '2' ? 2 : 1); }();
    std::unordered_map<uint64_t, bool> carpaint_ps_, carflow_vs_;
    std::unordered_map<uint64_t, bool> carenv_vs_;
    uint64_t carenv_draws_ = 0, carenv_frames_ = 0, carenv_fallback_ = 0, carenv_draws_at_present_ = 0;
    // The iOS reflection test (g_d9_diag): this frame's passes into the car reflection maps (cube faces) and the
    // shadow maps, and whether they were held.
    std::unordered_set<uint32_t> diag_shadow_ids_;
    uint32_t diag_cube_draws_ = 0, diag_cube_clears_ = 0, diag_cube_faces_ = 0, diag_shadow_draws_ = 0,
             diag_shadow_clears_ = 0, diag_held_ = 0, diag_prev_ = 0, diag_log_left_ = 0;
    uint64_t diag_frame_no_ = 0, diag_last_ns_ = 0;
    // True: skip this draw or clear (its target is a held map). Counts it for the log either way.
    bool diag_hold(const HostD9Target &t, bool clear) {
        const uint32_t m = g_d9_diag.load(std::memory_order_relaxed);
        Tex *c = t.color[0].id ? tex_ptr(t.color[0].id) : nullptr;
        const bool cube = c && c->desc.kind == HOST_D9_TEX_CUBE;
        const bool shadow = !cube && t.depth.id && diag_shadow_ids_.count(t.depth.id);
        if (cube) {
            ++(clear ? diag_cube_clears_ : diag_cube_draws_);
            diag_cube_faces_ |= 1u << (t.color[0].face & 7);
        } else if (shadow) {
            ++(clear ? diag_shadow_clears_ : diag_shadow_draws_);
        }
        const bool hold = (cube && (m & 1)) || (shadow && (m & 2));
        diag_held_ += hold;
        return hold;
    }
    // At Present: the log line for the frame just drawn, then the counters start again.
    void diag_frame() {
        const uint32_t m = g_d9_diag.load(std::memory_order_relaxed);
        if (m != diag_prev_) {
            fprintf(stderr, "[ios] reflection test: reflections %s, shadows %s, log %s\n", m & 1 ? "HELD" : "live",
                    m & 2 ? "HELD" : "live", m & 4 ? "on (600 frames)" : "off");
            if ((m & 4) && !(diag_prev_ & 4))
                diag_log_left_ = 600;
            if ((m & 8) && !(diag_prev_ & 8) && !diag_cap_list_) {
                diag_cap_pending_ = true;   // 5 s from now: time to close the panel (it pauses the race) and drive
                diag_cap_at_ns_ = os_monotonic_ns() + 5000000000ull;
            }
            diag_prev_ = m;
        }
        const uint64_t now = os_monotonic_ns();
        ++diag_frame_no_;
        if ((m & 4) && diag_log_left_) {
            --diag_log_left_;
            fprintf(stderr, "[ios] reflection test: frame %llu +%.2f ms: reflection map draws %u clears %u faces %02x, "
                            "shadow map draws %u clears %u (%zu maps), held %u\n",
                    (unsigned long long)diag_frame_no_, diag_last_ns_ ? (now - diag_last_ns_) / 1e6 : 0.0,
                    diag_cube_draws_, diag_cube_clears_, diag_cube_faces_, diag_shadow_draws_, diag_shadow_clears_,
                    diag_shadow_ids_.size(), diag_held_);
        }
        diag_last_ns_ = now;
        diag_cube_draws_ = diag_cube_clears_ = diag_cube_faces_ = diag_shadow_draws_ = diag_shadow_clears_ =
            diag_held_ = 0;
    }
    // Frame capture (bit 8): the next kDiagCapFrames presented frames, each as a picture (frame_NN.ppm) and a list
    // of its draws (frame_NN.txt), into Documents/Logs/capture-<time>. Consecutive frames, so a frame drawn between
    // two simulation steps can be compared with its neighbours.
    static constexpr uint32_t kDiagCapFrames = 12;
    bool diag_cap_pending_ = false;
    FILE *diag_cap_list_ = nullptr;
    uint32_t diag_cap_n_ = 0, diag_cap_draw_ = 0;
    std::string diag_cap_dir_;
    uint64_t diag_cap_last_ns_ = 0, diag_cap_at_ns_ = 0;
    static uint32_t diag_hash(const float *f, uint32_t count) {
        uint32_t h = 2166136261u;
        const uint8_t *b = reinterpret_cast<const uint8_t *>(f);
        for (size_t i = 0, n = f ? (size_t)count * 16 : 0; i < n; ++i)
            h = (h ^ b[i]) * 16777619u;
        return h;
    }
    void diag_record(const HostD9Draw &d) {
        fprintf(diag_cap_list_, "%u %s vs %016llx ps %016llx c0 %u:%u z %u tex %u %u %u %u %u %u prims %u vc %08x pc %08x\n",
                diag_cap_draw_++, d.label ? d.label : "-", (unsigned long long)d.vs_key, (unsigned long long)d.ps_key,
                d.target.color[0].id, d.target.color[0].face, d.target.depth.id, d.sampler_texture[0],
                d.sampler_texture[1], d.sampler_texture[2], d.sampler_texture[3], d.sampler_texture[4],
                d.sampler_texture[5], d.primitive_count, diag_hash(d.vconst, d.vconst_count),
                diag_hash(d.pconst, d.pconst_count));
    }
    void diag_open_list() {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%02u.txt", diag_cap_dir_.c_str(), diag_cap_n_);
        diag_cap_list_ = fopen(path, "w");
        diag_cap_draw_ = 0;
    }
    void diag_capture(Tex &t, uint32_t w, uint32_t h) {
        if (diag_cap_pending_) {   // starts with the next frame (this one was drawn before the start)
            if (os_monotonic_ns() < diag_cap_at_ns_)
                return;
            diag_cap_pending_ = false;
            NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
            const time_t now = time(nullptr);
            char stamp[32];
            strftime(stamp, sizeof stamp, "%H%M%S", localtime(&now));
            diag_cap_dir_ = std::string(docs.UTF8String) + "/Logs/capture-" + stamp;
            os_mkdir(diag_cap_dir_.c_str());
            diag_cap_n_ = 0;
            diag_cap_last_ns_ = os_monotonic_ns();
            diag_open_list();
            fprintf(stderr, "[ios] reflection test: capturing %u frames into %s%s\n", kDiagCapFrames,
                    diag_cap_dir_.c_str(), diag_cap_list_ ? "" : " (FAILED: no list file)");
            return;
        }
        const uint64_t now = os_monotonic_ns();
        fprintf(diag_cap_list_, "# present %llu, %ux%u, +%.2f ms since the last frame\n", (unsigned long long)(presents_ + 1),
                w, h, (now - diag_cap_last_ns_) / 1e6);
        diag_cap_last_ns_ = now;
        fclose(diag_cap_list_);
        diag_cap_list_ = nullptr;
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%02u.ppm", diag_cap_dir_.c_str(), diag_cap_n_);
        if (t.info.pixel == MTLPixelFormatBGRA8Unorm) {
            settle(t.used);
            std::vector<uint8_t> bgra((size_t)w * h * 4), rgb((size_t)w * h * 3);
            [t.texture getBytes:bgra.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
                rgb[i * 3] = bgra[i * 4 + 2];
                rgb[i * 3 + 1] = bgra[i * 4 + 1];
                rgb[i * 3 + 2] = bgra[i * 4];
            }
            host_write_ppm(path, rgb.data(), (int)w, (int)h);
        }
        if (++diag_cap_n_ < kDiagCapFrames)
            diag_open_list();
        else
            fprintf(stderr, "[ios] reflection test: capture done (%u frames)\n", kDiagCapFrames);
    }
    // Remaster run 13 front-end floor shadows (RECOMP_REMASTER_FEFLOOR=1; A/B switch fefloor; fef_capture above).
    // TEST ONLY: RECOMP_REMASTER_FEFLOOR_DEBUG=1 (same lookup, colour unchanged: must match the floor without it),
    // =2 (coordinates: green inside the map, darker where shadowed, red outside); RECOMP_REMASTER_FEFLOOR_K (0..1).
    const bool fef_on_ = [] { const char *e = getenv("RECOMP_REMASTER_FEFLOOR"); return e && *e == '1'; }();
    const int fef_debug_ = [] {
        const char *e = getenv("RECOMP_REMASTER_FEFLOOR_DEBUG");
        return e && *e >= '1' && *e <= '3' ? *e - '0' : 0;   // run 15: 3 = map depth vs floor depth
    }();
    const float fef_k_ = [] {
        const char *e = getenv("RECOMP_REMASTER_FEFLOOR_K");
        const float k = e && *e ? strtof(e, nullptr) : 0.55f;
        return k >= 0.0f && k <= 1.0f ? k : 0.55f;
    }();
    const std::vector<std::pair<uint64_t, uint64_t>> fef_progs_ = fef_progs_init();   // run 15: the floor pairs
    std::unordered_map<uint64_t, int32_t> fef_recv_;   // vs key -> clip | shadow << 8, or -1 (not a receiver)
    float fef_m_[16] = {};
    id<MTLTexture> fef_map_ = nil;
    uint32_t fef_target_ = 0;
    uint64_t fef_frame_ = 0, fef_cap_frame_ = 0;   // capture frame + 1 (0: none)
    uint64_t fef_floor_draws_ = 0, fef_shadowed_ = 0, fef_fallback_ = 0, fef_no_recv_ = 0, fef_captures_ = 0,
             fef_views_ = 0, fef_singular_ = 0, fef_build_fail_ = 0, fef_gate_[6] = {};
    bool wetroad_log_ = true;
    const double wetroad_t0_ = CACurrentMediaTime();
    uint64_t a2c_draws_ = 0, a2c_frames_ = 0;
    // Remaster run 12 step 0 (TEST ONLY, RECOMP_REMASTER_PBRPROBE=1; allow list RECOMP_REMASTER_PBRPROBE_KEYS=<keys.tsv>,
    // the rows marked "built"): the sun-share probe and census of WORLD lighting draws (review/run12/PLAN.md G0).
    const bool pbrprobe_on_ = [] { const char *e = getenv("RECOMP_REMASTER_PBRPROBE"); return e && *e == '1'; }();
    std::unordered_map<uint64_t, bool> pbr_ps_;
    std::unordered_map<uint64_t, d9msl::SunFlow> pbr_vs_;
    std::set<uint64_t> pbr_keys_;
    bool pbr_keys_loaded_ = false, pbr_log_consts_ = true;
    std::map<std::string, uint64_t> pbr_census_;   // "ps vs flow label view" -> draws (cumulative)
    std::map<uint64_t, uint64_t> pbr_key_draws_;   // allow-list stage-0 key -> probed draws (cumulative)
    uint64_t pbr_frames_ = 0, pbr_draws_ = 0, pbr_fallback_ = 0;
    NSUInteger pbr_main_w_ = 0;
    // Remaster run 12 PBR step 2 pilot (TEST ONLY until the owner approves; review/run12/patch-plan.md):
    // RECOMP_REMASTER_PBRWORLD=1 with RECOMP_REMASTER_PBRWORLD_PACK=<dir> (slot index.tsv + manifest.tsv); strength,
    // flat and debug switches in d3d9_shadergen.cpp (pbr_env). Never together with the probe (both use buffer(3)).
    const bool pbw_active_ = [] {
        const char *e = getenv("RECOMP_REMASTER_PBRWORLD"), *q = getenv("RECOMP_REMASTER_PBRPROBE");
        if (!(e && *e == '1'))
            return false;
        if (q && *q == '1') {
            fprintf(stderr, "[remaster] pbrworld: REFUSED (RECOMP_REMASTER_PBRPROBE is on; both use buffer 3)\n");
            return false;
        }
        return true;
    }();
    const bool pbw_debug_ = [] { const char *d = getenv("RECOMP_REMASTER_PBRWORLD_DEBUG"); return d && *d >= '1' && *d <= '3'; }();
    std::unordered_map<uint64_t, float> pbw_ecos_;   // manifest: pilot key -> packed ecos and sig at the run's strength
    std::unordered_map<uint64_t, bool> pbw_vs_;      // vs key -> sun flow and pbr registers found
    NSUInteger pbw_main_w_ = 0;
    // skips (draws of a WORLD lighting PS that stayed as today): 0 vs/label, 1 not the main view, 2 alpha test,
    // 3 stage 0 unusable, 4 albedo not resident, 5 slot not resident
    uint64_t pbw_frames_ = 0, pbw_draws_ = 0, pbw_fallback_ = 0, pbw_skip_[6] = {};
    uint64_t pbw_draws_at_present_ = 0;
    id<MTLBuffer> pwq_ = nil;
    id<MTLBuffer> pbq_ = nil;
    std::vector<Tex *> tex_index_;
    // What the encoder was last told in this pass, so a draw that repeats it
    // makes no call. Reset whenever a new encoder starts or a clear or blit
    // sets state of its own.
    struct EncoderState {
        __unsafe_unretained id<MTLRenderPipelineState> pipeline = nil;
        __unsafe_unretained id<MTLDepthStencilState> depth = nil;
        int cull = -1, fill = -1;
        uint32_t stencil_ref = 0xffffffffu;
        float bias = NAN, slope = NAN;
        bool viewport_valid = false, scissor_valid = false;
        MTLViewport viewport{};
        MTLScissorRect scissor{};
        __unsafe_unretained id<MTLTexture> texture[16] = {};
        __unsafe_unretained id<MTLSamplerState> sampler[16] = {};
    };
    EncoderState es_;
    std::unordered_map<uint32_t, Buf> buffers_;
    std::unordered_map<uint64_t, id<MTLFunction>> functions_;
    std::unordered_map<const void *, std::string> fn_warm_keys_;
    // v4 Metal 4 pipelines (m4_pipeline): each function's library; the unspecialized pipelines (one per shader pair,
    // vertex layout, sample count and attachment set); counts and times for the log.
    std::unordered_map<const void *, id<MTLLibrary>> fn_libs_;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> m4_bases_;
    uint64_t m4_base_n_ = 0, m4_spec_n_ = 0, m4_base_ns_ = 0, m4_spec_ns_ = 0, m4_fail_n_ = 0;
    // Run 15 (hitch): deferral is allowed only inside the main draw's variant fallback chain.
    bool defer_ok_ = false;
    std::unordered_map<std::string, id<MTLLibrary>> libs_by_src_;   // warm key -> library built this session
    std::unordered_map<uint64_t, std::string> deferred_src_;   // function key -> source compiling in the background
    uint32_t deferred_logged_ = 0, slow_logged_ = 0;
    uint32_t draw_compiles_ = 0, draw_compiles_slow_ = 0, warm_report_used_ = 0, warm_report_draw_ = 0, warm_report_bg_ = 0;
    uint64_t warm_report_ns_ = 0;
    bool msaa_fallback_logged_ = false;   // owner 8 Oct (Off/2x/4x): a refused sample count, once
    std::set<uint64_t> msaa_logged_;   // Test70: multisampled target sizes already reported   // Test70: function -> recorded library key
    std::unordered_map<uint64_t, VDesc> vdescs_;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines_;
    uint64_t shader_frame_ns_ = 0, shader_frame_max_ns_ = 0, shader_total_ns_ = 0;
    uint64_t shader_frame_lib_ns_ = 0, shader_frame_pso_ns_ = 0;
    std::unique_ptr<ShaderWarm> warm_;
    unsigned shader_frame_libs_ = 0, shader_frame_psos_ = 0, shader_reports_ = 0;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_states_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
    // A depth buffer of a pass's own size/samples, cached per source depth texture (depth_for_size). Each entry
    // remembers its source, so it is dropped with that texture (forget_fitted_depth): Test157b.
    struct FittedDepth {
        uintptr_t source = 0;
        id<MTLTexture> texture = nil;
    };
    std::unordered_map<uint64_t, FittedDepth> fitted_depth_;
    uint64_t draws_ = 0, presents_ = 0, stat_calls_ = 0;
    double report_time_ = 0;
    uint64_t report_frames_ = 0, report_draws_ = 0;
    std::map<std::string, uint64_t> skips_;
    std::set<std::string> undecoded_;
    uint32_t last_presented_ = 0, last_w_ = 0, last_h_ = 0;
    std::mutex kept_mutex_;
    id<MTLTexture> kept_ = nil;
    D9Cmd *kept_cmd_ = nil;
    uint32_t kept_w_ = 0, kept_h_ = 0;
    std::string probe_tag_;
    uint64_t probe_frame_ =
        recomp_env("D3D9_PROBE") ? strtoull(recomp_env("D3D9_PROBE"), nullptr, 10) : 0;
};

} // namespace

extern "C" void host_upscale_temporal_flip_jitter(int flip) {
    g_temporal_flip.store(flip ? 1 : 0, std::memory_order_relaxed);
}
extern "C" void host_upscale_temporal_show_motion(int on) {
    g_temporal_debug.store(on ? 1 : 0, std::memory_order_relaxed);
}
extern "C" int host_upscale_temporal_active(void) {
    return g_temporal_active.load(std::memory_order_relaxed);
}
extern "C" void host_upscale_set_rows(int rows) {
    const int before = g_upscale_rows.exchange(rows < 0 ? -1 : rows);
    if (before != (rows < 0 ? -1 : rows))
        fprintf(stderr, "[remaster] upscale: scene height %d%s\n", rows,
                rows < 0 ? " (back to RECOMP_REMASTER_UPSCALE)" : rows == 0 ? " (native)" : " px");
}

extern "C" void host_d9_diag_set(unsigned mask) {
    g_d9_diag.store(mask, std::memory_order_relaxed);
}

extern "C" void host_upscale_last_sizes(int *in_w, int *in_h, int *out_w, int *out_h) {
    const uint64_t i = g_upscale_in.load(), o = g_upscale_out.load();
    if (in_w)
        *in_w = int(i >> 32);
    if (in_h)
        *in_h = int(uint32_t(i));
    if (out_w)
        *out_w = int(o >> 32);
    if (out_h)
        *out_h = int(uint32_t(o));
}

size_t host_d9_metal_fitted_depth_entries() {
    return g_fitted_depth_entries.load();
}

D9Backend *d9_metal_create(gpu::Device *device) {
    auto *metal = dynamic_cast<gpu::MetalDevice *>(device);
    if (!metal)
        return nullptr;
    auto *r = new Renderer(metal);
    if (!r->ok()) {
        delete r;
        return nullptr;
    }
    g_fitted_depth_entries = 0;
    return r;
}

void d9_autorelease_run(void (*fn)(void *), void *arg) {
    @autoreleasepool {
        fn(arg);
    }
}
