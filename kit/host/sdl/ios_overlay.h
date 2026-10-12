// ios_overlay.h - the iOS settings panel: a UIKit panel above the game's Metal
// view, beside (not instead of) the kit's own F10 settings page. Its Home tab
// holds what only the iOS host can do: the MetalFX internal resolution
// (host_upscale_set_rows), a frame cap with a ProMotion frame-rate hint,
// thermal-driven dynamic resolution, stretch to fill, and two experimental
// launch options (NSUserDefaults). Its Graphics, Mods and Controls tabs show
// the F10 menu's own pages (mods_menu_open_headless), drawn natively; Controls
// also assigns the shift buttons by pressing them. A glass bubble names a
// controller as it connects, and the core mod is told its kind (`pad_kind`)
// so the menus' button icons match it.
//
// Opens and closes with L3+R3 on a controller, Cmd+S on a hardware keyboard,
// or a three-finger hold (a three-finger tap is the kit's F10 page).
#pragma once

#include <SDL3/SDL.h>

// Before the mods load (platform_ui_ios_prepare): the launch-time options as
// the environment the renderer and the core mod read (MetalFX scene markers,
// effects before the upscale, the simulation rate).
void ios_overlay_apply_launch_settings();

// Once the window exists (main thread): the panel, its triggers, the thermal
// observer, and the saved resolution and frame cap applied.
void ios_overlay_install(SDL_Window *window);

// True while the panel is up; the host withholds controller input from the game then.
bool ios_overlay_visible();
