#version 460
// Our version of the lit material pass's main fragment program 5eb3b2 (decision 0026): the scene's
// surfaces shaded with their materials, after the G-buffer and the half-resolution light passes.
// Same interface as the game's program. Every operation is in the game's order (fma where it
// fuses), so the result is bit-identical to it; names are what the values seem to be, "?" where
// unsure.
//
// Inputs:
//   constants 3 (material: d2.x emissive strength, d2.w > 0 fog on), 6 (scene lighting: d0 tint,
//   d1 shadow colour, d2 sun direction, d4 sun colour, d37.y indirect strength, d46 ambient
//   occlusion colour, d49 minimum light / d49.w smoothness scale, d50 ambient cube colour, d52
//   reflection and emissive scales), 7 (material: d0.z opacity floor, d1.x specular strength, d1.y
//   specular power, d2 x reflectance... , d6.y bump), 8 (view: d0 camera position, d10-d11 depth
//   linearisation, d32 half-res texel offsets, d34-d47 fog and sky), 12 (reflection box: rows 0-2)
//   textures: 0 colour (alpha: opacity), 1 normal map (x, y normal, z metalness, w smoothness),
//   9 the reflection cube, 13 and 20 half-res depths, 14 half-res shadow/occlusion (x shadow, y
//   occlusion, z ?), 19 half-res indirect light, 21 and 22 what is behind the surface (colour,
//   depth: for blending soft edges)

layout(set = 1, binding = 3, std140) uniform Material { vec4 d[4096]; } material;
layout(set = 1, binding = 6, std140) uniform Lighting { vec4 d[4096]; } lighting;
layout(set = 1, binding = 7, std140) uniform Surface { vec4 d[4096]; } surface;
layout(set = 1, binding = 8, std140) uniform View { vec4 d[4096]; } view;
layout(set = 1, binding = 12, std140) uniform Box { vec4 d[4096]; } box;
layout(set = 1, binding = 16) uniform sampler2D colour_map;
layout(set = 1, binding = 17) uniform sampler2D normal_map;
layout(set = 1, binding = 25) uniform samplerCube reflection;
layout(set = 1, binding = 29) uniform sampler2D half_depth;     // for the shadow buffer's resolution
layout(set = 1, binding = 30) uniform sampler2D half_shadow;
layout(set = 1, binding = 35) uniform sampler2D half_indirect;
layout(set = 1, binding = 36) uniform sampler2D half_depth2;    // for the indirect light's resolution
layout(set = 1, binding = 37) uniform sampler2D behind_colour;
layout(set = 1, binding = 38) uniform sampler2D behind_depth;

layout(location = 1) in vec4 v_colour;
layout(location = 3) in vec4 v_uv;
layout(location = 5) in vec4 v_normal;
layout(location = 6) in vec4 v_world;
layout(location = 14) in vec4 v_screen;
layout(location = 18) in vec4 v_tangent;

layout(location = 0) out vec4 o_colour;

const vec3 luma = vec3(0.298909992, 0.586610019, 0.114480004);

// Depth-aware upsampling of a half-resolution buffer: from the four neighbours' depths around
// this pixel (left/right `step.x` apart, up/down `step.y`), where this pixel's depth falls between
// them, as an offset of -step..step (0 where the neighbours agree). The game adds each axis's
// part as a vector with a 0 in the other lane: kept, because a NaN (neighbours at the same depth)
// times that 0 is NaN in both lanes.
vec2 upsample_offset(float d_left, float d_right, float d_up, float d_down, float depth, vec2 step) {
    vec2 g = vec2(d_right - d_left, d_down - d_up);
    vec2 t = clamp(vec2(depth - d_left, depth - d_up) * vec2(1.0 / g.x, 1.0 / g.y), 0.0, 1.0);
    vec2 f = fma(vec2(lessThan(vec2(0.0), abs(g))), t - 0.5, vec2(0.5));
    vec2 y_part = fma(vec2(f.y), vec2(0.0, step.y * 2.0), -vec2(0.0, step.y));
    vec2 x_part = fma(vec2(f.x), vec2(step.x * 2.0, 0.0), -vec2(step.x, 0.0));
    return y_part + x_part;
}

void main() {
    // ---- the surface normal: the normal map through the tangent frame (as gbuffer_frag.glsl)
    vec4 nmap = texture(normal_map, v_uv.xy);
    vec4 albedo = texture(colour_map, v_uv.xy);
    vec4 t = v_tangent * inversesqrt(dot(v_tangent, v_tangent));
    vec3 n = v_normal.xyz * inversesqrt(dot(v_normal.xyz, v_normal.xyz));
    vec3 b = fma(n.yzx, t.zxy, -(n.zxy * t.yzx)) * t.w;
    float bump = surface.d[6].y;
    vec3 d = vec3((nmap.xy - 0.5) * bump * 2.0, 1.0);
    d = d * inversesqrt(dot(d, d));
    d = fma(fma(d, vec3(0.5), vec3(0.5)), vec3(2.0), vec3(-1.0));
    d.xy *= bump;
    vec3 nw = fma(n, vec3(d.z), fma(t.xyz, vec3(d.x), b * d.y));
    nw *= inversesqrt(dot(nw, nw));

    // ---- the reflection direction, corrected for the reflection box (constants 12) when inside it
    vec3 eye = view.d[0].xyz - v_world.xyz;  // towards the camera
    eye *= inversesqrt(dot(eye, eye));
    vec3 refl = fma(nw * -dot(-eye, nw), vec3(2.0), -eye);
    vec3 rb = vec3(dot(refl, box.d[0].xyz), dot(refl, box.d[1].xyz), dot(box.d[2].xyz, refl));
    vec4 wp = vec4(v_world.xyz, 1.0);
    vec3 pb = vec3(dot(box.d[0], wp), dot(box.d[1], wp), dot(box.d[2], wp));  // the box is -1..1
    rb *= inversesqrt(dot(rb, rb));
    vec3 inv = vec3(1.0 / rb.x, 1.0 / rb.y, 1.0 / rb.z);
    vec3 exit = max(fma(-pb, inv, inv), fma(-pb, inv, -inv));
    bool inside = all(greaterThanEqual(vec3(1.0), abs(pb)));
    vec3 hit = fma(rb, vec3(min(exit.z, min(exit.y, exit.x))), pb);
    vec3 refl_dir = inside ? inversesqrt(dot(hit, hit)) * hit : rb;

    // ---- the material
    vec3 m2 = surface.d[2].xyz;
    float metal = nmap.z;
    vec3 f0 = max(vec3(m2.y), albedo.xyz * metal);                // specular colour
    float s = nmap.w * lighting.d[49].w;
    float rough = max(s * s, 9.999999750e-05);
    float alpha = clamp(albedo.w, 0.0, 1.0);
    float occl_strength = m2.x * alpha;
    float gloss = fma(-nmap.w, lighting.d[49].w, 1.0);
    float gloss2 = gloss * gloss, rough2 = rough * rough;
    float max_mip = float(textureQueryLevels(reflection)) + -1.0;
    vec3 diffuse = fma(albedo.xyz, vec3(-metal), albedo.xyz);      // albedo x (1 - metalness)
    float refl_mip = fma(max_mip, -gloss2 * gloss2, max_mip);
    float refl_scale = lighting.d[52].y * m2.z;
    float spec_scale = surface.d[1].x * lighting.d[52].x;
    float emissive = lighting.d[52].z * material.d[2].x;

    // ---- this pixel on the screen, and the half-res buffers sampled around it
    vec2 sp = v_screen.xy * gl_FragCoord.w;
    float depth = v_screen.z * gl_FragCoord.w;
    vec2 uv = vec2(sp.x, -sp.y) + vec2(0.0, 1.0);
    vec4 step = view.d[32];  // x, y: the shadow buffer's texel; z, w: the indirect light's
    float d_up = texture(half_depth, vec2(uv.x, uv.y - step.y)).x;
    float d_left = texture(half_depth, vec2(uv.x - step.x, uv.y)).x;
    float d_right = texture(half_depth, vec2(sp.x + step.x, uv.y)).x;
    float d_down = texture(half_depth, vec2(uv.x, uv.y + step.y)).x;
    vec3 shadow_occl = texture(half_shadow, uv + upsample_offset(d_left, d_right, d_up, d_down, depth, step.xy)).xyz;
    vec3 occlusion = fma(vec3(shadow_occl.y), lighting.d[46].xyz, vec3(-shadow_occl.y)) + 1.0;

    // ---- the sun: shadow (from the buffer, and where the surface turns away), Blinn-Phong/GGX-like specular
    float sun_len = inversesqrt(dot(lighting.d[2].xyz, lighting.d[2].xyz));
    vec3 sun = sun_len * lighting.d[2].xyz;
    float n_l = dot(nw, sun);
    float shade = max(clamp((n_l + -0.200000003) * -10.0, 0.0, 1.0), shadow_occl.x);
    float d2_left = texture(half_depth2, vec2(uv.x - step.z, uv.y)).x;
    float d2_up = texture(half_depth2, vec2(uv.x, uv.y - step.w)).x;
    float d2_right = texture(half_depth2, vec2(sp.x + step.z, uv.y)).x;
    float d2_down = texture(half_depth2, vec2(uv.x, uv.y + step.w)).x;
    float shade1 = min(shade, 1.0);
    vec3 shadow = fma(lighting.d[1].xyz, vec3(shade1), vec3(-shade1)) + 1.0;  // 1 lit .. the shadow colour
    vec3 h = fma(vec3(sun_len), lighting.d[2].xyz, eye);
    h *= inversesqrt(max(dot(h, h), 0.00100000005));
    float n_h = clamp(dot(nw, h), 0.0, 1.0);
    vec2 offset2 = upsample_offset(d2_left, d2_right, d2_up, d2_down, depth, step.zw);
    float l_h = clamp(dot(sun, h), 0.0, 1.0);
    vec3 occl_term = fma(-shadow * occlusion, vec3(occl_strength), vec3(occl_strength));
    float p = exp2(log2(n_h) * surface.d[1].y);
    float dn = fma(rough * rough, p, -p) + 1.00001001;
    float vis = (rough + 0.5) * max(l_h * l_h, 0.100000001);
    float spec = spec_scale / inversesqrt(max(rough2 / (dn * dn * vis) * 0.25, 9.999999750e-05));
    vec3 sun_spec = spec * lighting.d[4].xyz;
    sun_spec = fma(vec3(-shade), sun_spec, sun_spec);
    vec3 indirect = texture(half_indirect, uv + offset2).xyz;
    vec3 base = fma(f0, sun_spec, diffuse);

    // ---- light: the sun's diffuse and the indirect light, ambient and reflections from the cube map
    float n_v = clamp(dot(nw, eye), 0.0, 1.0);
    float omv = -n_v + 1.0;
    vec3 light = fma(lighting.d[4].xyz, vec3(clamp(n_l, 0.0, 1.0)), indirect * lighting.d[37].y);
    float omv2 = omv * omv, omv4 = omv2 * omv2;
    float fresnel = fma(-m2.y, omv4, omv4) + m2.y;
    vec3 lit = base * max(light, lighting.d[49].xyz);
    vec3 irradiance = textureLod(reflection, nw, max_mip).xyz;
    vec3 ambient = lighting.d[50].w * lighting.d[50].xyz;
    float k_refl = gloss * (refl_scale * fresnel);
    vec3 amb = diffuse * (ambient * irradiance);
    vec3 c = fma(lit, vec3(0.666000009), fma(vec3(-k_refl), amb, amb));
    vec3 f0s = f0 * -lighting.d[52].w;
    vec3 env = textureLod(reflection, refl_dir, refl_mip).xyz;
    vec3 refl_weight = fma(f0, vec3(lighting.d[52].w), fma(f0s, vec3(k_refl), vec3(k_refl)));
    c = fma(env * ambient, refl_weight, c);
    float out_alpha = fma(surface.d[0].z, -alpha + 1.0, alpha);
    vec3 glow = emissive * albedo.xyz;
    vec4 colour = vec4(fma(c * lighting.d[0].xyz, fma(shadow, occlusion, occl_term), out_alpha * glow), out_alpha);
    colour *= v_colour;  // as the game: (y, z, w, x) times (y, z, w, x)

    // ---- fog and sky scattering towards the camera (material constants 3 d2.w > 0)
    vec3 inscatter;
    float transmit;
    if (0.0 < material.d[2].w) {
        vec3 vdir = v_world.xyz - view.d[0].xyz;
        float inv_dist = inversesqrt(dot(vdir, vdir));
        vdir = inv_dist * vdir;
        vec3 c35 = view.d[35].xyz, c36 = view.d[36].xyz, c37 = view.d[37].xyz;
        vec3 band_a = view.d[34].xyz - c35;
        vec2 hz = vec2(view.d[47].y, view.d[47].x) + 1.0;
        vec2 hz_k = hz * vec2(0.115999997, 0.216000006);
        float far = min(0.00400000019 / inv_dist, 1.0);
        float inv_a = 1.0 / fma(hz.y, 0.216000006, -hz_k.x);
        vec2 up_zw = fma(vec2(vdir.y), vec2(far), hz_k);
        float up_x = fma(vdir.y, far, -hz_k.x);
        float up = vdir.y * far;  // how far up the view ray climbs
        float inv_k = 1.0 / hz_k.x;
        vec4 band = clamp(vec4(inv_a, inv_k, inv_k, inv_a) * vec4(up_x, up, up_zw), 0.0, 1.0);
        float g = view.d[44].x;  // the sun's halo (phase function asymmetry)
        vec3 sky = fma(vec3(band.x), band_a, c35);
        vec2 g2 = fma(vec2(g), vec2(g), vec2(2.0, 1.0));
        vec3 sky_mid = fma(vec3(band.z), c36 - c37, c37);
        vec3 sky_high = fma(vec3(band.y), c35 - c36, c36);
        float cos_sun = dot(vdir, lighting.d[2].xyz);
        float phase_d = -dot(vec2(cos_sun), vec2(g)) + g2.y;
        vec3 sky_low = fma(vec3(band.w), c37 - view.d[38].xyz, view.d[38].xyz);
        float cos2 = cos_sun * cos_sun;
        float mie_k = fma(-g * g, 1.0 / g2.x, 1.0 / g2.x);
        float mie = fma(cos2, exp2(-(log2(abs(phase_d)) * 1.5)), exp2(-(log2(abs(phase_d)) * 1.5))) * mie_k;
        float rayleigh = fma(cos2, 0.0596831031, 0.0596831031);  // 3 / (16 pi) (1 + cos^2)
        float mie_phase = mie * 0.119366206;                     // 3 / (8 pi)
        float ray12 = min(rayleigh * 12.0, 1.0);
        bool above_low = up >= -hz_k.x, above_high = up >= hz_k.x, rising = up >= 0.0;
        float dist = 1.0 / inv_dist;
        vec3 horizon = !above_low ? sky_low : sky_mid;
        if (rising) horizon = sky_high;
        if (!above_high) sky = horizon;
        vec4 c45 = view.d[45], c43 = view.d[43];
        vec2 fd = max(vec2(dist + c43.x, c45.z + dist), 0.0);
        vec2 blend = clamp(vec2(up, up_zw.y) / hz_k.y, 0.0, 1.0);
        vec2 c46 = view.d[46].xy;
        float height = (v_world.y + -c45.x) * c45.y * -1.44269502;
        vec3 c41 = view.d[41].xyz, c42 = view.d[42].xyz;
        vec3 sun_col_lo = fma(vec3(blend.y), c41 - c42, c42);
        vec3 sun_col_hi = fma(vec3(blend.x), view.d[40].xyz - c41, c41);
        float height_f = exp2(height);
        float sat = clamp(fd.x * c43.w, 0.0, 1.0);
        vec4 e = vec4(fd.x * c43.yz, c45.w * fd.y, fd.y * c46.x) * 1.44269502;
        vec4 absorb = -vec4(exp2(e.x), exp2(e.y), exp2(e.z), exp2(e.w)) + 1.0;
        vec4 amount = vec4(ray12, mie_phase, height_f, mie_phase * height_f) * absorb;
        float sky_luma = dot(sky, luma);
        sky = fma(vec3(sat), -sky_luma + sky, vec3(sky_luma));
        vec3 sun_col = fma(vec3(rising ? 1.0 : 0.0), sun_col_hi - sun_col_lo, sun_col_lo);
        vec2 amount_c = clamp(amount.zw, 0.0, 1.0);
        vec3 sun_scatter = fma(vec3(amount.y), sun_col, amount_c.y * sun_col);
        vec3 c39 = view.d[39].xyz;
        float c39_luma = dot(c39, luma);
        float sat2 = clamp(fd.y * c46.y, 0.0, 1.0);
        vec3 haze = fma(vec3(sat2), -c39_luma + c39, vec3(c39_luma)) * amount_c.x;
        vec4 s4 = vec4(amount.x * sky, amount.x) + vec4(haze, amount.z);
        inscatter = s4.xyz + sun_scatter;
        transmit = max(-s4.w + 1.0, 0.0) * max(fma(mie_phase, -absorb.y, 1.0), 0.0);
    } else {
        inscatter = vec3(0.0);
        transmit = 1.0;
    }

    // ---- fogged colour, then blended with what is behind by the depth difference (soft edges)
    vec3 behind = texture(behind_colour, uv).xyz;
    vec3 fogged = fma(colour.xyz, vec3(transmit), inscatter * clamp(shadow_occl.z + view.d[44].y, 0.0, 1.0));
    vec2 dd = vec2(texture(behind_depth, uv).x, depth);
    vec2 lin = fma(-dd, vec2(view.d[11].w), vec2(view.d[10].w)) *
               vec2(1.0 / fma(dd.x, view.d[11].z, -view.d[10].z), 1.0 / fma(dd.y, view.d[11].z, -view.d[10].z));
    float k = clamp(abs(lin.y + -lin.x) / lighting.d[39].y, 0.0, 1.0);
    o_colour = vec4(fma(vec3(k), fogged + -behind, behind), colour.w);
}
