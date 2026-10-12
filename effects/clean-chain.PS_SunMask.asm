ps_3_0
def c90 = 2.12599993e-01, 7.15200007e-01, 7.22000003e-02, 0.00000000e+00
def c91 = 1.00000000e+00, 0.00000000e+00, 5.00000000e-01, -5.00000000e-01
; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
texld r0.xyzw, v0.xyxx, s0.xyzw
; hdr+: min r0.xyz, r0.xyzz, c220.xxxx   ; statistics read min(frame, 1)
texld r1.xyzw, v0.xyxx, s1.xyzw
add r1.x, r1.x, -c30.x
mul_sat r1.x, r1.x, c30.y
add r1.y, c14.x, c91.w
cmp r1.x, r1.y, r1.x, c91.x
dp3 r2.x, r0.xyzx, c90.xyzx
add r2.x, r2.x, -c30.z
mul_sat r2.x, r2.x, c30.w
add r3.xy, v0.xyxx, -c28.xyxx
mul r3.x, r3.x, c28.w
mul r3.z, r3.x, r3.x
mad r3.z, r3.y, r3.y, r3.z
mad_sat r3.z, r3.z, -c29.w, c91.x
mul r3.z, r3.z, r3.z
mul r1.x, r1.x, r2.x
mul r1.x, r1.x, r3.z
mul r0.xyz, r0.xyzx, r1.xxxx
mov r0.w, c91.x
mov oC0.xyzw, r0.xyzw
