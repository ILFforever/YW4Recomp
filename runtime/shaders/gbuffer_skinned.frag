#version 460
// Our version of the game's skinned G-buffer fragment program b0aa5d (decision 0026), the one its
// skinned vertex program b742aa feeds (characters). Same interface: constant buffers 3 (material)
// and 9 (the rim term's settings), the colour texture in slot 0 and a 16x16 dither texture in slot
// 18; three colour targets as the other G-buffer programs, but the normal comes in view space and
// target 1 is used:
//   target 0: screen-space motion, depth
//   target 1: x, y the vertex colour's y and x, z 7/255 (a material id?), w the rim term
//   target 2: the view-space normal packed into 0..1, w 0
// Every operation is in the game's order, so the result is bit-identical to it.

layout(set = 1, binding = 3, std140) uniform Material { vec4 d[4096]; } material;
layout(set = 1, binding = 9, std140) uniform Rim { vec4 d[4096]; } rim;
layout(set = 1, binding = 16) uniform sampler2D colour_map;
layout(set = 1, binding = 34) uniform sampler2D dither;

layout(location = 1) in vec4 v_colour;
layout(location = 3) in vec4 v_uv;       // x, y: texture coordinates; z: the alpha test's threshold; w: rim offset
layout(location = 4) in vec4 v_normal;   // view space
layout(location = 14) in vec4 v_view;    // the view-space position
layout(location = 15) in vec4 v_last;    // last frame's clip position
layout(location = 16) in vec4 v_screen;  // (clip.xy + w) / 2, clip.zw

layout(location = 0) out vec4 o_motion;
layout(location = 1) out vec4 o_extra;
layout(location = 2) out vec4 o_normal;

void main() {
    // alpha test: the texture's alpha x the material's opacity
    float alpha = texture(colour_map, v_uv.xy).w * material.d[1].w;
    if (alpha < v_uv.z) discard;

    // fading: a 16x16 ordered dither, the pixel stays where the opacity reaches the dither's value
    vec2 p = gl_FragCoord.xy * 0.0625;
    vec2 f = fract(abs(p));
    vec2 cell = vec2(p.x >= -p.x ? f.x : -f.x, p.y >= -p.y ? f.y : -f.y) * 16.0;
    float threshold = texelFetch(dither, ivec2(trunc(cell)), 0).x;
    if (!(alpha >= threshold)) discard;

    // the rim term: how much the surface faces away from the eye, scaled into 0..1
    vec4 k = rim.d[5];
    vec3 eye = (v_view * inversesqrt(dot(v_view, v_view))).xyz;  // all four components, as the game does
    float r = fma(dot(v_normal.xyz, -eye), k.x - k.y, k.y) + v_uv.w;
    o_extra = vec4(clamp(v_colour.yx, 0.0, 1.0), 0.0274509806,
                   clamp(v_colour.z * (clamp(r * k.z, 0.0, 1.0) * k.w), 0.0, 1.0));

    vec3 n = v_normal.xyz * inversesqrt(dot(v_normal.xyz, v_normal.xyz));
    o_normal = vec4(fma(n, vec3(0.5), vec3(0.5)), 0.0);
    o_motion = vec4(fma(-v_last.xy, vec2(1.0 / v_last.w), gl_FragCoord.w * v_screen.xy), gl_FragCoord.z, 0.0);
}
