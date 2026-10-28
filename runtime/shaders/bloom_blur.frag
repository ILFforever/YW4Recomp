#version 450
// Bloom, step 2 (gpu_post.c, decision 0037): one blur pass, across or down: 4 pairs of taps (bloom.vert)
// with the weights pc.c[1] (centre, 1, 2, 3 steps), then pc.c[2] times the same pairs (zero in the game's
// passes, added as it does). Operation order as the game's program.

layout(push_constant) uniform Push { vec4 c[4]; } pc;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 3) in vec4 tap0;
layout(location = 4) in vec4 tap1;
layout(location = 5) in vec4 tap2;
layout(location = 6) in vec4 tap3;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 w = pc.c[1], v = pc.c[2];
    vec4 p1 = texture(src, tap1.xy) + texture(src, tap1.zw);
    vec4 r = p1 * w.y;
    vec4 p0 = texture(src, tap0.xy) + texture(src, tap0.zw);
    r = fma(p0, vec4(w.x), r);
    vec4 p2 = texture(src, tap2.xy) + texture(src, tap2.zw);
    r = fma(p2, vec4(w.z), r);
    vec4 p3 = texture(src, tap3.xy) + texture(src, tap3.zw);
    r = fma(p3, vec4(w.w), r);
    r = fma(vec4(v.x), p0, r);
    r = fma(vec4(v.y), p1, r);
    r = fma(vec4(v.z), p2, r);
    o_color = fma(vec4(v.w), p3, r);
}
