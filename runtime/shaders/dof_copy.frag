#version 450
// Distance blur, step 1 (gpu_post.c, decision 0038): the scene shrunk to the blur's size (the game's program
// multiplies by its quad's vertex colour, which is 1).

layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 3) in vec4 t0;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = texture(src, t0.xy);
}
