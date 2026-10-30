#version 460
// Our version of the shadow mask's fragment program 06d03ad7 (docs/frame.md "Lighting stage"): per
// half-size pixel, how much of the sun reaches the surface there, from the cascaded shadow maps.
// Same interface as the game's program (with the soft shadows of decision 0025 in it, as the
// translated one has them). Every operation is in the game's order (fma where it fuses), so the
// result is bit-identical to it; names are what the values seem to be.
//
// Inputs:
//   constants 8 (view: rows 20-23 the inverse projection, row 31 .zw the tap spread in map units),
//   12 (sun shadow: row 0 the light direction in view space, row 2 the depth bias (x constant, y
//   slope, z largest), row 3 .xy near and far for the fade, row 4+c .xy cascade c's tap spread,
//   row 7+c the cascade's range (.y its far end, row 8+c .x the next one's start), rows 10+4c..12+4c
//   view to light view, rows 22+4c..25+4c light view to map, rows 34+4c..37+4c view to map for the
//   centre lookup and the texel grid)
//   textures: 0 the G-buffer normal (rgb × 2 − 1), 1 the half-size depth, 15 the cascades' shadow
//   maps (a depth compare; its raw depth for the soft shadows' blocker search)
// Output: x lit (1) or in shadow (0), faded out toward the far end; y and z the linear depth; w 0.
// With no cascade at this depth: (0, 1, 0, 0).

layout(set = 1, binding = 8, std140) uniform View { vec4 d[4096]; } view;
layout(set = 1, binding = 12, std140) uniform Sun { vec4 d[4096]; } sun;
layout(set = 1, binding = 16) uniform sampler2D gbuffer_normal;
layout(set = 1, binding = 17) uniform sampler2D half_depth;
layout(set = 1, binding = 31) uniform sampler2DArrayShadow shadow_maps;
layout(set = 1, binding = 111) uniform sampler2DArray shadow_depth;  // the same maps' raw depth
layout(set = 1, binding = 95, std140) uniform YW4PCSS { vec4 mode; vec4 scale; vec4 tune; } yw4_pcss;

layout(location = 3) in vec4 v_uv;

layout(location = 0) out vec4 o_mask;

// yw4_shadow: one shadow map compare, soft (PCSS) or the game's. The same text recomp/shader.py
// (PCSS_COMMON, pcss_helper("SHADOWARRAY2D")) puts into the translated program: keep the two alike. Ours only:
// mode.y bit 1 searches with every other tap (the panel's SOFT SHADOW SEARCH).
const vec2 YW4_DISK[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));
vec2 yw4_seen_key[2];
float yw4_seen_lit[2];
int yw4_seen_n = 0;
float yw4_search_layer = -1.0, yw4_search_r = -1.0;
mat2 yw4_pcss_rotation() {
    vec2 cell = floor(gl_FragCoord.xy / max(yw4_pcss.tune.w, 1.0));
    float a = 6.2831853 * fract(52.9829189 * fract(dot(cell, vec2(0.06711056, 0.00583715))));
    return mat2(cos(a), sin(a), -sin(a), cos(a));
}
float yw4_pcss_remember(vec2 key, float lit) {
    if (yw4_seen_n < 2) {
        yw4_seen_key[yw4_seen_n] = key;
        yw4_seen_lit[yw4_seen_n] = lit;
        yw4_seen_n++;
    }
    return lit;
}
float yw4_shadow(sampler2DArrayShadow s, sampler2DArray d, vec4 c) {
    if (yw4_pcss.mode.x < 0.5) return texture(s, c);
    if (yw4_pcss.tune.y == 3.0) return texture(s, vec4(c.xy, c.z, c.w));
    if (yw4_pcss.tune.y == 4.0) return textureLod(d, vec3(c.xy, c.z), 0.0).r;
    if (yw4_pcss.tune.y == 5.0) return c.w > textureLod(d, vec3(c.xy, c.z), 0.0).r ? 1.0 : 0.0;
    if (yw4_pcss.tune.y == 6.0) return c.w;
    for (int i = 0; i < yw4_seen_n; i++)
        if (yw4_seen_key[i] == c.zw) return yw4_seen_lit[i];
    mat2 rot = yw4_pcss_rotation();
    float r;
    if (yw4_pcss.tune.z > 0.5 && yw4_search_layer == c.z) {
        if (yw4_search_r < 0.0) return yw4_pcss_remember(c.zw, texture(s, vec4(c.xy, c.z, c.w)));
        r = yw4_search_r;
    } else {
        float gaps = 0.0, casters = 0.0;
        int step = (int(yw4_pcss.mode.y) & 2) != 0 ? 2 : 1;  // ours, faster: every other tap
        for (int i = 0; i < 16; i += step) {
            float z = textureLod(d, vec3(c.xy + rot * YW4_DISK[i] * yw4_pcss.mode.z, c.z), 0.0).r;
            float gap = c.w - z;
            if (gap > yw4_pcss.tune.x) { gaps += gap; casters += 1.0; }
        }
        if (yw4_pcss.tune.y == 1.0) return casters * (float(step) / 16.0);
        yw4_search_layer = c.z, yw4_search_r = -1.0;
        if (casters == 0.0) return yw4_pcss_remember(c.zw, texture(s, vec4(c.xy, c.z, c.w)));
        r = clamp(gaps / casters * yw4_pcss.scale[min(int(c.z + 0.5), 3)], yw4_pcss.mode.w, yw4_pcss.mode.z);
        yw4_search_r = r;
    }
    if (yw4_pcss.tune.y == 2.0) return r / yw4_pcss.mode.z;
    float lit = 0.0;
    for (int i = 0; i < 16; i++)
        lit += texture(s, vec4(c.xy + rot * YW4_DISK[i] * r, c.z, c.w));
    return yw4_pcss_remember(c.zw, lit * (1.0 / 16.0));
}

// The sign of t as -1, 0 or 1, made the game's way (two compares as integers), so that 0 and -0 give +0.
int sign_of(float t) {
    return int(t > 0.0) - int(t < 0.0);
}

// A tap point beside the surface in light view, slid along the light's depth (z) back onto the
// plane through `at` with normal `n` (both in light view); `s` is the sign that orients the normal.
// Where n's part along the slide is 0 the result is NaN, as in the game.
vec3 onto_plane(vec3 tap, vec3 at, vec3 n, float s) {
    vec3 sn = s * n;
    float d = (dot(at, sn) + -dot(sn, tap)) / -(-s * sn.z);
    return fma(vec3(-s * d), vec3(0.0, 0.0, -1.0), tap);
}

// A light-view point in cascade c's map: (u, v, depth).
vec3 to_map(vec4 p, int c) {
    float w = dot(sun.d[25 + 4 * c], p);
    vec2 uv = vec2(dot(sun.d[22 + 4 * c], p), -dot(sun.d[23 + 4 * c], p)) + w;
    return vec3(uv * 0.5, dot(sun.d[24 + 4 * c], p)) / w;
}

// The view-space position in cascade c's map through rows 34-37: (u, v, depth).
vec3 to_grid(vec4 pos, int c) {
    float w = dot(pos, sun.d[37 + 4 * c]);
    vec2 uv = w + vec2(dot(pos, sun.d[34 + 4 * c]), -dot(pos, sun.d[35 + 4 * c]));
    return vec3(uv * 0.5, dot(pos, sun.d[36 + 4 * c])) / w;
}

// How far `uv` is from the centre of its texel in the 1024 grid (the larger axis).
float texel_offset(vec2 uv) {
    vec2 o = fma(floor(uv * 1024.0) + 0.5, vec2(0.0009765625), -uv);
    return max(abs(o.y), abs(o.x));
}

// The depth bias: grows with the distance from the texel centre over the surface's slope to the light.
float bias_of(float slope_distance, float n_dot_l) {
    vec3 b = sun.d[2].xyz;
    return min(fma(b.y, clamp(slope_distance / n_dot_l, 0.0, 1.0), b.x), b.z);
}

void main() {
    vec2 uv = v_uv.xy;

    // 1. Position: the half-size depth back through the inverse projection.
    float depth = textureLod(half_depth, uv, 0.0).x;
    vec4 clip = vec4(fma(v_uv.xy, vec2(2.0, -2.0), vec2(-1.0, 1.0)), depth, 1.0);
    float y = dot(clip, view.d[21]);
    float z = dot(view.d[22], clip);
    float w = dot(clip, view.d[23]);
    float x = dot(clip, view.d[20]);
    vec3 pos = vec3(x, y, z) / w;

    // 2. Cascade: the first whose range reaches this depth; toward the end of cascades 0 and 1 a
    // blend weight toward the next one.
    int cascade = -1, next = -1;
    float blend = 0.0;
    for (int c = 0; c < 3; c++) {
        float far_end = sun.d[7 + c].y;
        if (far_end >= -pos.z) {
            if (c < 2) {
                float next_start = sun.d[8 + c].x;
                blend = clamp((-pos.z + -next_start) / (-next_start + far_end), 0.0, 1.0);
                next = c + 1;
            }
            cascade = c;
            break;
        }
    }
    // (an if, not an early return, as the game has it: lookups take their level of detail from the
    // neighbouring pixels, which must still be running)
    if (cascade < 0) {
        o_mask = vec4(0.0, 1.0, 0.0, 0.0);
    } else {

        // 3. Normal and N·L; a surface facing away from the sun counts as lit.
        vec3 n = fma(textureLod(gbuffer_normal, uv, 0.0).xyz, vec3(2.0), vec3(-1.0));
        n = inversesqrt(abs(dot(n, n))) * n;
        float n_dot_l = dot(n, sun.d[0].xyz);
        float facing_away = -n_dot_l >= -9.999999750e-05 ? 1.0 : 0.0;

        // 4. Five lookups in the cascade: four taps beside the surface along the light's x and y,
        // kept on its plane, and the centre.
        int c = cascade;
        float layer = float(c);
        vec4 p = vec4(pos, 1.0);
        vec4 r10 = sun.d[10 + 4 * c], r11 = sun.d[11 + 4 * c], r12 = sun.d[12 + 4 * c];
        vec3 at = vec3(dot(p, r10), dot(p, r11), dot(p, r12));  // the surface in light view
        vec3 ln = vec3(dot(n, r10.xyz), dot(n, r11.xyz), dot(r12.xyz, n));
        ln = inversesqrt(abs(dot(ln, ln))) * ln;  // its normal in light view
        vec2 spread = view.d[31].zw * sun.d[4 + c].xy;
        int sx = sign_of(ln.x * -spread.x), sy = sign_of(ln.y * -spread.y);

        vec3 centre = to_grid(p, c);
        float inv_sin = inversesqrt(abs(fma(-n_dot_l, n_dot_l, 1.0)));
        float bias = bias_of(texel_offset(centre.xy) / inv_sin, n_dot_l);
        float shadow;
        if (yw4_pcss.mode.x >= 0.5 && (int(yw4_pcss.mode.y) & 1) != 0) {
            // ours, faster: with soft shadows on, one soft lookup at the centre. Its 16 taps spread far wider
            // than the game's five points (less than a texel apart), so the five only cost four more filters.
            shadow = max(yw4_shadow(shadow_maps, shadow_depth, vec4(centre.xy, layer, centre.z + -bias)), facing_away);
        } else {
            vec3 right = onto_plane(at + vec3(spread.x, 0.0, 0.0), at, ln, float(-sx));
            vec3 left = onto_plane(at + vec3(-spread.x, 0.0, 0.0), at, ln, float(sx));
            vec3 up = onto_plane(at + vec3(0.0, spread.y, 0.0), at, ln, float(-sy));
            vec3 down = onto_plane(fma(vec3(0.0, spread.y, 0.0), vec3(1.0, -1.0, 1.0), at), at, ln, float(sy));
            vec3 right_map = to_map(vec4(right, 1.0), c);
            vec3 left_map = to_map(vec4(left, 1.0), c);
            vec3 up_map = to_map(vec4(up, 1.0), c);
            vec3 down_map = to_map(vec4(down, 1.0), c);

            // in the game's order: the soft shadows remember earlier lookups of this pixel
            float lit_left = yw4_shadow(shadow_maps, shadow_depth, vec4(left_map.xy, layer, left_map.z + -bias));
            float lit_right = yw4_shadow(shadow_maps, shadow_depth, vec4(right_map.xy, layer, right_map.z + -bias));
            float lit_centre = yw4_shadow(shadow_maps, shadow_depth, vec4(centre.xy, layer, centre.z + -bias));
            float lit_up = yw4_shadow(shadow_maps, shadow_depth, vec4(up_map.xy, layer, up_map.z + -bias));
            float lit_down = yw4_shadow(shadow_maps, shadow_depth, vec4(down_map.xy, layer, down_map.z + -bias));
            float sum = lit_down + (lit_up + (lit_left + (lit_right + lit_centre)));
            shadow = max(sum * 0.199951172, facing_away);  // the average (0.2 in half precision)
        }

        // 5. Toward the next cascade: one centre lookup there, mixed in by the blend weight.
        if (next > 0) {
            vec3 g = to_grid(p, next);
            float b = bias_of((1.0 / inv_sin) * texel_offset(g.xy), n_dot_l);
            float lit_next = max(yw4_shadow(shadow_maps, shadow_depth, vec4(g.xy, float(next), g.z + -b)), facing_away);
            shadow = fma(blend, -shadow + lit_next, shadow);
        }

        // 6. Fade out toward the far end, by the linear depth.
        vec2 range = sun.d[3].xy;
        float lin = range.x / fma(-depth, -range.x + range.y, range.y);
        float s = clamp(lin, 0.0, 1.0);
        o_mask = vec4(fma(-s * s, shadow, shadow), lin, lin, 0.0);
    }
}
