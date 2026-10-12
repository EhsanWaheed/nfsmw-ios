#!/usr/bin/env python3
"""Writes the SSAO v1 programs of the clean chain: horizon-based
ambient occlusion (public HBAO technique, reduced: depth-derived normals, 4 directions x 3 steps, no trig, no flow
control) at half resolution, a depth-aware 5-tap blur and the apply pass. Our own code. Outputs (effects/):
  clean-chain.PS_Ao.asm       s0 = depth (R32F device depth, frame size, point) -> AoA (frame/2)
  clean-chain.PS_AoBlurH.asm  s0 = AoA (point) -> AoB;   clean-chain.PS_AoBlurV.asm  s0 = AoB (point) -> AoA
  clean-chain.PS_AoApply.asm  s0 = AoA (linear), s1 = frame (point), s2 = depth (point) -> frame (rgb)
Registers (host: clean_ao.h; manifest consts in clean-chain.txt):
  c1 DstTexelSize, c2 SrcTexelSize0 (runner)
  c50 AoProj   (tan half-FOV x, tan half-FOV y, near*far, far)
  c51 AoDepth  (far - near, sky device-depth cut, 1/far, 0)
  c52 AoParams (radius m, bias, strength, max darkening)
  c53 AoFade   (fade start m, fade end m, max screen radius in frame heights, luma guard lo)
  c54 AoBlur   (blur depth tolerance, luma guard hi, TEST view 0/1 AO grey/2 normals, TEST normals out of PS_Ao)
AoA/AoB: r = AO, g/b = view z / fade end in 16 bits (g = floor(e*255)/255, b = frc(e*255)) for the blur's depth weight.
usage: gen_ssao.py                 writes the four .asm files (asserts: <= 512 slots, <= 32 temps)
       gen_ssao.py --fx OUT.fx [--hdr]  builds clean-chain.fx (--hdr: clean-chain-hdr.fx, the HDR form) from effects/
                                   with fxgen.py, keeping the scoped constant groups below out of the constant tables
                                   of programs that do not use them (existing passes byte-identical); writes no .asm
       gen_ssao.py --check         numpy model of the generated programs on synthetic depth"""
import re, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / 'effects'
AO_REGS = range(50, 55)
# Host constants kept out of the constant table of every program that does not name one of the group's registers
# (existing programs byte-identical): Ao* c50-c54, GradeParams c86 (G2, grade_lut.py), MirrorParams c87 (mirror),
# GradeC0-C3 c92-c95 (G2 vt route).
SCOPED_GROUPS = (AO_REGS, (86,), (87,), (92, 93, 94, 95))   # + GradeC0-C3 (G2 vt route)
XYZW = 'xyzw'


class Prog:
    def __init__(self, samplers):
        self.L = ['ps_3_0']
        self.defs = {}
        self.body = []
        self.samplers = samplers

    def define(self, reg, vals):
        self.defs[reg] = vals

    def _src(self, s, mask, pad=None):
        neg = s.startswith('-')
        s = s[1:] if neg else s
        reg, _, comps = s.partition('.')
        if not comps:
            sw = 'xyzw'
        elif len(comps) == 4:
            sw = comps
        elif len(comps) == 1:
            sw = comps * 4
        elif pad:   # dp3 / texld: positional, padded
            sw = (comps + comps[-1] * 4)[:4]
        else:
            assert len(comps) == len(mask), (s, mask)
            sw = ''.join(comps[mask.index(c)] if c in mask else comps[0] for c in XYZW)
        return ('-' if neg else '') + reg + '.' + sw

    def op(self, name, dst, *srcs):
        reg, _, mask = dst.partition('.')
        mask = mask or 'xyzw'
        base = name.replace('_sat', '')
        pad = base in ('dp3', 'dp4')
        self.body.append(f'{name} {reg}.{mask}, ' + ', '.join(self._src(s, mask, pad) for s in srcs))

    def tex(self, dst, uv, s):
        self.body.append(f'texld {dst}.xyzw, {self._src(uv, "xyzw", True)}, s{s}.xyzw')

    def text(self):
        L = list(self.L)
        for reg, v in sorted(self.defs.items(), key=lambda kv: int(kv[0][1:])):
            L.append(f'def {reg} = ' + ', '.join(f'{x:.8e}' for x in v))
        L.append('dcl_texcoord0 v0')
        L += [f'dcl_2d s{i}' for i in range(self.samplers)]
        return '\n'.join(L + self.body) + '\n'

    def check(self, name):
        slots = len(self.body)                       # every op used here is one slot (no nrm / sincos / macros)
        temps = {int(m) for m in re.findall(r'\br(\d+)\.', '\n'.join(self.body))}
        assert slots <= 512, (name, slots)
        assert max(temps) < 32, (name, max(temps))
        texs = sum(1 for l in self.body if l.startswith('texld'))
        return f'{name}: {slots} slots ({texs} texld), {len(temps)} temps (max r{max(temps)})'


def depth_to_z(p, dst, d):
    """dst = view z from device depth d: n*f / (f - d (f - n))"""
    p.op('mad', dst, '-' + d, 'c51.x', 'c50.w')
    p.op('rcp', dst, dst)
    p.op('mul', dst, 'c50.z', dst)


def view_pos(p, dst, uv, t):
    """dst.xyz = view-space position at uv (x right, y down, z forward); t = scratch temp (texld target)"""
    p.tex(t, uv + '.xy', 0)
    depth_to_z(p, t + '.y', t + '.x')
    p.op('mad', dst + '.xy', uv + '.xy', 'c100.z', 'c100.w')     # 2uv - 1
    p.op('mul', dst + '.xy', dst + '.xy', 'c50.xy')              # ray at z = 1
    p.op('mul', dst + '.xy', dst + '.xy', t + '.y')
    p.op('mov', dst + '.z', t + '.y')


def gen_ao():
    p = Prog(1)
    p.define('c100', (1.0, 0.0, 2.0, -1.0))
    p.define('c101', (0.5, 0.25, 1e-24, 1e-4))
    p.define('c102', (1.0, 0.0, 0.92387953, 0.38268343))         # cos/sin 0, 22.5 deg
    p.define('c103', (0.70710678, 0.70710678, 0.38268343, 0.92387953))   # 45, 67.5 deg
    p.define('c104', (1.0 / 3.0, 255.0, 1.0 / 255.0, 2.0 / 3.0))
    # centre texel of the full-resolution depth under this half-resolution pixel
    p.op('mad', 'r0.xy', 'c2.xy', '-c101.x', 'v0.xy')
    p.tex('r1', 'r0.xy', 0)                                       # r1.x = device depth d
    depth_to_z(p, 'r1.z', 'r1.x')                                 # r1.z = zc
    p.op('mad', 'r2.xy', 'r0.xy', 'c100.z', 'c100.w')
    p.op('mul', 'r2.xy', 'r2.xy', 'c50.xy')
    p.op('mul', 'r2.xy', 'r2.xy', 'r1.z')
    p.op('mov', 'r2.z', 'r1.z')                                   # r2.xyz = P
    # normal from 4 neighbours (+-1 full-res texel); per axis the neighbour with the smaller |dz|
    p.op('add', 'r3.x', 'r0.x', 'c2.x'); p.op('mov', 'r3.y', 'r0.y'); view_pos(p, 'r4', 'r3', 'r16')   # right
    p.op('add', 'r3.x', 'r0.x', '-c2.x'); view_pos(p, 'r5', 'r3', 'r16')                                 # left
    p.op('mov', 'r3.x', 'r0.x'); p.op('add', 'r3.y', 'r0.y', 'c2.y'); view_pos(p, 'r6', 'r3', 'r16')    # down
    p.op('add', 'r3.y', 'r0.y', '-c2.y'); view_pos(p, 'r7', 'r3', 'r16')                                 # up
    for a, b in (('r4', 'r5'), ('r6', 'r7')):
        p.op('add', a + '.xyz', a + '.xyz', '-r2.xyz')            # forward difference
        p.op('add', b + '.xyz', 'r2.xyz', '-' + b + '.xyz')       # backward difference
        p.op('abs', 'r8.x', a + '.z')
        p.op('abs', 'r8.y', b + '.z')
        p.op('add', 'r8.x', 'r8.x', '-r8.y')
        p.op('cmp', a + '.xyz', 'r8.x', b + '.xyz', a + '.xyz')
    p.op('mul', 'r8.xyz', 'r4.yzx', 'r6.zxy')                     # n = dx x dy
    p.op('mad', 'r8.xyz', '-r4.zxy', 'r6.yzx', 'r8.xyz')
    p.op('dp3', 'r9.x', 'r8.xyz', 'r8.xyz')
    p.op('max', 'r9.x', 'r9.x', 'c101.z')
    p.op('rsq', 'r9.x', 'r9.x')
    p.op('mul', 'r8.xyz', 'r8.xyz', 'r9.x')
    p.op('dp3', 'r9.x', 'r8.xyz', 'r2.xyz')                       # facing the camera: n . P < 0
    p.op('cmp', 'r8.xyz', 'r9.x', '-r8.xyz', 'r8.xyz')
    # screen radius (frame heights) = R / (2 z tan_y), clamped to [1 full-res px, AoFade.z]; x scaled by 1/aspect
    p.op('mul', 'r9.y', 'r1.z', 'c50.y')
    p.op('add', 'r9.y', 'r9.y', 'r9.y')
    p.op('rcp', 'r9.y', 'r9.y')
    p.op('mul', 'r9.y', 'r9.y', 'c52.x')
    p.op('max', 'r9.y', 'r9.y', 'c2.y')
    p.op('min', 'r9.y', 'r9.y', 'c53.z')
    p.op('rcp', 'r9.z', 'c50.x')
    p.op('mul', 'r9.z', 'r9.z', 'c50.y')
    p.op('mul', 'r9.x', 'r9.y', 'r9.z')                            # r9.xy = radius in uv
    # 2x2 pattern: bx, by from the half-res pixel position; rotation 22.5 deg x (bx + 2 by); step jitter (k+1)/4
    p.op('mul', 'r10.xy', 'v0.xy', 'c1.zw')
    p.op('mul', 'r10.xy', 'r10.xy', 'c101.x')
    p.op('frc', 'r10.xy', 'r10.xy')
    p.op('add', 'r10.xy', 'r10.xy', '-c101.x')
    p.op('cmp', 'r10.xy', 'r10.xy', 'c100.x', 'c100.y')
    p.op('add', 'r11.xy', 'c102.zw', '-c102.xy')
    p.op('mad', 'r11.xy', 'r10.x', 'r11.xy', 'c102.xy')
    p.op('add', 'r11.zw', 'c103.zw', '-c103.xy')
    p.op('mad', 'r11.zw', 'r10.x', 'r11.zw', 'c103.xy')
    p.op('add', 'r11.zw', 'r11.zw', '-r11.xy')
    p.op('mad', 'r11.xy', 'r10.y', 'r11.zw', 'r11.xy')            # r11.xy = (cos, sin)
    p.op('mad', 'r10.z', 'r10.x', 'c101.y', 'c101.y')
    p.op('mad', 'r10.z', 'r10.y', 'c101.x', 'r10.z')
    p.op('mul', 'r10.z', 'r10.z', 'c104.x')                       # t0 = (k+1)/12
    p.op('add', 'r10.w', 'r10.z', 'c104.x')                       # t1
    p.op('add', 'r9.w', 'r10.z', 'c104.w')                        # t2
    p.op('mul', 'r12.xy', 'r11.xy', 'r9.xy')                      # d0 = (c, s) * radius
    p.op('mul', 'r12.zw', 'r11.yx', 'r9.xy')                      # (s, c) * radius
    p.op('mul', 'r12.z', 'r12.z', 'c100.w')                       # d1 = (-s, c) * radius
    p.op('mul', 'r17.x', 'c52.x', 'c52.x')
    p.op('rcp', 'r17.x', 'r17.x')                                 # 1 / R^2
    p.op('mov', 'r13.y', 'c100.y')                                # sum of horizons
    for dvec, sign in (('r12.xy', ''), ('r12.zw', ''), ('r12.xy', '-'), ('r12.zw', '-')):
        for j, t in enumerate(('r10.z', 'r10.w', 'r9.w')):
            p.op('mad', 'r14.xy', dvec, sign + t, 'r0.xy')
            p.op('mul', 'r14.xy', 'r14.xy', 'c2.zw')              # snap to the texel centre: the position is
            p.op('frc', 'r16.xy', 'r14.xy')                       # rebuilt at the uv the depth belongs to
            p.op('add', 'r14.xy', 'r14.xy', '-r16.xy')
            p.op('add', 'r14.xy', 'r14.xy', 'c101.x')
            p.op('mul', 'r14.xy', 'r14.xy', 'c2.xy')
            view_pos(p, 'r15', 'r14', 'r16')
            p.op('add', 'r15.xyz', 'r15.xyz', '-r2.xyz')          # v
            p.op('dp3', 'r16.x', 'r15.xyz', 'r15.xyz')            # |v|^2
            p.op('max', 'r16.y', 'r16.x', 'c101.z')
            p.op('rsq', 'r16.y', 'r16.y')
            p.op('dp3', 'r16.z', 'r15.xyz', 'r8.xyz')
            p.op('mul', 'r16.z', 'r16.z', 'r16.y')                # sin of the elevation above the tangent plane
            p.op('add_sat', 'r16.z', 'r16.z', '-c52.y')
            p.op('mad_sat', 'r16.w', '-r16.x', 'r17.x', 'c100.x') # falloff 1 - |v|^2 / R^2
            p.op('mul', 'r16.z', 'r16.z', 'r16.w')
            if j == 0:
                p.op('mov', 'r13.x', 'r16.z')
            else:
                p.op('max', 'r13.x', 'r13.x', 'r16.z')
        p.op('add', 'r13.y', 'r13.y', 'r13.x')
    p.op('mad', 'r13.y', 'r13.y', '-c101.y', 'c100.x')            # AO = 1 - mean
    p.op('add', 'r17.y', 'c53.y', '-c53.x')                       # distance fade
    p.op('rcp', 'r17.y', 'r17.y')
    p.op('add', 'r17.z', 'r1.z', '-c53.x')
    p.op('mul_sat', 'r17.z', 'r17.z', 'r17.y')
    p.op('add', 'r17.w', 'c100.x', '-r13.y')
    p.op('mad', 'r13.y', 'r17.z', 'r17.w', 'r13.y')
    p.op('add', 'r17.w', 'r1.x', '-c51.y')                        # sky / far: AO 1
    p.op('cmp', 'r13.y', 'r17.w', 'c100.x', 'r13.y')
    p.op('rcp', 'r18.x', 'c53.y')                                 # e = z / fade end, 16 bits in g, b
    p.op('mul_sat', 'r18.x', 'r1.z', 'r18.x')
    p.op('mul', 'r18.y', 'r18.x', 'c104.y')
    p.op('frc', 'r18.z', 'r18.y')
    p.op('add', 'r18.w', 'r18.y', '-r18.z')
    p.op('mul', 'r18.w', 'r18.w', 'c104.z')
    p.op('mov', 'r20.x', 'r13.y')
    p.op('mov', 'r20.y', 'r18.w')
    p.op('mov', 'r20.z', 'r18.z')
    p.op('mov', 'r20.w', 'c100.x')
    p.op('mad', 'r21.xyz', 'r8.xyz', 'c101.x', 'c101.x')          # TEST: normals as colour
    p.op('mov', 'r21.w', 'c100.x')
    p.op('add', 'r19.x', 'c54.w', '-c101.x')
    p.op('cmp', 'r20', 'r19.x', 'r21', 'r20')
    p.op('mov', 'oC0', 'r20')
    return p


def gen_blur(axis):
    p = Prog(1)
    p.define('c100', (1.0, 0.0, 2.0, -1.0))
    p.define('c101', (1.0 / 255.0, 1e-4, 0.0, 0.0))
    p.tex('r0', 'v0.xy', 0)
    p.op('mad', 'r1.z', 'r0.z', 'c101.x', 'r0.y')                 # e0
    p.op('mul', 'r1.x', 'r1.z', 'c54.x')
    p.op('max', 'r1.x', 'r1.x', 'c101.y')
    p.op('rcp', 'r1.y', 'r1.x')                                   # 1 / (tol e0)
    p.op('mul', 'r2.x', 'r0.x', 'c100.z')                         # centre weight 2
    p.op('mov', 'r2.y', 'c100.z')
    for k, gw in ((-2, 'c100.x'), (-1, 'c100.z'), (1, 'c100.z'), (2, 'c100.x')):
        a = 'x' if axis == 'h' else 'y'
        p.op('mov', 'r3.xy', 'v0.xy')
        p.op('mad', f'r3.{a}', f'c2.{a}', {-2: '-c100.z', -1: 'c100.w', 1: 'c100.x', 2: 'c100.z'}[k], f'v0.{a}')
        p.tex('r4', 'r3.xy', 0)
        p.op('mad', 'r5.x', 'r4.z', 'c101.x', 'r4.y')             # e
        p.op('add', 'r5.x', 'r5.x', '-r1.z')
        p.op('abs', 'r5.x', 'r5.x')
        p.op('mad_sat', 'r5.x', '-r5.x', 'r1.y', 'c100.x')        # depth weight
        p.op('mul', 'r5.x', 'r5.x', gw)
        p.op('mad', 'r2.x', 'r5.x', 'r4.x', 'r2.x')
        p.op('add', 'r2.y', 'r2.y', 'r5.x')
    p.op('rcp', 'r2.y', 'r2.y')
    p.op('mul', 'r0.x', 'r2.x', 'r2.y')
    p.op('mov', 'r0.w', 'c100.x')
    p.op('mov', 'oC0', 'r0')
    return p


def gen_apply():
    p = Prog(3)
    p.define('c100', (1.0, 0.0, 2.0, 3.0))
    p.define('c101', (0.2126, 0.7152, 0.0722, 0.5))
    p.define('c102', (1.5, 0.0, 0.0, 0.0))
    p.tex('r0', 'v0.xy', 0)                                       # AO (linear upsample)
    p.tex('r1', 'v0.xy', 1)                                       # frame
    p.tex('r2', 'v0.xy', 2)                                       # device depth
    p.op('add', 'r3.x', 'c100.x', '-c52.w')
    p.op('max', 'r3.x', 'r0.x', 'r3.x')                           # AO' = max(AO, 1 - max darkening)
    p.op('dp3', 'r4.x', 'r1.xyz', 'c101.xyz')                     # luma
    p.op('add', 'r3.y', 'c54.y', '-c53.w')
    p.op('rcp', 'r3.y', 'r3.y')
    p.op('add', 'r3.z', 'r4.x', '-c53.w')
    p.op('mul_sat', 'r3.z', 'r3.z', 'r3.y')                       # t
    p.op('mad', 'r3.w', 'r3.z', '-c100.z', 'c100.w')              # 3 - 2t
    p.op('mul', 'r3.w', 'r3.w', 'r3.z')
    p.op('mul', 'r3.w', 'r3.w', 'r3.z')                           # smoothstep
    p.op('add', 'r3.w', 'c100.x', '-r3.w')
    p.op('mul', 'r3.w', 'r3.w', 'c52.z')                          # k = strength (1 - guard)
    p.op('add', 'r4.y', 'r2.x', '-c51.y')
    p.op('cmp', 'r3.w', 'r4.y', 'c100.y', 'r3.w')                 # sky: k = 0
    p.op('add', 'r3.x', 'r3.x', '-c100.x')
    p.op('mad', 'r3.x', 'r3.w', 'r3.x', 'c100.x')                 # lerp(1, AO', k)
    p.op('mul', 'r5.xyz', 'r1.xyz', 'r3.x')
    p.op('mov', 'r5.w', 'r1.w')
    p.op('mov', 'r6.xyz', 'r0.x')                                 # TEST view 1: AO grey
    p.op('mov', 'r6.w', 'r1.w')
    p.op('add', 'r4.z', 'c54.z', '-c101.w')
    p.op('cmp', 'r5', 'r4.z', 'r6', 'r5')
    p.op('mov', 'r6.xyz', 'r0.xyz')                               # TEST view 2: AoA rgb (normals)
    p.op('add', 'r4.w', 'c54.z', '-c102.x')
    p.op('cmp', 'r5', 'r4.w', 'r6', 'r5')
    p.op('mov', 'oC0', 'r5')
    return p


PROGS = {'PS_Ao': gen_ao, 'PS_AoBlurH': lambda: gen_blur('h'), 'PS_AoBlurV': lambda: gen_blur('v'), 'PS_AoApply': gen_apply}


def write_asm():
    texts = {}
    for name, fn in PROGS.items():
        p = fn()
        print(p.check(name))
        texts[name] = p.text()
        (SRC / f'clean-chain.{name}.asm').write_text(texts[name])
    return texts


def build_fx(out, hdr=False):
    """fxgen.py with each SCOPED_GROUPS group (Ao* c50-c54, GradeParams c86, MirrorParams c87) left out of the constant
    table of every program that does not name one of its registers: the existing programs keep exactly their bytecode."""
    sys.path.insert(0, str(HERE))
    import d3d9asm, runpy
    plain = d3d9asm.assemble

    def scoped(target, consts, text):
        defined = {int(r) for r in re.findall(r'^\s*def\s+c(\d+)', text, re.M)}   # local literals, not the host inputs
        used = {int(r) for r in re.findall(r'\bc(\d+)\b', text)} - defined
        for group in SCOPED_GROUPS:
            if not used & set(group):
                consts = [c for c in consts if not (c[1] == 2 and c[2] in group)]
        return plain(target, consts, text)
    d3d9asm.assemble = scoped
    argv = sys.argv
    sys.argv = ['fxgen.py'] + (['--hdr'] if hdr else []) + [str(SRC / 'clean-chain.hlsl'), str(SRC), str(out)]   # --hdr: item 10
    try:
        runpy.run_path(str(HERE / 'fxgen.py'), run_name='__main__')
    finally:
        sys.argv = argv
        d3d9asm.assemble = plain


# ---------------------------------------------------------------- offline model (numpy interpreter of the asm)
def run_ps(text, consts, textures, w, h):
    """Executes ps_3_0 asm (the subset above) for every pixel of a w x h target. textures: {s: (array HxWxC, linear)}."""
    import numpy as np
    n = w * h
    ys, xs = np.mgrid[0:h, 0:w]
    reg = {'v0': np.zeros((n, 4), np.float32)}
    reg['v0'][:, 0] = ((xs + 0.5) / w).ravel()
    reg['v0'][:, 1] = ((ys + 0.5) / h).ravel()
    cst = {k: np.broadcast_to(np.asarray(v, np.float32), (n, 4)) for k, v in consts.items()}

    def get(tok):
        neg = tok.startswith('-')
        tok = tok[1:] if neg else tok
        name, _, sw = tok.partition('.')
        sw = sw or 'xyzw'
        base = reg.get(name) if name in reg else cst.get(name)
        if base is None:
            base = np.zeros((n, 4), np.float32)
        v = base[:, ['xyzw'.index(c) for c in sw]]
        return -v if neg else v

    def sample(s, uv):
        img, linear = textures[s]
        H, W = img.shape[:2]
        if img.ndim == 2:
            img = img[:, :, None]
        def fetch(ix, iy):
            ix = np.clip(ix, 0, W - 1); iy = np.clip(iy, 0, H - 1)
            return img[iy, ix]
        if not linear:
            out = fetch(np.floor(uv[:, 0] * W).astype(int), np.floor(uv[:, 1] * H).astype(int))
        else:
            fx = uv[:, 0] * W - 0.5; fy = uv[:, 1] * H - 0.5
            x0 = np.floor(fx).astype(int); y0 = np.floor(fy).astype(int)
            ax = (fx - x0)[:, None]; ay = (fy - y0)[:, None]
            out = ((1 - ax) * (1 - ay) * fetch(x0, y0) + ax * (1 - ay) * fetch(x0 + 1, y0) +
                   (1 - ax) * ay * fetch(x0, y0 + 1) + ax * ay * fetch(x0 + 1, y0 + 1))
        r = np.zeros((n, 4), np.float32)
        r[:, :out.shape[1]] = out
        if out.shape[1] == 1:
            r[:, 1:3] = 0; r[:, 3] = 1
        return r

    with np.errstate(all='ignore'):
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith(('ps_', 'dcl')):
                continue
            op, _, rest = line.partition(' ')
            toks = [t.strip() for t in rest.replace('=', ',', 1).split(',')] if op == 'def' else [t.strip() for t in rest.split(',')]
            if op == 'def':
                cst[toks[0]] = np.broadcast_to(np.array([float(x) for x in toks[1:5]], np.float32), (n, 4))
                continue
            sat = op.endswith('_sat'); op = op.replace('_sat', '')
            dname, _, mask = toks[0].partition('.')
            mask = mask or 'xyzw'
            a = [get(t) for t in toks[1:]] if op != 'texld' else None
            if op == 'texld':
                r = sample(int(toks[2][1]), get(toks[1])[:, :2])
            elif op == 'mov': r = a[0]
            elif op == 'add': r = a[0] + a[1]
            elif op == 'mul': r = a[0] * a[1]
            elif op == 'mad': r = a[0] * a[1] + a[2]
            elif op == 'rcp': r = np.repeat(1.0 / a[0][:, :1], 4, 1)
            elif op == 'rsq': r = np.repeat(1.0 / np.sqrt(np.abs(a[0][:, :1])), 4, 1)
            elif op == 'dp3': r = np.repeat((a[0][:, :3] * a[1][:, :3]).sum(1, keepdims=True), 4, 1)
            elif op == 'min': r = np.minimum(a[0], a[1])
            elif op == 'max': r = np.maximum(a[0], a[1])
            elif op == 'abs': r = np.abs(a[0])
            elif op == 'frc': r = a[0] - np.floor(a[0])
            elif op == 'cmp': r = np.where(a[0] >= 0, a[1], a[2])
            else: raise ValueError(op)
            r = r.astype(np.float32)
            if sat:
                r = np.clip(r, 0, 1)
            dst = reg.setdefault(dname, np.zeros((n, 4), np.float32)).copy()
            for c in mask:
                i = 'xyzw'.index(c)
                dst[:, i] = r[:, i]
            reg[dname] = dst
    return reg['oC0'].reshape(h, w, 4)


def check(texts):
    import numpy as np
    W, H = 640, 360
    near, far = 0.5, 10000.0

    def q8(img):   # argb8 render target storage
        return np.round(np.clip(img, 0, 1) * 255) / 255

    def scene(kind, vfov_deg, pitch_deg=4.0, cam_h=1.4):
        ty = np.tan(np.radians(vfov_deg) / 2); tx = ty * W / H
        ys, xs = np.mgrid[0:H, 0:W]
        rx = (2 * (xs + 0.5) / W - 1) * tx; ry = (2 * (ys + 0.5) / H - 1) * ty
        p = np.radians(pitch_deg)
        # world: Y up; camera at (0, cam_h, 0); forward f = (0, -sin p, cos p); down = (0, -cos p, -sin p)
        dx = rx; dy = -ry * np.cos(p) - np.sin(p); dz = -ry * np.sin(p) + np.cos(p)   # world ray for view z = 1
        z = np.full((H, W), np.inf)
        if kind in ('road', 'corner', 'sphere'):
            t = np.where(dy < 0, cam_h / np.maximum(-dy, 1e-9), np.inf)
            z = np.minimum(z, t)
        if kind == 'corner':   # wall at world X = 2.5 m, the ground meets it at a right angle
            t = np.where(dx > 0, 2.5 / np.maximum(dx, 1e-9), np.inf)
            z = np.minimum(z, t)
        if kind in ('sphere', 'float'):   # car proxy: sphere radius 1 m, 7 m ahead (on the ground, or floating in the sky)
            cy = 1.0 if kind == 'sphere' else 4.0
            c = np.array([0.0, cy - cam_h, 7.0 if kind == 'sphere' else 12.0])
            b = dx * c[0] + dy * c[1] + dz * c[2]; aa = dx * dx + dy * dy + dz * dz
            disc = b * b - aa * (c @ c - 1.0)
            t = np.where(disc > 0, (b - np.sqrt(np.maximum(disc, 0))) / aa, np.inf)
            z = np.minimum(z, np.where(t > 0, t, np.inf))
        d = np.where(np.isfinite(z), (far - near * far / np.maximum(z, near)) / (far - near), 1.0).astype(np.float32)
        return d, tx, ty, z

    def run(d, tx, ty, view=0):
        consts = {'c50': (tx, ty, near * far, far), 'c51': (far - near, min(0.99999, (far - near * far / 100.0) / (far - near)), 1 / far, 0),
                  'c52': (1.0, 0.03, 1.0, 0.35), 'c53': (60.0, 100.0, 0.08, 0.55), 'c54': (0.05, 0.9, 0, 1 if view == 2 else 0)}
        hw, hh = W // 2, H // 2
        c = dict(consts, c1=(1 / hw, 1 / hh, hw, hh), c2=(1 / W, 1 / H, W, H))
        ao = q8(run_ps(texts['PS_Ao'], c, {0: (d, False)}, hw, hh))
        if view == 2:
            return ao, ao
        c2 = dict(consts, c1=(1 / hw, 1 / hh, hw, hh), c2=(1 / hw, 1 / hh, hw, hh))
        b = q8(run_ps(texts['PS_AoBlurH'], c2, {0: (ao, False)}, hw, hh))
        b = q8(run_ps(texts['PS_AoBlurV'], c2, {0: (b, False)}, hw, hh))
        frame = np.full((H, W, 4), 0.3, np.float32)
        c3 = dict(consts, c1=(1 / W, 1 / H, W, H))
        out = run_ps(texts['PS_AoApply'], c3, {0: (b, True), 1: (frame, False), 2: (d, False)}, W, H)
        return ao, b, out[:, :, 0] / 0.3

    res = {}
    for vfov in (45.0, 65.0):
        d, tx, ty, z = scene('road', vfov)
        ao, b, f = run(d, tx, ty)
        ground = d[::2, ::2] < 0.99999   # half-res sample positions are the even texels (centre = texel 2i)
        res[f'road vfov {vfov:.0f}: AO min (raw / blurred), apply min'] = (float(ao[..., 0][ground].min()), float(b[..., 0][ground].min()), float(f.min()))
        nv, _ = run(d, tx, ty, view=2)
        nrm = nv[..., :3][ground & (z[::2, ::2] < 60)]
        res[f'road vfov {vfov:.0f}: normal colour spread (max-min per channel)'] = tuple(float(x) for x in (nrm.max(0) - nrm.min(0)))
    vfov = 55.0
    d, tx, ty, z = scene('corner', vfov)
    ao, b, f = run(d, tx, ty)
    zz = z[::2, ::2]
    res['corner: AO raw min, blurred min, blurred 5th pct (z < 30)'] = (float(ao[..., 0][zz < 30].min()), float(b[..., 0][zz < 30].min()),
                                                                       float(np.percentile(b[..., 0][zz < 30], 5)))
    # the ground strip next to the wall base (world X 2.0-2.4 m, z 5-15 m) and the open road (X < 0.5 m)
    ys, xs = np.mgrid[0:H, 0:W]
    rxv = (2 * (xs + 0.5) / W - 1) * tx
    X = (rxv * z)[::2, ::2]
    strip = (X > 2.0) & (X < 2.4) & (zz > 5) & (zz < 15) & (d[::2, ::2] < 0.99999)
    wall = (X > 2.45) & (zz > 5) & (zz < 15)
    openr = (X < 0.5) & (zz > 5) & (zz < 30)
    res['corner: blurred AO mean ground by wall base / wall / open road'] = (float(b[..., 0][strip].mean()), float(b[..., 0][wall].mean()), float(b[..., 0][openr].mean()))
    d, tx, ty, z = scene('sphere', vfov)
    ao, b, f = run(d, tx, ty)
    sky = d >= 0.99999
    res['sphere on road: blurred AO min, sky apply factor min/max'] = (float(b[..., 0].min()), float(f[sky].min()), float(f[sky].max()))
    d, tx, ty, z = scene('float', vfov)
    ao, b, f = run(d, tx, ty)
    sky_h = d[::2, ::2] >= 0.99999
    obj_h = ~sky_h
    # bleed: half-res pixels with AO < 0.999 farther than 2 px from the object (dilate the object mask by 2)
    from numpy.lib.stride_tricks import sliding_window_view
    pad = np.pad(obj_h, 2)
    near_obj = sliding_window_view(pad, (5, 5)).any(axis=(2, 3))
    bleed = int(((b[..., 0] < 0.999) & ~near_obj).sum())
    res['floating sphere vs sky: AO<0.999 beyond 2 half-res px, sky apply min, object AO min'] = (bleed, float(f[d >= 0.99999].min()), float(b[..., 0][obj_h].min()))
    for k, v in res.items():
        print(f'{k}: ' + ', '.join(f'{x:.4f}' if isinstance(x, float) else str(x) for x in v))
    return res


if __name__ == '__main__':
    if '--fx' in sys.argv:   # the setup: builds from effects/ as it is, never rewrites the .asm files
        build_fx(Path(sys.argv[sys.argv.index('--fx') + 1]), hdr='--hdr' in sys.argv)   # --hdr: clean-chain-hdr.fx
    else:
        texts = write_asm()
        if '--check' in sys.argv:
            check(texts)
