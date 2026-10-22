// Cut-out shadow casters (decision 0024): the position as shadow_static.vert; the texture
// coordinates and the material's alpha threshold go to the fragment stage, as the game's
// 744205... hands them to its fragment shader dceb89... (attribute 3: uv, threshold, 1).
#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 uv;

layout(push_constant) uniform Draw {
    vec4 row[4];      // projection x view x world, row-major
    float threshold;  // the material's: vertex constant buffer 3, byte 12
} draw;

layout(location = 3) out vec4 cut;

void main() {
    vec4 v = vec4(position, 1.0);
    gl_Position = vec4(dot(draw.row[0], v), dot(draw.row[1], v), dot(draw.row[2], v), dot(draw.row[3], v));
    cut = vec4(uv.xy, draw.threshold, 1.0);
}
