/* Native skeleton matrices (decision 0036).
 *
 * main+0xa77390 and main+0xa79c00 turn a skeleton's local bone transforms into matrices:
 * bone 0's local matrix, then for every other bone parent matrix x local matrix, in bone
 * order. The second one also calls main+0xa7a530 after a bone whose byte in a per-bone table
 * says so. They are the hottest game code on the main thread (about a quarter of its busy
 * time in a busy scene), and the recompiled versions run ~23 host instructions per ARM one.
 * The hooks (recomp/overrides.toml) do the same work here, with the same float operations in
 * the same order, so the results are bit-identical; YW4_BONES=check proves that in game.
 *
 * Arguments: x0 = the skeleton object (+0x18: its definition), x1 = the output matrices,
 * x2 = the local transforms, both 0x30 bytes per bone; x3 is passed on to the helpers; the
 * flag in w3 bit 0 (w4 for the second function) asks for main+0xa77c30 to build a bone whose
 * parent's scale isn't the usual one. Local transform: scale (x, y, z, -) at +0x00, rotation
 * quaternion (x, y, z, w) at +0x10, translation (x, y, z) at +0x20. Matrix: 3 rows of 4
 * floats, translation in the 4th column.
 * Definition: +0x20 bone count (u16), +0x22 bit 0 = the second mode below, the parent table
 * (u16 per bone) at 4 * (u16 +0x2a) + 4 * (u16 +0xa) and the second function's per-bone byte
 * table at 4 * (u16 +0x2e) + 4 * (u16 +0xa).
 *
 * What we don't do, so the game's code does it (decided before anything is written):
 *  - the definition's second mode (+0x22 bit 0), which tracks non-uniform scale down the
 *    hierarchy in a bit set and rebuilds such bones from their column lengths (frecpe);
 *  - any NaN in a local transform, and any parent that doesn't come before its child.
 * Without NaN inputs every NaN comes from an invalid operation, which ARM gives as the
 * default NaN and x86 as a negative one, so our results are fixed up to the ARM one.
 *
 * YW4_BONES=game: the game's code only. YW4_BONES=check: run the game's code, then ours from
 * the same inputs, compare every float and keep the game's results (the helpers run twice).
 * Default (native): ours.
 */
#include <emmintrin.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yw4rt.h"

enum { MODE_GAME, MODE_CHECK, MODE_NATIVE };
static int mode = -1;
static _Thread_local int in_original;  /* check mode: the game's own run of the function */

static GuestFn fn_scaled, fn_extra, fn_first, fn_second;  /* main+0xa77c30, 0xa7a530, the two */

static struct { uint64_t calls, native, other_mode, odd_input, scaled, extra, checked, mismatches; } st;
#define COUNT(f) __atomic_add_fetch(&st.f, 1, __ATOMIC_RELAXED)

#define U8(a) (*(const uint8_t *)(uintptr_t)(a))
#define U16(a) (*(const uint16_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))
#define FP(a) ((float *)(uintptr_t)(a))

static void init_mode(void) {
    const char *s = getenv("YW4_BONES");
    int m = s && !strcmp(s, "game") ? MODE_GAME : s && !strcmp(s, "check") ? MODE_CHECK : MODE_NATIVE;
    uint64_t b = yw4_image_base;
    fn_scaled = yw4_lookup(b + 0xa77c30), fn_extra = yw4_lookup(b + 0xa7a530);
    fn_first = yw4_lookup(b + 0xa77390), fn_second = yw4_lookup(b + 0xa79c00);
    if (!fn_scaled || !fn_extra || !fn_first || !fn_second) m = MODE_GAME;
    int unset = -1;
    if (!__atomic_compare_exchange_n(&mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    fprintf(stderr, "yw4: bones: %s\n", m == MODE_GAME ? "the game's code" : m == MODE_CHECK
                                          ? "ours checked against the game's" : "ours");
}

static void report(void) {
    fprintf(stderr, "yw4: bones: %" PRIu64 " calls, ours %" PRIu64 ", game's (second mode %" PRIu64
            ", odd input %" PRIu64 "); scaled-parent bones %" PRIu64 ", extra calls %" PRIu64
            "; checked %" PRIu64 ", mismatches %" PRIu64 "\n", st.calls, st.native, st.other_mode,
            st.odd_input, st.scaled, st.extra, st.checked, st.mismatches);
}

static int has_nan(const float *f, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t b;
        memcpy(&b, &f[i], 4);
        if ((b & 0x7FFFFFFFu) > 0x7F800000u) return 1;
    }
    return 0;
}

/* NaN lanes become ARM's default NaN. */
static inline __m128 arm_nan(__m128 v) {
    __m128 nan = _mm_cmpunord_ps(v, v);
    return _mm_or_ps(_mm_andnot_ps(nan, v), _mm_and_ps(nan, _mm_castsi128_ps(_mm_set1_epi32(0x7FC00000))));
}

/* A local transform as a matrix: the game's quaternion -> rotation (with its lane mask on
 * x, y, z), translation in column 3, then column c times scale c. */
static void local_matrix(uint64_t l, const uint32_t mask[4], __m128 r[3]) {
    const float *q = FP(l + 0x10), *sc = FP(l), *t = FP(l + 0x20);
    float m[3];
    for (int i = 0; i < 3; i++) {
        uint32_t b;
        memcpy(&b, &q[i], 4);
        b &= mask[i];
        memcpy(&m[i], &b, 4);
    }
    float s = 2.0f / ((q[2] * q[2] + q[0] * q[0]) + (q[3] * q[3] + q[1] * q[1]));
    float ax = m[0] * s, ay = m[1] * s, az = m[2] * s, w = q[3];
    float xw = ax * w, yw = ay * w, zw = az * w;
    float xx = ax * m[0], yy = ay * m[1], zz = az * m[2];
    float xy = ay * m[0], yz = az * m[1], zx = ax * m[2];
    __m128 scale = _mm_setr_ps(sc[0], sc[1], sc[2], 1.0f);
    r[0] = _mm_mul_ps(_mm_setr_ps(1.0f - (yy + zz), xy - zw, zx + yw, t[0]), scale);
    r[1] = _mm_mul_ps(_mm_setr_ps(zw + xy, 1.0f - (zz + xx), yz - xw, t[1]), scale);
    r[2] = _mm_mul_ps(_mm_setr_ps(zx - yw, yz + xw, 1.0f - (xx + yy), t[2]), scale);
}

static void store_rows(uint64_t o, const __m128 r[3]) {
    for (int i = 0; i < 3; i++) _mm_storeu_ps(FP(o + 16 * i), arm_nan(r[i]));
}

/* Our version. Returns 0, having written nothing, when the game's code must do it. */
static int native(CPUState *ctx, int second) {
    uint64_t obj = ctx->x[0], out = ctx->x[1], loc = ctx->x[2], arg3 = ctx->x[3];
    uint64_t def = U64(obj + 0x18);
    if (U8(def + 0x22) & 1) { COUNT(other_mode); return 0; }
    uint32_t n = U16(def + 0x20);
    uint64_t parents = def + 4ull * U16(def + 0x2a) + 4ull * U16(def + 0xa);
    uint64_t extras = def + 4ull * U16(def + 0x2e) + 4ull * U16(def + 0xa);
    for (uint32_t i = 0; i < (n ? n : 1); i++) {
        const float *l = FP(loc + 0x30ull * i);
        if (has_nan(l, 3) || has_nan(l + 4, 7) || (i && U16(parents + 2ull * i) >= i)) {
            COUNT(odd_input);
            return 0;
        }
    }
    uint64_t b = yw4_image_base;
    uint32_t mask[4];
    memcpy(mask, (const void *)(uintptr_t)(b + 0x5864fb0), 16);
    __m128 last = _mm_loadu_ps(FP(b + 0x5864f00));  /* times each parent row's column 3 */
    const float *usual = FP(b + 0x150e100);         /* the scale that needs no main+0xa77c30 */
    int scaled_check = (second ? ctx->x[4] : arg3) & 1;
    uint64_t sp = ctx->sp, fp = ctx->x[29];
    __m128 r[3];
    local_matrix(loc, mask, r);
    store_rows(out, r);
    for (uint32_t i = 1; i < U16(U64(obj + 0x18) + 0x20); i++) {
        uint64_t li = loc + 0x30ull * i, oi = out + 0x30ull * i;
        uint32_t p = U16(parents + 2ull * i);
        uint64_t lp = loc + 0x30ull * p, op = out + 0x30ull * p;
        const float *ps = FP(lp), *pm = FP(op);
        if (scaled_check && !(ps[0] == usual[0] && ps[1] == usual[1] && ps[2] == usual[2])) {
            COUNT(scaled);
            ctx->sp = sp - (second ? 0x90 : 0x80), ctx->x[29] = ctx->sp + 0x70;
            ctx->x[0] = oi, ctx->x[1] = li, ctx->x[2] = lp;
            memcpy(&ctx->v[0], pm, 16), memcpy(&ctx->v[1], pm + 4, 16), memcpy(&ctx->v[2], pm + 8, 16);
            ctx->x[30] = b + (second ? 0xa79dc8 : 0xa7754c);
            fn_scaled(ctx);  /* the first function passes its w3, the second whatever x3 holds */
            if (!second) ctx->x[3] = (uint32_t)arg3;
        } else {
            __m128 l[3], o[3];
            local_matrix(li, mask, l);
            for (int k = 0; k < 3; k++) {
                const float *row = pm + 4 * k;
                o[k] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(l[0], _mm_set1_ps(row[0])),
                                                        _mm_mul_ps(l[1], _mm_set1_ps(row[1]))),
                                             _mm_mul_ps(l[2], _mm_set1_ps(row[2]))),
                                  _mm_mul_ps(last, _mm_set1_ps(row[3])));
            }
            store_rows(oi, o);
        }
        uint32_t e = second ? (U8(extras + i) >> 1) & 15 : 0;
        if (e) {
            COUNT(extra);
            ctx->sp = sp - 0x90, ctx->x[29] = ctx->sp + 0x70;
            ctx->x[0] = oi, ctx->x[1] = e, ctx->x[2] = li, ctx->x[3] = arg3;
            ctx->x[30] = b + 0xa79fcc;
            fn_extra(ctx);
        }
    }
    ctx->sp = sp, ctx->x[29] = fp;
    COUNT(native);
    return 1;
}

/* Check mode: the game's run, then ours from the same starting matrices; the game's stays. */
static int check(CPUState *ctx, int second) {
    uint32_t n = U16(U64(ctx->x[0] + 0x18) + 0x20);
    size_t size = 0x30ull * (n ? n : 1);
    float *out = FP(ctx->x[1]);
    float *before = malloc(size), *want = malloc(size);
    memcpy(before, out, size);
    CPUState game = *ctx;
    in_original = 1;
    (second ? fn_second : fn_first)(&game);
    in_original = 0;
    memcpy(want, out, size);
    memcpy(out, before, size);
    CPUState ours = *ctx;
    if (native(&ours, second)) {
        uint64_t c = COUNT(checked);
        int bad = 0;
        for (size_t i = 0; i < size / 4; i++) {
            if (!memcmp(&out[i], &want[i], 4)) continue;
            if (bad++ < 4) {
                uint32_t g, w;
                memcpy(&g, &out[i], 4), memcpy(&w, &want[i], 4);
                fprintf(stderr, "yw4: bones: MISMATCH f_%s bone %zu float %zu: ours %08x (%g) game %08x (%g)\n",
                        second ? "a79c00" : "a77390", i / 12, i % 12, g, out[i], w, want[i]);
            }
        }
        if (bad) COUNT(mismatches);
        if (c % 20000 == 0) report();
    }
    memcpy(out, want, size);
    free(before), free(want);
    *ctx = game;
    return 1;
}

static int hook(CPUState *ctx, int second) {
    if (in_original) return 0;
    if (__builtin_expect(__atomic_load_n(&mode, __ATOMIC_ACQUIRE) < 0, 0)) init_mode();
    if (mode == MODE_GAME) return 0;
    if (COUNT(calls) % 500000 == 0) report();
    return mode == MODE_CHECK ? check(ctx, second) : native(ctx, second);
}

int yw4_hook_bones_a77390(CPUState *ctx) { return hook(ctx, 0); }
int yw4_hook_bones_a79c00(CPUState *ctx) { return hook(ctx, 1); }
