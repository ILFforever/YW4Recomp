// The engine's depth-only copy (game program 6b4575, with the full-screen quad 98c09b): texture 1's value
// as the depth, no colour. After the lit pass it copies the G-buffer's depth into another full-size depth
// target. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 17) uniform sampler2D depth;  // texture 1

layout(location = 3) in vec4 uv;

void main() {
    gl_FragDepth = texture(depth, uv.xy).x;
}
