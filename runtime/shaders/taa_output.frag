#version 460
#extension GL_GOOGLE_include_directive : require
// TAA's result (taa_resolve.frag, in the perceptual curve) written back to the game's target in its linear
// colour, a little sharpened: blending frames softens the picture, so each pixel is pushed away from its four
// neighbours' average (by params.x), kept within their range so edges get no bright or dark rims.
#include "aa.glsl"
layout(set = 0, binding = 0) uniform sampler2D result;
layout(location = 0) out vec4 o_color;

vec3 at(ivec2 p) { return texelFetch(result, clamp(p, ivec2(0), ivec2(aa.metrics.zw) - 1), 0).rgb; }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 c = at(p), n = at(p + ivec2(0, -1)), s = at(p + ivec2(0, 1)), w = at(p + ivec2(-1, 0)), e = at(p + ivec2(1, 0));
    vec3 lo = min(c, min(min(n, s), min(w, e))), hi = max(c, max(max(n, s), max(w, e)));
    vec3 sharp = clamp(c + (c - 0.25 * (n + s + w + e)) * aa.params.x, lo, hi);
    o_color = vec4(aa_decode(sharp), 1.0);
}
