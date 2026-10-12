#!/usr/bin/env python3
"""Write docs/SETTINGS.md from mods/core/nfsmw/mod.toml, so the settings list can never drift from the code.

    python3 tools/setup_kit/settings_doc.py            # rewrite docs/SETTINGS.md
    python3 tools/setup_kit/settings_doc.py --check    # exit 1 if docs/SETTINGS.md is out of date"""
import re, sys, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOD = ROOT / "mods/core/nfsmw/mod.toml"
OUT = ROOT / "docs/SETTINGS.md"
# The menu's pages and sections, in its order (kit/mods/settings_menu.cpp kSections). Group "hidden" is never listed.
PAGES = [("Graphics", [("graphics", "Picture"), ("graphics.remaster", "Advanced: Remaster features (follow Look)"),
                       ("graphics.effects", "Advanced: Effects"), ("graphics.cycle", "Advanced: Day and weather cycle"),
                       ("graphics.mirror", "Advanced: Mirror"), ("graphics.rain", "Advanced: Rain"),
                       ("graphics.engine", "Advanced: Engine and experiments")]),
         ("Display", [("display", "Screen"), ("display.quality", "Sharpness and upscaling"), ("display.hdr", "HDR")]),
         ("Audio", [("audio", "Audio")]),
         ("Xbox 360", [("xbox360", "Xbox 360 looks"), ("xbox360.particles", "Sparks and smoke"),
                       ("xbox360.pools", "Light pools")]),
         ("PC", [("pc", "Extra Options"), ("pc.keys", "Keys, camera and HUD")]),
         ("Mods", [("visual", "Screen and camera extras"), ("career", "Career and customization"),
                   ("fixes", "Verified fixes and compatibility"), ("diagnostics", "Diagnostics"), ("", "Other")]),
         ("Controls", [("controls", "Controller"), ("feedback", "Controller rumble")])]
APPLY = {"live": "at once", "restart": "after a restart", "renderer": "after Apply", "next_screen": "at the next screen or load",
         "new_career": "for new careers", "game_video": "with the game's video settings"}


def value_text(spec):
    d = spec.get("default")
    if spec.get("type") == "bool":
        return "on" if d else "off"
    if "choices" in spec:
        choices = spec["choices"].split("|")
        lo = spec.get("min", 0)
        i = d - lo
        if 0 <= i < len(choices) and choices[i]:
            return choices[i]
    unit = spec.get("unit", "")
    return f"{unit}{d}" if unit == "$" else f"{d}"


def render():
    settings = tomllib.loads(MOD.read_text())["settings"]
    groups = {}
    for key, spec in settings.items():
        groups.setdefault(spec.get("group", ""), []).append((key, spec))
    out = ["# Settings", "",
           "Every setting of the native app, written from `mods/core/nfsmw/mod.toml` by "
           "`tools/setup_kit/settings_doc.py` (do not edit by hand).", "",
           "Open the settings menu with **F10** (Fn+F10 on a laptop), the controller's touchpad button, or Back+Start. "
           "Each row says when a change takes effect. The same keys can be set in `NativeOptions.ini` in your profile "
           "folder (see the companion guide).", ""]
    hidden = groups.pop("hidden", [])
    for page, sections in PAGES:
        if not any(groups.get(gid) for gid, _ in sections):
            continue
        out += [f"## {page}", ""]
        for gid, title in sections:
            rows = groups.pop(gid, [])
            if not rows:
                continue
            out += [f"### {title}", "", "| Setting | Key | Default | Takes effect | What it does |", "|---|---|---|---|---|"]
            for key, spec in rows:
                label = spec.get("label", key).replace("|", "/")
                help_text = re.sub(r"\s+", " ", spec.get("help", "")).replace("|", "/")
                out.append(f"| {label} | `{key}` | {value_text(spec)} | {APPLY.get(spec.get('apply', ''), spec.get('apply', ''))} | {help_text} |")
            out.append("")
    if hidden:
        out += ["## Not in the menu", "",
                "Kept so that older profiles load cleanly, or fixed by the port; the menu does not list them.", "",
                "| Key | Value | Why |", "|---|---|---|"]
        for key, spec in hidden:
            help_text = re.sub(r"\s+", " ", spec.get("help", "")).replace("|", "/")
            out.append(f"| `{key}` | {value_text(spec)} | {help_text} |")
        out.append("")
    assert not groups, f"settings in unknown groups: {list(groups)}"
    return "\n".join(out)


if __name__ == "__main__":
    text = render()
    if "--check" in sys.argv:
        sys.exit(0 if OUT.exists() and OUT.read_text() == text else 1)
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)}")
