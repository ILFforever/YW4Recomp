#version 460
#extension GL_GOOGLE_include_directive : require
// FXAA (our own small version of the fast approximate anti-aliasing idea): where the luma of the
// neighbours says there is an edge, blend along it, two or four taps along the edge's direction; written
// back to the game's target in its linear colour.
#include "aa.glsl"
layout(set = 0, binding = 0) uniform sampler2D encoded;  // aa_encode.frag's output
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const float SPAN_MAX = 8.0, REDUCE_MUL = 1.0 / 8.0, REDUCE_MIN = 1.0 / 128.0;
const float EDGE_MIN = 0.0312, EDGE_REL = 0.125;  // contrast below which a texel is left alone

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 at(vec2 uv) { return textureLod(encoded, uv, 0.0).rgb; }

void main() {
    vec2 px = aa.metrics.xy;
    vec3 m = at(v_uv);
    float lm = luma(m);
    float nw = luma(at(v_uv + vec2(-1.0, -1.0) * px)), ne = luma(at(v_uv + vec2(1.0, -1.0) * px));
    float sw = luma(at(v_uv + vec2(-1.0, 1.0) * px)), se = luma(at(v_uv + vec2(1.0, 1.0) * px));
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(EDGE_MIN, hi * EDGE_REL)) {
        o_color = vec4(aa_decode(m), 1.0);
        return;
    }
    // the edge runs across the steepest change of luma
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.25 * REDUCE_MUL, REDUCE_MIN);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-SPAN_MAX), vec2(SPAN_MAX)) * px;
    vec3 a = 0.5 * (at(v_uv + dir * (1.0 / 3.0 - 0.5)) + at(v_uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (at(v_uv - dir * 0.5) + at(v_uv + dir * 0.5));
    float lb = luma(b);
    o_color = vec4(aa_decode(lb < lo || lb > hi ? a : b), 1.0);  // the wider blend only if it stays in range
}
