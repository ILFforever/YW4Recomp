#version 460
#extension GL_GOOGLE_include_directive : require
// Temporal anti-aliasing (runtime/src/gpu_aa.c): the 3D draws are shifted by a different fraction of a pixel
// every frame (gpu_taa_jitter), so blending each frame into the history gathers many samples per pixel.
// Last frame's result is fetched where this pixel was then, from the game's own motion vectors (G-buffer
// target 0: this frame's screen position minus last frame's, in 0..1 screen units), with a sharp
// Catmull-Rom filter, and pulled into the range of colours around the pixel now, so things that appeared,
// left or moved on their own (the game's motion is often the camera's only) don't leave trails.
// Works in the perceptual curve (aa.glsl); writes the new history in it.
#include "aa.glsl"
layout(set = 0, binding = 0) uniform sampler2D current;  // aa_encode.frag's output (this frame, shifted)
layout(set = 0, binding = 1) uniform sampler2D motion;   // the game's motion target
layout(set = 0, binding = 4) uniform sampler2D history;  // last frame's result
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 to_ycocg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 to_rgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Catmull-Rom in 5 bilinear taps (the 4 corner taps' weights are small enough to leave out)
vec3 history_at(vec2 uv) {
    vec2 size = aa.metrics.zw, pos = uv * size, c1 = floor(pos - 0.5) + 0.5, f = pos - c1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (c1 - 1.0) / size, t3 = (c1 + 2.0) / size, t12 = (c1 + w2 / w12) / size;
    vec3 r = textureLod(history, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y) +
             textureLod(history, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y) +
             textureLod(history, t12, 0.0).rgb * (w12.x * w12.y) +
             textureLod(history, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y) +
             textureLod(history, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    float sum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / sum, vec3(0.0));
}

// h moved toward the box's centre until it is inside the box
vec3 clip_to_box(vec3 lo, vec3 hi, vec3 h) {
    vec3 c = 0.5 * (hi + lo), e = 0.5 * (hi - lo) + 1e-5, v = h - c, a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : h;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = ivec2(aa.metrics.zw), msize = textureSize(motion, 0);
    vec3 cur = to_ycocg(texelFetch(current, p, 0).rgb), lo = cur, hi = cur, m1 = vec3(0.0), m2 = vec3(0.0);
    vec2 mv = vec2(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), size - 1);
            vec3 c = to_ycocg(texelFetch(current, q, 0).rgb);
            lo = min(lo, c), hi = max(hi, c), m1 += c, m2 += c * c;
            // the longest motion around: the edges of a moving thing take its motion, not the background's
            vec2 v = texelFetch(motion, q * msize / size, 0).xy;
            if (dot(v, v) > dot(mv, mv)) mv = v;
        }
    mv *= aa.params.zw;  // the game's screen y runs upwards here when the window origin is flipped
    if (aa.params.y == 4.0) {  // YW4_AA_DEBUG=4 (a second run of this pass, not into the history): the motion, x red, y green, scaled by 50
        o_color = vec4(aa_encode(vec3(abs(mv) * 50.0, 0.0)), 1.0);
        return;
    }
    vec2 prev = v_uv - mv;
    if (aa.params.x < 0.5 || any(lessThan(prev, vec2(0.0))) || any(greaterThan(prev, vec2(1.0)))) {
        o_color = vec4(to_rgb(cur), 1.0);  // no history here: start from this frame
        return;
    }
    if (aa.params.y == 5.0) {  // YW4_AA_DEBUG=5: how far last frame's result, moved, is from this frame (x4)
        o_color = vec4(aa_encode(abs(history_at(prev) - to_rgb(cur)) * 4.0), 1.0);
        return;
    }
    // variance box, inside the neighbourhood's own range
    vec3 mean = m1 / 9.0, sd = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 hist = clip_to_box(max(lo, mean - 1.25 * sd), min(hi, mean + 1.25 * sd), to_ycocg(history_at(prev)));
    // a tenth of the new frame, up to a quarter while moving fast (less smear on quick turns)
    float a = mix(0.1, 0.25, clamp(length(mv * aa.metrics.zw) / 16.0, 0.0, 1.0));
    o_color = vec4(max(to_rgb(mix(hist, cur, a)), vec3(0.0)), 1.0);
}
