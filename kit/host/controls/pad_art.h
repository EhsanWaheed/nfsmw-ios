// pad_art.h - the DualSense-styled look of the on-screen pad controls: face
// buttons with their glyphs, shoulders and triggers, the dpad, stick bases
// and knobs, and the small system buttons. Pure functions over a Canvas, so
// controls_tests checks their pixels without a GPU. Keys and toggles keep
// the keypad's flat look (overlay_paint.cpp).
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md, 8.1.
#pragma once

#include "overlay.h"
#include "raster.h"

namespace controls {

// Paints a Button, Dpad, Stick or Action control at c.rect, offset by the
// canvas origin (ox, oy) in drawable pixels. A held stick's knob is not
// painted (Overlay draws it as its own quad); a resting stick's knob is,
// at the base's centre, so an idle stick still looks like one.
void paint_control(Canvas &cv, const DrawControl &c, int ox, int oy);

// A concave stick knob of radius r (knob_radius() of the stick's travel),
// centred on (cx, cy): a dark disc with its highlight up and to the left,
// brighter while pressed. Overlay paints it once into its own small texture.
void paint_knob(Canvas &cv, double cx, double cy, double r, bool pressed);

// The glass look (v4.3): only what sits on the glass, which Overlay draws on the GPU under it: face glyphs in their
// PlayStation colours, labels in the system font (raster.h set_text_hook), the D-pad's arrows. Keys, toggles and
// actions too. A stick has nothing on its glass.
void paint_control_glass(Canvas &cv, const DrawControl &c, int ox, int oy);

// The glass shape of a control, for Overlay's glass pass: its rectangle in drawable pixels, corner radius, tint
// (rgb 0..1, strength) and how see-through (1 opaque). False: no glass (a pressed stick's base is drawn, its knob
// is a separate shape from glass_knob).
struct GlassShape {
    Rect rect;
    double radius = 0;
    float tint[4] = {1, 1, 1, 0};
    double pressed = 0;
    double alpha = 1;
};
bool glass_shape(const DrawControl &c, GlassShape *out);
// A held stick's knob as a glass disc at its offset.
GlassShape glass_knob(const DrawControl &c);

// The knob's radius for a stick whose travel is radius_px.
double knob_radius(int radius_px);

} // namespace controls
