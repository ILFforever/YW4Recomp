/* NVN colour formats on the CPU (pixels.h).
 *
 * Format numbers: NVN's enum, in the order of the table in tegra.c (R8, R8_SNORM, R8UI,
 * R8I, then R16F, R16, ... in groups). Only the formats a clear or a window texture needs so
 * far are here; anything else returns 0 and the caller says so.
 */
#include <math.h>
#include <string.h>

#include "pixels.h"

enum {
    F_R8 = 0x01, F_R16F = 0x05, F_R32F = 0x0A, F_RG8 = 0x0D, F_RG16F = 0x11, F_RG32F = 0x16,
    F_RGBA8 = 0x25, F_RGBA16F = 0x29, F_RGBA32F = 0x2E, F_RGBX8_SRGB = 0x37, F_RGBA8_SRGB = 0x38,
    F_RGB10A2 = 0x3D, F_R11G11B10F = 0x3F, F_BGRX8 = 0x98, F_BGRA8 = 0x99, F_BGRX8_SRGB = 0x9A, F_BGRA8_SRGB = 0x9B,
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static uint8_t unorm8(float v) { return (uint8_t)(clamp01(v) * 255.0f + 0.5f); }
static float srgb(float v) {  /* linear -> sRGB transfer function */
    v = clamp01(v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

/* IEEE half from float (round to nearest even; overflow to infinity). */
static uint16_t half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e == 0xFF) return (uint16_t)(sign | 0x7C00 | (m ? 0x200 : 0));
    int ex = (int)e - 127 + 15;
    if (ex >= 31) return (uint16_t)(sign | 0x7C00);
    if (ex <= 0) {
        if (ex < -10) return (uint16_t)sign;
        m |= 0x800000;
        uint32_t shift = (uint32_t)(14 - ex), h = m >> shift, rest = m & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (h & 1))) h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = (uint32_t)ex << 10 | m >> 13, rest = m & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) h++;
    return (uint16_t)(sign | h);
}

static float from_half(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, x;
    if (e == 0) {
        float f = (float)m * (1.0f / 16777216.0f);  /* subnormal: m * 2^-24 */
        return sign ? -f : f;
    }
    x = sign | (e == 31 ? 0xFFu << 23 | m << 13 : (e - 15 + 127) << 23 | m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* Unsigned small floats of R11G11B10F: 5-bit exponent, 6 or 5 bits of mantissa, no sign. */
static uint32_t small_float(float f, int mbits) {
    if (!(f > 0)) return 0;
    uint16_t h = half(f);  /* same exponent as a half; cut the mantissa */
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 31) return (31u << mbits) | (m ? 1 : 0);
    return e << mbits | m >> (10 - mbits);
}

int pixel_encode(uint32_t format, const float c[4], uint8_t out[16]) {
    switch (format) {
    case F_R8: out[0] = unorm8(c[0]); return 1;
    case F_RG8: out[0] = unorm8(c[0]), out[1] = unorm8(c[1]); return 2;
    case F_RGBA8: for (int i = 0; i < 4; i++) out[i] = unorm8(c[i]); return 4;
    case F_RGBA8_SRGB: case F_RGBX8_SRGB:
        for (int i = 0; i < 3; i++) out[i] = unorm8(srgb(c[i]));
        out[3] = unorm8(c[3]);
        return 4;
    case F_BGRA8: case F_BGRX8:
        out[0] = unorm8(c[2]), out[1] = unorm8(c[1]), out[2] = unorm8(c[0]), out[3] = unorm8(c[3]);
        return 4;
    case F_BGRA8_SRGB: case F_BGRX8_SRGB:
        out[0] = unorm8(srgb(c[2])), out[1] = unorm8(srgb(c[1])), out[2] = unorm8(srgb(c[0])), out[3] = unorm8(c[3]);
        return 4;
    case F_R16F: case F_RG16F: case F_RGBA16F: {
        int n = format == F_R16F ? 1 : format == F_RG16F ? 2 : 4;
        for (int i = 0; i < n; i++) {
            uint16_t h = half(c[i]);
            memcpy(out + 2 * i, &h, 2);
        }
        return 2 * n;
    }
    case F_R32F: case F_RG32F: case F_RGBA32F: {
        int n = format == F_R32F ? 1 : format == F_RG32F ? 2 : 4;
        memcpy(out, c, 4 * (size_t)n);
        return 4 * n;
    }
    case F_RGB10A2: {
        uint32_t v = (uint32_t)(clamp01(c[0]) * 1023 + 0.5f) | (uint32_t)(clamp01(c[1]) * 1023 + 0.5f) << 10 |
                     (uint32_t)(clamp01(c[2]) * 1023 + 0.5f) << 20 | (uint32_t)(clamp01(c[3]) * 3 + 0.5f) << 30;
        memcpy(out, &v, 4);
        return 4;
    }
    case F_R11G11B10F: {
        uint32_t v = small_float(c[0], 6) | small_float(c[1], 6) << 11 | small_float(c[2], 5) << 22;
        memcpy(out, &v, 4);
        return 4;
    }
    }
    return 0;
}

int pixel_to_rgba8(uint32_t format, const uint8_t *t, uint8_t out[4]) {
    switch (format) {
    case F_RGBA8: case F_RGBA8_SRGB: memcpy(out, t, 4); return 1;
    case F_RGBX8_SRGB: memcpy(out, t, 3), out[3] = 255; return 1;
    case F_BGRA8: case F_BGRA8_SRGB: out[0] = t[2], out[1] = t[1], out[2] = t[0], out[3] = t[3]; return 1;
    case F_BGRX8: case F_BGRX8_SRGB: out[0] = t[2], out[1] = t[1], out[2] = t[0], out[3] = 255; return 1;
    case F_RGB10A2: {
        uint32_t v;
        memcpy(&v, t, 4);
        for (int i = 0; i < 3; i++) out[i] = (uint8_t)(((v >> (10 * i)) & 1023) >> 2);
        out[3] = (uint8_t)((v >> 30) * 85);
        return 1;
    }
    case F_RGBA16F:
        for (int i = 0; i < 4; i++) {
            uint16_t h;
            memcpy(&h, t + 2 * i, 2);
            out[i] = unorm8(from_half(h));
        }
        return 1;
    }
    return 0;
}
