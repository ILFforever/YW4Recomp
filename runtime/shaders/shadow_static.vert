// Static shadow casters (decision 0024): the game's depth-only shader b320bd... computes each
// clip coordinate as dot(row, position) with the rows of projection x view x world; so does
// this one, with the rows in push constants. No fragment stage: depth only.
#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform Draw {
    vec4 row[4];  // projection x view x world, row-major
} draw;

void main() {
    vec4 v = vec4(position, 1.0);
    gl_Position = vec4(dot(draw.row[0], v), dot(draw.row[1], v), dot(draw.row[2], v), dot(draw.row[3], v));
}
