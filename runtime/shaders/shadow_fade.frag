// Fading instanced shadow casters (decision 0024): as the game's b7cd6e..., drop the fragment
// where the copy's fade is below the 16x16 dither texture at the pixel (mod 16). The fade comes
// per copy (shadow_instanced.vert), decoded from the copy as the game's 9904c2... does. Depth only.
#version 450

layout(set = 0, binding = 0) uniform sampler2D dither;

layout(location = 4) flat in float fade;

void main() {
    ivec2 p = ivec2(trunc(fract(gl_FragCoord.xy * 0.0625) * 16.0));
    if (fade < texelFetch(dither, p, 0).x) discard;
}
