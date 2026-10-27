#version 460
#extension GL_GOOGLE_include_directive : require
// SMAA pass 1: edges from the luma of the encoded scene.
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_off[3];
layout(location = 0) out vec4 o_edges;

void main() { o_edges = vec4(SMAALumaEdgeDetectionPS(v_uv, v_off, colorTex), 0.0, 0.0); }
