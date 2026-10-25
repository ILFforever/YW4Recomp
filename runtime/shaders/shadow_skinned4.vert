// Skinned shadow casters with up to 4 bones per vertex (decision 0024): as the game's 26b707...
// Weights are floats (the vertex format turns them into 0..1), bone indices whole numbers, both
// in the formats and at the offsets the game's draw had (bindings 1 and 2). The blended 3x4
// matrix moves the vertex, then projection x view x world (push constants) as in
// shadow_static.vert. The order of the multiply-adds is the game's: bone y first, then x, z, w.
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 weights;
layout(location = 2) in uvec4 bones;

layout(set = 0, binding = 0, std140) uniform Palette {
    vec4 row[3 * 256];  // the submesh's bones, 3x4 each
} palette;

layout(push_constant) uniform Draw {
    vec4 row[4];  // projection x view x world, row-major
} draw;

void main() {
    uvec4 b = bones * 3u;
    vec4 m[3];
    for (int r = 0; r < 3; r++) {
        vec4 s = weights.y * palette.row[b.y + uint(r)];
        s = fma(vec4(weights.x), palette.row[b.x + uint(r)], s);
        s = fma(vec4(weights.z), palette.row[b.z + uint(r)], s);
        m[r] = fma(palette.row[b.w + uint(r)], vec4(weights.w), s);
    }
    vec4 v = vec4(dot(position, m[0]), dot(position, m[1]), dot(position, m[2]), position.w);
    gl_Position = vec4(dot(v, draw.row[0]), dot(v, draw.row[1]), dot(v, draw.row[2]), dot(draw.row[3], v));
}
