// Cut-out shadow casters (decision 0024): as the game's dceb89..., drop the fragment where
// the material's texture is more transparent than its threshold. Depth only.
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 3) in vec4 cut;

void main() {
    if (texture(tex, cut.xy).w < cut.z) discard;
}
