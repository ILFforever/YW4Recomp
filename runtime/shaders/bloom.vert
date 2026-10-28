#version 450
// Our bloom pass (runtime/src/gpu_bloom.c, decision 0037): the game's full-screen quad, its corners and
// texture coordinates as the engine's vertex buffer holds them, drawn as triangles 0,1,2 and 0,2,3 the way
// the runtime draws the game's quads, so every pixel gets the same interpolated coordinate.
// Blur (pc.c[3].x = 1): the 8 tap positions of a pass, the centre twice and +-1, 2, 3 steps (pc.c[0].xy).

layout(push_constant) uniform Push { vec4 c[4]; } pc;
layout(location = 3) out vec4 tap0;  // centre, centre (the bright pass and composite read .xy)
layout(location = 4) out vec4 tap1;  // +1 step, -1 step
layout(location = 5) out vec4 tap2;  // +2, -2
layout(location = 6) out vec4 tap3;  // +3, -3

const vec2 POS[4] = vec2[4](vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
const vec2 UV[4] = vec2[4](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
const int CORNER[6] = int[6](0, 1, 2, 0, 2, 3);

void main() {
    int k = CORNER[gl_VertexIndex];
    vec2 uv = UV[k];
    gl_Position = vec4(POS[k], 0.0, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;  // as the runtime's position fix
    if (pc.c[3].x == 0.0) {
        tap0 = uv.xyxy, tap1 = tap2 = tap3 = vec4(0.0);
        return;
    }
    // the step's multiples as the game's program adds them: s, 2s = s + s, 3s = s + 2s
    vec4 s = pc.c[0];
    vec2 s1 = s.xy + s.zw, s2 = s.xy + s1, s3 = s.xy + s2;
    tap0 = vec4(uv + s.zw, uv - s.zw);
    tap1 = vec4(uv + s1, uv - s1);
    tap2 = vec4(uv + s2, uv - s2);
    tap3 = vec4(uv + s3, uv - s3);
}
