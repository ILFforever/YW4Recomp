#version 450
// Distance blur, step 2 (gpu_post.c, decision 0038): one blur pass, across or down: 8 pairs of taps (dof.vert)
// with the Gaussian weights pc.c[3] (centre, 1-3 steps) and pc.c[4] (4-7). Each pair as the game's program sums
// it (w * the second tap, then w * the first added to that), the pairs added up in its order.

layout(push_constant) uniform Push { vec4 c[8]; } pc;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 3) in vec4 t0;
layout(location = 4) in vec4 t1;
layout(location = 5) in vec4 t2;
layout(location = 6) in vec4 t3;
layout(location = 14) in vec4 t4;
layout(location = 15) in vec4 t5;
layout(location = 16) in vec4 t6;
layout(location = 17) in vec4 t7;
layout(location = 0) out vec4 o_color;

vec4 pair(vec4 t, float w) {
    return fma(vec4(w), texture(src, t.xy), vec4(w) * texture(src, t.zw));
}

void main() {
    vec4 a = pc.c[3], b = pc.c[4];
    vec4 r = pair(t0, a.x) + pair(t1, a.y);
    r = r + pair(t2, a.z);
    r = r + pair(t3, a.w);
    r = r + pair(t4, b.x);
    vec4 r1 = r + pair(t5, b.y);
    vec4 r6 = pair(t6, b.z), r7 = pair(t7, b.w);
    o_color = (r1 + r6) + r7;
}
