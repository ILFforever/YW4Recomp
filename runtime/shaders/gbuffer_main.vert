#version 460
// Our G-buffer vertex program (decision 0026): what the game's main G-buffer vertex program
// (bc358b) computes, written out. Same interface as the translated program, so it runs in its
// place with the game's state: constant buffers 1 (view), 2 (object), 3 (material) and the
// outputs its fragment programs read.

layout(set = 0, binding = 1, std140) uniform View { vec4 d[4096]; } view;
layout(set = 0, binding = 2, std140) uniform Object { vec4 d[4096]; } object;
layout(set = 0, binding = 3, std140) uniform Material { vec4 d[4096]; } material;

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_colour;
layout(location = 2) in vec4 a_normal;
layout(location = 3) in vec4 a_tangent;   // w: the bitangent's sign
layout(location = 4) in vec4 a_uv;

layout(location = 1) out vec4 v_colour;      // vertex colour x material colour
layout(location = 3) out vec4 v_uv;          // x, y: texture coordinates; z: material d0.w
layout(location = 4) out vec4 v_normal;      // world space
layout(location = 5) out vec4 v_world;       // world position
layout(location = 6) out vec4 v_screen;      // (clip.xy + w) / 2, clip.zw
layout(location = 14) out vec4 v_last;       // the same for last frame's position
layout(location = 15) out vec4 v_tangent;    // world space, w: sign
layout(location = 25) out vec4 v_flag;

layout(push_constant) uniform YW4 { uint pos_fix; } yw4;

void main() {
    // object rows: 0-2 world, 8-11 projection x view x world, 12-15 last frame's world;
    // view rows 32-35: last frame's projection x view
    float mirror = material.d[0].x;  // -1 mirrors the mesh in x
    vec4 p = vec4(a_position.x * mirror, a_position.yzw);

    vec4 clip = vec4(dot(p, object.d[8]), dot(p, object.d[9]), dot(p, object.d[10]), dot(object.d[11], p));
    gl_Position = clip;
    v_screen = vec4((clip.w + clip.xy) * 0.5, clip.zw);

    vec4 last_world = vec4(dot(p, object.d[12]), dot(p, object.d[13]), dot(p, object.d[14]), dot(p, object.d[15]));
    vec4 last = vec4(dot(last_world, view.d[32]), dot(last_world, view.d[33]), dot(last_world, view.d[34]),
                     dot(view.d[35], last_world));
    v_last = vec4((last.w + last.xy) * 0.5, last.zw);

    v_world = vec4(dot(p, object.d[0]), dot(p, object.d[1]), dot(p, object.d[2]), 1.0);
    vec4 t = mirror * a_tangent;
    v_tangent = vec4(dot(object.d[0].xyz, t.xyz), dot(object.d[1].xyz, t.xyz), dot(object.d[2].xyz, t.xyz), t.w);
    v_normal = vec4(dot(a_normal.xyz, object.d[0].xyz), dot(a_normal.xyz, object.d[1].xyz),
                    dot(object.d[2].xyz, a_normal.xyz), 0.0);

    vec3 uv1 = vec3(a_uv.xy, 1.0);  // through the material's 2x3 texture transform (rows 4, 5)
    v_uv = vec4(dot(uv1, material.d[4].xyz), dot(material.d[5].xyz, uv1), material.d[0].w, 0.0);
    v_colour = a_colour * material.d[1];
    v_flag = vec4(1.0, 0.0, 0.0, 0.0);

    if ((yw4.pos_fix & 1u) != 0u) gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
