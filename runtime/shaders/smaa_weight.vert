#version 460
#extension GL_GOOGLE_include_directive : require
// SMAA pass 2 (blending weights), vertex stage.
#define SMAA_INCLUDE_PS 0
#include "smaa.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_pix;
layout(location = 2) out vec4 v_off[3];

void main() {
    v_uv = smaa_fullscreen();
    SMAABlendingWeightCalculationVS(v_uv, v_pix, v_off);
}
