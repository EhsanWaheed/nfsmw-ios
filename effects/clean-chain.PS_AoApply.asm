ps_3_0
def c100 = 1.00000000e+00, 0.00000000e+00, 2.00000000e+00, 3.00000000e+00
def c101 = 2.12600000e-01, 7.15200000e-01, 7.22000000e-02, 5.00000000e-01
def c102 = 1.50000000e+00, 0.00000000e+00, 0.00000000e+00, 0.00000000e+00
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
texld r0.xyzw, v0.xyyy, s0.xyzw
texld r1.xyzw, v0.xyyy, s1.xyzw
texld r2.xyzw, v0.xyyy, s2.xyzw
add r3.x, c100.xxxx, -c52.wwww
max r3.x, r0.xxxx, r3.xxxx
dp3 r4.x, r1.xyzz, c101.xyzz
add r3.y, c54.yyyy, -c53.wwww
rcp r3.y, r3.yyyy
add r3.z, r4.xxxx, -c53.wwww
mul_sat r3.z, r3.zzzz, r3.yyyy
mad r3.w, r3.zzzz, -c100.zzzz, c100.wwww
mul r3.w, r3.wwww, r3.zzzz
mul r3.w, r3.wwww, r3.zzzz
add r3.w, c100.xxxx, -r3.wwww
mul r3.w, r3.wwww, c52.zzzz
add r4.y, r2.xxxx, -c51.yyyy
cmp r3.w, r4.yyyy, c100.yyyy, r3.wwww
add r3.x, r3.xxxx, -c100.xxxx
mad r3.x, r3.wwww, r3.xxxx, c100.xxxx
mul r5.xyz, r1.xyzx, r3.xxxx
mov r5.w, r1.wwww
mov r6.xyz, r0.xxxx
mov r6.w, r1.wwww
add r4.z, c54.zzzz, -c101.wwww
cmp r5.xyzw, r4.zzzz, r6.xyzw, r5.xyzw
mov r6.xyz, r0.xyzx
add r4.w, c54.zzzz, -c102.xxxx
cmp r5.xyzw, r4.wwww, r6.xyzw, r5.xyzw
mov oC0.xyzw, r5.xyzw
