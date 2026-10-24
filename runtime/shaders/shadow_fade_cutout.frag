// Fading instanced cut-out shadow casters (decision 0024): as the game's d5f536..., the cut-out
// test of shadow_cutout.frag, then the dither test of shadow_fade.frag. Depth only.
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform sampler2D dither;

layout(location = 3) in vec4 cut;
layout(location = 4) flat in float fade;

void main() {
    if (texture(tex, cut.xy).w < cut.z) discard;
    ivec2 p = ivec2(trunc(fract(gl_FragCoord.xy * 0.0625) * 16.0));
    if (fade < texelFetch(dither, p, 0).x) discard;
}
