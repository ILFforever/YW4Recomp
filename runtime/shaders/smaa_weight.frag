#version 460
#extension GL_GOOGLE_include_directive : require
// SMAA pass 2: blending weights from the edges' shapes (area and search lookup textures).
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"
layout(set = 0, binding = 1) uniform sampler2D edgesTex;
layout(set = 0, binding = 2) uniform sampler2D areaTex;
layout(set = 0, binding = 3) uniform sampler2D searchTex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pix;
layout(location = 2) in vec4 v_off[3];
layout(location = 0) out vec4 o_weights;

void main() { o_weights = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off, edgesTex, areaTex, searchTex, vec4(0.0)); }
