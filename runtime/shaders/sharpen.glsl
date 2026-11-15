// SHARPENING's filter (decision 0076), shared by taa_output.frag (the 3D, in the perceptual curve) and
// ui_sharpen.frag (text and menus, in the window image's own values). Both have values in 0..1.
//
// Contrast-adaptive, after the method AMD published as FidelityFX CAS: the pixel c is pushed away from its four
// neighbours' average, by less where the 3x3 neighbourhood already has strong contrast (its darkest against its
// brightest). Soft, low-contrast detail gets the most; edges that are already crisp get little, so they don't ring.
// The result stays within the four neighbours' range: no bright or dark rims. (Our own code from the published
// idea, not AMD's.) The push is 2x the amount where the area is flat (MEDIUM 0.5 -> 1.0, MAX 1.0 -> 2.0).
vec3 sharpen(vec3 c, vec3 n, vec3 s, vec3 w, vec3 e, vec3 nw, vec3 ne, vec3 sw, vec3 se, float amount) {
    vec3 lo = min(c, min(min(n, s), min(w, e))), hi = max(c, max(max(n, s), max(w, e)));
    // the diagonals widen the range the contrast is judged on (CAS's "soft" min and max); the clamp stays the cross's
    vec3 lo9 = min(lo, min(min(nw, ne), min(sw, se))), hi9 = max(hi, max(max(nw, ne), max(sw, se)));
    // 1 where the neighbourhood is flat, toward 0 where it spans dark to bright
    vec3 adapt = sqrt(clamp(lo9 / max(hi9, vec3(1e-4)), 0.0, 1.0));
    return clamp(c + (c - 0.25 * (n + s + w + e)) * (2.0 * amount * adapt), lo, hi);
}
