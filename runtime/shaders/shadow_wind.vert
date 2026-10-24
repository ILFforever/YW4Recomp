// Wind-swayed shadow casters (decision 0024): as the game's fa9033... (instanced) and 61ebac...
// (not), always with the cut-out fragment program. The position after copy x world (one row set
// per copy in the copies ring; the world's alone for 61ebac) is moved by the wind in world space,
// then projection x view:
// - the wind map (vs texture 12) at the position's xz (cb6 row 47: scale, offset) gives a height
//   band (z, w) and a bend direction (x, y); s: where the vertex is in the band, -1..1;
// - the wind noise (vs texture 11) at two scales (cb7 row 3: x, z) of xz plus wind direction x
//   time (cb6 row 36: xy, w), offset by the vertex's phase (weights.z), scaled by cb7 row 3 (y, w)
//   and the vertex's weights (x, y);
// - offset = noise x strength (cb6 row 36 z x cb7 row 0 x) + bend x (1 - s^2) x weight x cb7 row 0 y.
// The operations are the game's, in its order. The draw's constants are a header entry in the ring.
#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 uv;
layout(location = 2) in vec4 weights;

struct Copy {
    vec4 row[4];
    vec4 extra;
};
layout(set = 1, binding = 0, std430) readonly buffer Copies {
    Copy copy[];
} copies;

layout(set = 0, binding = 1) uniform sampler2D noise;     // vs texture 11
layout(set = 0, binding = 2) uniform sampler2D wind_map;  // vs texture 12

layout(push_constant) uniform Draw {
    vec4 vp[4];       // projection x view, row-major
    float threshold;  // the material's: vertex constant buffer 3, byte 12
    uint header;      // the draw's constants in the ring: cb6 rows 36, 47, cb7 rows 0, 3
} draw;

layout(location = 3) out vec4 cut;

void main() {
    Copy hd = copies.copy[draw.header];
    vec4 c36 = hd.row[0], c47 = hd.row[1], c70 = hd.row[2], c73 = hd.row[3];
    Copy c = copies.copy[gl_InstanceIndex];
    vec4 v = vec4(position, 1.0);
    vec3 p = vec3(dot(c.row[0], v), dot(c.row[1], v), dot(c.row[2], v));

    vec4 m = textureLod(wind_map, (p.xz - c47.zw) * c47.xy, 0.0);
    float band = m.z - m.w;
    float s = (p.y - fma(band, 0.5, m.w)) / band * 2.0;
    vec2 q = fma(c36.xy, vec2(c36.w), p.xz);
    float sa = min(abs(s), 1.0);
    vec2 qx = fma(q, vec2(1.0 / c73.x), vec2(weights.z));
    vec2 qz = fma(q, vec2(1.0 / c73.z), vec2(weights.z));
    vec3 na = textureLod(noise, qz, 0.0).xyz - 0.5;
    vec3 nb = textureLod(noise, qx, 0.0).xyz - 0.5;
    vec3 sway = fma(nb * c73.y, vec3(weights.x), weights.y * (c73.w * na));
    float k = -sa * sa;
    vec3 bend = vec3(fma(k, m.x, m.x), 0.0, fma(k, m.y, m.y));
    bend = clamp(weights.y + weights.x, 0.0, 1.0) * bend;
    bend = c70.y * bend;
    vec4 w = vec4(fma(sway, vec3(c36.z * c70.x), bend) + p, 1.0);

    gl_Position = vec4(dot(draw.vp[0], w), dot(draw.vp[1], w), dot(draw.vp[2], w), dot(draw.vp[3], w));
    cut = vec4(uv.xy, draw.threshold, 1.0);
}
