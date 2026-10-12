ps_3_0
; Mirror grade + glass (run 13; review/run13/mirror-parity-design.md M1/M2). Our own program.
; s0 = MirrorCopy (point), s1 = AdaptHistory.cur (point), s2 = Q4Moment (point). c87 MirrorParams = (grade 0/1,
; glass 0..1, mirror width px, mirror height px). Grade: the main view's exposure gain (same model and clamp as
; PS_ExposureBloom, no bloom) and the finish shadow lift (FinishShadowShape, FinishParams.y). Glass: cool tint
; (0.97, 0.985, 1.0) at 100 %, 6 % rounded-rect edge darkening over the outer 4 % of the width, 1-px top highlight.
def c92, 0.5, 0, 1, 0.25
def c93, 1.44269502, 0.000001, 0.06, 0.04
def c94, -0.03, -0.015, 0, 0.15
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
texld r0.xyzw, v0.xyyy, s0.xyzw
mov r8.xyzw, c92.xxxx
texld r1.xyzw, r8.xyyy, s1.xyzw
texld r2.xyzw, r8.xyyy, s2.xyzw
max r2.x, r2.xxxx, c92.yyyy
log r2.x, r2.xxxx
mul r2.x, r2.xxxx, c92.wwww
exp r2.x, r2.xxxx
mad r3.x, c6.yyyy, r1.xxxx, c6.xxxx
mad r3.x, c6.zzzz, r2.xxxx, r3.xxxx
mul r3.x, r3.xxxx, c6.wwww
mul r3.x, r3.xxxx, c93.xxxx
exp r3.x, r3.xxxx
max r3.x, r3.xxxx, c7.xxxx
min r3.x, r3.xxxx, c7.yyyy
add r3.x, r3.xxxx, -c92.zzzz
cmp r3.y, -r1.xxxx, c92.yyyy, c87.xxxx
mad r3.x, r3.yyyy, r3.xxxx, c92.zzzz
mul_sat r4.xyz, r0.xyzz, r3.xxxx
mov_sat r5.x, c10.yyyy
max r5.y, c13.xxxx, c92.yyyy
mul r5.x, r5.xxxx, r5.yyyy
mul r5.x, r5.xxxx, c87.xxxx
max r5.y, c13.yyyy, c93.yyyy
add r6.xyz, c92.zzzz, -r4.xyzz
mul r6.xyz, r6.xyzz, r6.xyzz
mul r6.xyz, r6.xyzz, r4.xyzz
mul r6.xyz, r6.xyzz, r5.xxxx
add r7.xyz, r4.xyzz, r5.yyyy
rcp r7.x, r7.xxxx
rcp r7.y, r7.yyyy
rcp r7.z, r7.zzzz
mad_sat r4.xyz, r6.xyzz, r7.xyzz, r4.xyzz
mad r6.xyz, c94.xyzz, c87.yyyy, c92.zzzz
mul r4.xyz, r4.xyzz, r6.xyzz
add r6.xy, c92.zzzz, -v0.xyyy
min r6.xy, r6.xyyy, v0.xyyy
mul r6.xy, r6.xyyy, c87.zwww
mul r6.z, c87.zzzz, c93.wwww
add r6.xy, r6.zzzz, -r6.xyyy
max r6.xy, r6.xyyy, c92.yyyy
mul r7.xy, r6.xyyy, r6.xyyy
add r7.x, r7.xxxx, r7.yyyy
rcp r6.z, r6.zzzz
mul r7.x, r7.xxxx, r6.zzzz
mul r7.x, r7.xxxx, r6.zzzz
min r7.x, r7.xxxx, c92.zzzz
mul r7.x, r7.xxxx, c93.zzzz
mul r7.x, r7.xxxx, c87.yyyy
add r7.x, c92.zzzz, -r7.xxxx
mul r4.xyz, r4.xyzz, r7.xxxx
mul r7.y, v0.yyyy, c87.wwww
add r7.y, c92.zzzz, -r7.yyyy
mul r7.z, c94.wwww, c87.yyyy
cmp r7.z, r7.yyyy, r7.zzzz, c92.yyyy
add r6.xyz, c92.zzzz, -r4.xyzz
mad_sat r4.xyz, r6.xyzz, r7.zzzz, r4.xyzz
mov r4.w, r0.wwww
mov oC0.xyzw, r4.xyzw
