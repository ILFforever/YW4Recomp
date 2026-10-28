#version 450
// Bloom, step 3 (gpu_post.c, decision 0037): the three blurred levels onto the scene (blend: the scene times
// one minus our alpha, plus our colour). Each level times its brightness (pc.c[0].x, c[0].z, c[1].x,
// clamped to 0..1) is combined as a screen, 1 - (1-a)(1-b)(1-c); alpha from the brightest channel of each
// with the weights c[0].y, c[0].w, c[1].y. Operation order as the game's program.

layout(push_constant) uniform Push { vec4 c[4]; } pc;
layout(set = 0, binding = 0) uniform sampler2D level0;
layout(set = 0, binding = 1) uniform sampler2D level1;
layout(set = 0, binding = 2) uniform sampler2D level2;
layout(location = 3) in vec4 tap0;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 k0 = pc.c[0], k1 = pc.c[1];
    vec3 c = clamp(texture(level2, tap0.xy).rgb * k1.x, 0.0, 1.0);
    vec3 b = clamp(texture(level1, tap0.xy).rgb * k0.z, 0.0, 1.0);
    vec3 keep = 1.0 - b;
    float mb = max(b.z, max(b.y, b.x));
    vec3 a = clamp(texture(level0, tap0.xy).rgb * k0.x, 0.0, 1.0);
    float mc = max(c.z, max(c.y, c.x));
    keep = fma(-a, keep, keep);
    float ma = max(a.z, max(a.y, a.x));
    vec3 screen = fma(-c, keep, keep);
    float t = -ma * k0.y;
    float u = fma(-mb, k0.w, 1.0);
    u = fma(t, u, u);
    float alpha = fma(-mc * k1.y, u, u);
    o_color = vec4(1.0 - screen, 1.0 - alpha);
}
