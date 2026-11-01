// The engine's colour and depth copy (game program 95ea1b, with the full-screen quad 98c09b): a target
// drawn from two textures at the same place, colour times the quad's colour, and the depth texture's
// value as the depth. Used shrunk, for the half-size images (f_80d270) and the third-size ones
// (f_8050d0, main+0x80f988). docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 16) uniform sampler2D colour;  // texture 0
layout(set = 1, binding = 17) uniform sampler2D depth;   // texture 1

layout(location = 1) in vec4 tint;  // the quad's colour
layout(location = 3) in vec4 uv;

layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = texture(colour, uv.xy) * tint;
    gl_FragDepth = texture(depth, uv.xy).x;
}
