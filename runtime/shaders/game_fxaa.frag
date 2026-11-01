// The engine's own FXAA (game program 3f9fc9, with the quad 2e3291; the anti-aliasing setting's FXAA is
// fxaa.frag): the console version of FXAA 3. Four corner samples half a texel out give the brightness
// gradient; blurs along the edge it shows of two samples (A) and four (B); B unless its brightness falls
// outside the corners' and the centre's range. Operations in the game's order, so the result can match it
// bit for bit. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 0, std140) uniform Fxaa {
    vec4 a;  // xy one texel, w the longest step (in texels)
    vec4 b;  // x how much the corners' brightness shortens a step
} fxaa;

layout(set = 1, binding = 16) uniform sampler2D scene;  // texture 0

layout(location = 3) in vec4 uv;  // xy the pixel, zw half a texel

layout(location = 0) out vec4 o_colour;

// brightness weights, as the game has them (F16 constants for the corners and centre, F32 for blur B)
const vec3 luma16 = vec3(0.299072266, 0.586914063, 0.114013672);
const vec3 luma32 = vec3(0.298999995, 0.587000012, 0.114);

float luma(vec2 at) { return dot(textureLod(scene, at, 0.0).xyz, luma16); }

void main() {
    vec4 out_ = fma(uv.zwzw, vec4(1.0, -1.0, -1.0, 1.0), uv.xyxy);  // (u+z, v-w), (u-z, v+w)
    float ne = luma(out_.xy);
    float nw = luma(-uv.zw + uv.xy);
    float se = luma(uv.zw + uv.xy);
    float sw = luma(out_.zw);

    float west_east = (nw + sw) + -(ne + se);  // the game: (nw + sw) - (ne + se)
    float north = ne + nw;
    float sum = se + (sw + north);
    float north_south = -(se + sw) + north;
    float reduce = max((sum * fxaa.b.x) * 0.25, 0.0078125);
    float denom = reduce + min(abs(west_east), abs(north_south));
    vec2 dir = vec2(-north_south, west_east) / denom;
    dir = min(max(dir, vec2(-fxaa.a.w)), vec2(fxaa.a.w)) * fxaa.a.xy;

    vec3 e = textureLod(scene, fma(dir, vec2(-0.166666672), uv.xy), 0.0).xyz;
    vec3 f = textureLod(scene, fma(dir, vec2(0.166666672), uv.xy), 0.0).xyz;
    vec3 g = textureLod(scene, fma(dir, vec2(-0.5), uv.xy), 0.0).xyz;
    vec3 h = textureLod(scene, fma(dir, vec2(0.5), uv.xy), 0.0).xyz;
    vec3 two = f + e;
    vec3 four = (two + (h + g)) * 0.25;
    float four_luma = dot(four, luma32);

    float centre = luma(uv.xy);
    float lo = min(centre, min(min(ne, se), min(nw, sw)));
    float hi = max(max(max(ne, se), max(nw, sw)), centre);
    bool outside = hi < four_luma || four_luma < lo;
    o_colour = vec4(outside ? two * 0.5 : four, 1.0);
}
