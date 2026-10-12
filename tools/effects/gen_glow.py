#!/usr/bin/env python3
"""Writes clean-chain.PS_Glow.asm (module H, R2 run 8): analytic single scattering of up to 8 point lamps along each
pixel's view ray, from the eye to the scene depth (public technique: the closed-form integral of 1/(h^2 + s^2)
along the ray, (atan((t-b)/h) - atan(-b/h)) / h). Our own code. Registers:
  c32+2i  lamp i: view-space position xyz (x right, y down, z forward), w = core radius^2 (softens h near 0)
  c33+2i  lamp i: rgb = colour x intensity (0 = unused), w unused
  c48     (near*far, far, far - near, 0)          c49  (tan half-FOV x, tan half-FOV y, strength, darkness power)
s0 = frame (darkness: the glow fades where the frame is already bright), s1 = scene depth (R32F device depth)."""
from pathlib import Path
L = []
a = L.append
a('ps_3_0')
a('def c100 = 1.00000000e+00, 0.00000000e+00, 2.00000000e+00, -1.00000000e+00')
a('def c101 = 9.99866009e-01, -3.30299497e-01, 1.80141002e-01, -8.51330012e-02')
a('def c102 = 2.08351002e-02, 1.57079637e+00, 2.12599993e-01, 7.15200007e-01')
a('def c103 = 7.22000003e-02, 1.00000001e-04, 0.00000000e+00, 0.00000000e+00')
a('def c104 = 2.12599993e-01, 7.15200007e-01, 7.22000003e-02, 0.00000000e+00')
a('dcl_texcoord0 v0')
a('dcl_2d s0')
a('dcl_2d s1')
# view ray (z = 1) and the distance along it to the scene: depth z * |ray|
a('mad r0.xy, v0.xyxx, c100.zzzz, c100.wwww')      # 2uv - 1
a('mul r0.xy, r0.xyxx, c49.xyxx')
a('mov r0.z, c100.x')
a('dp3 r0.w, r0.xyzx, r0.xyzx')
a('rsq r1.w, r0.w')                                 # 1/|ray|
a('mul r1.xyz, r0.xyzx, r1.wwww')                   # unit direction
a('rcp r2.w, r1.w')                                 # |ray|
a('texld r3.xyzw, v0.xyxx, s1.xyzw')                # device depth d
a('mad r3.y, -r3.x, c48.z, c48.y')                  # f - d (f - n)
a('rcp r3.y, r3.y')
a('mul r3.x, c48.x, r3.y')                          # view z
a('mul r2.x, r3.x, r2.w')                           # t max along the unit ray
a('mov r4.xyz, c100.yyyy')                          # accumulated glow
def atan(dst, src, t):
    """dst.x = atan(src.x); temporaries r{t}.xyzw"""
    a(f'abs r{t}.x, {src}')                         # a = |x|
    a(f'add r{t}.y, r{t}.x, -c100.x')               # a - 1 (>= 0: use 1/a)
    a(f'rcp r{t}.z, r{t}.x')
    a(f'cmp r{t}.z, r{t}.y, r{t}.z, r{t}.x')        # u = a > 1 ? 1/a : a
    a(f'mul r{t}.w, r{t}.z, r{t}.z')                # u^2
    a(f'mad {dst}, r{t}.w, c102.x, c101.w')
    a(f'mad {dst}, r{t}.w, {dst}, c101.z')
    a(f'mad {dst}, r{t}.w, {dst}, c101.y')
    a(f'mad {dst}, r{t}.w, {dst}, c101.x')
    a(f'mul {dst}, {dst}, r{t}.z')                  # atan(u)
    a(f'add r{t}.w, c102.y, -{dst}')                # pi/2 - atan(u)
    a(f'cmp {dst}, r{t}.y, r{t}.w, {dst}')          # atan(a)
    a(f'cmp r{t}.w, {src}, c100.x, c100.w')         # sign(x) (0 -> +)
    a(f'mul {dst}, {dst}, r{t}.w')
for i in range(8):
    P, C = f'c{32 + 2 * i}', f'c{33 + 2 * i}'
    a(f'dp3 r5.x, {P}.xyzx, r1.xyzx')               # b = closest-approach distance along the ray
    a(f'dp3 r5.y, {P}.xyzx, {P}.xyzx')
    a(f'mad r5.y, -r5.x, r5.x, r5.y')               # h^2 = |p|^2 - b^2
    a(f'add r5.y, r5.y, {P}.w')                     # + core^2
    a(f'max r5.y, r5.y, c103.y')
    a('rsq r5.z, r5.y')                             # 1/h
    a('add r6.x, r2.x, -r5.x')                      # t - b
    a('mul r6.x, r6.x, r5.z')
    a('mul r6.y, -r5.x, r5.z')                      # -b / h
    atan('r7.x', 'r6.x', 8)
    atan('r7.y', 'r6.y', 9)
    a('add r7.x, r7.x, -r7.y')
    a('mul r7.x, r7.x, r5.z')                       # integral / h
    a(f'mad r4.xyz, {C}.xyzx, r7.xxxx, r4.xyzx')
# darkness: (1 - luma(frame))^power, strength
a('texld r10.xyzw, v0.xyxx, s0.xyzw')
a('dp3 r10.x, r10.xyzx, c104.xyzx')               # Rec. 709 luma
a('add_sat r10.x, c100.x, -r10.x')
a('pow r10.x, r10.x, c49.w')
a('mul r10.x, r10.x, c49.z')
a('mul r4.xyz, r4.xyzx, r10.xxxx')
a('mov r4.w, c100.x')
a('mov oC0.xyzw, r4.xyzw')
Path(__file__).resolve().parents[2].joinpath('effects', 'clean-chain.PS_Glow.asm').write_text('\n'.join(L) + '\n')
print(len(L), 'lines')
