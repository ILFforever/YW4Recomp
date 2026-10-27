#version 460
#extension GL_GOOGLE_include_directive : require
// Anti-aliasing self-test (YW4_AA_DEBUG=3, runtime/src/gpu_aa.c): in place of the scene, hard-edged stripes at
// shallow angles (about 5 degrees in the upper half, 20 in the lower), grey on black, already in the perceptual
// curve. Working SMAA or FXAA turns their staircases into ramps.
#include "aa.glsl"
layout(location = 0) out vec4 o_color;

void main() {
    vec2 p = gl_FragCoord.xy;
    float slope = p.y < 0.5 * aa.metrics.w ? 0.09 : 0.36;
    o_color = vec4(vec3(step(0.5, fract((p.y - p.x * slope) / 64.0)) * 0.7), 1.0);
}
