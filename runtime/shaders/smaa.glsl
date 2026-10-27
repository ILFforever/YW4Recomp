// SMAA (third_party/smaa, MIT) for the anti-aliasing pass (runtime/src/gpu_aa.c): the target's size from
// the push constant, the ultra preset (half the high preset's edge threshold: on the game's soft, low-contrast
// edges the high preset left jaggies FXAA smoothed; a test pattern showed SMAA itself works, YW4_AA_DEBUG=3).
// A file defines SMAA_INCLUDE_VS or SMAA_INCLUDE_PS as 0 first.
// Bindings: 0 the encoded scene, 1 edges, 2 area texture, 3 search texture, 4 blend weights.
#include "aa.glsl"
#define SMAA_RT_METRICS aa.metrics
#define SMAA_GLSL_4
#define SMAA_PRESET_ULTRA
#include "../../third_party/smaa/SMAA.hlsl"

#if SMAA_INCLUDE_VS
vec2 smaa_fullscreen() {  // one triangle over the target, (0, 0) at the top-left
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    return uv;
}
#endif
