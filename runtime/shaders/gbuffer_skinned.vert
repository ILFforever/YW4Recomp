#version 460
// Our version of the game's skinned G-buffer vertex program b742aa (decision 0026; characters),
// with its interface: constant buffers 1 (view), 2 (object: view x world in rows 4-7, projection x
// view x world in 8-11, last frame's view x world in 16-19), 3 (material), 4 (this frame's bones),
// 5 (last frame's bones), and the outputs gbuffer_skinned.frag reads.
//
// Vertices whose attribute 3's z is at most 0.1 are skinned: up to 8 bones (3x4 each), weights
// eight 16-bit numbers in attribute 4 (low half, high half of each word), bone numbers eight bytes
// in attribute 5 (two per word: low byte, next byte), plus the material's first bone (constants 3
// d0.y, added to each word as the game does). The other vertices aren't moved, and attribute 5
// holds their last frame's position. Every operation is in the game's order, so the result is
// bit-identical to it.

layout(set = 0, binding = 1, std140) uniform View { vec4 d[4096]; } view;
layout(set = 0, binding = 2, std140) uniform Object { vec4 d[4096]; } object;
layout(set = 0, binding = 3, std140) uniform Material { vec4 d[4096]; } material;
layout(set = 0, binding = 4, std140) uniform Bones { vec4 d[4096]; } bones;
layout(set = 0, binding = 5, std140) uniform LastBones { vec4 d[4096]; } last_bones;

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_colour;
layout(location = 2) in vec4 a_normal;
layout(location = 3) in vec4 a_uv;       // z: above 0.1 not skinned
layout(location = 4) in uvec4 a_weights;
layout(location = 5) in uvec4 a_bones;   // not skinned: last frame's position (float bits)

layout(location = 1) out vec4 v_colour;
layout(location = 3) out vec4 v_uv;      // x, y: texture coordinates; z: the alpha test's threshold; w: rim offset
layout(location = 4) out vec4 v_normal;  // view space
layout(location = 14) out vec4 v_view;   // view-space position
layout(location = 15) out vec4 v_last;   // last frame's clip position: (x + w) / 2, (y + w) / 2, z, w
layout(location = 16) out vec4 v_screen; // (clip.xy + w) / 2, clip.zw

layout(push_constant) uniform YW4 { uint pos_fix; } yw4;

const float unit = 1.525902190e-05;  // 1 / 65535: the weights sum to 65535

// Bone b's row r in a palette, by vec4 index (3 rows a bone)
#define ROW(pal, b, r) pal.d[(b) * 3u + uint(r)]

void main() {
    float mirror = material.d[0].x;  // -1 mirrors the mesh in x
    vec4 p = vec4(a_position.x * mirror, a_position.yzw);
    vec3 pos, last, normal;
    if (0.1 >= a_uv.z) {
        uvec4 b = uvec4(uint(max(trunc(material.d[0].y), 0.0))) + a_bones;
        uint xl = b.x & 255u, xh = b.x >> 8, yl = b.y & 255u, yh = b.y >> 8;
        uint zl = b.z & 255u, zh = b.z >> 8, wl = b.w & 255u, wh = b.w >> 8;
        float w_xl = float(a_weights.x & 65535u), w_xh = float(a_weights.x >> 16);
        float w_yl = float(a_weights.y & 65535u), w_yh = float(a_weights.y >> 16);
        float w_zl = float(a_weights.z & 65535u), w_zh = float(a_weights.z >> 16);
        float w_wl = float(a_weights.w & 65535u), w_wh = float(a_weights.w >> 16);
        bool eight = 0.0 < w_zl;  // bones 5-8 only when the fifth weight isn't 0

        // this frame's blended matrix, then scaled by 1 / 65535
        vec4 m[3];
        for (int r = 0; r < 3; r++) {
            vec4 s = w_xh * ROW(bones, xh, r);
            s = fma(vec4(w_xl), ROW(bones, xl, r), s);
            s = fma(vec4(w_yl), ROW(bones, yl, r), s);
            m[r] = fma(vec4(w_yh), ROW(bones, yh, r), s);
        }
        if (eight)
            for (int r = 0; r < 3; r++) {
                vec4 s = fma(ROW(bones, zl, r), vec4(w_zl), m[r]);
                s = fma(vec4(w_zh), ROW(bones, zh, r), s);
                s = fma(ROW(bones, wl, r), vec4(w_wl), s);
                m[r] = fma(ROW(bones, wh, r), vec4(w_wh), s);
            }

        // last frame's, the same way
        vec4 l[3];
        for (int r = 0; r < 3; r++) {
            vec4 s = w_xh * ROW(last_bones, xh, r);
            s = fma(vec4(w_xl), ROW(last_bones, xl, r), s);
            s = fma(vec4(w_yl), ROW(last_bones, yl, r), s);
            l[r] = fma(vec4(w_yh), ROW(last_bones, yh, r), s);
        }
        if (eight)
            for (int r = 0; r < 3; r++) {
                vec4 s = fma(vec4(w_zl), ROW(last_bones, zl, r), l[r]);
                s = fma(vec4(w_zh), ROW(last_bones, zh, r), s);
                s = fma(vec4(w_wl), ROW(last_bones, wl, r), s);
                l[r] = fma(vec4(w_wh), ROW(last_bones, wh, r), s);
            }

        for (int r = 0; r < 3; r++) m[r] *= unit;
        last = vec3(dot(p * l[0], vec4(unit)), dot(p * l[1], vec4(unit)), dot(l[2] * p, vec4(unit)));
        pos = vec3(dot(p, m[0]), dot(p, m[1]), dot(p, m[2]));
        normal = vec3(dot(a_normal.xyz, m[0].xyz), dot(a_normal.xyz, m[1].xyz), dot(m[2].xyz, a_normal.xyz));
    } else {
        last = vec3(uintBitsToFloat(a_bones.x) * mirror, uintBitsToFloat(a_bones.yz));
        pos = p.xyz;
        normal = a_normal.xyz;
    }

    // last frame's position: last frame's view x world (rows 16-19), then view constants 28-31
    vec4 lp = vec4(last, a_position.w);
    vec4 lv = vec4(dot(lp, object.d[16]), dot(lp, object.d[17]), dot(lp, object.d[18]), dot(object.d[19], lp));
    float lw = dot(view.d[31], lv);
    v_last = vec4((lw + vec2(dot(lv, view.d[28]), dot(lv, view.d[29]))) * 0.5, dot(lv, view.d[30]), lw);

    vec4 wp = vec4(pos, a_position.w);
    vec4 clip = vec4(dot(wp, object.d[8]), dot(wp, object.d[9]), dot(wp, object.d[10]), dot(object.d[11], wp));
    gl_Position = clip;
    v_screen = vec4((clip.w + clip.xy) * 0.5, clip.zw);

    // the rim term's offset: 0.4 near the camera, rising to 1 by 25.5 units away
    float fade = clamp((25.5 - clip.w) * 0.0392156877, 0.0, 1.0);
    fade = min(fma(-fade, 2.5, 2.5), 1.0);

    vec3 nv = vec3(dot(object.d[4].xyz, normal), dot(object.d[5].xyz, normal), dot(object.d[6].xyz, normal));
    v_normal.xyz = inversesqrt(dot(nv, nv)) * nv;
    v_view = vec4(dot(wp, object.d[4]), dot(wp, object.d[5]), dot(wp, object.d[6]), dot(wp, object.d[7]));

    vec3 uv1 = vec3(a_uv.xy, 1.0);  // through the material's 2x3 texture transform (rows 4, 5)
    v_uv = vec4(dot(uv1, material.d[4].xyz), dot(material.d[5].xyz, uv1), material.d[0].w, 1.0 - min(fade, 0.6));
    v_colour = a_colour;

    if ((yw4.pos_fix & 1u) != 0u) gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
