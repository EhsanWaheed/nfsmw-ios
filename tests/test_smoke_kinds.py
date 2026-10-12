"""Run 14 lit smoke: the emitter -> kind table (mods/core/nfsmw/smoke_kinds.h). Every key must be Attrib::StringHash32
(Jenkins lookup2, init 0xABCDEF00) of the name on its line, the kind must follow the name (design
review/run14/smoke-realism-design.md 3 and 7), the tyre keys must include smoke.h's denser-smoke table, and the kind
numbers must match the kit's host/gpu/metal/smoke_light.h."""
from pathlib import Path
import re, struct

ROOT = Path(__file__).resolve().parents[1]


def hash32(s, init=0xABCDEF00):
    k = s.encode()
    M = 0xffffffff
    a = b = 0x9E3779B9
    c = init
    n, i = len(k), 0

    def mix(a, b, c):
        a = (a - b - c) & M; a ^= c >> 13
        b = (b - c - a) & M; b ^= (a << 8) & M
        c = (c - a - b) & M; c ^= b >> 13
        a = (a - b - c) & M; a ^= c >> 12
        b = (b - c - a) & M; b ^= (a << 16) & M
        c = (c - a - b) & M; c ^= b >> 5
        a = (a - b - c) & M; a ^= c >> 3
        b = (b - c - a) & M; b ^= (a << 10) & M
        c = (c - a - b) & M; c ^= b >> 15
        return a, b, c
    while n - i >= 12:
        a = (a + struct.unpack_from('<I', k, i)[0]) & M
        b = (b + struct.unpack_from('<I', k, i + 4)[0]) & M
        c = (c + struct.unpack_from('<I', k, i + 8)[0]) & M
        a, b, c = mix(a, b, c)
        i += 12
    c = (c + n) & M
    rest = k[i:] + b'\0' * 11
    a = (a + struct.unpack_from('<I', rest, 0)[0]) & M
    b = (b + struct.unpack_from('<I', rest, 4)[0]) & M
    c = (c + (struct.unpack_from('<I', rest, 8)[0] << 8)) & M
    return mix(a, b, c)[2]


def table():
    src = (ROOT / 'mods/core/nfsmw/smoke_kinds.h').read_text()
    rows = re.findall(r'\{0x([0-9a-f]{8})u, (SMK_\w+)\},\s*/\* (\w+) \*/', src)
    kinds = dict((m[0], int(m[1])) for m in re.findall(r'(SMK_\w+) = (\d+)', src))
    return rows, kinds


def expected_kind(name):
    if name.startswith(('emtd_', 'emcar_tireblow')):
        return 'SMK_TYRE'
    if name == 'fxgame_flare_smoke':
        return 'SMK_FLARE_RED'
    if name == 'fxgame_flare_smoke_grn':
        return 'SMK_FLARE_GREEN'
    if name in ('emsmk_env_black', 'emsmk_env_brown'):
        return 'SMK_STACK_SOOT'
    if name.startswith('emsmk_env'):
        return 'SMK_STACK_STEAM'
    if name.startswith('emenv_fog'):
        return 'SMK_FOG'
    if 'steam' in name:
        return 'SMK_STEAM'
    raise AssertionError(name)


def test_hash_matches_known_key():
    assert hash32('emtd_sk_asphaltfastsmk1') == 0xe00557c4   # smoke.h, verified in game by the census


def test_keys_are_hashes_of_their_names_and_kinds_follow_names():
    rows, kinds = table()
    assert len(rows) >= 29
    keys = [int(k, 16) for k, _, _ in rows]
    assert len(set(keys)) == len(keys)
    for key, kind, name in rows:
        assert hash32(name) == int(key, 16), name
        assert kind == expected_kind(name), name
        assert 1 <= kinds[kind] <= kinds['SMK_KIND_MAX']


def test_tyre_keys_cover_denser_smoke_table():
    rows, _ = table()
    tyre = {int(k, 16) for k, kind, _ in rows if kind == 'SMK_TYRE'}
    src = (ROOT / 'mods/core/nfsmw/smoke.h').read_text()
    table_src = src[src.index('k_smoke_keys[]'):src.index('};', src.index('k_smoke_keys[]'))]
    for k in re.findall(r'0x([0-9a-f]{8})u', table_src):
        assert int(k, 16) in tyre, k


def test_kind_numbers_match_the_kit():
    _, kinds = table()
    kit = (ROOT / 'kit/host/gpu/metal/smoke_light.h').read_text()
    pairs = dict((m[0], int(m[1])) for m in re.findall(r'KIND_(\w+) = (\d+)', kit))
    for name in ('TYRE', 'FLARE_RED', 'STACK_STEAM', 'STACK_SOOT', 'FOG', 'STEAM', 'FLARE_GREEN'):
        assert pairs[name] == kinds['SMK_' + name], name
    assert pairs['MAX'] == kinds['SMK_KIND_MAX']
    mod = (ROOT / 'mods/core/nfsmw/smoke_light.h').read_text()
    for state, value in (('KIND', 248), ('SUN', 245), ('POS', 242)):
        assert re.search(r'#define SML_RS_%s +%du' % (state, value), mod), state
        assert re.search(r'RS_%s = %d,' % (state, value), kit), state
