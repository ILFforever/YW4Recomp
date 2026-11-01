// The engine's clamped copy (game program 2571d0, with the quad abf6e5): texture 0 with negative values
// raised to 0. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 16) uniform sampler2D source;  // texture 0

layout(location = 3) in vec4 uv;

layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = max(texture(source, uv.xy), vec4(0.0));  // in the game's order: the texel first
}
