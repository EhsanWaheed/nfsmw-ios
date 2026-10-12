ps_3_0
def c92 = 1.25000000e-01, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00
dcl_texcoord0 v0
dcl_2d s0
add r1.xy, c28.xyxx, -v0.xyxx
mul r1.xy, r1.xyxx, c31.xxxx
mul r1.xy, r1.xyxx, c92.xxxx
mov r0.xy, v0.xyxx
mov r2.xyzw, c92.yyyy
mov r3.x, c92.w
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r3.x, r3.x, c31.y
add r0.xy, r0.xyxx, r1.xyxx
texld r4.xyzw, r0.xyxx, s0.xyzw
mad r2.xyzw, r4.xyzw, r3.xxxx, r2.xyzw
mul r2.xyz, r2.xyzx, c31.zzzz
mov r2.w, c92.w
mov oC0.xyzw, r2.xyzw
