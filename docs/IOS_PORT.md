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

`build/ios/SpeedRecomp-NGF.ipa` (about 60 MB, "No Game Files"): the same app without `game/`. Made from an APFS
clone of the app (`cp -cR`, then remove `game/`, `zip -9`). On first launch the launcher asks the tester for their
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

A UIKit panel above the game, opened with L3+R3, Cmd+S, or a three-finger tap or hold (on iOS a three-finger tap
no longer sends F10: `kit/host/input_touch.cpp`). Settings persist in NSUserDefaults (`ios.*` keys). Tabs:

- **Home** (iOS-only settings):
  - *Output resolution* (720p default, 900p, 1080p, the screen's own; next launch): the game's own back buffer,
    shaped to the screen. The host publishes the screen size scaled to this height (`RECOMP_IOS_OUTPUT_ROWS`, read
    in `kit/host/sdl/main.cpp` `note_screen_size`) and the core mod's Auto resolution uses it.
  - *3D resolution* (540p, 600p, 720p ... Native; instant): the MetalFX scene height (`host_upscale_set_rows`,
    `kit/host/gpu/metal/d3d9_metal.mm`). Native = the output size, no MetalFX. First-run default: the largest step
    at most 85% of the output (600p at 720p).
  - *Frame cap* 30/60/90/120 with a ProMotion rate hint.
  - *Dynamic resolution*: one 3D step down at thermal fair, two at serious, 540p at critical; back up as it cools.
  - *Stretch to fill* (`host_compositor_set_stretch`, `kit/host/compositor.cpp`).
  - *Performance*: switches for the Definitive look's heaviest extras, which the Detail presets do not touch
    (`quality_presets.h` sets only the game's own video values): ambient occlusion, motion blur, background blur,
    mirror scenery (live), softer shadows and the HD mirror (next launch).
  - *Effects at internal resolution* (on by default; next launch): glow / DoF / blur before the upscale.
- **Graphics / Mods / Controls**: the F10 menu's own pages drawn natively. `mods_menu_open_headless` /
  `mods_menu_close_headless` (`kit/mods/settings_menu.cpp`) open the menu as a model without drawing the page or
  taking the keyboard; the panel reads `mods_menu_snapshot` and acts with `mods_menu_pointer_line`. Apply/Discard
  sit in the footer. Controls adds the connected controller and "Assign" for the shift buttons (press a button;
  kit order square, cross, circle, triangle, l1, r1, l2, r2, select, start, l3, r3, ps).
- First run on a fresh profile: Detail set to Low once (`seedFirstRun`).

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

## Open

- The Controls tab's squeeze with a Backbone connected: controls now below required priority; not yet confirmed.
- Profile with Instruments (from the Xcode project) to find what else heats the phone.
