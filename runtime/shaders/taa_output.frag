#version 460
#extension GL_GOOGLE_include_directive : require
// The anti-aliasing's last pass: a result in the perceptual curve (TAA's blend, DLSS's output, or the encoded scene
// for OFF / FXAA / SMAA) written back to the game's target in its linear colour, sharpened by params.x (the
// SHARPENING setting, 0 off .. 1 max; decision 0076).
//
// The sharpening is contrast-adaptive, after the method AMD published as FidelityFX CAS: each pixel is pushed away
// from its four neighbours' average, by less where the 3x3 neighbourhood already has strong contrast (its darkest
// against its brightest). Soft, low-contrast detail (grass, cloth, wall textures) gets the most; edges that are
// already crisp get little, so they don't ring. The result stays within the four neighbours' range, so even then
// no bright or dark rims. (Our own code from the published idea, not AMD's.)
#include "aa.glsl"
layout(set = 0, binding = 0) uniform sampler2D result;
layout(location = 0) out vec4 o_color;

vec3 at(ivec2 p) { return texelFetch(result, clamp(p, ivec2(0), ivec2(aa.metrics.zw) - 1), 0).rgb; }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 c = at(p);
    if (aa.params.x <= 0.0) {  // SHARPENING off: a plain copy
        o_color = vec4(aa_decode(c), 1.0);
        return;
    }
    vec3 n = at(p + ivec2(0, -1)), s = at(p + ivec2(0, 1)), w = at(p + ivec2(-1, 0)), e = at(p + ivec2(1, 0));
    vec3 lo = min(c, min(min(n, s), min(w, e))), hi = max(c, max(max(n, s), max(w, e)));
    // the diagonals widen the range the contrast is judged on (CAS's "soft" min and max); the clamp stays the cross's
    vec3 nw = at(p + ivec2(-1, -1)), ne = at(p + ivec2(1, -1)), sw = at(p + ivec2(-1, 1)), se = at(p + ivec2(1, 1));
    vec3 lo9 = min(lo, min(min(nw, ne), min(sw, se))), hi9 = max(hi, max(max(nw, ne), max(sw, se)));
    // 1 where the neighbourhood is flat, toward 0 where it spans dark to bright (the curve keeps values in 0..1)
    vec3 amount = sqrt(clamp(lo9 / max(hi9, vec3(1e-4)), 0.0, 1.0));
    // the push: 2x the setting where flat (MEDIUM 1.0, MAX 2.0), less with contrast
    vec3 sharp = c + (c - 0.25 * (n + s + w + e)) * (2.0 * aa.params.x * amount);
    o_color = vec4(aa_decode(clamp(sharp, lo, hi)), 1.0);
}
