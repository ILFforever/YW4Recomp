// The engine's tone mapping (game program 22d015, with the quad abf6e5): the lit scene's colour through an
// extended Reinhard curve on its luminance, scaled by the eye adaptation's brightness (a storage buffer), and
// blended in by a strength that a G-buffer flag changes per pixel. Operations in the game's order (its
// fused multiply-adds, divisions and reciprocals), so the result can match it bit for bit.
// docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 0, std140) uniform Tone {
    vec4 a;  // x white point, y exposure, z strength, w strength scale where flagged
    vec4 b;  // x adaptation rate, y adaptation target
} tone;

layout(set = 1, binding = 64, std430) readonly buffer Adaptation {
    float brightness;  // the eye adaptation's current brightness
} adaptation;

layout(set = 1, binding = 16) uniform sampler2D scene;  // texture 0: the lit scene
layout(set = 1, binding = 18) uniform sampler2D flags;  // texture 2: z holds the flag in bit 0 (as /255)

layout(location = 3) in vec4 uv;

layout(location = 0) out vec4 o_colour;

void main() {
    float flagged = (int(trunc(fma(texture(flags, uv.xy).z, 255.0, 0.5))) & 1) != 0 ? 1.0 : 0.0;
    vec4 colour = max(texture(scene, uv.xy), vec4(0.0));
    float lum = dot(colour.xyz, vec3(0.298828125, 0.586425781, 0.114501953));

    float adapted = fma(tone.b.x, -adaptation.brightness + tone.b.y, adaptation.brightness) + 0.00100000005;
    float white2 = tone.a.x * tone.a.x;
    float l = (lum * tone.a.y) / adapted;
    float curve = fma(l / white2, l, l) / (l + 1.0);  // L (1 + L / white^2) / (1 + L)

    vec3 mapped = fma(colour.xxyz * curve, vec4(1.0 / (lum + 9.999999750e-05)), -colour.xxyz).yzw;  // each channel x curve / lum, minus itself
    float strength = fma(fma(tone.a.w, flagged, -flagged), tone.a.z, tone.a.z);
    o_colour.xyz = fma(vec3(strength), mapped, colour.xyz);
    o_colour.w = colour.w;
}
