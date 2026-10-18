# Need for Speed: Most Wanted (2005) — Native for Apple Silicon Macs

A native macOS build of the 2005 PC game, made by **static recompilation**: the game's own x86 code is translated to
C ahead of time and compiled for Apple Silicon, with a Metal renderer, native audio and native controller support.
There is no emulator, no Wine and no CrossOver.

> **You need your own copy of the game.** This repository contains **no game code, executables, artwork, sound,
> movies, tracks or texture packs**. The setup kit takes the files from **your** installation and builds the app on
> **your** Mac. Nothing it builds is meant to be shared.

This project is not affiliated with or endorsed by Electronic Arts. *Need for Speed* and *Most Wanted* are
trademarks of Electronic Arts Inc.

## iPhone and iPad (this fork)

This fork of [elforeign/nfs-most-wanted-mac](https://github.com/elforeign/nfs-most-wanted-mac) adds an iOS port.
The test build (`NFSMWIOS-NGF.ipa`, from [Releases](https://github.com/EhsanWaheed/nfsmw-ios/releases)) contains
**no game files**. You add your own copy of the game the first time you open it.

### What you need

**Your own PC copy of the game, version 1.3** (the *Black Edition*, or the original game updated with the official
1.3 patch). Other versions will not work: the app checks your `speed.exe` and turns away anything else.

From your game's install folder (usually `C:\Program Files\EA GAMES\Need for Speed Most Wanted`), copy:

| Copy this | |
|---|---|
| `speed.exe` | the game itself (must be version 1.3) |
| `CARS` `FRONTEND` `GLOBAL` `LANGUAGES` `MOVIES` `NIS` `SOUND` `TRACKS` | the game's data: all of these folders |
| `CREDITS` `MEMCARD` `SUBTITLES` | small extras: copy them too |

That's about 2.8 GB. You don't need anything else (`.dll` files, the uninstaller, `Support`, saves).

**Easiest:** copy your whole game folder and name the copy **`game`**. Zip it, move the ZIP to your iPhone or iPad
(AirDrop, iCloud Drive or a cable), unzip it in the Files app, then place the **`game`** folder in the app's
Documents folder (where that is: see Install, step 3). The app ignores anything inside it doesn't use.

### Install

1. Install `NFSMWIOS-NGF.ipa` with [LiveContainer](https://github.com/LiveContainer/LiveContainer) or SideStore
   (iOS 17 or later), and open it once so its Documents folder exists.
2. Put the files above in a folder named exactly **`game`**, so you have `game/speed.exe`, `game/CARS`,
   `game/GLOBAL` and so on.
3. Copy that **`game`** folder into the app's **Documents** folder (Files app, or Finder with the device plugged in):
   - **SideStore** (installed as a normal app): Files → On My iPhone → **Need for Speed: Most Wanted** → put
     `game` there.
   - **LiveContainer**: Files → On My iPhone → **LiveContainer** → `Data` → `Application` → the app's data folder
     (its name is shown in LiveContainer's settings for the app) → **`Documents`** → put `game` there.
4. Open the app. It finds `Documents/game` and starts the game.

Prefer not to copy files by hand? Open the app without a `game` folder and it offers to import one: choose your
game folder or a ZIP of it, wherever it is (Files, iCloud Drive), and it copies it into `Documents/game` for you.

### Settings

Tap three fingers on the screen (or press L3+R3 on a controller, or Cmd+S or F10 on a keyboard) to open the
settings. For
a cool, smooth iPhone: **Output 720p**, **3D 600p**, **Frame cap 60**. Details and how it was built:
**[docs/IOS_PORT.md](docs/IOS_PORT.md)**.

**v3:** Graphics → **Upscaler** can switch from MetalFX Spatial to **Temporal (experimental)**: the 3D scene is built
up over several frames from each object's own motion, for smoother edges and finer detail (restart to apply). Also
new: the game pauses when the settings open in a race, locking the phone no longer freezes it (sound comes back
too), an **80 fps** cap that holds (ProMotion's 120/80/60/40/30 steps), 40 fps in menus and 30 while paused to keep
the phone cool, and the Stuff Pack smoke crash at the race countdown is fixed.

**v4:** the game now draws through **Metal 4** with **MetalFX 4** (Diagnostics → Renderer keeps **Metal
(compatibility)**, the v3 renderer, for older devices or if anything looks wrong). It is about as light as v3 on the
render thread and lighter on the GPU, and much lighter over a long session: lamp-lit roads and walls are drawn once
instead of twice, the auto-brightness measurement takes half the passes, and the game's vertex buffers are no
longer copied whole on every update. Temporal is now **Temporal (enhanced)** (Spatial stays the default) and keeps
its history through menus and cut-scenes.

**v4.2:** **Temporal** is now the default upscaler (**Spatial (compatibility)** stays in Graphics), sharper: the
game's textures get a mip bias for the upscale, and smoke, sparks and other blended effects mark themselves so they
no longer smear. The lighting flicker with ambient occlusion on under Temporal (main menu and races) is fixed. A new
Liquid Glass launcher with the Black Edition cover art, controller navigation and button hints that match the
connected pad (PlayStation cross/circle on a DualSense or Backbone One PlayStation Edition, A/B on others); Quit
now closes the app. Every front-end screen (main menu, garage, shops, car lots) runs at 30 fps so the phone cools
down between races; the open world and races are not capped. The game is shown on screen directly again (no extra
compositing pass), and the app reports its real version (4.2).

## Play it on your Mac

1. Have your own installed copy of the PC game (Black Edition, version 1.3) on your Mac.
2. Download the **Setup app** (`NFS-Most-Wanted-Native-Setup` disk image) from the
   [latest release](https://github.com/elforeign/nfs-most-wanted-mac/releases/latest), open it, and choose your game folder.
3. Click **Build**. About 10 minutes later on a recent Mac (longer on older ones) the game is in your Applications folder.

The Setup app holds no game files: it builds the game on your Mac from your copy. Step by step, including the one-time
"Open Anyway" for an app that is not notarized by Apple: **[Getting started](docs/GETTING_STARTED.md)**. You can
also build in Terminal with `./setup.sh`.

## What you get

- Native Apple Silicon app (tested at 4K 60 fps on an M5 Pro MacBook Pro with HDR off; with HDR and 2x anti-aliasing
  heavy rain is near the limit), any resolution and aspect ratio, true widescreen (field of view, HUD and minimap
  moved out to the edges, widescreen movies fitted correctly).
- **One Look setting:** **Definitive** (default), **Xbox 360** or **PC**. Definitive is every finished improvement:
  rebuilt effects written for this port (glow, gentle auto brightness for tunnels, motion blur at speed, movie-style
  depth of field), natural wet roads, road sun sheen, car and glass reflections, water, wall depth,
  ambient occlusion, sun rays, lamp glow, softer shadows, soft particles, smooth leaves and fences, and a golden colour
  filter to taste. *Xbox 360* is the console look rebuilt for this port; *PC* shows the game exactly as it shipped.
  Every improvement also has its own switch.
- **Day and weather cycles** (both optional): a slow day, and clear, overcast, rain and drying roads.
- **Light pools** cast by the PC game's own street lamps and tunnel lights onto the road and walls.
- Rear-view mirror: sharp HD picture, full scenery, car shadows, headlights and police lights, knocked-over props.
- **Real HDR** for HDR TVs, and **anti-aliasing Off / 2x / 4x**.
- **Audio:** the game's own 5.1 mix as Mono, Stereo or 5.1 PCM, switchable live, plus an optional **Dolby Digital
  5.1** bitstream for TVs and soundbars that misplace multichannel PCM.
- **Controllers:** DualSense tested over USB and Bluetooth (other controllers macOS supports should work), PlayStation-style layout and button icons, any
  button remappable in the game's own Controls menu, analog triggers as pedals, rumble from the game's own
  force-feedback in an Xbox 360 or PC style.
- **Extra Options, built in:** Black Edition content, replay beaten Blacklist rivals, special vinyls, max-performance
  shop button, 6 rival reward markers, longer profile names, starting cash, police heat levels 1–10, helicopter
  takedown bounty, skip the music track while driving (T or L3), the bounty, prologue and Busted-scene fixes, the
  World Map in races, subtitles, always rain, and optional unlock-everything, barrier removal and no catch-up.
- Optional community packs you already own work too: the Xbox 360 Stuff Pack's **textures** and the
  **XenonEffects** sparks and light trails. See the [companion guide](docs/COMPANION_GUIDE.md).

What's new in 2.0 and the known issues: [companion guide](docs/COMPANION_GUIDE.md#whats-new-in-20).

## Get started

1. Read **[Getting started](docs/GETTING_STARTED.md)**: what you need, then the Setup app (or one Terminal command).
2. Then the **[companion guide](docs/COMPANION_GUIDE.md)**: playing, the F10 settings menu, controllers, audio,
   and adding the optional mods.
3. Problems: **[Troubleshooting](docs/TROUBLESHOOTING.md)**. All settings: **[Settings](docs/SETTINGS.md)**.

## How it works

The translator, runtime, Direct3D 9 → Metal renderer, audio and input hosts are
[recomp-kit](https://github.com/veritr1x/recomp-kit) by veritr1x, included in `kit/` with this port's changes; this game's configuration started as
veritr1x's [nfsmw-recomp](https://github.com/veritr1x/nfsmw-recomp). This repository adds the fixes, features and
setup kit that make it a complete, playable Mac game: `game.toml` (the game's identity, addresses and translation
fixes), `native/` (native replacements for a few game functions), `mods/core/nfsmw/` (the core mod: widescreen,
settings, effects, controls, audio, Extra Options), `effects/` (the rebuilt effects' shader programs, assembled at
setup by `tools/effects/`) and `tools/setup_kit/` (the setup). The Xbox 360 look is our own rebuild: the console
version was studied as a reference only, and none of its code, shaders or files are included.

You provide your own game folder, including its `speed.exe`. The setup checks that the exe is one the port can use
before it builds anything.

## Credits

This port is built on **recomp-kit** and **nfsmw-recomp** by **veritr1x**.

Community mods this port learned from or works with, and the people their readmes credit:

- **Extra Options** — ExOpts Team
- **Need for Speed Most Wanted (2005) – Xbox 360 Stuff Pack 4.2** (its textures) — "by a bunch of people", including
  osdever, SpeedyHeart, MaxHwoy, Kevin4e, Aero_ and Xanvier
- **NFS HD Reflections** — Aero_; based on Extra Options by the ExOpts Team; thanks to osdever
- **NFS XtendedInput** — xan1242 (Lovro Pleše) and Berkay Yiğit; button icons by Aero_ (AeroWidescreen); injector by
  LINK/2012
- **NFSMW XenonEffects** — xan1242 (Lovro Pleše)
- **NFS Most Wanted Widescreen Fix** — ThirteenAG
- **TexWizard** — R-033; thanks to nlgxzef

No code from these mods is included; their behaviour is reimplemented natively. Licence notices are in [NOTICE](NOTICE).

## Licence

MIT licence (see [LICENSE](LICENSE)).
