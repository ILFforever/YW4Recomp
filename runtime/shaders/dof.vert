#version 450
// Our distance blur (runtime/src/gpu_post.c, decision 0038): the game's full-screen quad, as bloom.vert, and per
// mode (pc.c[5].x) what the game's vertex programs pass on:
//   0 shrink: the texture coordinate.
//   1 blur: 8 pairs of taps, the centre twice and +-1..7 steps of pc.c[2] (each step added to the last, as the
//     game's program does), into locations 3-6 and 14-17.
//   2 composite: the centre and its 4 diagonal neighbours, pc.c[2].xy apart (one texel of the depth).

layout(push_constant) uniform Push { vec4 c[8]; } pc;
layout(location = 3) out vec4 t0;
layout(location = 4) out vec4 t1;
layout(location = 5) out vec4 t2;
layout(location = 6) out vec4 t3;
layout(location = 14) out vec4 t4;
layout(location = 15) out vec4 t5;
layout(location = 16) out vec4 t6;
layout(location = 17) out vec4 t7;

const vec2 POS[4] = vec2[4](vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
const vec2 UV[4] = vec2[4](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
const int CORNER[6] = int[6](0, 1, 2, 0, 2, 3);

void main() {
    int k = CORNER[gl_VertexIndex];
    vec2 uv = UV[k];
    gl_Position = vec4(POS[k], 0.0, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;  // as the runtime's position fix
    t0 = uv.xyxy, t1 = t2 = t3 = t4 = t5 = t6 = t7 = vec4(0.0);
    vec4 s = pc.c[2];
    if (pc.c[5].x == 1.0) {
        vec2 s1 = s.xy + s.zw, s2 = s.xy + s1, s3 = s.xy + s2, s4 = s.xy + s3, s5 = s.xy + s4, s6 = s.xy + s5,
             s7 = s.xy + s6;
        t0 = vec4(uv + s.zw, uv - s.zw);
        t1 = vec4(uv + s1, uv - s1);
        t2 = vec4(uv + s2, uv - s2);
        t3 = vec4(uv + s3, uv - s3);
        t4 = vec4(uv + s4, uv - s4);
        t5 = vec4(uv + s5, uv - s5);
        t6 = vec4(uv + s6, uv - s6);
        t7 = vec4(uv + s7, uv - s7);
    } else if (pc.c[5].x == 2.0) {
        t1 = vec4(uv + s.xy, fma(s.xy, vec2(1.0, -1.0), uv));
        t2 = vec4(fma(s.xy, vec2(-1.0, 1.0), uv), uv - s.xy);
    }
}
