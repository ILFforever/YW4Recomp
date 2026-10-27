#version 460
// One triangle over the whole target (anti-aliasing passes: encode, FXAA): texture coordinates 0..1,
// (0, 0) at the top-left.
layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
