# The iOS port

How Need for Speed: Most Wanted runs on iPhone and iPad, what was changed for it (October 2026), how to build
and test it, and what is still open. Written for the owner and for a later Claude session picking the work up.

The game is a static recompilation of the owner's `speed.exe` (the recomp kit in `kit/`, the game's core mod in
`mods/core/nfsmw`). The iOS host is SDL3 + UIKit in `kit/host/sdl/`; the renderer is the kit's D3D9-to-Metal
backend (`kit/host/gpu/metal/`).

## Building

### The normal way (Terminal)

```sh
cd ~/Library/NFSMW-Native-Setup/source
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer .venv/bin/python tools/build.py --target ios --unsigned
```

Output: `build/ios/SpeedRecomp.ipa` (game files included, about 3.2 GB) for LiveContainer / SideStore.
`--team <your Team ID>` instead of `--unsigned` signs with the owner's Apple ID instead.

`tools/build.py` first makes the controller button pack (see below) into `build/generated/`, then runs the kit's
build (CMake, Xcode generator, project `build/cmake/ios/PopulousRecomp.xcodeproj`).

### From Xcode

Open `build/cmake/ios/PopulousRecomp.xcodeproj` (a Desktop alias, "NFSMW iOS (Xcode project)", points at it),
target `recomp_app`, your iPhone, Run. Its cache is set to sign with team `<your Team ID>`
(`build/cmake/ios/CMakeCache.txt`: `RECOMP_IOS_TEAM`, `RECOMP_IOS_UNSIGNED=OFF`; the unsigned original is
`CMakeCache.txt.unsigned-backup`). The project is generated: source edits made in Xcode are kept (they are the
repository's files), build-setting edits made in Xcode are lost when CMake regenerates it. Put lasting settings in
`kit/cmake/IosBundle.cmake` or `kit/CMakeLists.txt`.

### From a Claude session running inside Xcode

There `xcodebuild` cannot write its module cache (`/var/folders/.../org.llvm.clang/ModuleCache.noindex`:
"Operation not permitted", even with the sandbox lifted), so `tools/build.py --target ios` fails. Build with Ninja
instead (the directory `build/cmake/ios-ninja` is configured already, same options as the `ios` preset plus
`-DRECOMP_IOS_UNSIGNED=ON`):

1. The button pack, if `tools/padicons/make_pad_buttons.swift` changed:
   `xcrun swiftc -O -module-cache-path /tmp/swiftcache tools/padicons/make_pad_buttons.swift -o /tmp/mpb && /tmp/mpb build/generated/padicons/GLOBAL/RecompPadButtons.tpk`
2. `CLANG_MODULE_CACHE_PATH=/tmp/clangmodcache .venv/bin/cmake --build build/cmake/ios-ninja --target recomp_app -j 8`
   (about 4 minutes; the app is `build/ios/SpeedRecomp.app`). CMake writes the bundle's Info.plist at configure
   time: do not delete the `.app` after configuring, or configure again (`.venv/bin/cmake -S kit -B build/cmake/ios-ninja`).
3. Finish what Xcode would have done: copy `build/cmake/ios-ninja/ffmpeg/lib/{libavformat.61,libavcodec.61,libavutil.59}.dylib`
   (`cp -L`) into the app's `Frameworks/`, `PkgInfo` from `build/ios/Release/SpeedRecomp.app`, and merge that
   bundle's extra Info.plist keys (DT*, `UIRequiredDeviceCapabilities`, `BuildMachineOSBuild`) into the new plist
   with `plistlib` (do not copy the old plist over: it has the old bundle id).
4. Package with `zip -qr -0 -X -y SpeedRecomp.ipa Payload` (the app moved into a `Payload/` folder and back), not
   `ditto --sequesterRsrc`: the session's provenance attributes would become `__MACOSX/` entries.

### The testers' IPA (no game files)

`build/ios/NFSMWIOS-NGF.ipa` (about 60 MB, "No Game Files"): the same app without `game/`. Made from an APFS
clone of the app (`cp -cR`, then remove `game/`), then **scrubbed**: the build paths in the binary and the FFmpeg
dylibs carry the Mac's user name, so `/Users/<name>/` is replaced with a same-length `/Users/nfsmw-build/`, each
binary re-signed ad hoc (`codesign -f -s -`), and the result checked for the owner's identifiers (none may remain)
before `zip -9`. The owner's own copy with the game, `NFSMWIOS.ipa`, is packaged as in step 4 above and kept private. On first launch the launcher asks the tester for their
own game (a folder or ZIP from the Files app, or copied in with Finder). Note that the app still contains code
translated from `speed.exe` and effect programs generated from the owner's game; sharing it is the owner's call.

### What a tester's game copy must contain

The launcher checks `speed.exe` by its SHA-256 and accepts only the build this port was translated from: the
official **v1.3** PC executable (`speed.exe`, 6,029,312 bytes, SHA-256
`80774c2e5d619b4f120b48d4462896fd504c263399d203a238769cffde1d253c`; the Black Edition and the 1.3 patch). Any other
version is refused at import. The folder to import (as a folder or a ZIP) is the game's install folder:

- `speed.exe`
- `CARS/`, `FRONTEND/`, `GLOBAL/`, `LANGUAGES/`, `MOVIES/`, `NIS/`, `SOUND/`, `TRACKS/` (required)
- `CREDITS/`, `MEMCARD/`, `SUBTITLES/` and the small files beside `speed.exe` (keep them; they are tiny)

Not needed (left out like the bundled copy, `game.toml` [bundle]): `*.dll`, `*_inst.exe`, `*.txt`, `server.cfg`,
`Uninstall`, `Support`, `scripts`, `SAVE`. About 2.8 GB in all.

### Publishing (GitHub: EhsanWaheed/nfsmw-ios, public)

The repository holds source only: `build/` (the translation of `speed.exe`, IPAs), `analysis/` (decompiled
pseudocode), `original/` (the owner's game) and `*.ipa` are ignored. The only app package ever shared is
`SpeedRecomp-NGF.ipa` (no game files), attached to a GitHub release, never committed; the full `SpeedRecomp.ipa`
is the owner's own copy and stays private.

## What the iOS port has (October 2026)

### Settings panel (`kit/host/sdl/ios_overlay.mm`)

A UIKit panel (Liquid Glass on iOS 26+) above the game, opened with L3+R3, Cmd+S, F10, the on-screen F10 button,
or a three-finger tap or hold. It replaces the kit's F10 page on iOS: `mods_page_set_redirect`
(`kit/mods/settings_page.cpp`) sends F10 and every request to open the page to the panel, so the page never draws
(a three-finger tap no longer sends F10 either: `kit/host/input_touch.cpp`). The page also never draws behind the panel:
`mods_menu_render` (`kit/mods/settings_menu_draw.cpp`) requires the page itself to be open, since the headless model
the panel's tabs use has a snapshot too (v2.1; it drew behind the Graphics, Mods and Controls tabs). Settings persist in NSUserDefaults (`ios.*` keys). Tabs:

- **Home** (iOS-only settings):
  - *Output resolution* (720p default, 900p, 1080p, the screen's own; next launch): the game's own back buffer,
    shaped to the screen. The host publishes the screen size scaled to this height (`RECOMP_IOS_OUTPUT_ROWS`, read
    in `kit/host/sdl/main.cpp` `note_screen_size`) and the core mod's Auto resolution uses it.
  - *3D resolution* (540p, 600p, 720p ... Native; instant): the MetalFX scene height (`host_upscale_set_rows`,
    `kit/host/gpu/metal/d3d9_metal.mm`). Native = the output size, no MetalFX. First-run default: the largest step
    at most 85% of the output (600p at 720p). A step above 5/6 of the output draws at 5/6 (v3, `upscale_begin`;
    v2.3 drew it native): the spatial scaler at a ratio near 1 (800p onto 900p, 9/8) left a grid of fine lines every
    1/(ratio-1) scene pixels, sharp where scene and output pixels line up and soft between.
  - *Upscaler* (v3; next launch, with the restart card): **Temporal** (default since v4.1) or Spatial (compatibility), below.
  - *Frame cap* 30/60/80/120 (v3: 80, not 90 — ProMotion runs at 120/80/60/40/30) with a ProMotion rate hint. The
    game's own limiter (`kit/runtime/frame_deadline.h`) paces any cap from 20 to 240 (it took only 40/60/120, so 80
    and 30 ran on the display's pacing alone and went over). 40 fps in the front end's menus and 30 while paused or
    with the panel open (`applyFrameCap`, game phase from private render state 248, `host_d9_game_phase`).
  - *Dynamic resolution*: one 3D step down at thermal fair, two at serious, 540p at critical; back up as it cools.
  - (*Stretch to fill* was removed in v2: the output has the screen's own shape, so it had nothing to do; a saved
    value is cleared and `host_compositor_set_stretch(0)` applied.)
  - *Performance*: switches for the Definitive look's heaviest extras, which the Detail presets do not touch
    (`quality_presets.h` sets only the game's own video values): ambient occlusion, motion blur, background blur,
    mirror scenery (live), softer shadows, the HD mirror and Always rain (next launch). Always rain turns the
    weather cycle off for the session (`eo_extra.h`): the cycle's dry phases stopped the game's forced rain.
  - *Effects at internal resolution* (on by default; next launch): glow / DoF / blur before the upscale.
  - *Max performance (experimental)* (v2.1; next launch, with a Liquid Glass restart card): for hotter or older
    phones, aiming at 60 fps. `kMaxPerfRecipe`: Detail Low and every heavy extra off, the player's values saved in
    `ios.maxPerfSaved` and put back when it goes off; dynamic resolution forced on. The Definitive look, not the PC
    look: the PC look never upscales (`upscale.h`), so dynamic resolution could not help it. `RECOMP_IOS_LOW_POWER=1`
    makes the host's event wait sleep through a sub-millisecond remainder instead of polling (`main.cpp` service).
    The frame cap itself already sleeps (`native_frame_clock` retires the game's GetTickCount spin).
- **Graphics / Mods / Controls**: the F10 menu's own pages drawn natively. `mods_menu_open_headless` /
  `mods_menu_close_headless` (`kit/mods/settings_menu.cpp`) open the menu as a model without drawing the page or
  taking the keyboard; the panel reads `mods_menu_snapshot` and acts with `mods_menu_pointer_line`. Apply/Discard
  sit in the footer. Controls adds the connected controller and "Assign" for the shift buttons (press a button;
  kit order square, cross, circle, triangle, l1, r1, l2, r2, select, start, l3, r3, ps).
- **Diagnostics** (v2.3; live, off at every launch): the reflection test, `host_d9_diag_set` in `d3d9_metal.mm`:
  hold the car reflection maps (cube-face draws and clears skipped), hold the shadow map (square depth textures the
  scene samples), log those passes for 600 frames, and capture 12 consecutive frames (`frame_NN.ppm` + a draw list
  `frame_NN.txt` each) into `Documents/Logs/capture-<time>`, starting 5 s after the switch (the panel pauses the race).
- **MetalFX temporal** (v3, experimental; `RECOMP_REMASTER_UPSCALE_TEMPORAL=1` + the foundation and its jitter, set at
  launch by the panel). Per-draw motion vectors, the technique of the Xbox 360 port bep1s69/nfsmw-ios (written anew
  here): every depth-tested scene draw gets an identity (vertex program, stream 0 buffer/offset, index buffer, range,
  and how many draws of the same came before it this scene); its vertex constants are kept, and the next scene's
  matching draw gets them back (`tmp_bind`, vertex buffers 20/21). The motion variants (`d3d9_shadergen.cpp`
  `motion_vs_patch` / `motion_ps_patch`) run the vertex program's whole body a second time on those previous
  constants (any position maths: the car's world x view-projection, skinning) and write previous minus current
  position to colour 1 (RG16F, `tmp_.om`), unblended, also for blended draws that write depth (car paint, glass).
  Pixels without a vector (sky, particles, new objects) get the foundation's camera reprojection from the depth, only
  when its view-projection pick agrees with the game camera. Halton(2,3) jitter on the depth-tested draws only: the
  PC game's full-screen 2D passes inside the scene (our clean chain: AO apply, motion blur / DoF, glow, finish; the
  front end's world layer) copy the picture and draw it back, and jittering them again shook the picture by several
  pixels. `MTLFXTemporalScaler` gets the scene, its depth, the vectors, the jitter as drawn (+, y down) and a reset
  only on real breaks (cut, camera jump, FOV, a gap, a resize); the composed native depth is read unjittered for the
  effects after the scene. Anything missing on a frame: that frame is scaled spatially. Diagnostics: *Show motion
  vectors* (hue = direction) and *Flip temporal jitter*. Log: `[remaster] temporal:` every 600 frames.
- **Background and the panel** (v3): in the background the game is held at its next Present (`host_d9_suspended`,
  no GPU work where iOS refuses it) and the audio output is reopened on return (a screen lock stopped it); opening the
  panel in a race presses the game's own pause (Escape).
- First run on a fresh profile: Detail set to Low once (`seedFirstRun`).
- The final stretch of the output to the screen (`compositor.cpp` + `shaders_msl.h` `compositor_sample`) is filtered
  on iOS when the scale is fractional (nearest sampling everywhere else, as before).

### Metal 4 renderer (v4)

The Direct3D 9 renderer encodes on a Metal 4 queue with MetalFX 4 scalers (plan and design: `docs/METAL4_PLAN.md`;
wrappers `kit/host/gpu/metal/d3d9_encoders.h`). Diagnostics → *Renderer*: **Metal 4** (default) or **Metal
(compatibility)** (the v3 path), next launch; a device without Metal 4 runs the v3 path by itself. One long-lived
residency set holds every resource (and keeps it alive until the GPU is past it), argument-table entries are set
only when they change, small constant blocks are reused, and barriers are precise (Diagnostics *Barriers: Safe* puts
one before every pass). Pipelines are built once per shader pair and specialized per blend state (keyed by the
vertex layout's contents). The efficiency work that came with it, for both renderers:

- **Buffer locks** (`kit/dx/d3d9.cpp`): a Lock copies and uploads only the bytes locked (it copied the whole buffer
  both ways), DISCARD and NOOVERWRITE reach the renderer, and a buffer the GPU still reads cycles through up to 128
  replacements instead of making new ones (one 96 KB buffer is rewritten up to 34 times a frame).
- **Pool fusion** for pixel shaders 1.x and wet roads: the lamp light is added in the same draw (85-99% of pool
  draws fused, was ~2%); the blended ones stay two-pass.
- **Exposure** (`effects/clean-chain.txt`, `tools/effects/gen_luma2.py`): the log and quartic luminance chains are
  one RGBA32F chain (12 passes -> 6).
- **Temporal history** resets when the game's cut-scene flag comes on, or on a jump, FOV change or a turn of more
  than 30 degrees, not on every frame the flag is held.
- The perf lines (`[gpu] perf ...`, every 600 frames) give the render thread's encode time, GPU time, draws, passes,
  barriers, residency changes, new resources per frame and the buffers making them.

### Controller button icons

The pad prompts once drew blank: `frontend_pad.h` swapped them to textures from XtendedInput's
`XtendedInputButtons.tpk`, which is not part of the game. Now:

- `tools/padicons/make_pad_buttons.swift` draws PlayStation (`PS3_*`) and Xbox (`XBX_*`) glyphs (CoreGraphics, no
  game data) and writes a TPK v5 pack, 64x64 DXT3 with one level, in the layout of the game's own packs (verified
  against `GLOBAL/GLOBALB.BUN` and `CARS/*/TEXTURES.BIN`). `--preview <png>` shows them decoded.
- `frontend_pad.h` loads `GLOBAL\RecompPadButtons.tpk` through an overlay (`<mod>/padicons`); without the pack the
  menus keep the keyboard prompts. The style follows `pad_glyphs` (0 match controller, 1 PlayStation, 2 Xbox) and
  `pad_kind` (hidden; the iOS host sets it from the controller: DualSense, DualShock and "PlayStation" names → 1).
- A Liquid Glass bubble (`UIGlassEffect`, iOS 26+; blur below) names a controller as it connects or goes.

### Menus on a 4:3 iPad

The game's widescreen menu layout is forced on (`game.toml`), with some objects near the 16:9 edges (the TRAX song
box, page titles). On a 4:3 screen they fell off the sides. With `RECOMP_FE_FIT_WIDE=1` (set by the iOS host)
`mods/core/nfsmw/video_modes.h` scales the front end down just enough for a 742-unit band (854 - 2 x 56) to fit:
0.86 at 4:3, nothing at 16:10 or wider. The in-race HUD and movies are not affected.

### MetalFX in the front end

`mods/core/nfsmw/upscale.h`: with `RECOMP_UPSCALE_FRONTEND=1` (set by the iOS host) the scene markers are sent in
the front end too, not only in the world, so the garage scene is upscaled. The PC look still sends none.

### App identity

Bundle id `com.E5N97.nfsmw` (`game.toml`). Icon: `build/ios-icon.png` when present (a crop of the owner's
capture of the main menu's M3 GTR; kept out of git), through `kit/tools/extract_icon.py --image`, which also writes
@3x sizes now.

## Findings worth remembering

- **Frame rate**: every pixel of the output costs (menus, HUD, effects), not only the 3D scene. The screen's full
  size (2868x1320) took an iPhone 17 Pro Max from 60-75 fps to 30-45; the 720p output fixed it.
- **Crashes at race start** were the experimental 120 Hz simulation (`NFSMW_SIM_RATE=120`); it is removed and
  forced off. The game's logic only holds at 60 updates a second.
- **Heat**: the frame cap matters most (CPU and GPU work both scale with it). The owner prefers manual control: no
  automatic cap.
- **Logs from the phone**: the app runs inside LiveContainer (`com.kdt.livecontainer.<your Team ID>`). With the old
  bundle id its logs were at `Documents/Data/Application/<uuid>/Documents/Logs`
  (`xcrun devicectl device copy from --domain-type appDataContainer ...`, iPhone UDID <your iPhone's UDID>).
  Under the new id they were not found there (perhaps LiveContainer's app-group storage). Each launch's first lines
  show the build: `[ios] launch options: output 720 rows, ...`.
- **A stale build**: LiveContainer once kept running an older binary after an IPA import; compare the binary's size
  and the launch-options line.

- **Car shadows moving in races** (v2.3): the day cycle moved the sun a little every frame, so the shadows were
  redrawn differently each frame. The time of day now moves in 1200 steps a day (`RECOMP_TOD_STEPS`, `eo_extra.h`).
- **Car flicker in rain above 60 fps** (14 Oct, not fixed): only in rain and only with a cap above 60 (the
  simulation stays at 60 Hz, so some frames fall between two updates). Ruled out on the iPad with the Diagnostics
  switches and settings: the car reflection and shadow maps, car paint (Natural, Xbox 360, Off), wet roads,
  ambient occlusion, motion blur. Left: the game's own rain drawing. A 60 fps cap avoids it (the player's choice).
- **The weather in a log**: `core.nfsmw: Weather: <phase> (cycle x)`; a test of anything rain-related needs the rain
  phase or Always rain.
- **Builds from the Claude session after an Xcode update**: if `xcode-select` points at the Command Line Tools, its
  older linker cannot read the new iOS SDK's `.tbd` files ("unknown architecture"); build with
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.

## Open

- The Controls tab's squeeze with a Backbone connected: controls now below required priority; not yet confirmed.
- Profile with Instruments (from the Xcode project) to find what else heats the phone.
