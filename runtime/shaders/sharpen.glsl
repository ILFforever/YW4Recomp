// SHARPENING's filter (decision 0076), shared by taa_output.frag (the 3D, in the perceptual curve) and
// ui_sharpen.frag (text and menus, in the window image's own values). Both have values in 0..1.
//
// The method of AMD FidelityFX RCAS ("robust contrast-adaptive sharpening", FSR's own sharpener, MIT licence), so
// TAA, DLSS, OFF, FXAA and SMAA sharpen the way FSR does at the same setting. The pixel e is pushed away from its
// four neighbours (b above, h below, d left, f right) by one weight (a "lobe") for all three colours, as large as
// the neighbours allow without the result leaving 0..1 in any colour, at most RCAS_LIMIT, then scaled by the amount.
// Soft detail gets a strong push; strong edges get less, so they don't ring. The amount is FSR's sharpness: 0..1,
// in stops (1.0 the full lobe, 0.5 half of it, 0.0 a quarter); 0 or below means none (the callers skip it).
// The first filter here (a push clamped to the neighbours' range) stopped gaining on soft texture: on DLSS its MAX
// came out like FSR's MEDIUM, and stronger looked blocky before it caught up (issue #11, 2026-11-16).
const float RCAS_LIMIT = 0.25 - 1.0 / 16.0;

vec3 sharpen(vec3 e, vec3 b, vec3 h, vec3 d, vec3 f, float amount) {
    float con = exp2(-2.0 * (1.0 - clamp(amount, 0.0, 1.0)));  // FSR's sharpness -> RCAS's weight, 2 stops down to 0
    vec3 mn4 = min(min(b, h), min(d, f)), mx4 = max(max(b, h), max(d, f));
    // the most negative lobe each colour takes before the result would go below 0 (hit_min) or above 1 (hit_max)
    vec3 hit_min = min(mn4, e) / max(4.0 * mx4, vec3(1e-5));
    vec3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    vec3 l = max(-hit_min, hit_max);
    float lobe = max(-RCAS_LIMIT, min(max(l.r, max(l.g, l.b)), 0.0)) * con;
    return (lobe * (b + h + d + f) + e) / (4.0 * lobe + 1.0);
}
