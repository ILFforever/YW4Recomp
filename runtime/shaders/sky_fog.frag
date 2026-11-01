// The engine's sky and aerial perspective (game program 627287, with the quad 3ef32a at the far plane): per
// pixel a camera ray, a sky gradient by the ray's height, and scattering toward the sun (Rayleigh and a
// Henyey-Greenstein-like Mie phase) over the distance and height, from a small fog image looked up edge-aware.
// Every operation in the game's order (its fused multiply-adds, reciprocals and odd steps such as (v + 1) - 1),
// so the result can match it bit for bit; the names are ours. docs/frame.md "Lighting stage".
#version 460

layout(set = 1, binding = 0, std140) uniform Pass {
    vec4 c;  // x field of view, y aspect, z ray length
} pass;

layout(set = 1, binding = 6, std140) uniform Light {
    vec4 rows[4];  // row 2: the sun's direction
} light;

layout(set = 1, binding = 8, std140) uniform Camera {
    vec4 rows[48];  // 0 position, 1-2 axes, 32 fog texel, 34-46 sky and fog settings, 47 horizon
} cam;

layout(set = 1, binding = 29) uniform sampler2D fog_depth;  // texture 13
layout(set = 1, binding = 30) uniform sampler2D fog;        // texture 14 (z: how much fog)

layout(location = 1) in vec4 base;  // the quad's colour (0)
layout(location = 3) in vec4 uv;    // xy the pixel, z the ray scale

layout(location = 0) out vec4 o_colour;

const vec3 luma = vec3(0.298909992, 0.586610019, 0.114480004);

void main() {
    // the camera ray
    float half_fov = pass.c.x * 0.5;
    float c_ = cos(half_fov);
    float tan_half = sin(half_fov) / c_;
    float k = uv.z * tan_half;
    vec2 ndc = fma(uv.yx, vec2(-2.0, 2.0), vec2(1.0, -1.0));
    vec2 plane = ndc * k;
    vec3 ax1 = cam.rows[1].xyz, ax2 = cam.rows[2].xyz;
    vec3 side = fma(ax1.yzx, ax2.zxy, -(ax1.zxy * ax2.yzx));
    vec3 ray = fma(vec3(plane.y * pass.c.y), side, fma(vec3(uv.z), ax1, plane.x * ax2));
    float rs = inversesqrt(abs(dot(ray, ray)));
    float ray_y = rs * ray.y;
    vec3 d = vec3(rs * ray.x, ray_y, rs * ray.z) * pass.c.z;
    float inv_len = inversesqrt(abs(dot(d, d)));

    // the sky gradient by the ray's height
    vec2 horizon = cam.rows[47].xy;
    float lo = (horizon.y + 1.0) * 0.115999997, hi = (horizon.x + 1.0) * 0.216000006;
    float m = min(0.00400000019 / inv_len, 1.0);
    vec3 dn = inv_len * d;
    float h = dn.y * m;
    float rcp_a = 1.0 / fma(horizon.x + 1.0, 0.216000006, -lo), rcp_b = 1.0 / lo;
    vec4 t = clamp(vec4(fma(dn.y, m, -lo), h, fma(dn.y, m, lo), fma(dn.y, m, hi)) * vec4(rcp_a, rcp_b, rcp_b, rcp_a), 0.0, 1.0);
    vec3 c34 = cam.rows[34].xyz, c35 = cam.rows[35].xyz, c36 = cam.rows[36].xyz, c37 = cam.rows[37].xyz, c38 = cam.rows[38].xyz;
    vec3 below_far = fma(vec3(t.w), c37 + -c38, c38);
    vec3 below_near = fma(vec3(t.z), -c37 + c36, c37);
    float cos_sun = dot(dn, light.rows[2].xyz);
    vec2 up = clamp(vec2(h, fma(dn.y, m, hi)) / hi, 0.0, 1.0);
    vec3 sky = h >= -lo ? below_near : below_far;
    vec3 band = fma(vec3(t.y), c35 + -c36, c36);
    if (!(h >= 0.0)) band = sky;
    float above = h >= 0.0 ? 1.0 : 0.0;
    vec3 top = fma(vec3(t.x), c34 + -c35, c35);
    vec3 ground = h >= lo ? top : band;  // the colour toward the ray
    vec3 c40 = cam.rows[40].xyz, c41 = cam.rows[41].xyz, c42 = cam.rows[42].xyz;
    vec3 sky_low = fma(vec3(up.y), c41 + -c42, c42);
    vec3 sky_high = fma(vec3(up.x), c40 + -c41, c41);

    // how much fog: height falloff, and the fog image looked up edge-aware around the pixel
    float height = fma(ray_y, pass.c.z, cam.rows[0].y);
    vec2 falloff = cam.rows[45].xy;
    float height_fog = exp2(((height + -falloff.x) * falloff.y) * -1.44269502);
    vec2 texel = cam.rows[32].xy;
    vec2 near = vec2(texture(fog_depth, uv.xy + -vec2(texel.x, 0.0)).x,    // left
                     texture(fog_depth, vec2(uv.x - 0.0, uv.y + -texel.y)).x);  // up
    vec2 right_at = uv.xy + vec2(texel.x, 1.0);
    right_at = right_at + vec2(0.0, -1.0);
    vec2 far = vec2(texture(fog_depth, right_at).x, texture(fog_depth, vec2(0.0, texel.y) + uv.xy).x);
    vec2 span = -near + far;
    vec2 edge = vec2(lessThan(vec2(0.0), abs(span)));
    vec2 rcp = vec2(1.0 / span.x, 1.0 / span.y);
    vec2 f = clamp(fma(-near, rcp, rcp), 0.0, 1.0);
    f = fma(edge, f + -0.5, vec2(0.5));
    vec2 sx = f.x * vec2(texel.x, 0.0), sy = vec2(0.0, texel.y) * f.y;
    vec2 at = fma(sy, vec2(2.0), -vec2(0.0, texel.y)) + fma(sx, vec2(2.0), -vec2(texel.x, 0.0));
    float fog_amount = texture(fog, at + uv.xy).z;
    float dist = 1.0 / inv_len;
    float fog_alpha = clamp(fog_amount + cam.rows[44].y, 0.0, 1.0);

    // scattering
    vec3 sky_col = fma(vec3(above), sky_high + -sky_low, sky_low);
    vec4 c43 = cam.rows[43];
    vec2 depth_in = max(vec2(dist + c43.x, dist + cam.rows[45].z), vec2(0.0));
    vec2 c46 = cam.rows[46].xy;
    vec4 optical = vec4(depth_in.x * c43.y, depth_in.x * c43.z, depth_in.y * cam.rows[45].w, depth_in.y * c46.x) * 1.44269502;
    float g = cam.rows[44].x;
    vec2 g2 = fma(vec2(g), vec2(g), vec2(2.0, 1.0));
    float hg = log2(abs(-dot(vec2(cos_sun), vec2(g)) + g2.y)) * 1.5;
    float hg_norm = fma(-g * g, 1.0 / g2.x, 1.0 / g2.x);
    float cos2 = cos_sun * cos_sun;
    float mie = (fma(cos2, exp2(-hg), exp2(-hg)) * hg_norm) * 0.119366206;
    float rayleigh = min(fma(cos2, 0.0596831031, 0.0596831031) * 12.0, 1.0);
    vec4 absorbed = -vec4(exp2(optical.x), exp2(optical.y), exp2(optical.z), exp2(optical.w)) + 1.0;
    vec4 amounts = vec4(rayleigh, mie, height_fog, mie * height_fog) * absorbed;
    vec2 sat_zw = clamp(amounts.zw, 0.0, 1.0);
    float ground_luma = dot(ground, luma);
    vec3 scattered = fma(vec3(amounts.y), sky_col, sat_zw.y * sky_col);
    vec3 c39 = cam.rows[39].xyz;
    float c39_luma = dot(c39, luma);
    vec3 grey_ground = fma(vec3(clamp(depth_in.x * c43.w, 0.0, 1.0)), -ground_luma + ground, vec3(ground_luma));
    vec3 grey_far = fma(vec3(clamp(depth_in.y * c46.y, 0.0, 1.0)), -c39_luma + c39, vec3(c39_luma));
    vec4 sum = vec4(amounts.x * grey_ground, amounts.x) + vec4(grey_far * sat_zw.x, amounts.z);
    vec3 colour = sum.xyz + scattered;
    float keep = max(-sum.w + 1.0, 0.0) * max(fma(mie, -absorbed.y, 1.0), 0.0);
    o_colour.xyz = fma(base.xyz, vec3(keep), fog_alpha * colour);
    o_colour.w = base.w;
}
