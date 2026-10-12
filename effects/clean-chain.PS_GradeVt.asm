ps_3_0
; G2 vt route (run 13): the 360 visual treatment's grade order, our own program.
; s0 = GradeInput (point): exposed frame, no bloom. s1 = GradeLuma (linear): local luminance s. s2 = BloomV (linear).
; curve = ((C3 s + C2) s + C1) s + C0 (c92-c95, brightness folded in by the host); d = L + S (c - L) (c86.y = S);
; o = d curve.x + curve.yzw c; out = sat(c + w (o - c) + BloomParams.z bloom) (c86.x = w, c8.z); alpha kept.
def c90, 0.2126, 0.7152, 0.0722, 0
; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
texld r0.xyzw, v0.xyyy, s0.xyzw
texld r1.xyzw, v0.xyyy, s1.xyzw
mad r2.xyzw, c95.xyzw, r1.xxxx, c94.xyzw
mad r2.xyzw, r2.xyzw, r1.xxxx, c93.xyzw
mad r2.xyzw, r2.xyzw, r1.xxxx, c92.xyzw
dp3 r3.x, r0.xyzz, c90.xyzz
add r4.xyz, r0.xyzz, -r3.xxxx
mad r4.xyz, r4.xyzz, c86.yyyy, r3.xxxx
mul r4.xyz, r4.xyzz, r2.xxxx
mad r4.xyz, r2.yzww, r0.xyzz, r4.xyzz
add r4.xyz, r4.xyzz, -r0.xyzz
mad r4.xyz, r4.xyzz, c86.xxxx, r0.xyzz
texld r5.xyzw, v0.xyyy, s2.xyzw
mad_sat r0.xyz, r5.xyzz, c8.zzzz, r4.xyzz   ; hdr: mad r0.xyz, r5.xyzz, c8.zzzz, r4.xyzz | max r0.xyz, r0.xyzz, c220.yyyy
mov oC0.xyzw, r0.xyzw
