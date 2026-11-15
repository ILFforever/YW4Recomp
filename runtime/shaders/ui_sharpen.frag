#version 460
#extension GL_GOOGLE_include_directive : require
// SHARPEN TEXT AND MENUS (decision 0076): the presented window image, game UI included, sharpened like the 3D
// (sharpen.glsl) in its own 0..1 values, written back over it. The 3D was sharpened already at the anti-aliasing
// point, so a pixel that still equals the picture as it was there (`before`: the scene before the UI, copied into
// an image like this one where the window shows it) is left alone; only what the UI changed is sharpened.
// params: x the amount, y 1 when `before` is this frame's (else, a frame with no 3D such as the title or a
// full-screen menu, everything is UI).
#include "aa.glsl"
#include "sharpen.glsl"
layout(set = 0, binding = 0) uniform sampler2D presented;  // a copy of the window image
layout(set = 0, binding = 1) uniform sampler2D before;     // the scene before the UI, at the window image's size
layout(location = 0) out vec4 o_color;

vec4 at(ivec2 p) { return texelFetch(presented, clamp(p, ivec2(0), ivec2(aa.metrics.zw) - 1), 0); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = at(p);
    if (aa.params.y > 0.5) {
        vec3 b = texelFetch(before, p, 0).rgb;
        if (all(lessThan(abs(c.rgb - b), vec3(1.5 / 255.0)))) {  // the 3D, untouched by the UI
            o_color = c;
            return;
        }
    }
    vec3 sharp = sharpen(c.rgb, at(p + ivec2(0, -1)).rgb, at(p + ivec2(0, 1)).rgb, at(p + ivec2(-1, 0)).rgb,
                         at(p + ivec2(1, 0)).rgb, at(p + ivec2(-1, -1)).rgb, at(p + ivec2(1, -1)).rgb,
                         at(p + ivec2(-1, 1)).rgb, at(p + ivec2(1, 1)).rgb, aa.params.x);
    o_color = vec4(sharp, c.a);
}
