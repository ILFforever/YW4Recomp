// Instanced shadow casters (decision 0024): one draw for all the copies the game's callback kept,
// as the game draws them. Each copy's projection x view x copy x world (rows, multiplied on the
// CPU) and its fade come from the copies ring (set 1), at the draw's first instance + i; then as
// shadow_static.vert. The fade goes to the fading programs' fragment stage (1 when not fading).
#version 450

layout(location = 0) in vec3 position;

struct Copy {
    vec4 row[4];  // projection x view x copy x world, row-major
    vec4 extra;   // x: fade
};
layout(set = 1, binding = 0, std430) readonly buffer Copies {
    Copy copy[];
} copies;

layout(location = 4) flat out float fade;

void main() {
    Copy c = copies.copy[gl_InstanceIndex];
    vec4 v = vec4(position, 1.0);
    gl_Position = vec4(dot(c.row[0], v), dot(c.row[1], v), dot(c.row[2], v), dot(c.row[3], v));
    fade = c.extra.x;
}
