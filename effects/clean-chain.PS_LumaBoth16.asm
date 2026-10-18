ps_3_0
dcl_texcoord0 v0
def c28 = -3.75000000e-01, -3.75000000e-01, -1.25000000e-01, -3.75000000e-01
def c29 = 1.25000000e-01, -3.75000000e-01, 3.75000000e-01, -3.75000000e-01
def c30 = -3.75000000e-01, -1.25000000e-01, -1.25000000e-01, -1.25000000e-01
def c31 = 1.25000000e-01, -1.25000000e-01, 3.75000000e-01, -1.25000000e-01
def c32 = -3.75000000e-01, 1.25000000e-01, -1.25000000e-01, 1.25000000e-01
def c33 = 1.25000000e-01, 1.25000000e-01, 3.75000000e-01, 1.25000000e-01
def c34 = -3.75000000e-01, 3.75000000e-01, -1.25000000e-01, 3.75000000e-01
def c35 = 1.25000000e-01, 3.75000000e-01, 3.75000000e-01, 3.75000000e-01
def c36 = 2.12599993e-01, 7.15200007e-01, 7.22000003e-02, 6.93147182e-01
def c37 = 6.25000000e-02, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00
; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero
dcl_2d s0
mov r6.xy, c1.xyxx
mov r6.z, c4.xxxx
mov r5.xy, c37.yyyy
mul r0.xy, c28.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c28.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c29.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c29.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c30.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c30.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c31.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c31.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c32.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c32.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c33.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c33.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c34.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c34.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c35.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r0.xy, c35.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
; hdr+: min r1.xyz, r1.xyzz, c220.xxxx   ; statistics read min(frame, 1)
dp3 r2.x, r1.xyzx, c36.xyzx
max r2.y, r2.xxxx, r6.zzzz
log r2.y, r2.yyyy
mad r5.x, r2.yyyy, c36.wwww, r5.xxxx
max r2.z, r2.xxxx, c37.yyyy
mul r2.z, r2.zzzz, r2.zzzz
mul r2.z, r2.zzzz, r2.zzzz
add r5.y, r5.yyyy, r2.zzzz
mul r5.xy, r5.xyxx, c37.xxxx
mov r5.zw, c37.yyyw
mov oC0.xyzw, r5.xyzw
