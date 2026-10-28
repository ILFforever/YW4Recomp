#version 450
// Distance blur, step 3 (gpu_post.c, decision 0038): the blurred image over the scene by depth. The depth (slot 1)
// becomes the view distance (near far / ((near - far) d + far)), then a blur amount 0..1 that grows past the
// focus: pc.c[0] = (near far, near - far, near - focus start, 1 / focus range; the panel's half setting halves
// .w), pc.c[1] = two thresholds and their reciprocals; pc.c[5].y = the camera's far distance (its camera block,
// row 3 .y). Where the centre is under the second threshold, the smallest amount of its 4 diagonal neighbours
// can take over (the edges of near objects). The scene keeps (1 - alpha); pixels that would keep almost all of
// it are discarded. Same operations in the same order as the game's program.

layout(push_constant) uniform Push { vec4 c[8]; } pc;
layout(set = 0, binding = 0) uniform sampler2D blurred;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(location = 3) in vec4 t0;
layout(location = 4) in vec4 t1;
layout(location = 5) in vec4 t2;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 c0 = pc.c[0], c1 = pc.c[1];
    float far = pc.c[5].y;
    float a = fma(c0.y, texture(depth, t0.xy).x, far);
    a = 1.0 / a;
    a = fma(c0.x, a, c0.z);
    a = clamp(a * c0.w, 0.0, 1.0);
    float amount = a;
    if (a < c1.y) {
        vec4 d = vec4(texture(depth, t1.xy).x, texture(depth, t1.zw).x, texture(depth, t2.xy).x,
                      texture(depth, t2.zw).x);
        d = fma(vec4(c0.y), d, vec4(far));
        d = vec4(1.0 / d.x, 1.0 / d.y, 1.0 / d.z, 1.0 / d.w);
        d = fma(vec4(c0.x), d, vec4(c0.z));
        d = clamp(d * c0.w, 0.0, 1.0);
        vec2 m2 = min(d.zw, d.xy);
        float m = min(m2.y, m2.x);
        float take = c1.x >= m ? 1.0 : 0.0;
        amount = fma(take, m - a, a);
    }
    vec2 past = vec2(amount >= c1.x ? 1.0 : 0.0, amount >= c1.y ? 1.0 : 0.0);
    vec2 scaled = clamp(c1.zw * vec2(amount, 1.0 - amount), 0.0, 1.0);
    float x = fma(past.x, -scaled.x, past.x);
    float z = x + scaled.x;
    float keep = fma(past.y, scaled.y - z, z);
    if (-keep < -0.999000013) discard;
    o_color = vec4(texture(blurred, t0.xy).rgb, 1.0 - keep);
}
