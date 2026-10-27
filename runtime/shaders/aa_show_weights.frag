#version 460
// Anti-aliasing debug view (YW4_AA_DEBUG=2, runtime/src/gpu_aa.c): SMAA's blend weights in place of the
// picture (x4, so small weights show).
layout(set = 0, binding = 4) uniform sampler2D blendTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() { o_color = vec4(textureLod(blendTex, v_uv, 0.0).rgb * 4.0, 1.0); }
