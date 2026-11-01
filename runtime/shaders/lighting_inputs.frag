// The engine's lighting inputs (game program 790774, f_7f4c90, with the full-screen quad 98c09b): at half
// size, the shadow mask and two more factors packed into one target, and the normals unpacked into a
// second, each with the linear depth. Read by the full-screen lighting pass. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 16) uniform sampler2D mask;     // texture 0: the shadow mask (x shadow, y linear depth)
layout(set = 1, binding = 17) uniform sampler2D factor1;  // texture 1 (1x1 in the town by day)
layout(set = 1, binding = 18) uniform sampler2D factor2;  // texture 2 (1x1 in the town by day)
layout(set = 1, binding = 19) uniform sampler2D normals;  // texture 3: the half-size G-buffer normals (0..1)

layout(location = 3) in vec4 uv;

layout(location = 0) out vec4 o_factors;  // shadow, factor 1, factor 2, linear depth
layout(location = 1) out vec4 o_normal;   // normal in -1..1, linear depth

void main() {
    vec2 m = texture(mask, uv.xy).xy;
    vec3 n = texture(normals, uv.xy).xyz;
    o_normal = vec4((n + -0.5) * 2.0, m.y);  // the game's operations: add -0.5, then times 2
    o_factors = vec4(m.x, texture(factor1, uv.xy).x, texture(factor2, uv.xy).x, m.y);
}
