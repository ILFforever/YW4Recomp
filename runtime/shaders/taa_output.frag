#version 460
#extension GL_GOOGLE_include_directive : require
// The anti-aliasing's last pass: a result in the perceptual curve (TAA's blend, DLSS's output, or the encoded scene
// for OFF / FXAA / SMAA) written back to the game's target in its linear colour, sharpened by params.x (the
// SHARPENING setting, 0 off .. 1 max; sharpen.glsl, decision 0076).
#include "aa.glsl"
#include "sharpen.glsl"
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
    vec3 sharp = sharpen(c, at(p + ivec2(0, -1)), at(p + ivec2(0, 1)), at(p + ivec2(-1, 0)), at(p + ivec2(1, 0)),
                         aa.params.x);
    o_color = vec4(aa_decode(sharp), 1.0);
}
