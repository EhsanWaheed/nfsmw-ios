ps_3_0
; G2 vt route (run 13): local luminance s for the grade's curve. Our own program.
; s0 = GradeInput (linear, frame size) -> GradeLuma (frame/4): 4 bilinear taps at +-1 source texel average the 4x4
; block; out = Rec.709 luma in every channel. c2 SrcTexelSize0 = (1/w, 1/h, w, h).
def c90, 0.2126, 0.7152, 0.0722, 0.25
def c91, 1, -1, 0, 0
; hdr+: def c220 = 1.0, 0.0, 0.0, 0.0   ; HDR constants: one, zero
dcl_texcoord0 v0
dcl_2d s0
mad r0.xyzw, c2.xyxy, c91.yyxy, v0.xyxy
mad r1.xyzw, c2.xyxy, c91.yxxx, v0.xyxy
texld r2.xyzw, r0.xyyy, s0.xyzw
; hdr+: min r2.xyz, r2.xyzz, c220.xxxx   ; statistics read min(frame, 1)
texld r3.xyzw, r0.zwww, s0.xyzw
; hdr+: min r3.xyz, r3.xyzz, c220.xxxx   ; statistics read min(frame, 1)
texld r4.xyzw, r1.xyyy, s0.xyzw
; hdr+: min r4.xyz, r4.xyzz, c220.xxxx   ; statistics read min(frame, 1)
texld r5.xyzw, r1.zwww, s0.xyzw
; hdr+: min r5.xyz, r5.xyzz, c220.xxxx   ; statistics read min(frame, 1)
add r2.xyzw, r2.xyzw, r3.xyzw
add r2.xyzw, r2.xyzw, r4.xyzw
add r2.xyzw, r2.xyzw, r5.xyzw
dp3 r2.x, r2.xyzz, c90.xyzz
mul r2.xyzw, r2.xxxx, c90.wwww
mov oC0.xyzw, r2.xyzw
