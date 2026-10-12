"""The rebuilt effects the setup assembles from effects/ (tools/setup_kit/setup.py step 5) are byte-identical to the
ones 2.0.0 was tested with: tests/effects.sha256. After an intended change to effects/, update that file in the same
commit. Needs no game files (the light pools, made from the player's speed.exe, are not covered here)."""
import hashlib, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def built(tmp):
    py = sys.executable
    run = lambda *a: subprocess.run([py, *map(str, a)], cwd=ROOT, check=True, capture_output=True)
    run('tools/effects/gen_ssao.py', '--fx', tmp / 'clean-chain.fx')
    run('tools/effects/gen_ssao.py', '--fx', tmp / 'clean-chain-hdr.fx', '--hdr')
    run('tools/effects/fxgen.py', 'effects/post-host.hlsl', 'effects', tmp / 'post-host.fx')
    (tmp / 'clean-chain.txt').write_bytes((ROOT / 'effects/clean-chain.txt').read_bytes())
    return tmp


def test_effects_match_the_tested_build(tmp_path):
    out = built(tmp_path)
    want = dict(reversed(line.split()) for line in (ROOT / 'tests/effects.sha256').read_text().splitlines() if line)
    got = {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in want}
    assert got == want


def test_fx_build_leaves_the_sources_alone(tmp_path):
    before = {p.name: p.read_bytes() for p in (ROOT / 'effects').iterdir()}
    built(tmp_path)
    assert {p.name: p.read_bytes() for p in (ROOT / 'effects').iterdir()} == before
