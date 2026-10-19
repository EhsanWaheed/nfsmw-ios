// raster.h - a small premultiplied-RGBA canvas the controls overlay draws
// into on the CPU: anti-aliased rects, rounded rects, discs, rings, polygons,
// strokes and the 6x8 font, each fillable with a flat colour or a linear or
// radial gradient. Coverage is 4x4 supersampled per pixel, inside the
// shape's bounding box only. Moved out of the old keypad overlay (Task 7);
// the gradient fills and anti-aliasing are for the DualSense look (Task 11).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace controls {

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

// A fill: flat, or a gradient between two colours.
struct Paint {
    enum Kind { Flat, Linear, Radial } kind = Flat;
    Rgba c0{}, c1{};
    double x0 = 0, y0 = 0, x1 = 0,
           y1 = 0; // Linear: from (x0,y0) to (x1,y1); Radial: centre x0,y0, radius x1
    // True writes colour * coverage * the canvas opacity straight into the
    // pixel instead of compositing source-over. The keypad's pixels depend
    // on this: a key at alpha 220 must overwrite the backdrop drawn under it
    // at alpha 150, not blend with it.
    bool replace = false;
};

// v4.3: a platform text rasterizer for the glass look's labels (iOS: CoreText, and "sf:<name>" draws that SF
// Symbol). Writes a premultiplied RGBA bitmap of the text in `color` at `size_px` (the cap-to-baseline line
// height) into *rgba, *w x *h; false when it cannot. Without one, Canvas::label falls back to the 6x8 font.
using TextHook = bool (*)(const char *utf8, double size_px, bool bold, Rgba color, std::vector<uint8_t> *rgba,
                          int *w, int *h);
void set_text_hook(TextHook hook);
bool have_text_hook();

// Premultiplied RGBA, source-over (or, with Paint::replace, plain
// overwrite), anti-aliased by 4x4 supersampled coverage.
class Canvas {
  public:
    // `px`, a w x h RGBA buffer the caller owns, is written into (never
    // resized); everything is clipped to it. `opacity` (clamped to [0,1])
    // scales every paint's alpha, flat or gradient, replace or not.
    Canvas(std::vector<uint8_t> &px, int w, int h, double opacity = 1.0);

    void rect(double x, double y, double w, double h, const Paint &p);
    void round_rect(double x, double y, double w, double h, double radius, const Paint &p);
    // A filled circle centred on (cx, cy).
    void disc(double cx, double cy, double r, const Paint &p);
    // An annulus: the area between r_inner and r_outer.
    void ring(double cx, double cy, double r_outer, double r_inner, const Paint &p);
    // A closed polygon (even-odd), e.g. a dpad arrow or a trapezoid.
    void polygon(const std::vector<std::pair<double, double>> &pts, const Paint &p);
    // A polyline stroke with round joins (the union of segment capsules);
    // used for the face glyphs. `closed` also strokes the last->first edge.
    void stroke(const std::vector<std::pair<double, double>> &pts, double width, bool closed,
                const Paint &p);
    // 6x8 glyphs at `scale`x: 6*scale pixels per column, 8*scale per row.
    void text(int x, int y, const char *s, Rgba c, int scale = 2, bool replace = false);
    int text_width(const char *s, int scale = 2) const;
    // Text centred on (cx, cy) about size_px tall, no wider than max_w: the text hook's when there is one (cached
    // per string, size and colour), else the 6x8 font at the largest scale that fits.
    void label(double cx, double cy, const char *s, double size_px, double max_w, Rgba c, bool bold = true);
    Rgba at(int x, int y) const;

  private:
    // Rasterizes `inside` (a point-in-shape test in canvas pixel space) over
    // its bounding box [x0,x1) x [y0,y1), clipped to the canvas.
    template <class Inside>
    void fill_shape(double bx0, double by0, double bx1, double by1, const Paint &p,
                    const Inside &inside);

    std::vector<uint8_t> &px_;
    int w_, h_;
    double opacity_;
};

} // namespace controls
