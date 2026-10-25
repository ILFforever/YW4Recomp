#version 460
// Our version of the game's G-buffer fragment program ca3b58 (decision 0026): gbuffer_frag.glsl with
// the vertex normal and the alpha test.
#extension GL_GOOGLE_include_directive : require
#define CUTOUT
#include "gbuffer_frag.glsl"
