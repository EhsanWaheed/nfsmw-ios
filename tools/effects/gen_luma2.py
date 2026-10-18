#!/usr/bin/env python3
"""Writes the merged exposure-measurement programs (v4, owner's long-session efficiency work):

  clean-chain.PS_LumaBoth16.asm   s0 = frame (point) -> Luma4 (frame/4, RGBA32F): x = mean log luminance,
                                  y = mean quartic luminance, over the same 4x4 stratified footprint as
                                  PS_LumaLog16 and PS_LumaQuartic16 (the frame is read once, not twice)
  clean-chain.PS_Reduce16x2.asm   s0 = Luma* (point) -> the next level: PS_Reduce16 on x and y together

The log path and the quartic path are the old programs' maths exactly: y = dot(rgb, Rec.709), log path
ln(max(y, LumaParams.x)) (log2 x ln 2), quartic path max(y, 0)^4, each averaged over 16 taps. The two chains of six
passes become one chain of six (Luma4 .. LogMean), LogMean.x feeds clean_adapt and LogMean.y the exposure passes.
"""
from pathlib import Path

TAPS = [(-0.375, -0.375), (-0.125, -0.375), (0.125, -0.375), (0.375, -0.375),
        (-0.375, -0.125), (-0.125, -0.125), (0.125, -0.125), (0.375, -0.125),
        (-0.375, 0.125), (-0.125, 0.125), (0.125, 0.125), (0.375, 0.125),
        (-0.375, 0.375), (-0.125, 0.375), (0.125, 0.375), (0.375, 0.375)]


def f(x):
    return f'{x:.8e}'


def offsets():
    # c28..c35: two taps per register (.xy, .zw)
    out = []
    for i in range(0, 16, 2):
        a, b = TAPS[i], TAPS[i + 1]
        out.append(f'def c{28 + i // 2} = {f(a[0])}, {f(a[1])}, {f(b[0])}, {f(b[1])}')
    return out


def tap_address(k):
    reg, half = 28 + k // 2, ('xyxx' if k % 2 == 0 else 'zwxx')
    return [f'mul r0.xy, c{reg}.{half}, r6.xyxx',
            'add r0.xy, v0.xyxx, r0.xyxx']


def both():
    L = ['ps_3_0', 'dcl_texcoord0 v0'] + offsets()
    L += ['def c36 = 2.12599993e-01, 7.15200007e-01, 7.22000003e-02, 6.93147182e-01   ; Rec.709, ln 2',
          'def c37 = 6.25000000e-02, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00   ; 1/16, 0, 0, 1',
          '; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero',
          'dcl_2d s0',
          'mov r6.xy, c1.xyxx          ; DstTexelSize.xy',
          'mov r6.z, c4.xxxx           ; LumaParams.x (the log floor)',
          'mov r5.xy, c37.yyyy         ; sums: x log, y quartic']
    for k in range(16):
        L += tap_address(k)
        L += ['texld r1.xyzw, r0.xyxx, s0.xyzw',
              '; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)',
              'dp3 r2.x, r1.xyzx, c36.xyzx',
              'max r2.y, r2.xxxx, r6.zzzz',
              'log r2.y, r2.yyyy',
              'mad r5.x, r2.yyyy, c36.wwww, r5.xxxx',
              'max r2.z, r2.xxxx, c37.yyyy',
              'mul r2.z, r2.zzzz, r2.zzzz',
              'mul r2.z, r2.zzzz, r2.zzzz',
              'add r5.y, r5.yyyy, r2.zzzz']
    L += ['mul r5.xy, r5.xyxx, c37.xxxx',
          'mov r5.zw, c37.yyyw',
          'mov oC0.xyzw, r5.xyzw']
    return L


def reduce2():
    L = ['ps_3_0', 'dcl_texcoord0 v0'] + offsets()
    L += ['def c37 = 6.25000000e-02, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00   ; 1/16, 0, 0, 1',
          'dcl_2d s0',
          'mov r6.xy, c1.xyxx          ; DstTexelSize.xy',
          'mov r5.xy, c37.yyyy']
    for k in range(16):
        L += tap_address(k)
        L += ['texld r1.xyzw, r0.xyxx, s0.xyzw',
              'add r5.xy, r5.xyxx, r1.xyxx']
    L += ['mul r5.xy, r5.xyxx, c37.xxxx',
          'mov r5.zw, c37.yyyw',
          'mov oC0.xyzw, r5.xyzw']
    return L


def text(lines):   # comments only on their own lines (the '; hdr+:' tags); none after an instruction
    return '\n'.join(l if l.startswith(';') else l.split(';')[0].rstrip() for l in lines) + '\n'


fx = Path(__file__).resolve().parents[2] / 'effects'
(fx / 'clean-chain.PS_LumaBoth16.asm').write_text(text(both()))
(fx / 'clean-chain.PS_Reduce16x2.asm').write_text(text(reduce2()))
