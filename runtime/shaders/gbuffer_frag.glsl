// Our G-buffer fragment programs (decision 0026), included by gbuffer_*.frag: what the game's main
// G-buffer fragment programs compute, written out once, with their interface. The game has one
// program per combination of these switches, defined before the include:
//   NORMAL_MAP  the surface normal from a normal map (texture slot 1: x, y the normal, w gloss);
//               without it, the vertex normal, and slot 1's w is still the gloss
//   CUTOUT      alpha test: pixels where texture slot 0's alpha is below the material's threshold
//               (passed by the vertex program in v_uv.z) are discarded
//   PLAIN       no material at all: only the normal; target 2's w is 0
// Inputs: constant buffers 1 (view), 7 (material), 8 (scene). Outputs, three colour targets:
//   target 0: screen-space motion (this frame's position minus last frame's), depth
//   target 1: zero
//   target 2: the view-space normal packed into 0..1, and a view-dependent lighting term
// Every operation is in the game's order, with fma where the game's program fuses, so the result is
// bit-identical to it.

layout(set = 1, binding = 1, std140) uniform View { vec4 d[4096]; } view;
#ifndef PLAIN
layout(set = 1, binding = 7, std140) uniform Material { vec4 d[4096]; } material;
layout(set = 1, binding = 8, std140) uniform Scene { vec4 d[4096]; } scene;
layout(set = 1, binding = 17) uniform sampler2D normal_map;
#endif
#ifdef CUTOUT
layout(set = 1, binding = 16) uniform sampler2D colour_map;
#endif

layout(location = 3) in vec4 v_uv;      // x, y: texture coordinates; z: the alpha test's threshold
layout(location = 4) in vec4 v_normal;
layout(location = 5) in vec4 v_world;
layout(location = 6) in vec4 v_screen;
layout(location = 14) in vec4 v_last;
#ifdef NORMAL_MAP
layout(location = 15) in vec4 v_tangent;  // w: the bitangent's sign
#endif

layout(location = 0) out vec4 o_motion;
layout(location = 1) out vec4 o_zero;
layout(location = 2) out vec4 o_normal;

void main() {
#ifdef CUTOUT
    if (clamp(texture(colour_map, v_uv.xy), 0.0, 1.0).w < v_uv.z) discard;
#endif
    vec3 n = v_normal.xyz * inversesqrt(dot(v_normal.xyz, v_normal.xyz));
#ifdef NORMAL_MAP
    vec4 tex = texture(normal_map, v_uv.xy);
    vec4 t = v_tangent * inversesqrt(dot(v_tangent, v_tangent));  // all four components, as the game does
    vec3 b = fma(n.yzx, t.zxy, -(n.zxy * t.yzx)) * t.w;  // cross(n, t) x the sign

    // the normal map's x and y, scaled by the material's bump strength; z = 1
    float bump = material.d[6].y;
    vec3 d = vec3((tex.xy - 0.5) * bump * 2.0, 1.0);
    d = d * inversesqrt(dot(d, d));
    d = fma(fma(d, vec3(0.5), vec3(0.5)), vec3(2.0), vec3(-1.0));  // the game packs and unpacks once
    d.xy *= bump;
    vec3 nw = fma(n, vec3(d.z), fma(t.xyz, vec3(d.x), b * d.y));
    nw *= inversesqrt(dot(nw, nw));
#else
    vec3 nw = n;
#endif

    // to view space (view rows 0-2)
    vec3 nv = vec3(dot(nw, view.d[0].xyz), dot(nw, view.d[1].xyz), dot(view.d[2].xyz, nw));
    nv *= inversesqrt(dot(nv, nv));

#ifdef PLAIN
    o_normal = vec4(fma(nv, vec3(0.5), vec3(0.5)), 0.0);
#else
#ifndef NORMAL_MAP
    vec4 tex = vec4(0.0, 0.0, 0.0, texture(normal_map, v_uv.xy).w);  // only the gloss
#endif
    // view-dependent term from the eye direction (scene d0: the camera position) and the material
    vec3 eye = scene.d[0].xyz - v_world.xyz;
    eye *= inversesqrt(dot(eye, eye));
    float gloss = 1.0 - tex.w;
    float m = min(material.d[2].x, gloss);
    float range = clamp(material.d[2].y - m, 0.0, 1.0);
    float facing = clamp(fma(-dot(nw, eye) - m, 1.0 / range, 1.0 / range), 0.0, 1.0);
    float term = fma(tex.w * material.d[2].z * facing, material.d[4].z, gloss);
    o_normal = vec4(fma(nv, vec3(0.5), vec3(0.5)), term * material.d[4].w);
#endif

    o_zero = vec4(0.0);
    o_motion = vec4(fma(-v_last.xy, vec2(1.0 / v_last.w), v_screen.xy * gl_FragCoord.w), gl_FragCoord.z, 0.0);
}
