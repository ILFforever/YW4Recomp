// Instanced cut-out shadow casters (decision 0024): the copies as shadow_instanced.vert, the
// texture coordinates and threshold as shadow_cutout.vert.
#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 uv;

struct Copy {
    vec4 row[4];  // projection x view x copy x world, row-major
    vec4 extra;   // x: fade
};
layout(set = 1, binding = 0, std430) readonly buffer Copies {
    Copy copy[];
} copies;

layout(push_constant) uniform Draw {
    float threshold;  // the material's: vertex constant buffer 3, byte 12
} draw;

layout(location = 3) out vec4 cut;
layout(location = 4) flat out float fade;

void main() {
    Copy c = copies.copy[gl_InstanceIndex];
    vec4 v = vec4(position, 1.0);
    gl_Position = vec4(dot(c.row[0], v), dot(c.row[1], v), dot(c.row[2], v), dot(c.row[3], v));
    cut = vec4(uv.xy, draw.threshold, 1.0);
    fade = c.extra.x;
}
