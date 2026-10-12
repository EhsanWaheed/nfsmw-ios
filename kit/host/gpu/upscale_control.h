// upscale_control.h - the host's live control of the MetalFX scene upscale
// (host/gpu/metal/d3d9_metal.mm, Remaster run 13). The core mod marks the 3D
// scene; between its markers the scene draws into a smaller backing that
// MTLFXSpatialScaler scales up to the back buffer, and the HUD and menus draw
// at the back buffer's own size afterwards.
//
// Without a call here the scene's size follows RECOMP_REMASTER_UPSCALE (a
// fixed factor read at launch), exactly as before. Once a host calls
// host_upscale_set_rows, that height wins, and changes from the next scene on
// (the backings and the scaler are made again at the new size).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The 3D scene's height in physical pixels; its width follows the back
// buffer's aspect. 0 draws the scene at the back buffer's size (no upscale);
// a negative value returns control to RECOMP_REMASTER_UPSCALE. A height at or
// above the back buffer's also draws native. The scale is at most 4x.
void host_upscale_set_rows(int rows);

// The size the last upscaled scene drew at and the size it was scaled to, in
// physical pixels; 0 before one has (or while the scene draws native).
void host_upscale_last_sizes(int *in_w, int *in_h, int *out_w, int *out_h);

// The presenter's picture placement (host/compositor.cpp): 0 keeps the game's
// aspect (bars where it differs from the screen's), 1 stretches it to fill.
void host_compositor_set_stretch(int stretch);

#ifdef __cplusplus
}
#endif
