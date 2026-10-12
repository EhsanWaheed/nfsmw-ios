# Companion guide

How to play the native Mac version, change its settings, and add the optional mods. If you have not built the app
yet, start with [Getting started](GETTING_STARTED.md).

## What's new in 2.0

**The look.** One setting now chooses the whole picture: F10 → Graphics → **Look**. *Definitive* (default) has every
finished improvement; *Xbox 360* is the console look, rebuilt for this port, without the new improvements; *PC* is
the original PC game. Each improvement keeps its own switch under **Advanced** if you want to mix. New in Definitive:
smooth leaves and fences (with 2x or 4x anti-aliasing on), soft particles (smoke and spray fade where they meet the
road), natural wet roads, road sun sheen, natural car reflections, building glass, water, wall depth, ambient
occlusion, sun rays, lamp glow, softer shadows and garage floor shadows. A **golden colour filter**
(Auto / On / Off) matches the Xbox 360's colour treatment; Auto means on for the Xbox 360 and PC looks and off for
Definitive. Denser tyre smoke, and the Stuff Pack's extra smoke layers (no pack needed).

**Time and weather.** A slow day cycle (a full day over 60 minutes of driving; it pauses in menus) and a weather cycle
(clear, overcast, rain and drying roads over 20 minutes, starting at a random point each session). Both are yours to
turn off (*Time of day* → Original (fixed), *Weather* → Original) in every Look.

**HDR.** *HDR (for HDR TVs)* is now a real HDR picture: the game renders in high range and tone-maps to your TV's own
brightness range, with *HDR: how bright highlights get* to taste. With HDR on, 4x anti-aliasing runs as 2x (4x
stuttered at 4K); your saved 4x comes back when HDR is off. HDR stays off by default.

**Anti-aliasing.** *Smooth edges (anti-aliasing)* is now Off / 2x / 4x and starts at Off. If you had 4x in 1.0 it has
been moved to Off once: choose again.

**The settings menu** has seven pages: Graphics, Display, Audio, Xbox 360, PC, Mods and Controls. The Graphics page is
a short Picture list (Look, Detail level, Time of day, Weather) with everything else under Advanced. New: an Xbox 360
page (Xbox 360 textures, sky and start screen, sparks and light trails, smoke, light pools), rumble style (Xbox 360 or
PC), rumble on cobbles and metal, and a crash fade. Settings marked *(in development)* are visible but stay off until
they are finished. Retired: *Blur: use the scene's own depth* (always on), *Mirror: glow on lights*, and grass blades.
Renamed: *Overall look* → *Look*, *Texture pack* → *Xbox 360 textures*. Dolby Digital for 5.1 is now on by default
for new profiles (plain 5.1 PCM lost the menu music on some TV chains); a 1.0 profile keeps its own choice.

**More Extra Options built in:** always rain, all police on the minimap, the heat-10 bounty fix, the prologue fix, the
unused Busted scenes, percent in one-lap races, the HUD while looking back, subtitles, heat keys (F7, Page Up), the
World Map in races, and an optional no catch-up. Controller remapping from 1.0.2 is unchanged.

**Known issues.** The first minutes of driving after a new build may hitch once while each new shader is prepared
(the Setup app will prepare them ahead in a later release). Rain at 4K with HDR and 2x anti-aliasing sits at the
GPU's limit on an M5 Pro. The mouse pointer is offset after you resize the window to another aspect ratio (a fix is
planned). There is no HD texture pack yet: *Remaster HD textures* and *Sunlit wall relief* show the game's own textures until
a later release lets the Setup app make the pack on your Mac.

## Playing

Open **NFS Most Wanted Native**. The game starts as on PC: the EA logo and intro movies, then the title screen.
Press **Return**, **Cross** or **A** to start. Everything in the game's own menus works as it did on PC.

- **Full screen / window:** the app opens full screen on the display it starts on. F10 → Display → *Window*
  switches between full screen, borderless and a window; the window's green button works too.
- **Resolution:** *Auto* (the default) renders at your screen's own size. The game's Video menu lists every common
  size; the F10 menu has `width`/`height` overrides.
- **Quit:** from the game's main menu, or **Cmd+Q**. The game saves as it always did; quitting mid-race loses only
  that race.

## The settings menu (F10)

Press **F10** (**Fn+F10** on a MacBook), the DualSense **touchpad button**, or **Back+Start** on other controllers.
The game pauses its input while the menu is open.

- **Pages:** *Graphics*, *Display*, *Audio*, *Xbox 360*, *PC*, *Mods* and *Controls*. **Tab** or **L1/R1** changes
  page. On Graphics, the short *Picture* list comes first; *Advanced* opens every other graphics setting.
- **Move:** arrow keys, the D-pad or left stick, or point with the mouse. **Left/Right** (or the **< >** arrows)
  change a value; **Return**/**Cross** selects; **Escape**/**Circle** or **F10** closes.
- **When changes apply:** every row says so — at once, at the next screen or load, for new careers, or after a
  restart. *Anti-aliasing* and *Detail level* wait for **Apply** (or **Discard**), because they reset the renderer once.
- Settings are saved automatically, per profile folder.

Every setting, its default and what it does: [Settings](SETTINGS.md).

### Recommended starting points

- **The look:** Graphics → *Look* → **Definitive** (default), **Xbox 360** or **PC**. Under Advanced, *Rebuilt
  effects* turns the glow, auto brightness, motion blur and depth of field on or off together; each has its own
  strength slider.
- **Anti-aliasing:** Display → *Smooth edges (anti-aliasing)* → **2x** or **4x** if your Mac keeps 60 fps with it.
- **HDR TV:** Display → *HDR (for HDR TVs)* → on. The game switches the TV's HDR mode itself and switches it back when
  you quit. *HDR: how bright highlights get* adjusts the punch. With HDR on, 4x anti-aliasing runs as 2x.
- **Day and weather:** Graphics → *Time of day* and *Weather*; the cycle lengths are under Advanced.
- **Frame rate:** the game simulates at 60 Hz and presents at up to 60 fps. Leave *Game speed* at 60.

## Controllers

Connect a controller over USB or Bluetooth before or after starting the game. With **PlayStation-style buttons**
(`ps2_controls`, on by default):

| Action | DualSense / DualShock | Xbox layout |
|---|---|---|
| Accelerate | Cross (or right stick up) | A |
| Brake / reverse | Square (or right stick down) | X |
| Handbrake | Circle | B |
| Nitrous | R1 | RB |
| Speedbreaker | L1 | LB |
| Shift down / up (manual gears) | L2 / R2 | LT / RT |
| Look back | Triangle | Y |
| Change camera | R3 | Right stick click |
| Skip music track | L3 | Left stick click |
| Reset car | Create | View |
| Pause | Options | Menu |
| HUD / map | D-pad | D-pad |

### Change the buttons

Any driving action can go on any button, trigger or stick direction, in the game's own menu:

1. In the game, open **Options → Controls**. It opens on the **Keyboard** page: press **R2** (or **Tab**) to switch
   to the **Controller** page.
2. Pick an action (Accelerate, Brake / Reverse, Handbrake, N2O, …) in the **Primary** or **Secondary** column and
   press **Cross** (or **Return**).
3. Press the button, trigger or stick direction you want for it. The screen shows its name, for example *R2 / RT*
   or *Left Stick Up*. If another action had that button, it moves to this one.
4. Press **Triangle** (or **Esc**) when you are done. The layout is saved with your profile.

On that page **Square** clears a slot and **Circle** puts this port's layout back (on the keyboard: **2** and **1**).
A trigger works as a full analog pedal whatever you give it, so *Accelerate* on R2 and *Brake* on L2 drive like a
modern racing game. For exactly that, the quickest way is the switch below.

- **Triggers as pedals** (`trigger_pedals`, F10 → Controls): R2 accelerates and L2 brakes, both analog, at
  once. Shifting then moves off the triggers; give *Shift Up*/*Shift Down* other buttons on the Controls screen, or
  with *Shift up/down button* in F10 (numbers: 0 Square/X, 1 Cross/A, 2 Circle/B, 3 Triangle/Y, 4 L1/LB, 5 R1/RB,
  6 L2/LT, 7 R2/RT, 8 Create/View, 9 Options/Menu, 10 L3, 11 R3).
- **Button icons:** the menus show PlayStation symbols (`ps_icons`) when your game folder has XtendedInput's button
  textures, `GLOBAL/XtendedInputButtons.tpk` (see [Optional mods](#optional-mods)). Without them the menus keep the
  game's own prompts.
- **Rumble:** from the game's own force-feedback (road surfaces, wheelspin, impacts). F10 → Controls → *Rumble:
  crashes %* and *Rumble: driving %* set the strength (0 turns each off); *Rumble style* picks the Xbox 360 or the PC
  feel, and *Rumble: cobbles and metal* adds the road surface.
- Turn **PlayStation-style buttons** off to start from the PC version's own controller layout instead: **Circle**
  (Defaults) on the Controls screen then gives that layout.

Keyboard and mouse work exactly as on PC. **T** skips the music track, in the menus and (with *Skip music track
while driving*, on by default) while driving.

## Audio

The game always mixes its original discrete 5.1 sound. In the game's **Options → Audio → Audio Mode** choose:

- **Stereo** (the default for new profiles): headphones, Mac speakers, most TVs.
- **Mono**.
- **5.1 PCM**: a receiver or soundbar that takes 6-channel PCM over HDMI.

Each choice is heard immediately while you cycle; **Accept** keeps it. Movies follow the same mode.

If 5.1 voices come out of the wrong speakers or sound muffled (some TV → soundbar HDMI chains scramble multichannel
PCM), turn on **Dolby Digital (for TVs and receivers)** in F10 → Audio (on by default for profiles made since 2.0;
turn it off for plain 5.1 PCM). The game then sends its 5.1 mix as a
Dolby Digital bitstream, which carries its own speaker layout, like a console. While the game runs it has exclusive
use of that audio output (other apps' sound moves elsewhere until you quit), and it restores the output when it quits.

## Screenshots and cinematic shots

Turn these on in F10 → PC → *Keys, camera and HUD*, and *Save and load positions* in F10 → Mods (all off by default;
restart after turning one on). They work while driving:

- **Pause key freezes the camera** — **Pause/Break** stops the camera where it is while the car drives on; press
  again to follow the car.
- **Free camera (Backspace)** — the game's own free debug camera, and back.
- **Light keys (H and O)** — headlights and, on a police car, the light bar.
- **Keep the HUD when looking back**.
- **Save and load positions** — **Left Shift + 1–5** saves where you are, **Left Ctrl + 1–5** jumps back, to repeat a
  shot.

The game's own camera button (R3 on a controller) cycles the bumper, hood and chase cameras.

## Saves and settings

Your profile folder is `~/Library/Application Support/NFS Most Wanted Native/`. It holds:

- your **saves** (the game's own profile files, exactly as on PC);
- `mod-settings.json` — the F10 settings;
- `NativeOptions.ini` — optional overrides (below);
- `Logs/` — the latest run's log, useful for bug reports.

Back up this folder to keep your career. PC save files can be copied in, and back out to a PC.

### NativeOptions.ini

A text file for setting options before launch, including the original PC Extra Options names (`StartingCash`,
`ForceBlackEdition`, `SelectableMarkerCount` …). Uncomment a line to force that value at every launch; comment it
again to let F10 control it. Quit the game before editing.

## Extra Options

The useful parts of the PC *Extra Options* mod are built in natively (no ASI loader). F10 → Mods → *Career and
customization* (2.0 adds the fixes, minimap, heat-key, World Map and no catch-up rows there, and *Always rain* and
*Subtitles* on the PC page):

| Option | Default |
|---|---|
| Black Edition cars, races and content | on |
| Race beaten Blacklist rivals again | on |
| Special vinyls category | on |
| Max performance button in the shop | on |
| Rival reward markers to pick | 6 of 6 |
| Longer profile names (15 characters) | on |
| New career: $10,000 bonus (as with an Underground 2 save) | on |
| New career: extra starting cash | $0 |
| Choose police heat levels (1–10; heat 6–10 send up to 8 cars) | off |
| Helicopter takedown bounty (100,000 and an announcement) | on |
| Skip music track while driving (T, or L3 on a controller) | on |
| Unlock everything (all cars, parts, events, areas and hidden tracks) | off |
| Remove the glowing barriers around locked areas | off |
| Open the Old Bridge | off |

Career options apply to new careers; the others after a restart.

## Optional mods

The app never runs Windows `.asi` or `.dll` mods. Three popular PC mods' **files** work through native loaders
instead. All are optional and use **your own** downloads; you don't need a PC. On your Mac:

1. Download the mod in your browser and double-click the download in Finder to unzip it. (For a `.rar` or `.7z`
   file, use a free unarchiver such as *The Unarchiver* from the App Store.)
2. Copy **only the file or folder named below** into your game folder (the folder you gave the setup, with
   `speed.exe` in it). Leave the mod's other files out: its `.asi` and `.dll` files are Windows code, and its
   replacement game files are untested with this port.
3. Open the **Setup app** again, choose the same game folder and click **Build** (or run `./setup.sh` again). It finds
   the new files by itself; the build is quicker than the first one.

| Mod | What to copy from the download | Where it goes in your game folder |
|---|---|---|
| **Xbox 360 Stuff Pack 4.2** (its textures) | the `TexWizardX360` folder (in the *Easy Installation* download: `Files/TRACKS/TexWizardX360`) | `TRACKS` → so you have `TRACKS/TexWizardX360` |
| **XenonEffects** (sparks and light trails) | `XenonEffects.tpk` (in the XenonEffects mod's `scripts` folder; the Stuff Pack has it too, in `Files/scripts`) | `scripts` (make the folder if it isn't there) |
| **PlayStation button icons** (Original Button Pack or NFS XtendedInput) | `XtendedInputButtons.tpk` from the pack's **NFSMW** folder (`NFSMW/GLOBAL`), not the NFSC or NFSPS ones | `GLOBAL` |

Tip: if you can't find a file, open the unzipped download in Finder and type its name in the search field
(choose *This folder*, not *This Mac*).

After the build:

- **Xbox 360 textures:** F10 → Xbox 360 → **Xbox 360 textures** is on by default. The *PC* look launches without
  them.
- **Sparks and light trails:** F10 → Xbox 360 → **Sparks and light trails** (on by default).
- **Button icons:** F10 → Controls → **PlayStation button icons** (on by default).

If you choose the file in the Setup app instead (*XenonEffects… Choose*), or pass
`--xenon-effects /path/to/XenonEffects.tpk` to `./setup.sh`, it doesn't need to be in the game folder.

If your game folder came from a PC that already had the Stuff Pack installed, its textures are used as above; the
pack's other changed game files were made for its Windows code and are untested with this port.

### What else works

- **Converted widescreen movies** that replace files in the game's `MOVIES` folder play, fitted to the screen
  (`movie_fit`).
- The **widescreen fix**, **HD Reflections** mirror improvements, **XtendedInput**'s controller support and the
  **Extra Options** listed above are already built in — don't install them (only XtendedInput's button-icon file is
  used, as above).
- Anything else that is an `.asi`/`.dll` will not load. Mods that only replace game data files (cars, vinyls,
  textures in `.BIN`/`.BUN` files) may work if the PC game accepts them; they are untested.

## Reporting a problem

Note what you did and when, then attach `Logs/` from your profile folder. F10 → Mods → Diagnostics has *Log my
position* (to point at a spot on the map) and *Log scenery pop-in*. See [Troubleshooting](TROUBLESHOOTING.md) first.
