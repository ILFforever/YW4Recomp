// Skinned shadow casters (decision 0024): as the game's 71faac..., up to 8 bones per vertex.
// Weights: eight 16-bit numbers in four 32-bit words (low half, high half); bone indices: eight
// bytes in four 16-bit words, paired with the weights the same way. Bones 5-8 only when the
// fifth weight isn't 0. The blended 3x4 matrix moves the vertex (weights sum to 65535), then
// projection x view x world (push constants) as in shadow_static.vert. The order of the
// multiply-adds is the game's: high half of word 0 first, then low 0, low 1, high 1; then low
// 2, high 2, low 3, high 3.
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in uvec4 weights;
layout(location = 2) in uvec4 bones;

layout(set = 0, binding = 0, std140) uniform Palette {
    vec4 row[3 * 256];  // the submesh's bones, 3x4 each
} palette;

layout(push_constant) uniform Draw {
    vec4 row[4];  // projection x view x world, row-major
} draw;

void main() {
    uint b0 = bones.x & 255u, b1 = bones.x >> 8, b2 = bones.y & 255u, b3 = bones.y >> 8;
    float w0 = float(weights.x & 65535u), w1 = float(weights.x >> 16);
    float w2 = float(weights.y & 65535u), w3 = float(weights.y >> 16);
    vec4 m[3];
    for (int r = 0; r < 3; r++) {
        vec4 s = w1 * palette.row[b1 * 3u + uint(r)];
        s = fma(vec4(w0), palette.row[b0 * 3u + uint(r)], s);
        s = fma(vec4(w2), palette.row[b2 * 3u + uint(r)], s);
        m[r] = fma(palette.row[b3 * 3u + uint(r)], vec4(w3), s);
    }
    float w4 = float(weights.z & 65535u);
    if (w4 > 0.0) {
        uint b4 = bones.z & 255u, b5 = bones.z >> 8, b6 = bones.w & 255u, b7 = bones.w >> 8;
        float w5 = float(weights.z >> 16), w6 = float(weights.w & 65535u), w7 = float(weights.w >> 16);
        for (int r = 0; r < 3; r++) {
            vec4 s = fma(palette.row[b4 * 3u + uint(r)], vec4(w4), m[r]);
            s = fma(vec4(w5), palette.row[b5 * 3u + uint(r)], s);
            s = fma(palette.row[b6 * 3u + uint(r)], vec4(w6), s);
            m[r] = fma(palette.row[b7 * 3u + uint(r)], vec4(w7), s);
        }
    }
    const float unit = 1.525902190e-05;  // 1 / 65535
    vec4 v = vec4(dot(position * m[0], vec4(unit)), dot(position * m[1], vec4(unit)),
                  dot(position * m[2], vec4(unit)), position.w);
    gl_Position = vec4(dot(v, draw.row[0]), dot(v, draw.row[1]), dot(v, draw.row[2]), dot(draw.row[3], v));
}
