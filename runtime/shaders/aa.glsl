// Shared by the anti-aliasing pass (runtime/src/gpu_aa.c): the target's size, and the curve between the
// game's linear HDR colour and the perceptual one the edge detection and blending work on (reversible).
layout(push_constant) uniform AaPush { vec4 metrics; } aa;  // 1/w, 1/h, w, h

vec3 aa_encode(vec3 c) { c = max(c, vec3(0.0)); return sqrt(c / (1.0 + c)); }
vec3 aa_decode(vec3 e) { e = clamp(e, vec3(0.0), vec3(0.999)); e *= e; return e / (1.0 - e); }
