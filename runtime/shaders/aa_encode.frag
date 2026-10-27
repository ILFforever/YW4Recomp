#version 460
#extension GL_GOOGLE_include_directive : require
// The game's scene, texel for texel, in the perceptual curve (aa.glsl): the input of FXAA and SMAA.
#include "aa.glsl"
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(aa_encode(texelFetch(scene, ivec2(gl_FragCoord.xy), 0).rgb), 1.0); }
