// The engine's fill (game program d2aaa3, with the full-screen quad 98c09b): the quad's colour, as it is.
// docs/frame.md "Lighting stage".
#version 460

layout(location = 1) in vec4 colour;

layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = colour;
}
