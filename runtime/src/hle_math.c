/* libm: the sdk's math functions, bit-exact (decision 0013).
 *
 * The sdk's libm is musl 1.1.19's (checked against the sdk's own code on the ARM64
 * reference machine: tools/yw4/sdkref.py), compiled for AArch64. Here:
 *  - most functions are musl 1.1.19's C (third_party/musl, one documented patch);
 *  - ceil/floor/sqrt/fabs are single instructions in musl's aarch64 build (FRINTP, FRINTM,
 *    FSQRT, FABS), so they use the recompiler's exact models of those (yw4fp.h, decision
 *    0008). musl's C calls sqrt/sqrtf/floor internally; those calls land here too.
 *  - NaN results are made ARM's: x86 generates a negative default NaN and picks the first
 *    operand when both are NaN; AArch64 generates +NaN (0x7ff8...) and prefers a signalling
 *    NaN (decision 0008). Where musl returns a NaN argument without arithmetic, both
 *    machines already agree.
 *
 * Guest calling convention: double/float arguments and results in v0/v1, pointers and ints
 * in x0.
 */
#include "hle.h"
#include "yw4fp.h"

/* musl 1.1.19 (third_party/musl/yw4/glue.h gives every function a musl_ prefix) */
#define MUSL_DD(n) double musl_##n(double);
#define MUSL_FF(n) float musl_##n(float);
MUSL_DD(acos) MUSL_DD(asin) MUSL_DD(atan) MUSL_DD(cos) MUSL_DD(cosh) MUSL_DD(exp) MUSL_DD(log)
MUSL_DD(log10) MUSL_DD(sin) MUSL_DD(sinh) MUSL_DD(tan) MUSL_DD(tanh)
MUSL_FF(acosf) MUSL_FF(asinf) MUSL_FF(atanf) MUSL_FF(cosf) MUSL_FF(exp2f) MUSL_FF(expf)
MUSL_FF(log10f) MUSL_FF(logf) MUSL_FF(sinf) MUSL_FF(tanf)
double musl_atan2(double, double), musl_fmod(double, double), musl_pow(double, double);
float musl_atan2f(float, float), musl_fmodf(float, float), musl_powf(float, float);
double musl_frexp(double, int *), musl_ldexp(double, int), musl_modf(double, double *);
float musl_modff(float, float *);

/* What musl's aarch64 build has as instructions, also used by musl's own C. */
double musl_sqrt(double x) { return f64_of(f64_sqrt(bits_f64(x))); }
float musl_sqrtf(float x) { return f32_of(f32_sqrt(bits_f32(x))); }
double musl_floor(double x) { return f64_of(f64_rint(bits_f64(x), 'm')); }

/* ARM NaN results (see the top of the file). */
static uint64_t nan64(uint64_t r, uint64_t a, uint64_t b, int nargs) {
    if (!f64_isnan(r)) return r;
    int na = f64_isnan(a), nb = nargs == 2 && f64_isnan(b);
    if (!na && !nb) return F64_DEFAULT_NAN;
    if (na && nb) f64_nans2(a, b, &r);
    return r;
}
static uint32_t nan32(uint32_t r, uint32_t a, uint32_t b, int nargs) {
    if (!f32_isnan(r)) return r;
    int na = f32_isnan(a), nb = nargs == 2 && f32_isnan(b);
    if (!na && !nb) return F32_DEFAULT_NAN;
    if (na && nb) f32_nans2(a, b, &r);
    return r;
}

/* Scalar FP results clear the rest of the vector register, as the instructions do. */
static void ret64(CPUState *ctx, uint64_t r) { ctx->v[0].d[0] = r, ctx->v[0].d[1] = 0; }
static void ret32(CPUState *ctx, uint32_t r) { ctx->v[0].d[0] = r, ctx->v[0].d[1] = 0; }

#define DD(n)                                                                        \
    void hle_math_##n(CPUState *ctx) {                                               \
        uint64_t a = ctx->v[0].d[0];                                                 \
        ret64(ctx, nan64(bits_f64(musl_##n(f64_of(a))), a, 0, 1));                   \
    }
#define FF(n)                                                                        \
    void hle_math_##n(CPUState *ctx) {                                               \
        uint32_t a = ctx->v[0].s[0];                                                 \
        ret32(ctx, nan32(bits_f32(musl_##n(f32_of(a))), a, 0, 1));                   \
    }
#define DDD(n)                                                                       \
    void hle_math_##n(CPUState *ctx) {                                               \
        uint64_t a = ctx->v[0].d[0], b = ctx->v[1].d[0];                             \
        ret64(ctx, nan64(bits_f64(musl_##n(f64_of(a), f64_of(b))), a, b, 2));        \
    }
#define FFF(n)                                                                       \
    void hle_math_##n(CPUState *ctx) {                                               \
        uint32_t a = ctx->v[0].s[0], b = ctx->v[1].s[0];                             \
        ret32(ctx, nan32(bits_f32(musl_##n(f32_of(a), f32_of(b))), a, b, 2));        \
    }
DD(acos) DD(asin) DD(atan) DD(cos) DD(cosh) DD(exp) DD(log) DD(log10) DD(sin) DD(sinh) DD(tan) DD(tanh)
FF(acosf) FF(asinf) FF(atanf) FF(cosf) FF(exp2f) FF(expf) FF(log10f) FF(logf) FF(sinf) FF(tanf)
DDD(atan2) DDD(fmod) DDD(pow)
FFF(atan2f) FFF(fmodf) FFF(powf)

/* Instructions in musl's aarch64 build */
void hle_math_ceil(CPUState *ctx) { ret64(ctx, f64_rint(ctx->v[0].d[0], 'p')); }
void hle_math_floor(CPUState *ctx) { ret64(ctx, f64_rint(ctx->v[0].d[0], 'm')); }
void hle_math_sqrt(CPUState *ctx) { ret64(ctx, f64_sqrt(ctx->v[0].d[0])); }
void hle_math_fabs(CPUState *ctx) { ret64(ctx, ctx->v[0].d[0] & ~(1ull << 63)); }
void hle_math_ceilf(CPUState *ctx) { ret32(ctx, f32_rint(ctx->v[0].s[0], 'p')); }
void hle_math_floorf(CPUState *ctx) { ret32(ctx, f32_rint(ctx->v[0].s[0], 'm')); }
void hle_math_sqrtf(CPUState *ctx) { ret32(ctx, f32_sqrt(ctx->v[0].s[0])); }
void hle_math_fabsf(CPUState *ctx) { ret32(ctx, ctx->v[0].s[0] & ~(1u << 31)); }

/* double frexp(double, int *exp). For inf and NaN musl leaves *exp untouched, so the
 * guest's value goes in first. */
void hle_math_frexp(CPUState *ctx) {
    uint64_t a = ctx->v[0].d[0];
    int32_t *out = GPTR(ctx->x[0]);
    int e = *out;
    double r = musl_frexp(f64_of(a), &e);
    *out = e;
    ret64(ctx, nan64(bits_f64(r), a, 0, 1));
}

/* double ldexp(double, int) */
void hle_math_ldexp(CPUState *ctx) {
    uint64_t a = ctx->v[0].d[0];
    ret64(ctx, nan64(bits_f64(musl_ldexp(f64_of(a), (int)(int32_t)ctx->x[0])), a, 0, 1));
}

/* double modf(double, double *iptr), float modff(float, float *iptr) */
void hle_math_modf(CPUState *ctx) {
    uint64_t a = ctx->v[0].d[0];
    double ip;
    double r = musl_modf(f64_of(a), &ip);
    *(uint64_t *)GPTR(ctx->x[0]) = nan64(bits_f64(ip), a, 0, 1);
    ret64(ctx, nan64(bits_f64(r), a, 0, 1));
}
void hle_math_modff(CPUState *ctx) {
    uint32_t a = ctx->v[0].s[0];
    float ip;
    float r = musl_modff(f32_of(a), &ip);
    *(uint32_t *)GPTR(ctx->x[0]) = nan32(bits_f32(ip), a, 0, 1);
    ret32(ctx, nan32(bits_f32(r), a, 0, 1));
}
