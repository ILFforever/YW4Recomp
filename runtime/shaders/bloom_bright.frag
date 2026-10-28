#version 450
// Bloom, step 1 (gpu_bloom.c, decision 0037): the bright parts of the scene (in0) and of the half-size image
// in1, each over its threshold: in1 where it stands out from the scene by more than its difference allows,
// and the scene itself. pc.c[0] / c[1]: per-channel scale (rgb) and threshold (w) for in0 / in1 (renderer
// +0x800, +0x7f0). The same operations in the same order as the game's program, so the result is identical.

layout(push_constant) uniform Push { vec4 c[4]; } pc;
layout(set = 0, binding = 0) uniform sampler2D in0;
layout(set = 0, binding = 1) uniform sampler2D in1;
layout(location = 3) in vec4 tap0;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 a = texture(in1, tap0.xy), s = texture(in0, tap0.xy);
    vec3 d = a.rgb - s.rgb;
    float m = max(abs(d.z), max(abs(d.y), abs(d.x)));
    vec3 lit = max(fma(vec3(-m), vec3(1.5), a.rgb), vec3(0.0));
    vec4 from1 = vec4(fma(lit, pc.c[1].rgb, vec3(-pc.c[1].w)), a.w);
    vec4 from0 = vec4(fma(s.rgb, pc.c[0].rgb, vec3(-pc.c[0].w)), s.w);
    o_color = max(max(from1, from0), vec4(0.0));
}
