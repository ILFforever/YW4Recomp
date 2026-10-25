#version 460
// Our version of the lit material pass's main vertex program 046b2a (decision 0026), with its
// interface: constant buffers 2 (object: world in rows 0-2, projection x view x world in 8-11) and
// 3 (material), and the outputs its fragment programs read. Every operation is in the game's order.

layout(set = 0, binding = 2, std140) uniform Object { vec4 d[4096]; } object;
layout(set = 0, binding = 3, std140) uniform Material { vec4 d[4096]; } material;

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_colour;
layout(location = 2) in vec4 a_normal;
layout(location = 3) in vec4 a_tangent;  // w: the bitangent's sign
layout(location = 4) in vec4 a_uv;
layout(location = 5) in vec4 a_uv2;      // a second set of texture coordinates (light maps?)

layout(location = 1) out vec4 v_colour;   // vertex colour x material colour
layout(location = 3) out vec4 v_uv;       // x, y: through the material's texture transform; z, w: the second set
layout(location = 5) out vec4 v_normal;   // world space, w 0
layout(location = 6) out vec4 v_world;    // world position, w 1
layout(location = 14) out vec4 v_screen;  // (clip.xy + w) / 2, clip.zw
layout(location = 18) out vec4 v_tangent; // world space, w: sign
layout(location = 25) out vec4 v_flag;

layout(push_constant) uniform YW4 { uint pos_fix; } yw4;

void main() {
    float mirror = material.d[0].x;  // -1 mirrors the mesh in x
    vec4 p = vec4(a_position.x * mirror, a_position.yzw);

    vec4 clip = vec4(dot(p, object.d[8]), dot(p, object.d[9]), dot(p, object.d[10]), dot(object.d[11], p));
    gl_Position = clip;
    v_screen = vec4((clip.w + clip.xy) * 0.5, clip.zw);

    v_world = vec4(dot(p, object.d[0]), dot(p, object.d[1]), dot(p, object.d[2]), 1.0);
    vec4 t = mirror * a_tangent;
    v_tangent = vec4(dot(object.d[0].xyz, t.xyz), dot(object.d[1].xyz, t.xyz), dot(object.d[2].xyz, t.xyz), t.w);
    v_normal = vec4(dot(a_normal.xyz, object.d[0].xyz), dot(a_normal.xyz, object.d[1].xyz),
                    dot(object.d[2].xyz, a_normal.xyz), 0.0);

    vec3 uv1 = vec3(a_uv.xy, 1.0);  // through the material's 2x3 texture transform (rows 4, 5)
    v_uv = vec4(dot(uv1, material.d[4].xyz), dot(material.d[5].xyz, uv1), a_uv2.xy);
    v_colour = a_colour * material.d[1];
    v_flag = vec4(1.0, 0.0, 0.0, 0.0);

    if ((yw4.pos_fix & 1u) != 0u) gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
