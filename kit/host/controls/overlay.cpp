// overlay.cpp - the GPU half of overlay.h: one raster per layer, and the knobs.
#include "overlay.h"

#include "overlay_paint.h"
#include "pad_art.h"
#include "raster.h"

#include <algorithm>
#include <cmath>

namespace controls {

namespace {

// The knob texture's side: big enough for the largest stick on a 3x screen,
// and scaled linearly for any other.
constexpr int kKnobSize = 256;

} // namespace

Overlay::~Overlay() {
    release();
}

void Overlay::release() {
    if (device_) {
        for (LayerTexture &t : layers_)
            if (t.texture)
                device_->destroy(t.texture);
        if (knob_)
            device_->destroy(knob_);
        if (dummy_)
            device_->destroy(dummy_);
    }
    dummy_ = gpu::Texture();
    glass_state_ = -1;
    layers_.clear();
    knob_ = gpu::Texture();
    knob_opacity_ = -1;
}

// Re-rasterizes and re-uploads layer `index` when its revision or the
// drawable size changed; any other layer's texture is left alone.
void Overlay::update_layer(gpu::Device *device, const ControlsView &view, int index, int w, int h) {
    LayerTexture &t = layers_[index];
    const ControlsView::Layer &layer = view.layers[index];
    if (t.built && t.revision == layer.revision && dw_ == w && dh_ == h)
        return;
    // Clip to the drawable.
    const Rect &r = layer.rect;
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
    const Rect clipped{x0, y0, x1 - x0, y1 - y0};
    t.built = true;
    t.revision = layer.revision;
    t.rect = clipped.empty() ? Rect{} : clipped;
    if (t.rect.empty())
        return;
    pixels_.assign(size_t(t.rect.w) * t.rect.h * 4, 0);
    // The glass look's glyphs stay solid over the glass whatever the opacity setting (the glass itself fades).
    Canvas c(pixels_, t.rect.w, t.rect.h, glass_ ? std::min(1.0, 0.35 + view.opacity) : view.opacity);
    paint_layer(c, view, index, t.rect, glass_);
    if (!t.texture || t.tex_w != t.rect.w || t.tex_h != t.rect.h) {
        if (t.texture)
            device->destroy(t.texture);
        t.texture = device->create_texture(
            {t.rect.w, t.rect.h, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        t.tex_w = t.rect.w;
        t.tex_h = t.rect.h;
    }
    if (t.texture)
        device->upload(t.texture, {0, 0, t.rect.w, t.rect.h}, pixels_.data(), t.rect.w * 4);
}

// Paints the knob once, and again only when the opacity changes.
void Overlay::update_knob(gpu::Device *device, double opacity) {
    if (knob_ && knob_opacity_ == opacity)
        return;
    if (!knob_)
        knob_ = device->create_texture(
            {kKnobSize, kKnobSize, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
    if (!knob_)
        return;
    std::vector<uint8_t> px(size_t(kKnobSize) * kKnobSize * 4, 0);
    Canvas c(px, kKnobSize, kKnobSize, opacity);
    paint_knob(c, kKnobSize / 2.0, kKnobSize / 2.0, kKnobSize / 2.0 - 1, true);
    device->upload(knob_, {0, 0, kKnobSize, kKnobSize}, px.data(), kKnobSize * 4);
    knob_opacity_ = opacity;
}

void Overlay::blit(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h) {
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("hud", state);
    if (!pipeline)
        return;
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    for (const Quad &q : quads_) {
        // Clip-space rectangle: x, y of the top-left corner, width, and a
        // negative height (y up), the encoding the hud pipeline's strip used.
        const float x0 = -1.0f + 2.0f * float(q.rect.x) / float(w);
        const float y0 = 1.0f - 2.0f * float(q.rect.y) / float(h);
        float rect[] = {x0, y0, 2.0f * float(q.rect.w) / float(w),
                        -2.0f * float(q.rect.h) / float(h)};
        device->set_bytes(cb, gpu::Stage::Vertex, 0, rect, sizeof rect);
        device->set_texture(cb, gpu::Stage::Fragment, 0, q.texture);
        device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
        device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    }
    device->end_render_pass(cb);
}

// Whether this device has the "glass" pipeline (Metal only); asked once.
bool Overlay::glass_available(gpu::Device *device, gpu::Format format) {
#if defined(__APPLE__)
    if (glass_state_ < 0) {
        gpu::RenderState state;
        state.color_format[0] = format;
        state.color_count = 1;
        state.blend_enabled = true;
        state.src_rgb = state.src_alpha = gpu::Blend::One;
        state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
        glass_state_ = device->render_pipeline("glass", state) ? 1 : 0;
    }
    return glass_state_ == 1;
#else
    (void)device;
    (void)format;
    return false;
#endif
}

// One glass piece per control (or, with `knobs`, per held or resting stick knob), each its own small quad: the
// fragment shader only runs over the controls and a shadow margin around them.
void Overlay::draw_glass(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
                         const ControlsView &view, const Backdrop &backdrop, bool knobs) {
    std::vector<GlassShape> shapes;
    for (const DrawControl &d : view.controls) {
        if (d.kind == Kind::Stick && knobs) {
            if (d.radius_px > 0)
                shapes.push_back(glass_knob(d));
            continue;
        }
        GlassShape g;
        if (!knobs && glass_shape(d, &g))
            shapes.push_back(g);
    }
    if (shapes.empty())
        return;
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("glass", state);
    if (!pipeline)
        return;
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    const bool has_bd = backdrop.texture && backdrop.w > 0 && backdrop.h > 0;
    if (!has_bd && !dummy_) {
        // The shader declares the backdrop; without one it is bound to a pixel it never reads.
        dummy_ = device->create_texture({1, 1, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        const uint8_t px0[4] = {0, 0, 0, 255};
        if (dummy_)
            device->upload(dummy_, {0, 0, 1, 1}, px0, 4);
    }
    device->set_texture(cb, gpu::Stage::Fragment, 0, has_bd ? backdrop.texture : dummy_);
    device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
    const double px = view.px_per_pt;
    // The glass fades with the opacity setting, but never below a third (it is mostly the picture anyway).
    const double fade = std::clamp(view.opacity * 1.2, 0.35, 1.0);
    for (const GlassShape &g : shapes) {
        const float u[20] = {float(g.rect.x), float(g.rect.y), float(g.rect.w), float(g.rect.h),
                             float(w), float(h), float(backdrop.x), float(backdrop.y),
                             g.tint[0], g.tint[1], g.tint[2], g.tint[3],
                             float(g.radius), float(g.pressed), float(g.alpha * fade), float(px),
                             float(backdrop.w), float(backdrop.h), has_bd ? 1.0f : 0.0f, float(6.0 * px)};
        device->set_bytes(cb, gpu::Stage::Vertex, 0, u, sizeof u);
        device->set_bytes(cb, gpu::Stage::Fragment, 0, u, sizeof u);
        device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    }
    device->end_render_pass(cb);
}

void Overlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
                   const ControlsView &view, const Backdrop &backdrop) {
    if (!device || !target || !cb || w <= 0 || h <= 0 || !view.wanted)
        return;
    if (device_ != device) {
        release();
        device_ = device;
    }
    // The glass look, where it can be drawn; switching it repaints every layer.
    const bool glass = view.glass && !view.editing && glass_available(device, device->describe(target).format);
    if (glass != glass_) {
        glass_ = glass;
        for (LayerTexture &t : layers_)
            t.built = false;
    }
    // A different layer count (a new layout) starts every layer afresh.
    if (layers_.size() != view.layers.size()) {
        for (LayerTexture &t : layers_)
            if (t.texture)
                device->destroy(t.texture);
        layers_.assign(view.layers.size(), LayerTexture{});
    }
    quads_.clear();
    for (int i = 0; i < int(layers_.size()); ++i) {
        update_layer(device, view, i, w, h);
        const LayerTexture &t = layers_[i];
        if (t.texture && !t.rect.empty() && t.tex_w == t.rect.w && t.tex_h == t.rect.h)
            quads_.push_back({t.texture, t.rect});
    }
    dw_ = w;
    dh_ = h;
    if (glass_) {
        // Glass under, the glyphs and labels on it, then the sticks' knobs over both.
        draw_glass(device, cb, target, w, h, view, backdrop, false);
        if (!quads_.empty())
            blit(device, cb, target, w, h);
        draw_glass(device, cb, target, w, h, view, backdrop, true);
        return;
    }
    // Every held stick's knob, at its base plus the offset.
    for (const DrawControl &d : view.controls) {
        if (d.kind != Kind::Stick || !d.pressed || d.radius_px <= 0)
            continue;
        update_knob(device, view.opacity);
        if (!knob_)
            break;
        const int kr = int(std::lround(knob_radius(d.radius_px)));
        const double cx = d.base_x + d.knob_x * d.radius_px;
        const double cy = d.base_y + d.knob_y * d.radius_px;
        quads_.push_back(
            {knob_, Rect{int(std::lround(cx)) - kr, int(std::lround(cy)) - kr, 2 * kr, 2 * kr}});
    }
    if (!quads_.empty())
        blit(device, cb, target, w, h);
}

} // namespace controls
