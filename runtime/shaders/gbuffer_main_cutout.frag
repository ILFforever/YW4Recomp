#version 460
// Our version of the game's G-buffer fragment program 01fd74 (decision 0026): gbuffer_frag.glsl with
// the normal map and the alpha test.
#extension GL_GOOGLE_include_directive : require
#define NORMAL_MAP
#define CUTOUT
#include "gbuffer_frag.glsl"
