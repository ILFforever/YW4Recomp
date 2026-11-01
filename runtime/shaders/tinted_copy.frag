// The engine's tinted copy (game program 2d68c8, with the full-screen quad 98c09b): texture 0 times the
// quad's colour. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 16) uniform sampler2D source;  // texture 0

layout(location = 1) in vec4 tint;
layout(location = 3) in vec4 uv;

layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = texture(source, uv.xy) * tint;
}
