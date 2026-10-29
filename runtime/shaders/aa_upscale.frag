#version 460
// Upscaling without FSR (runtime/src/gpu_aa.c): the anti-aliased 3D, encoded at the render size, stretched over
// the output-sized scene image (bilinear, across the whole image) and decoded back to the game's linear colour.
#extension GL_GOOGLE_include_directive : require
#include "aa.glsl"
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(aa_decode(texture(scene, v_uv).rgb), 1.0); }
