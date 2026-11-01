// The engine's lamp lights (game program 42759b, with its vertex program 198f41): point lights drawn as
// volumes at a third of the size and added up. Per pixel: the position from the depth (texture 1) through the
// camera's inverse projection, the vector to the light (its view-space position and radius from the vertex
// stage), a falloff of saturate((distance - radius) / -radius)^2 times the light's colour, and N.L with the
// normals (texture 0, 0..1). Operations in the game's order, so the result can match it bit for bit.
// docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 8, std140) uniform Camera {
    vec4 rows[32];  // rows 20..23: the inverse projection
} camera;

layout(set = 1, binding = 16) uniform sampler2D normals;  // texture 0
layout(set = 1, binding = 17) uniform sampler2D depth;    // texture 1

layout(location = 1) in vec4 colour;  // the light's
layout(location = 3) in vec4 light;   // xyz its view-space position, w its radius (as the vertex stage gives it)
layout(location = 4) in vec4 screen;  // xy the clip position mapped to 0..w

layout(location = 0) out vec4 o_light;

void main() {
    vec2 s = (screen * gl_FragCoord.wwww).xy;
    vec2 at = vec2(s.x, -s.y + 1.0);
    vec4 clip = vec4(fma(at, vec2(2.0, -2.0), vec2(-1.0, 1.0)), texture(depth, at).x, 1.0);
    float w = dot(clip, camera.rows[23]);
    vec3 pos = vec3(dot(clip, camera.rows[20]), dot(clip, camera.rows[21]), dot(camera.rows[22], clip));
    vec3 to_light = fma(-pos, vec3(1.0 / w), light.xyz);
    float inv_len = inversesqrt(abs(dot(to_light, to_light)));
    float falloff = clamp(((-light.w) + 1.0 / inv_len) / (-light.w), 0.0, 1.0);
    vec3 lit = (falloff * falloff) * colour.xyz;
    vec3 n = (texture(normals, at).xyz + -0.5) * 2.0;
    float ndl = clamp(dot(n, inv_len * to_light), 0.0, 1.0);
    o_light = vec4(ndl * lit, 1.0);
}
