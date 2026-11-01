// The engine's edge-aware upsample (game program 3c94bd, with the full-screen quad 98c09b): a small image
// (texture 0) brought to full size without bleeding across edges. The small image's depth (texture 1) one
// texel left, right, up and down, and the full-size depth (texture 2) at the pixel: per axis, where the
// neighbours differ, how far the pixel's depth lies between them (0..1, else 0.5) moves the sample point by
// (2f - 1) texels. Operations in the game's order (multiplies by reciprocals, fused multiply-adds), so the
// result can match it bit for bit. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 0, std140) uniform Pass {
    vec4 texel;  // xy: the small image's texel size
} pass;

layout(set = 1, binding = 16) uniform sampler2D image;       // texture 0: the small image
layout(set = 1, binding = 17) uniform sampler2D small_depth; // texture 1
layout(set = 1, binding = 18) uniform sampler2D full_depth;  // texture 2

layout(location = 3) in vec4 uv;

layout(location = 0) out vec4 o_colour;

void main() {
    vec4 step = vec4(pass.texel.x, 0.0, 0.0, pass.texel.y);  // x across, w down
    vec4 before = -step + uv.xyxy;                           // (left), (up)
    vec4 after = step + uv.xyxy;                             // (right), (down)
    vec2 near = vec2(textureLod(small_depth, before.xy, 0.0).x, textureLod(small_depth, before.zw, 0.0).x);
    vec2 far = vec2(textureLod(small_depth, after.xy, 0.0).x, textureLod(small_depth, after.zw, 0.0).x);
    vec2 span = -near + far;
    vec2 edge = vec2(lessThan(vec2(0.0), abs(span)));  // 1 where the neighbours differ
    float centre = textureLod(full_depth, uv.xy, 0.0).x;
    vec2 f = clamp((-near + centre) * vec2(1.0 / span.x, 1.0 / span.y), 0.0, 1.0);
    f = fma(edge, f + -0.5, vec2(0.5));                      // 0.5 where they don't
    vec4 moved = fma(step * f.xxyy, vec4(2.0), -step);       // (2f - 1) texels
    vec2 at = (moved.zw + moved.xy) + uv.xy;
    o_colour = textureLod(image, at, 0.0);
}
