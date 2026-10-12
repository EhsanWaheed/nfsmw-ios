ps_3_0
def c100 = 1.00000000e+00, 0.00000000e+00, 2.00000000e+00, -1.00000000e+00
def c101 = 5.00000000e-01, 2.50000000e-01, 1.00000000e-24, 1.00000000e-04
def c102 = 1.00000000e+00, 0.00000000e+00, 9.23879530e-01, 3.82683430e-01
def c103 = 7.07106780e-01, 7.07106780e-01, 3.82683430e-01, 9.23879530e-01
def c104 = 3.33333333e-01, 2.55000000e+02, 3.92156863e-03, 6.66666667e-01
dcl_texcoord0 v0
dcl_2d s0
mad r0.xy, c2.xyxx, -c101.xxxx, v0.xyxx
texld r1.xyzw, r0.xyyy, s0.xyzw
mad r1.z, -r1.xxxx, c51.xxxx, c50.wwww
rcp r1.z, r1.zzzz
mul r1.z, c50.zzzz, r1.zzzz
mad r2.xy, r0.xyxx, c100.zzzz, c100.wwww
mul r2.xy, r2.xyxx, c50.xyxx
mul r2.xy, r2.xyxx, r1.zzzz
mov r2.z, r1.zzzz
add r3.x, r0.xxxx, c2.xxxx
mov r3.y, r0.yyyy
texld r16.xyzw, r3.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r4.xy, r3.xyxx, c100.zzzz, c100.wwww
mul r4.xy, r4.xyxx, c50.xyxx
mul r4.xy, r4.xyxx, r16.yyyy
mov r4.z, r16.yyyy
add r3.x, r0.xxxx, -c2.xxxx
texld r16.xyzw, r3.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r5.xy, r3.xyxx, c100.zzzz, c100.wwww
mul r5.xy, r5.xyxx, c50.xyxx
mul r5.xy, r5.xyxx, r16.yyyy
mov r5.z, r16.yyyy
mov r3.x, r0.xxxx
add r3.y, r0.yyyy, c2.yyyy
texld r16.xyzw, r3.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r6.xy, r3.xyxx, c100.zzzz, c100.wwww
mul r6.xy, r6.xyxx, c50.xyxx
mul r6.xy, r6.xyxx, r16.yyyy
mov r6.z, r16.yyyy
add r3.y, r0.yyyy, -c2.yyyy
texld r16.xyzw, r3.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r7.xy, r3.xyxx, c100.zzzz, c100.wwww
mul r7.xy, r7.xyxx, c50.xyxx
mul r7.xy, r7.xyxx, r16.yyyy
mov r7.z, r16.yyyy
add r4.xyz, r4.xyzx, -r2.xyzx
add r5.xyz, r2.xyzx, -r5.xyzx
abs r8.x, r4.zzzz
abs r8.y, r5.zzzz
add r8.x, r8.xxxx, -r8.yyyy
cmp r4.xyz, r8.xxxx, r5.xyzx, r4.xyzx
add r6.xyz, r6.xyzx, -r2.xyzx
add r7.xyz, r2.xyzx, -r7.xyzx
abs r8.x, r6.zzzz
abs r8.y, r7.zzzz
add r8.x, r8.xxxx, -r8.yyyy
cmp r6.xyz, r8.xxxx, r7.xyzx, r6.xyzx
mul r8.xyz, r4.yzxy, r6.zxyz
mad r8.xyz, -r4.zxyz, r6.yzxy, r8.xyzx
dp3 r9.x, r8.xyzz, r8.xyzz
max r9.x, r9.xxxx, c101.zzzz
rsq r9.x, r9.xxxx
mul r8.xyz, r8.xyzx, r9.xxxx
dp3 r9.x, r8.xyzz, r2.xyzz
cmp r8.xyz, r9.xxxx, -r8.xyzx, r8.xyzx
mul r9.y, r1.zzzz, c50.yyyy
add r9.y, r9.yyyy, r9.yyyy
rcp r9.y, r9.yyyy
mul r9.y, r9.yyyy, c52.xxxx
max r9.y, r9.yyyy, c2.yyyy
min r9.y, r9.yyyy, c53.zzzz
rcp r9.z, c50.xxxx
mul r9.z, r9.zzzz, c50.yyyy
mul r9.x, r9.yyyy, r9.zzzz
mul r10.xy, v0.xyxx, c1.zwzz
mul r10.xy, r10.xyxx, c101.xxxx
frc r10.xy, r10.xyxx
add r10.xy, r10.xyxx, -c101.xxxx
cmp r10.xy, r10.xyxx, c100.xxxx, c100.yyyy
add r11.xy, c102.zwzz, -c102.xyxx
mad r11.xy, r10.xxxx, r11.xyxx, c102.xyxx
add r11.zw, c103.zzzw, -c103.xxxy
mad r11.zw, r10.xxxx, r11.zzzw, c103.xxxy
add r11.zw, r11.zzzw, -r11.xxxy
mad r11.xy, r10.yyyy, r11.zwzz, r11.xyxx
mad r10.z, r10.xxxx, c101.yyyy, c101.yyyy
mad r10.z, r10.yyyy, c101.xxxx, r10.zzzz
mul r10.z, r10.zzzz, c104.xxxx
add r10.w, r10.zzzz, c104.xxxx
add r9.w, r10.zzzz, c104.wwww
mul r12.xy, r11.xyxx, r9.xyxx
mul r12.zw, r11.yyyx, r9.xxxy
mul r12.z, r12.zzzz, c100.wwww
mul r17.x, c52.xxxx, c52.xxxx
rcp r17.x, r17.xxxx
mov r13.y, c100.yyyy
mad r14.xy, r12.xyxx, r10.zzzz, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
mov r13.x, r16.zzzz
mad r14.xy, r12.xyxx, r10.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
mad r14.xy, r12.xyxx, r9.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
add r13.y, r13.yyyy, r13.xxxx
mad r14.xy, r12.zwzz, r10.zzzz, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
mov r13.x, r16.zzzz
mad r14.xy, r12.zwzz, r10.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
mad r14.xy, r12.zwzz, r9.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
add r13.y, r13.yyyy, r13.xxxx
mad r14.xy, r12.xyxx, -r10.zzzz, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
mov r13.x, r16.zzzz
mad r14.xy, r12.xyxx, -r10.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
mad r14.xy, r12.xyxx, -r9.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
add r13.y, r13.yyyy, r13.xxxx
mad r14.xy, r12.zwzz, -r10.zzzz, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
mov r13.x, r16.zzzz
mad r14.xy, r12.zwzz, -r10.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
mad r14.xy, r12.zwzz, -r9.wwww, r0.xyxx
mul r14.xy, r14.xyxx, c2.zwzz
frc r16.xy, r14.xyxx
add r14.xy, r14.xyxx, -r16.xyxx
add r14.xy, r14.xyxx, c101.xxxx
mul r14.xy, r14.xyxx, c2.xyxx
texld r16.xyzw, r14.xyyy, s0.xyzw
mad r16.y, -r16.xxxx, c51.xxxx, c50.wwww
rcp r16.y, r16.yyyy
mul r16.y, c50.zzzz, r16.yyyy
mad r15.xy, r14.xyxx, c100.zzzz, c100.wwww
mul r15.xy, r15.xyxx, c50.xyxx
mul r15.xy, r15.xyxx, r16.yyyy
mov r15.z, r16.yyyy
add r15.xyz, r15.xyzx, -r2.xyzx
dp3 r16.x, r15.xyzz, r15.xyzz
max r16.y, r16.xxxx, c101.zzzz
rsq r16.y, r16.yyyy
dp3 r16.z, r15.xyzz, r8.xyzz
mul r16.z, r16.zzzz, r16.yyyy
add_sat r16.z, r16.zzzz, -c52.yyyy
mad_sat r16.w, -r16.xxxx, r17.xxxx, c100.xxxx
mul r16.z, r16.zzzz, r16.wwww
max r13.x, r13.xxxx, r16.zzzz
add r13.y, r13.yyyy, r13.xxxx
mad r13.y, r13.yyyy, -c101.yyyy, c100.xxxx
add r17.y, c53.yyyy, -c53.xxxx
rcp r17.y, r17.yyyy
add r17.z, r1.zzzz, -c53.xxxx
mul_sat r17.z, r17.zzzz, r17.yyyy
add r17.w, c100.xxxx, -r13.yyyy
mad r13.y, r17.zzzz, r17.wwww, r13.yyyy
add r17.w, r1.xxxx, -c51.yyyy
cmp r13.y, r17.wwww, c100.xxxx, r13.yyyy
rcp r18.x, c53.yyyy
mul_sat r18.x, r1.zzzz, r18.xxxx
mul r18.y, r18.xxxx, c104.yyyy
frc r18.z, r18.yyyy
add r18.w, r18.yyyy, -r18.zzzz
mul r18.w, r18.wwww, c104.zzzz
mov r20.x, r13.yyyy
mov r20.y, r18.wwww
mov r20.z, r18.zzzz
mov r20.w, c100.xxxx
mad r21.xyz, r8.xyzx, c101.xxxx, c101.xxxx
mov r21.w, c100.xxxx
add r19.x, c54.wwww, -c101.xxxx
cmp r20.xyzw, r19.xxxx, r21.xyzw, r20.xyzw
mov oC0.xyzw, r20.xyzw
