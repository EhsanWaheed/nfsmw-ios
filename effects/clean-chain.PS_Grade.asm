ps_3_0
; G2 golden colour filter (run 13; review/run13/golden-filter-plan.md section 3). Our own program.
; s0 = GradeInput (point): the exposed + bloomed frame. s1 = GradeLut 256x1 (linear): rgb = T(L), a = k * 128/255.
; out = c + w (T(L) + k (c - L) - c), saturated; alpha kept. c86 GradeParams.x = w.
def c90, 0.2126, 0.7152, 0.0722, 0
def c91, 0.99609375, 0.001953125, 0.5, 1.9921875
; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
texld r0.xyzw, v0.xyyy, s0.xyzw
; hdr+: max r21.x, r0.x, r0.y | max r21.x, r21.x, r0.z | max r21.x, r21.x, c220.x   ; m = max(1, maxc(c))
; hdr+: rcp r21.y, r21.x | mul r0.xyz, r0.xyzz, r21.yyyy   ; grade c/m (in 0..1)
dp3 r1.x, r0.xyzz, c90.xyzz
mov r2.xyzw, c91.zzzz
mad r2.x, r1.xxxx, c91.xxxx, c91.yyyy
texld r3.xyzw, r2.xyyy, s1.xyzw
add r4.xyz, r0.xyzz, -r1.xxxx
mul r4.xyz, r4.xyzz, r3.wwww
mad r4.xyz, r4.xyzz, c91.wwww, r3.xyzz
add r4.xyz, r4.xyzz, -r0.xyzz
mad_sat r0.xyz, r4.xyzz, c86.xxxx, r0.xyzz
; hdr+: mul r0.xyz, r0.xyzz, r21.xxxx   ; and back by m
mov oC0.xyzw, r0.xyzw
