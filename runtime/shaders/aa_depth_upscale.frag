#version 460
// Upscaling (runtime/src/gpu_aa.c): the 3D's depth, stretched into the output-sized depth image, so the draws
// after the anti-aliasing point (the UI, and world markers such as the "!" above people, which the game hides
// behind what stands in front) test against it as before. Nearest texel: depths don't blend.
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(location = 0) in vec2 v_uv;

void main() {
    ivec2 size = textureSize(depth, 0);
    gl_FragDepth = texelFetch(depth, clamp(ivec2(v_uv * vec2(size)), ivec2(0), size - 1), 0).r;
}
