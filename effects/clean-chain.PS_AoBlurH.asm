ps_3_0
def c100 = 1.00000000e+00, 0.00000000e+00, 2.00000000e+00, -1.00000000e+00
def c101 = 3.92156863e-03, 1.00000000e-04, 0.00000000e+00, 0.00000000e+00
dcl_texcoord0 v0
dcl_2d s0
texld r0.xyzw, v0.xyyy, s0.xyzw
mad r1.z, r0.zzzz, c101.xxxx, r0.yyyy
mul r1.x, r1.zzzz, c54.xxxx
max r1.x, r1.xxxx, c101.yyyy
rcp r1.y, r1.xxxx
mul r2.x, r0.xxxx, c100.zzzz
mov r2.y, c100.zzzz
mov r3.xy, v0.xyxx
mad r3.x, c2.xxxx, -c100.zzzz, v0.xxxx
texld r4.xyzw, r3.xyyy, s0.xyzw
mad r5.x, r4.zzzz, c101.xxxx, r4.yyyy
add r5.x, r5.xxxx, -r1.zzzz
abs r5.x, r5.xxxx
mad_sat r5.x, -r5.xxxx, r1.yyyy, c100.xxxx
mul r5.x, r5.xxxx, c100.xxxx
mad r2.x, r5.xxxx, r4.xxxx, r2.xxxx
add r2.y, r2.yyyy, r5.xxxx
mov r3.xy, v0.xyxx
mad r3.x, c2.xxxx, c100.wwww, v0.xxxx
texld r4.xyzw, r3.xyyy, s0.xyzw
mad r5.x, r4.zzzz, c101.xxxx, r4.yyyy
add r5.x, r5.xxxx, -r1.zzzz
abs r5.x, r5.xxxx
mad_sat r5.x, -r5.xxxx, r1.yyyy, c100.xxxx
mul r5.x, r5.xxxx, c100.zzzz
mad r2.x, r5.xxxx, r4.xxxx, r2.xxxx
add r2.y, r2.yyyy, r5.xxxx
mov r3.xy, v0.xyxx
mad r3.x, c2.xxxx, c100.xxxx, v0.xxxx
texld r4.xyzw, r3.xyyy, s0.xyzw
mad r5.x, r4.zzzz, c101.xxxx, r4.yyyy
add r5.x, r5.xxxx, -r1.zzzz
abs r5.x, r5.xxxx
mad_sat r5.x, -r5.xxxx, r1.yyyy, c100.xxxx
mul r5.x, r5.xxxx, c100.zzzz
mad r2.x, r5.xxxx, r4.xxxx, r2.xxxx
add r2.y, r2.yyyy, r5.xxxx
mov r3.xy, v0.xyxx
mad r3.x, c2.xxxx, c100.zzzz, v0.xxxx
texld r4.xyzw, r3.xyyy, s0.xyzw
mad r5.x, r4.zzzz, c101.xxxx, r4.yyyy
add r5.x, r5.xxxx, -r1.zzzz
abs r5.x, r5.xxxx
mad_sat r5.x, -r5.xxxx, r1.yyyy, c100.xxxx
mul r5.x, r5.xxxx, c100.xxxx
mad r2.x, r5.xxxx, r4.xxxx, r2.xxxx
add r2.y, r2.yyyy, r5.xxxx
rcp r2.y, r2.yyyy
mul r0.x, r2.xxxx, r2.yyyy
mov r0.w, c100.xxxx
mov oC0.xyzw, r0.xyzw
