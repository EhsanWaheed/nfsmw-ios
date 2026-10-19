// shaders_msl.h - the MSL behind every program gpu/shaders.md names. Moved
// verbatim from compositor.mm, performance_overlay.h and d3d_render.mm; the
// slot numbers are the ones shaders.md documents.
#pragma once

namespace gpu::metal {

// "compositor": compositor_vertex / compositor_fragment
inline const char *const kCompositorSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 rect; float4 uv; float2 drawable; uint opaque; uint pad; };
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying compositor_vertex(uint id [[vertex_id]], constant Quad& q [[buffer(0)]]) {
    const float2 corners[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 p=q.rect.xy+corners[id]*q.rect.zw;
    Varying v;
    v.position=float4(p.x/q.drawable.x*2-1,1-p.y/q.drawable.y*2,0,1);
    v.uv=q.uv.xy+corners[id]*q.uv.zw;
    return v;
}
// q.pad 1 (iOS, compositor.cpp): a source stretched by a fraction (1280x960 onto a 2752x2064 iPad) is filtered;
// nearest would make some source pixels two screen pixels wide and some three, a fixed grid of lines. A whole
// multiple (and every 1:1 element) stays nearest, as everywhere else.
float4 compositor_sample(texture2d<float> tex, constant Quad& q, float2 uv) {
    constexpr sampler nearest(coord::normalized,address::clamp_to_edge,filter::nearest);
    constexpr sampler smooth(coord::normalized,address::clamp_to_edge,filter::linear);
    if(q.pad) {
        const float2 k=q.rect.zw/max(float2(tex.get_width(),tex.get_height())*q.uv.zw,1.0);
        if(any(abs(k-round(k))>0.01)) return tex.sample(smooth,uv);
    }
    return tex.sample(nearest,uv);
}
fragment float4 compositor_fragment(Varying v [[stage_in]],
                                    constant Quad& q [[buffer(0)]],
                                    texture2d<float> tex [[texture(0)]]) {
    float4 c=compositor_sample(tex,q,v.uv);
    if(q.opaque) c.a=1;
    return c;
}
// Test86 HDR output (RGBA16F linear extended sRGB drawable, 1.0 = SDR white). mode 0: decode the sRGB
// source (UI, menus, movies); 1: the game frame - decode, then where its alpha marks the scene (the core
// mod sets it before the HUD; the HUD clears it) expand the highlights above `knee` toward `headroom`,
// hue kept; 2: the source is already linear (a re-fit of the composed frame).
// 3 (PLAN_R2 item 10, review/run14/hdr-design.md s3): the game frame is the renderer's float back buffer, values above 1
// kept: extended decode; under the scene mark identity to SDR white, then a hue-kept shoulder on the max channel m,
// f(m) = 1 + (H-1) u/(1+u), u = (m-1)/(H-1) (f(1)=1, f'(1)=1, f(inf)=H), hot cores whitened by `desat` (the spare
// slot); outside the mark (HUD, mirror frame) clipped at SDR white. 4: the same frame without a scene mark: clipped.
struct Hdr { float mode; float headroom; float knee; float desat; };
float3 hdr_shoulder(float3 lin, float H, float D) {
    const float m = max(max(lin.r, lin.g), lin.b);
    if (m <= 1.0 || H <= 1.001) return min(lin, 1.0);
    const float u = (m - 1.0) / (H - 1.0);
    const float f = 1.0 + (H - 1.0) * u / (1.0 + u);
    const float3 rgb = lin * (f / m);
    return mix(rgb, float3(f), D * (f - 1.0) / (H - 1.0));
}
float3 hdr_decode(float3 c) {
    c = max(c, 0.0);
    return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}
fragment float4 compositor_hdr_fragment(Varying v [[stage_in]],
                                        constant Quad& q [[buffer(0)]],
                                        constant Hdr& h [[buffer(1)]],
                                        texture2d<float> tex [[texture(0)]]) {
    float4 c=compositor_sample(tex,q,v.uv);
    const float mask=c.a;
    if(q.opaque) c.a=1;
    if(h.mode>4.5 && v.uv.y<0.1) {   // R2 #10 TEST bars: 1, 2, 4, 8 x SDR white through the mode-3 shoulder
        const float bar=exp2(floor(saturate(v.uv.x)*3.999));
        return float4(hdr_shoulder(float3(bar),h.headroom,h.desat),1.0);
    }
    if(h.mode>2.5) {   // item 10 (5 = 3 with the TEST bars)
        const float3 lin=hdr_decode(c.rgb);
        return float4((h.mode<3.5 || h.mode>4.5) && mask>0.5 ? hdr_shoulder(lin,h.headroom,h.desat) : min(lin,1.0), c.a);
    }
    if(h.mode>1.5) return c;
    float3 lin=hdr_decode(c.rgb);
    if(h.mode>0.5 && mask>0.5 && h.headroom>1.0) {
        const float L=dot(lin,float3(0.2126,0.7152,0.0722));
        const float t=saturate((L-h.knee)/max(1.0-h.knee,1e-3));
        lin*=1.0+(h.headroom-1.0)*t*t;
    }
    return float4(lin,c.a);
}
)MSL";

// "hud": hud_v / hud_f
inline const char *const kHudSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V hud_v(uint i [[vertex_id]], constant float4& r [[buffer(0)]]) {
    float2 q = float2(i & 1, i >> 1);
    V v; v.p = float4(r.xy + q * r.zw, 0, 1); v.uv = q; return v;
}
fragment float4 hud_f(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::linear);
    return t.sample(s, v.uv);
}

// "glass" (v4.3): one on-screen control as a piece of Liquid Glass over the finished frame (host/controls/overlay.cpp).
// The frame behind it is bent at the rim (a lens that is thicker at its edge), lightly blurred and lifted, tinted by
// the control's colour, with a lit rim from the upper left and a soft shadow under it. Premultiplied out.
struct Glass {
    float4 rect;  // the shape, drawable pixels: x, y, w, h
    float4 view;  // drawable w, h; where the backdrop's (0, 0) sits in the drawable: x, y
    float4 tint;  // rgb, strength
    float4 p;     // corner radius px, pressed 0..1, alpha, pixels per point
    float4 bd;    // backdrop w, h, 1 when there is one, shadow margin px
};
struct GV { float4 p [[position]]; float2 pos; };
static float glass_sd(float2 p, float2 b, float r) {
    const float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
vertex GV glass_v(uint i [[vertex_id]], constant Glass& g [[buffer(0)]]) {
    const float2 q = float2(i & 1, i >> 1);
    const float m = g.bd.w;
    const float2 pos = g.rect.xy - m + q * (g.rect.zw + 2.0 * m);
    GV v;
    v.p = float4(pos.x / g.view.x * 2.0 - 1.0, 1.0 - pos.y / g.view.y * 2.0, 0, 1);
    v.pos = pos;
    return v;
}
static float3 glass_tap(texture2d<float> bd, sampler s, float2 uv) {
    return (any(uv < 0.0) || any(uv > 1.0)) ? float3(0.047, 0.055, 0.07) : bd.sample(s, uv).rgb;
}
fragment float4 glass_f(GV v [[stage_in]], constant Glass& g [[buffer(0)]], texture2d<float> bd [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    const float2 hb = g.rect.zw * 0.5, c = g.rect.xy + hb;
    const float r = min(g.p.x, min(hb.x, hb.y));
    const float px = max(g.p.w, 1.0);
    const float d = glass_sd(v.pos - c, hb, r);
    if (d > 0.5) {
        // The shadow: the shape moved down a little, fading out over the margin.
        const float ds = glass_sd(v.pos - c - float2(0, 2.0 * px), hb, r);
        const float f = 1.0 - smoothstep(0.0, max(g.bd.w, 1.0), ds);
        return float4(0, 0, 0, 0.20 * g.p.z * f * f);
    }
    const float cover = saturate(0.5 - d);
    // The rim's outward normal, from the distance field.
    const float2 n = normalize(float2(glass_sd(v.pos + float2(1, 0) - c, hb, r) - glass_sd(v.pos - float2(1, 0) - c, hb, r),
                                      glass_sd(v.pos + float2(0, 1) - c, hb, r) - glass_sd(v.pos - float2(0, 1) - c, hb, r)) + 1e-5);
    const float inner = -d;
    const float bevel = max(min(min(hb.x, hb.y), 14.0 * px) * 0.9, 1.0);
    const float t = saturate(1.0 - inner / bevel);   // 1 on the rim, 0 inside the bevel
    // Lens: the rim pulls the picture from further in, and the whole piece magnifies a touch.
    const float2 sp = v.pos - n * (t * t * t) * bevel * 0.6 - (v.pos - c) * 0.05;
    float3 col;
    if (g.bd.z > 0.5) {
        const float br = 2.6 * px * (0.6 + 0.4 * t);
        col = glass_tap(bd, s, (sp - g.view.zw) / g.bd.xy);
        // Six more taps on a ring (bilinear does the rest); a faint piece (a resting stick's hint) takes one.
        if (g.p.z >= 0.5) {
            for (int k = 0; k < 6; ++k) {
                const float a = float(k) * 1.04719755 + 0.5;
                col += glass_tap(bd, s, (sp + float2(cos(a), sin(a)) * br - g.view.zw) / g.bd.xy);
            }
            col /= 7.0;
        }
    } else {
        col = float3(0.10, 0.11, 0.13);
    }
    const float lum = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = max(mix(float3(lum), col, 1.2), 0.0);   // a little more colour, as glass shows it
    col = col * 0.80 + 0.09;                       // frosted: lifted, less contrast
    col = mix(col, g.tint.rgb, g.tint.a);
    col += g.p.y * (0.10 + g.tint.rgb * 0.22);     // pressed: it lights up
    const float2 L = normalize(float2(-0.55, -0.85));
    const float spec = pow(saturate(dot(n, L)), 3.0) * pow(t, 4.0) * 0.55
                     + pow(saturate(dot(n, -L)), 3.0) * pow(t, 5.0) * 0.18;
    const float rim = (1.0 - smoothstep(0.0, 1.2 * px, inner)) * 0.20;
    col += spec + rim;
    const float a = cover * g.p.z;
    return float4(saturate(col) * a, a);
}
)MSL";

// "d3d", "surface_upload", "guest_readback", "guest_readback_fused", "native_brightness"
inline const char *const kD3DSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct HVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
    float sr, sg, sb, sa;
};
struct Uniforms {
    float4x4 mvp;
    uint  pretransformed;
    uint  textured;
    uint  texblend;
    uint  alphatest;
    uint  alphafunc;
    float alpharef;
    uint  specular;
    uint  texture_has_alpha;
    uint  fogmode;        // 0 off, 1 vertex, 2 exp, 3 exp2, 4 linear
    float fogstart, fogend, fogdensity;
    float fogr, fogg, fogb;
    float pointsize;
    uint terrain_detail;
};
struct VOut {
    float4 position [[position]];
    float  size [[point_size]];
    float2 uv;
    float4 color;
    float4 spec;
    float  fogdist;
};
// Colour, and the coverage mask that says this pixel was rasterized. A
// discarded fragment writes neither.
struct FOut {
    float4 color    [[color(0)]];
    float  coverage [[color(1)]];
};

vertex VOut d3d_vertex(uint vid [[vertex_id]],
                       const device HVertex* v [[buffer(0)]],
                       constant Uniforms& u [[buffer(1)]]) {
    HVertex s = v[vid];
    VOut o;
    o.position = u.pretransformed ? float4(s.x, s.y, s.z, s.w)
                                  : u.mvp * float4(s.x, s.y, s.z, 1.0);
    o.size = u.pointsize;
    o.uv = float2(s.u, s.v);
    o.color = float4(s.r, s.g, s.b, s.a);
    o.spec = float4(s.sr, s.sg, s.sb, s.sa);
    // Table fog is a function of eye-space distance, which after a standard
    // projection is the clip w.
    o.fogdist = abs(o.position.w);
    return o;
}

// Shade a world fragment using the guest material and optional terrain detail.
// Apply the legacy texture/alpha rules before writing color and coverage attachments.
fragment FOut d3d_fragment(VOut in [[stage_in]],
                           constant Uniforms& u [[buffer(1)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]],
                           texture2d<float> detail [[texture(1)]],
                           sampler detail_samp [[sampler(1)]]) {
    float4 c = in.color;
    if (u.textured != 0) {
        float4 t = tex.sample(samp, in.uv);
        if (u.terrain_detail != 0) {
            // An independent material sample supplies real fine structures;
            // enlarging the guest's 16/32 texels cannot create those. The
            // source remains the color/lighting/coastline mask. Blue water
            // fades this land-only layer out, including mixed shore texels.
            float3 chroma = t.rgb / max(max(t.r, t.g), max(t.b, 0.01f));
            float land = 1.0f - smoothstep(0.02f, 0.22f, chroma.b - chroma.r);
            float grass = smoothstep(-0.03f, 0.16f, chroma.g - chroma.r);
            float stone = 1.0f - smoothstep(0.06f, 0.28f, max(chroma.r, chroma.g) - min(chroma.r, min(chroma.g, chroma.b)));
            float sand = smoothstep(0.28f, 0.62f, max(t.r, t.g));
            float4 structure = (detail.sample(detail_samp, in.uv) * 255.0f - 128.0f) / 128.0f;
            float material = mix(structure.a, structure.g, sand);
            material = mix(material, structure.b, stone);
            material = mix(material, structure.r, grass);
            t.rgb = saturate(t.rgb * (1.0f + material * 0.85f * land));
        }
        switch (u.texblend) {
            case 1: case 7:                                   // DECAL, COPY
                c = t; break;
            case 2:                                           // MODULATE
                // The legacy rule: the texture's alpha when it has one,
                // otherwise the diffuse's. Taking the vertex alpha from a
                // vertex that never set it - and plenty of D3DTLVERTEX
                // colours leave it at zero - makes every blended pixel
                // invisible and the whole scene the colour of the clear.
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
            case 3:                                           // DECALALPHA
                c = float4(mix(in.color.rgb, t.rgb, t.a), in.color.a); break;
            case 5:                                           // DECALMASK
                c = float4(t.rgb, in.color.a); break;
            case 8:                                           // ADD
                c = float4(saturate(t.rgb + in.color.rgb), in.color.a); break;
            case 4:                                           // MODULATEALPHA
                c = float4(t.rgb * in.color.rgb, t.a * in.color.a); break;
            default:
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
        }
    }
    if (u.specular != 0) c = float4(saturate(c.rgb + in.spec.rgb), c.a);
    if (u.alphatest != 0) {
        bool pass;
        switch (u.alphafunc) {
            case 1: pass = false;                break;   // NEVER
            case 2: pass = c.a <  u.alpharef;    break;   // LESS
            case 3: pass = c.a == u.alpharef;    break;   // EQUAL
            case 4: pass = c.a <= u.alpharef;    break;   // LESSEQUAL
            case 5: pass = c.a >  u.alpharef;    break;   // GREATER
            case 6: pass = c.a != u.alpharef;    break;   // NOTEQUAL
            case 7: pass = c.a >= u.alpharef;    break;   // GREATEREQUAL
            default: pass = true;                break;   // ALWAYS
        }
        if (!pass) discard_fragment();
    }
    if (u.fogmode != 0) {
        // The fog factor is how much of the fragment survives: 1 is clear.
        float f = 1.0;
        float d = in.fogdist;
        switch (u.fogmode) {
            case 1: f = in.spec.a; break;                          // vertex fog
            case 2: f = exp(-u.fogdensity * d); break;             // EXP
            case 3: f = exp(-(u.fogdensity * d) * (u.fogdensity * d)); break;
            default:                                               // LINEAR
                f = (u.fogend - d) / max(u.fogend - u.fogstart, 1e-6);
                break;
        }
        f = saturate(f);
        c = float4(mix(float3(u.fogr, u.fogg, u.fogb), c.rgb, f), c.a);
    }
    FOut o;
    o.color = c;
    o.coverage = 1.0;
    return o;
}
// Reduce guest-visible reads on the GPU; never decompress a 4K texture on
// the CPU merely to select 640x480 pixel centres. Each lane also counts its
// disjoint native cell so existing full-resolution diagnostics stay exact.
kernel void guest_readback_fused(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    uint2 lo=(guest*native_size+guest_size-1)/guest_size;
    uint2 hi=((guest+1)*native_size+guest_size-1)/guest_size;
    uint lit=0;
    for(uint y=lo.y;y<hi.y;++y) for(uint x=lo.x;x<hi.x;++x)
        lit+=any(round(color.read(uint2(x,y)).rgb*255.0f)>8.0f) ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),lit);
}
// Guest sampling and native brightness reduction have different grids.
// Keeping the sampling lanes short avoids a variable native-cell loop in
// every guest lane. Packing and coverage rules match the fused reference.
kernel void guest_readback(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),0);
}
// A 16x16 threadgroup covers a 32x32 native tile. Each lane reads four
// adjacent pixels and each SIMD group writes one sum. Padded lanes contribute
// zero but participate in the reduction; no contended global atomic is used.
kernel void native_brightness(texture2d<float, access::read> color [[texture(0)]],
                              device uint* output [[buffer(0)]],
                              constant uint4* p [[buffer(1)]],
                              uint2 tid [[thread_position_in_grid]],
                              uint2 group [[threadgroup_position_in_grid]],
                              uint lane [[thread_index_in_simdgroup]],
                              uint simd_group [[simdgroup_index_in_threadgroup]]) {
    uint2 pos=p[0].xy+tid*2;
    uint lit=0;
    for(uint y=0;y<2;++y) for(uint x=0;x<2;++x) {
        uint2 q=pos+uint2(x,y);
        if(all(q<p[0].zw)) lit+=any(round(color.read(q).rgb*255.0f)>8.0f) ? 1:0;
    }
    uint sum=simd_sum(lit);
    if(lane==0) output[p[1].x+(group.y*p[1].y+group.x)*p[1].z+simd_group]=sum;
}
// Seed a native target directly from an immutable guest-sized buffer. Integer
// channel expansion and floor(x * guest / native) preserve the CPU upload's
// exact pixels without expanding the whole 4K image on the guest thread.
vertex float4 surface_upload_vertex(uint id [[vertex_id]]) {
    float2 corner=float2((id<<1)&2,id&2);
    return float4(corner*2.0f-1.0f,0,1);
}
fragment float4 surface_upload_fragment(float4 position [[position]],
    device const uchar* pixels [[buffer(0)]],
    constant uint4* p [[buffer(1)]], constant uint* palette [[buffer(2)]]) {
    uint2 xy=uint2(position.xy)*p[0].xy/p[0].zw;
    uint offset=xy.y*p[1].x+xy.x*(p[1].y==8 ? 1:(p[1].y<=16 ? 2:4));
    uint3 rgb;
    if(p[1].y==8) {
        uint index=pixels[offset];
        uint c=p[1].z ? palette[index] : index*0x010101u;
        rgb=uint3((c>>16)&255,(c>>8)&255,c&255);
    } else {
        uint value=uint(pixels[offset])|(uint(pixels[offset+1])<<8);
        if(p[1].y>16) value|=(uint(pixels[offset+2])<<16)|(uint(pixels[offset+3])<<24);
        rgb=((uint3(value)&p[2].xyz)>>p[3].xyz)*255/p[4].xyz;
    }
    return float4(float3(rgb)/255.0f,1);
}
)MSL";

} // namespace gpu::metal
