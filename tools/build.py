#!/usr/bin/env python3
"""Build Need for Speed: Most Wanted with the kit in kit/. Every option is the kit's: see kit/tools/build.py --help."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
if not (KIT / "tools/build.py").is_file():
    sys.exit("The kit folder is missing: clone the whole repository again")


def pad_buttons():
    """The controller button glyphs (tools/padicons), drawn on this Mac into build/generated, which the app
    bundles into the core mod's folder. Remade only when the script changes; skipped off macOS (the menus then
    keep the keyboard prompts)."""
    script = ROOT / "tools/padicons/make_pad_buttons.swift"
    out = ROOT / "build/generated/padicons/GLOBAL/RecompPadButtons.tpk"
    if sys.platform != "darwin" or not script.is_file():
        return
    if out.is_file() and out.stat().st_mtime >= script.stat().st_mtime:
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "make_pad_buttons"
        built = subprocess.call(["xcrun", "swiftc", "-O", "-module-cache-path", str(Path(tmp) / "cache"),
                                 str(script), "-o", str(exe)])
        if built != 0 or subprocess.call([str(exe), str(out)]) != 0:
            print("warning: the controller button glyphs could not be made; menus keep the keyboard prompts",
                  file=sys.stderr)


pad_buttons()
sys.exit(subprocess.call([sys.executable, str(KIT / "tools/build.py"), "--game-dir", str(ROOT)] + sys.argv[1:],
                         cwd=ROOT))
