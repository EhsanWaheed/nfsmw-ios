ps_3_0
def c94 = 3.00000000e-01, 2.50000000e+00, 9.00000000e-01, 4.00000000e+01
def c95 = 8.00000000e+00, 0.00000000e+00, 0.00000000e+00, 0.00000000e+00
def c93 = 1.00000000e+00, 0.00000000e+00, -5.00000000e-01, 0.00000000e+00
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
texld r0.xyzw, v0.xyxx, s0.xyzw
mul r0.xyz, r0.xyzx, c29.xyzx
texld r1.xyzw, v0.xyxx, s1.xyzw
add r1.x, r1.x, -c30.x
mul_sat r1.x, r1.x, c30.y
add r1.y, c14.x, c93.z
cmp r1.x, r1.y, r1.x, c93.x
add r3.xy, v0.xyxx, -c28.xyxx
mul r3.x, r3.x, c28.w
mul r3.z, r3.x, r3.x
mad r3.z, r3.y, r3.y, r3.z
rsq r3.w, r3.z
rcp r3.w, r3.w
mad_sat r4.x, r3.w, -c94.y, c93.x
mul r4.y, r4.x, r4.x
mul r4.y, r4.y, r4.y
mul r4.y, r4.y, c94.x
mad_sat r4.z, r3.w, -c94.w, c93.x
pow r4.z, r4.z, c95.x
mad r4.y, r4.z, c94.z, r4.y
mul r4.y, r4.y, r1.x
mad r0.xyz, c29.xyzx, r4.yyyy, r0.xyzx
mul r0.xyz, r0.xyzx, c28.zzzz
add r5.x, v0.x, -c31.w
cmp r5.x, r5.x, c93.x, c93.y
mul r0.xyz, r0.xyzx, r5.xxxx
mov r0.w, c93.y
mov oC0.xyzw, r0.xyzw
