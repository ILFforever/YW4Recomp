/* 4x4 float matrices as the game's shaders use them: row-major, out[i] = dot(row i, v). */
#ifndef YW4_MAT4_H
#define YW4_MAT4_H

#include <stdint.h>
#include <string.h>

typedef struct { float m[16]; } Mat4;

static inline Mat4 mat4_mul(const Mat4 *a, const Mat4 *b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

/* 16 floats at a guest address */
static inline Mat4 mat4_load(uint64_t addr) {
    Mat4 r;
    memcpy(r.m, (const void *)(uintptr_t)addr, sizeof r.m);
    return r;
}

/* the largest difference between n floats, relative to the larger value (absolute below 1) */
static inline float mat4_diff(const float *a, const float *b, int n) {
    float worst = 0;
    for (int i = 0; i < n; i++) {
        float d = a[i] - b[i], s = (a[i] < 0 ? -a[i] : a[i]) > 1 ? (a[i] < 0 ? -a[i] : a[i]) : 1;
        if (d < 0) d = -d;
        if (d / s > worst) worst = d / s;
    }
    return worst;
}

#endif
