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
def c37 = 6.25000000e-02, 0.00000000e+00, 0.00000000e+00, 1.00000000e+00
dcl_2d s0
mov r6.xy, c1.xyxx
mov r5.xy, c37.yyyy
mul r0.xy, c28.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c28.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c29.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c29.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c30.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c30.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c31.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c31.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c32.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c32.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c33.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c33.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c34.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c34.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c35.xyxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r0.xy, c35.zwxx, r6.xyxx
add r0.xy, v0.xyxx, r0.xyxx
texld r1.xyzw, r0.xyxx, s0.xyzw
add r5.xy, r5.xyxx, r1.xyxx
mul r5.xy, r5.xyxx, c37.xxxx
mov r5.zw, c37.yyyw
mov oC0.xyzw, r5.xyzw
