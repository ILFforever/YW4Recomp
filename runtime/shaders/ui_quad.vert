#version 450
// A see-through picture blended onto the window (window.c: the performance HUD): two triangles over the
// rectangle rect (x0, y0, x1, y1 in clip space), its texture read from (0, 0) to uv (the drawn part).
layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv;
} pc;
layout(location = 0) out vec2 v_uv;

void main() {
    const vec2 corner[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 t = corner[gl_VertexIndex];
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, t), 0.0, 1.0);
    v_uv = t * pc.uv.xy;
}
