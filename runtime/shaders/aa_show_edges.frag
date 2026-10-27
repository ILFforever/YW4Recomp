#version 460
// Anti-aliasing debug view (YW4_AA_DEBUG=1, runtime/src/gpu_aa.c): SMAA's edges in place of the picture
// (red: an edge on the left, green: on top).
layout(set = 0, binding = 1) uniform sampler2D edgesTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(textureLod(edgesTex, v_uv, 0.0).rg, 0.0, 1.0); }
