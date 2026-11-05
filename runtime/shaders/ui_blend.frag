#version 450
// The picture's texel as it is: premultiplied alpha, which the pipeline blends as src + dst * (1 - src.a).
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    color = texture(tex, v_uv);
}
