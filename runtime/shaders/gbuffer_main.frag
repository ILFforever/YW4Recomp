#version 460
// Our version of the game's G-buffer fragment program a93492 (decision 0026): gbuffer_frag.glsl with
// the normal map.
#extension GL_GOOGLE_include_directive : require
#define NORMAL_MAP
#include "gbuffer_frag.glsl"
