ps_3_0
def c100 = 1.00000000e+00, 0.00000000e+00, 2.00000000e+00, -1.00000000e+00
def c101 = 9.99866009e-01, -3.30299497e-01, 1.80141002e-01, -8.51330012e-02
def c102 = 2.08351002e-02, 1.57079637e+00, 2.12599993e-01, 7.15200007e-01
def c103 = 7.22000003e-02, 1.00000001e-04, 0.00000000e+00, 0.00000000e+00
def c104 = 2.12599993e-01, 7.15200007e-01, 7.22000003e-02, 0.00000000e+00
; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
mad r0.xy, v0.xyxx, c100.zzzz, c100.wwww
mul r0.xy, r0.xyxx, c49.xyxx
mov r0.z, c100.x
dp3 r0.w, r0.xyzx, r0.xyzx
rsq r1.w, r0.w
mul r1.xyz, r0.xyzx, r1.wwww
rcp r2.w, r1.w
texld r3.xyzw, v0.xyxx, s1.xyzw
mad r3.y, -r3.x, c48.z, c48.y
rcp r3.y, r3.y
mul r3.x, c48.x, r3.y
mul r2.x, r3.x, r2.w
mov r4.xyz, c100.yyyy
dp3 r5.x, c32.xyzx, r1.xyzx
dp3 r5.y, c32.xyzx, c32.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c32.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c33.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c34.xyzx, r1.xyzx
dp3 r5.y, c34.xyzx, c34.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c34.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c35.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c36.xyzx, r1.xyzx
dp3 r5.y, c36.xyzx, c36.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c36.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c37.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c38.xyzx, r1.xyzx
dp3 r5.y, c38.xyzx, c38.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c38.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c39.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c40.xyzx, r1.xyzx
dp3 r5.y, c40.xyzx, c40.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c40.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c41.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c42.xyzx, r1.xyzx
dp3 r5.y, c42.xyzx, c42.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c42.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c43.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c44.xyzx, r1.xyzx
dp3 r5.y, c44.xyzx, c44.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c44.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c45.xyzx, r7.xxxx, r4.xyzx
dp3 r5.x, c46.xyzx, r1.xyzx
dp3 r5.y, c46.xyzx, c46.xyzx
mad r5.y, -r5.x, r5.x, r5.y
add r5.y, r5.y, c46.w
max r5.y, r5.y, c103.y
rsq r5.z, r5.y
add r6.x, r2.x, -r5.x
mul r6.x, r6.x, r5.z
mul r6.y, -r5.x, r5.z
abs r8.x, r6.x
add r8.y, r8.x, -c100.x
rcp r8.z, r8.x
cmp r8.z, r8.y, r8.z, r8.x
mul r8.w, r8.z, r8.z
mad r7.x, r8.w, c102.x, c101.w
mad r7.x, r8.w, r7.x, c101.z
mad r7.x, r8.w, r7.x, c101.y
mad r7.x, r8.w, r7.x, c101.x
mul r7.x, r7.x, r8.z
add r8.w, c102.y, -r7.x
cmp r7.x, r8.y, r8.w, r7.x
cmp r8.w, r6.x, c100.x, c100.w
mul r7.x, r7.x, r8.w
abs r9.x, r6.y
add r9.y, r9.x, -c100.x
rcp r9.z, r9.x
cmp r9.z, r9.y, r9.z, r9.x
mul r9.w, r9.z, r9.z
mad r7.y, r9.w, c102.x, c101.w
mad r7.y, r9.w, r7.y, c101.z
mad r7.y, r9.w, r7.y, c101.y
mad r7.y, r9.w, r7.y, c101.x
mul r7.y, r7.y, r9.z
add r9.w, c102.y, -r7.y
cmp r7.y, r9.y, r9.w, r7.y
cmp r9.w, r6.y, c100.x, c100.w
mul r7.y, r7.y, r9.w
add r7.x, r7.x, -r7.y
mul r7.x, r7.x, r5.z
mad r4.xyz, c47.xyzx, r7.xxxx, r4.xyzx
texld r10.xyzw, v0.xyxx, s0.xyzw
; hdr+: min r10.xyz, r10.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r10.x, r10.xyzx, c104.xyzx
add_sat r10.x, c100.x, -r10.x
pow r10.x, r10.x, c49.w
mul r10.x, r10.x, c49.z
mul r4.xyz, r4.xyzx, r10.xxxx
mov r4.w, c100.x
mov oC0.xyzw, r4.xyzw
