#version 460
#extension GL_GOOGLE_include_directive : require
// SMAA pass 3: each texel blended with its neighbours by the weights, written back to the game's target in
// its linear colour.
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 4) uniform sampler2D blendTex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_off;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(aa_decode(SMAANeighborhoodBlendingPS(v_uv, v_off, colorTex, blendTex).rgb), 1.0); }
