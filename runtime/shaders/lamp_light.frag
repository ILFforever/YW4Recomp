// The engine's lamp lights (game program 42759b, with its vertex program 198f41): point lights drawn as
// volumes at a third of the size and added up. Per pixel: the position from the depth (texture 1) through the
// camera's inverse projection, the vector to the light (its view-space position and radius from the vertex
// stage), a falloff of saturate((distance - radius) / -radius)^2 times the light's colour, and N.L with the
// normals (texture 0, 0..1). Operations in the game's order, so the result can match it bit for bit.
// docs/frame.md "Lighting stage".
//
// Ours, faster, the same result (2026-11-18): a pixel that can only add 0 stops early. A light whose colour is 0
// (in daylight every lamp is) reads nothing; a pixel past the light's reach (falloff 0: most of a volume's pixels)
// skips the normals. Both write the (0, 0, 0, 1) the full sum gives there. At 4K with full-size effects the pass
// went from 1.09 / 1.22 / 0.49 ms to 0.49 / 0.45 / 0.18 (busy street, the owner's progress save, schoolyard): its
// reads come from 4K images at a third of the size, so each saved read counts. The normals are read with the
// gradients taken before any pixel stops (textureGrad), not texture()'s own from the neighbours: after an early
// return a neighbour may not be running (AMD, issue #8). The colour is one light's, the same across a volume, so its
// test doesn't split the four pixels of a block.
//
// Ours: lamp shadows (decision 0081, the settings panel's LAMP SHADOWS row). The game's lamps cast no shadow. The
// runtime draws the characters into a cube of depth images for each of the few lamps nearest them (lamp_shadow.c); a
// pixel of such a lamp looks itself up in its lamp's cube (lamp_lit below). Off, the program is the one above, bit
// for bit.
#version 460

layout(set = 1, binding = 8, std140) uniform Camera {
    vec4 rows[32];  // rows 20..23: the inverse projection
} camera;

layout(set = 1, binding = 16) uniform sampler2D normals;  // texture 0
layout(set = 1, binding = 17) uniform sampler2D depth;    // texture 1

// The runtime's shadow settings (gpu_draw.c pcss_settings_fill; the shadow mask reads the first four).
// lamp: the shadows' strength (how much of the lamp's light they take away; 0: off), the filter's reach in texels, the bias in view units (about metres), debug (1: where lamps
// reach in green, their shadows in red, in place of the light)
layout(set = 1, binding = 95, std140) uniform YW4PCSS { vec4 mode; vec4 scale; vec4 tune; vec4 contact; vec4 lamp; } yw4_pcss;

// The lamp shadows (lamp_shadow.h LampShadowTable): how many lamps have them and the images' texel size; the camera's
// view to world (rows); each such lamp's world position and reach, and how far its shadows have faded in; each of its
// six views' world-to-clip rows (lamp x 6
// + view, then 4 rows each). The images: one layer per view, depth compared (1: nothing in the way).
layout(set = 1, binding = 94, std140) uniform LampShadows {
    vec4 info;
    vec4 view_to_world[4];
    vec4 lamp[6];
    vec4 fade[2];  // each lamp's shadows faded in (0..1): lamp k at fade[k / 4][k % 4]
    vec4 face_vp[36 * 4];
} lamps;
layout(set = 1, binding = 93) uniform sampler2DArrayShadow lamp_maps;
layout(set = 1, binding = 92) uniform sampler2DArray lamp_depth;  // the same images, raw depths (nearest)

layout(location = 1) in vec4 colour;  // the light's
layout(location = 3) in vec4 light;   // xyz its view-space position, w its radius (as the vertex stage gives it)
layout(location = 4) in vec4 screen;  // xy the clip position mapped to 0..w

layout(location = 0) out vec4 o_light;

// How much of this lamp's light reaches view-space point p (normal n; the lamp at view-space l): 1 unless the lamp is
// one with shadows and a character stands between. The point goes to world space, moves off its surface along the
// normal (more with distance: a texel of the lamp's views covers more there), picks the view of the lamp's cube that
// holds it (its largest axis from the lamp, as lamp_shadow.c face_matrix numbers them) and compares its depth there,
// 3x3 taps `softness` texels apart, each a bilinear blend of four compares. No derivatives: textureGrad with none, as
// this runs after pixels may have stopped (AMD, issue #8).
float lamp_lit(vec3 p, vec3 n, vec3 l) {
    int count = int(lamps.info.x);
    vec4 pv = vec4(p, 1.0), lv = vec4(l, 1.0);
    vec3 lw = vec3(dot(lamps.view_to_world[0], lv), dot(lamps.view_to_world[1], lv), dot(lamps.view_to_world[2], lv));
    int k = -1;
    for (int i = 0; i < count; i++)
        if (distance(lw, lamps.lamp[i].xyz) < 0.05) k = i;
    if (k < 0) return 1.0;
    vec3 pw = vec3(dot(lamps.view_to_world[0], pv), dot(lamps.view_to_world[1], pv), dot(lamps.view_to_world[2], pv));
    vec3 nw = normalize(vec3(dot(lamps.view_to_world[0].xyz, n), dot(lamps.view_to_world[1].xyz, n),
                             dot(lamps.view_to_world[2].xyz, n)));
    float texel_world = 2.1 * distance(pw, lamps.lamp[k].xyz) * lamps.info.y;  // one texel's width there (90+ degrees)
    pw += nw * (texel_world * 1.5 + yw4_pcss.lamp.z);
    vec3 d = pw - lamps.lamp[k].xyz, a = abs(d);
    int face = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5);
    int layer = k * 6 + face;
    vec4 w4 = vec4(pw, 1.0);
    vec4 c = vec4(dot(lamps.face_vp[layer * 4], w4), dot(lamps.face_vp[layer * 4 + 1], w4),
                  dot(lamps.face_vp[layer * 4 + 2], w4), dot(lamps.face_vp[layer * 4 + 3], w4));
    vec3 ndc = c.xyz / c.w;
    vec2 uv = fma(ndc.xy, vec2(0.5), vec2(0.5));
    // What stands in the way, and how far it is from this point (owner, 2026-11-19: our shadows were long next to the
    // game's sun shadows). A real lamp isn't a point: a shadow gets lighter and softer the farther it falls from what
    // casts it, dark at the feet, faint at the head's end. The raw depths around (a view's depth is a + b / distance
    // along its axis: lamp_shadow.c face_matrix) of those nearer the lamp than this point give the caster's distance;
    // the shadow fades out from info.z to info.w metres past it (1 to 3 m: from 0.3 to 2 m they were all too light).
    float far = max(lamps.lamp[k].w, 0.2), near = 0.05;
    float za = far / (far - near), zb = -far * near / (far - near);
    float blockers = 0.0, found = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            float z = textureGrad(lamp_depth, vec3(fma(vec2(x, y), vec2(2.0 * lamps.info.y), uv), float(layer)),
                                  vec2(0.0), vec2(0.0)).x;
            if (z < ndc.z) blockers += zb / (z - za), found += 1.0;
        }
    if (found == 0.0) return 1.0;
    float blocker = blockers / found, gap = max(c.w - blocker, 0.0);
    float length_fade = 1.0 - smoothstep(lamps.info.z, lamps.info.w, gap);
    // (the filter also widened with the distance, as an area light's penumbra does: the shapes went to blobs on top of
    // the lamp light's third-size picture, owner 2026-11-19; the fade alone stays)
    vec2 step = vec2(yw4_pcss.lamp.y * lamps.info.y);
    float sum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sum += textureGrad(lamp_maps, vec4(fma(vec2(x, y), step, uv), float(layer), ndc.z), vec2(0.0), vec2(0.0));
    return mix(1.0, sum * (1.0 / 9.0), lamps.fade[k >> 2][k & 3] * length_fade);
}

void main() {
    vec2 s = (screen * gl_FragCoord.wwww).xy;
    vec2 at = vec2(s.x, -s.y + 1.0);
    vec2 at_dx = dFdx(at), at_dy = dFdy(at);  // while every pixel of the block runs
    if (colour.x == 0.0 && colour.y == 0.0 && colour.z == 0.0) {
        o_light = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec4 clip = vec4(fma(at, vec2(2.0, -2.0), vec2(-1.0, 1.0)), textureGrad(depth, at, at_dx, at_dy).x, 1.0);
    float w = dot(clip, camera.rows[23]);
    vec3 pos = vec3(dot(clip, camera.rows[20]), dot(clip, camera.rows[21]), dot(camera.rows[22], clip));
    vec3 to_light = fma(-pos, vec3(1.0 / w), light.xyz);
    float inv_len = inversesqrt(abs(dot(to_light, to_light)));
    float falloff = clamp(((-light.w) + 1.0 / inv_len) / (-light.w), 0.0, 1.0);
    if (falloff == 0.0) {
        o_light = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 lit = (falloff * falloff) * colour.xyz;
    vec3 n = (textureGrad(normals, at, at_dx, at_dy).xyz + -0.5) * 2.0;
    float ndl = clamp(dot(n, inv_len * to_light), 0.0, 1.0);
    o_light = vec4(ndl * lit, 1.0);
    // lamp shadows (ours). Not where the lamp adds under 3% of its brightest (facing away, or the dim edge of its
    // reach): a shadow there can't be seen
    if (yw4_pcss.lamp.x > 0.0 && falloff * falloff * ndl > 0.03) {
        float sh = lamp_lit(pos * (1.0 / w), n, light.xyz);
        o_light.xyz = yw4_pcss.lamp.w == 1.0 ? vec3(1.0 - sh, 0.25, 0.0) : o_light.xyz * mix(1.0, sh, yw4_pcss.lamp.x);
    }
}
