/* Floating-point helpers with ARM semantics (Arm ARM pseudocode, FPCR = 0: round to nearest
 * even, no flush-to-zero, no default-NaN mode; decision 0008).
 *
 * Values are passed as raw bit patterns (uint32_t = single, uint64_t = double) so nothing
 * depends on how the host treats NaNs. For non-NaN inputs and results, IEEE arithmetic on
 * x86-64 SSE rounds exactly like ARM, so the plain C operation is used. NaNs differ: ARM
 * returns the first signalling NaN (quietened), else the first quiet NaN, else the default
 * NaN (positive), while x86 produces a negative "indefinite" NaN and picks NaN operands in
 * instruction-operand order. Every helper therefore fixes up NaN results explicitly.
 *
 * FPSR exception flags are not modelled (main never reads FPSR).
 * Build generated code with -ffp-contract=off and without -ffast-math.
 */
#ifndef YW4FP_H
#define YW4FP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Exact square root and fused multiply-add. Library sqrt()/fma() are not always correctly
 * rounded (MinGW's double versions are off by one ulp on hard cases; found by the Pi tests),
 * so use the CPU instructions: SSE2 sqrtsd and FMA3 vfmadd (x86-64 CPUs since 2013). */
#if defined(__x86_64__)
#include <immintrin.h>
static inline double yw4_sqrt(double x) { return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x))); }
static inline float yw4_sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
__attribute__((target("fma"))) static inline double yw4_fma(double a, double b, double c) {
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
}
#else
static inline double yw4_sqrt(double x) { return __builtin_sqrt(x); }
static inline float yw4_sqrtf(float x) { return __builtin_sqrtf(x); }
static inline double yw4_fma(double a, double b, double c) { return __builtin_fma(a, b, c); }
#endif

static inline float f32_of(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline double f64_of(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
static inline uint32_t bits_f32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline uint64_t bits_f64(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }

#define F32_DEFAULT_NAN 0x7FC00000u
#define F64_DEFAULT_NAN 0x7FF8000000000000ull

static inline int f32_isnan(uint32_t a) { return (a & 0x7FFFFFFFu) > 0x7F800000u; }
static inline int f64_isnan(uint64_t a) { return (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline int f32_issnan(uint32_t a) { return f32_isnan(a) && !(a & 0x00400000u); }
static inline int f64_issnan(uint64_t a) { return f64_isnan(a) && !(a & 0x0008000000000000ull); }

/* FPProcessNaNs for two operands. Returns 1 and sets *r if either is a NaN. */
static inline int f32_nans2(uint32_t a, uint32_t b, uint32_t *r) {
    if (f32_issnan(a)) { *r = a | 0x00400000u; return 1; }
    if (f32_issnan(b)) { *r = b | 0x00400000u; return 1; }
    if (f32_isnan(a)) { *r = a; return 1; }
    if (f32_isnan(b)) { *r = b; return 1; }
    return 0;
}
static inline int f64_nans2(uint64_t a, uint64_t b, uint64_t *r) {
    if (f64_issnan(a)) { *r = a | 0x0008000000000000ull; return 1; }
    if (f64_issnan(b)) { *r = b | 0x0008000000000000ull; return 1; }
    if (f64_isnan(a)) { *r = a; return 1; }
    if (f64_isnan(b)) { *r = b; return 1; }
    return 0;
}

/* Two-operand arithmetic: NaN operands first, then the host operation; a NaN produced by
 * an invalid operation (inf - inf, 0 * inf, 0 / 0, ...) becomes the default NaN. */
#define YW4_FP_BINOP(name, op)                                                       \
    static inline uint32_t f32_##name(uint32_t a, uint32_t b) {                      \
        uint32_t r;                                                                  \
        if (f32_nans2(a, b, &r)) return r;                                           \
        r = bits_f32(f32_of(a) op f32_of(b));                                        \
        return f32_isnan(r) ? F32_DEFAULT_NAN : r;                                   \
    }                                                                                \
    static inline uint64_t f64_##name(uint64_t a, uint64_t b) {                      \
        uint64_t r;                                                                  \
        if (f64_nans2(a, b, &r)) return r;                                           \
        r = bits_f64(f64_of(a) op f64_of(b));                                        \
        return f64_isnan(r) ? F64_DEFAULT_NAN : r;                                   \
    }
YW4_FP_BINOP(add, +)
YW4_FP_BINOP(sub, -)
YW4_FP_BINOP(mul, *)
YW4_FP_BINOP(div, /)
#undef YW4_FP_BINOP

static inline uint32_t f32_sqrt(uint32_t a) {
    if (f32_isnan(a)) return f32_issnan(a) ? a | 0x00400000u : a;
    uint32_t r = bits_f32(yw4_sqrtf(f32_of(a)));
    return f32_isnan(r) ? F32_DEFAULT_NAN : r;
}
static inline uint64_t f64_sqrt(uint64_t a) {
    if (f64_isnan(a)) return f64_issnan(a) ? a | 0x0008000000000000ull : a;
    uint64_t r = bits_f64(yw4_sqrt(f64_of(a)));
    return f64_isnan(r) ? F64_DEFAULT_NAN : r;
}

/* FMAX/FMIN: NaNs propagate; max(+0, -0) = +0 and min(+0, -0) = -0. */
static inline uint32_t f32_max(uint32_t a, uint32_t b) {
    uint32_t r;
    if (f32_nans2(a, b, &r)) return r;
    float x = f32_of(a), y = f32_of(b);
    if (x == y) return a & b;  /* equal: only zeros can differ, and then only in sign */
    return x > y ? a : b;
}
static inline uint32_t f32_min(uint32_t a, uint32_t b) {
    uint32_t r;
    if (f32_nans2(a, b, &r)) return r;
    float x = f32_of(a), y = f32_of(b);
    if (x == y) return a | b;
    return x < y ? a : b;
}
static inline uint64_t f64_max(uint64_t a, uint64_t b) {
    uint64_t r;
    if (f64_nans2(a, b, &r)) return r;
    double x = f64_of(a), y = f64_of(b);
    if (x == y) return a & b;
    return x > y ? a : b;
}
static inline uint64_t f64_min(uint64_t a, uint64_t b) {
    uint64_t r;
    if (f64_nans2(a, b, &r)) return r;
    double x = f64_of(a), y = f64_of(b);
    if (x == y) return a | b;
    return x < y ? a : b;
}

/* FMAXNM/FMINNM: a single quiet NaN is ignored (the number wins); otherwise as FMAX/FMIN. */
static inline uint32_t f32_maxnm(uint32_t a, uint32_t b) {
    if (f32_isnan(a) && !f32_issnan(a) && !f32_isnan(b)) return b;
    if (f32_isnan(b) && !f32_issnan(b) && !f32_isnan(a)) return a;
    return f32_max(a, b);
}
static inline uint32_t f32_minnm(uint32_t a, uint32_t b) {
    if (f32_isnan(a) && !f32_issnan(a) && !f32_isnan(b)) return b;
    if (f32_isnan(b) && !f32_issnan(b) && !f32_isnan(a)) return a;
    return f32_min(a, b);
}
static inline uint64_t f64_maxnm(uint64_t a, uint64_t b) {
    if (f64_isnan(a) && !f64_issnan(a) && !f64_isnan(b)) return b;
    if (f64_isnan(b) && !f64_issnan(b) && !f64_isnan(a)) return a;
    return f64_max(a, b);
}
static inline uint64_t f64_minnm(uint64_t a, uint64_t b) {
    if (f64_isnan(a) && !f64_issnan(a) && !f64_isnan(b)) return b;
    if (f64_isnan(b) && !f64_issnan(b) && !f64_isnan(a)) return a;
    return f64_min(a, b);
}

/* FCMP/FCMPE: NZCV = 0011 unordered, 0110 equal, 1000 less, 0010 greater. */
static inline uint64_t f32_cmp(uint32_t a, uint32_t b) {
    if (f32_isnan(a) || f32_isnan(b)) return 0x30000000u;
    float x = f32_of(a), y = f32_of(b);
    return x == y ? 0x60000000u : x < y ? 0x80000000u : 0x20000000u;
}
static inline uint64_t f64_cmp(uint64_t a, uint64_t b) {
    if (f64_isnan(a) || f64_isnan(b)) return 0x30000000u;
    double x = f64_of(a), y = f64_of(b);
    return x == y ? 0x60000000u : x < y ? 0x80000000u : 0x20000000u;
}

/* FCVT between single and double. NaNs keep sign and the top payload bits, and are quietened. */
static inline uint64_t f32_to_f64(uint32_t a) {
    if (f32_isnan(a))
        return (uint64_t)(a >> 31) << 63 | 0x7FF8000000000000ull | (uint64_t)(a & 0x003FFFFFu) << 29;
    return bits_f64((double)f32_of(a));
}
static inline uint32_t f64_to_f32(uint64_t a) {
    if (f64_isnan(a))
        return (uint32_t)(a >> 63) << 31 | 0x7FC00000u | (uint32_t)((a >> 29) & 0x003FFFFFu);
    return bits_f32((float)f64_of(a));
}

/* Round to an integral value in a mode: 'n' nearest even, 'a' nearest away from zero,
 * 'p' toward +inf, 'm' toward -inf, 'z' toward zero. Written out instead of using libm
 * (MinGW's round() gets 0.49999997 wrong; found by the Pi tests). Exact: x - trunc(x)
 * is always representable, and the sign of zero results is kept. */
static inline double yw4_round(double x, char mode) {
    if (!(__builtin_fabs(x) < 4503599627370496.0)) return x;  /* NaN, inf, or already integral (>= 2^52) */
    double t = __builtin_copysign((double)(int64_t)x, x);
    double frac = x - t, one = __builtin_copysign(1.0, x);
    switch (mode) {
    case 'n':
        if (__builtin_fabs(frac) > 0.5 || (__builtin_fabs(frac) == 0.5 && ((int64_t)t & 1))) t += one;
        break;
    case 'a':
        if (__builtin_fabs(frac) >= 0.5) t += one;
        break;
    case 'p':
        if (frac > 0) t += 1.0;
        break;
    case 'm':
        if (frac < 0) t -= 1.0;
        break;
    default:
        break;
    }
    return t;
}

/* FRECPS / FRSQRTS: 2 - a*b and (3 - a*b) / 2, fused (one rounding). The NaN rules see -a.
 * Single precision is computed in double: a float product is exact there, and rounding
 * double -> float after one double operation gives the same result as a single rounding. */
/* Special cases shared by the step functions: 1 and *r set if they decide the result.
 * `a` is already negated. */
static inline int f32_step_special(uint32_t a, uint32_t b, uint32_t inf_times_zero, uint32_t *r) {
    if (f32_nans2(a, b, r)) return 1;
    int infa = (a & 0x7FFFFFFFu) == 0x7F800000u, infb = (b & 0x7FFFFFFFu) == 0x7F800000u;
    int zeroa = !(a & 0x7FFFFFFFu), zerob = !(b & 0x7FFFFFFFu);
    if ((infa && zerob) || (zeroa && infb)) { *r = inf_times_zero; return 1; }
    if (infa || infb) { *r = ((a ^ b) & 0x80000000u) | 0x7F800000u; return 1; }
    return 0;
}
static inline int f64_step_special(uint64_t a, uint64_t b, uint64_t inf_times_zero, uint64_t *r) {
    const uint64_t mag = 0x7FFFFFFFFFFFFFFFull, inf = 0x7FF0000000000000ull;
    if (f64_nans2(a, b, r)) return 1;
    int infa = (a & mag) == inf, infb = (b & mag) == inf, zeroa = !(a & mag), zerob = !(b & mag);
    if ((infa && zerob) || (zeroa && infb)) { *r = inf_times_zero; return 1; }
    if (infa || infb) { *r = ((a ^ b) & ~mag) | inf; return 1; }
    return 0;
}
static inline uint32_t f32_recps(uint32_t a, uint32_t b) {
    uint32_t r;
    a ^= 0x80000000u;
    if (f32_step_special(a, b, 0x40000000u, &r)) return r;  /* 2.0 */
    return bits_f32((float)(2.0 + (double)f32_of(a) * (double)f32_of(b)));
}
static inline uint32_t f32_rsqrts(uint32_t a, uint32_t b) {
    uint32_t r;
    a ^= 0x80000000u;
    if (f32_step_special(a, b, 0x3FC00000u, &r)) return r;  /* 1.5 */
    return bits_f32((float)((3.0 + (double)f32_of(a) * (double)f32_of(b)) / 2.0));
}
static inline uint64_t f64_recps(uint64_t a, uint64_t b) {
    uint64_t r;
    a ^= 0x8000000000000000ull;
    if (f64_step_special(a, b, 0x4000000000000000ull, &r)) return r;
    return bits_f64(yw4_fma(f64_of(a), f64_of(b), 2.0));
}
static inline uint64_t f64_rsqrts(uint64_t a, uint64_t b) {
    /* Rounds 3 - a*b, then halves: differs from ARM only if the result is subnormal. */
    uint64_t r;
    a ^= 0x8000000000000000ull;
    if (f64_step_special(a, b, 0x3FF8000000000000ull, &r)) return r;
    return bits_f64(yw4_fma(f64_of(a), f64_of(b), 3.0) * 0.5);
}

/* FRECPE / FRSQRTE: 8-bit estimates from the Arm ARM tables (RecipEstimate,
 * RecipSqrtEstimate), on a 52-bit fraction like the pseudocode. */
#define YW4_M52 0x000FFFFFFFFFFFFFull
static inline uint32_t yw4_recip_estimate(uint32_t a) {
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}
static inline uint32_t yw4_rsqrt_estimate(uint32_t a) {
    if (a < 256) {
        a = a * 2 + 1;
    } else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    uint32_t b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}
/* Shared core. bias = 127 or 1023; mant_bits = 23 or 52; returns (sign-less) exponent and
 * the 52-bit result fraction. */
static inline void yw4_recpe_core(int exp, uint64_t frac, int bias, int *rexp, uint64_t *rfrac) {
    if (exp == 0) {
        if (!(frac >> 51 & 1)) { exp = -1; frac = (frac << 2) & YW4_M52; }
        else frac = (frac << 1) & YW4_M52;
    }
    uint32_t scaled = 0x100 | (uint32_t)(frac >> 44);
    int e = 2 * bias - 1 - exp;
    uint64_t f = (uint64_t)(yw4_recip_estimate(scaled) & 0xFF) << 44;
    if (e == 0) f = 1ull << 51 | f >> 1;
    else if (e == -1) { f = 1ull << 50 | f >> 2; e = 0; }
    *rexp = e;
    *rfrac = f;
}
static inline void yw4_rsqrte_core(int exp, uint64_t frac, int bias, int *rexp, uint64_t *rfrac) {
    if (exp == 0) {
        while (!(frac >> 51 & 1)) { frac = (frac << 1) & YW4_M52; exp--; }
        frac = (frac << 1) & YW4_M52;
    }
    uint32_t scaled = (exp & 1) == 0 ? 0x100 | (uint32_t)(frac >> 44) : 0x80 | (uint32_t)(frac >> 45);
    *rexp = (3 * bias - 1 - exp) / 2;
    *rfrac = (uint64_t)(yw4_rsqrt_estimate(scaled) & 0xFF) << 44;
}
static inline uint32_t f32_recpe(uint32_t a) {
    uint32_t sign = a & 0x80000000u, mag = a & 0x7FFFFFFFu;
    if (f32_isnan(a)) return a | 0x00400000u;
    if (mag == 0x7F800000u) return sign;
    if (mag < 0x00200000u) return sign | 0x7F800000u;  /* 0 or below 2^-128: the result overflows */
    int e;
    uint64_t f;
    yw4_recpe_core((int)(mag >> 23), (uint64_t)(mag & 0x7FFFFF) << 29, 127, &e, &f);
    return sign | (uint32_t)(e & 0xFF) << 23 | (uint32_t)(f >> 29);
}
static inline uint64_t f64_recpe(uint64_t a) {
    uint64_t sign = a & 0x8000000000000000ull, mag = a & 0x7FFFFFFFFFFFFFFFull;
    if (f64_isnan(a)) return a | 0x0008000000000000ull;
    if (mag == 0x7FF0000000000000ull) return sign;
    if (mag < 0x0004000000000000ull) return sign | 0x7FF0000000000000ull;  /* below 2^-1024 */
    int e;
    uint64_t f;
    yw4_recpe_core((int)(mag >> 52), mag & YW4_M52, 1023, &e, &f);
    return sign | (uint64_t)(e & 0x7FF) << 52 | f;
}
static inline uint32_t f32_rsqrte(uint32_t a) {
    uint32_t mag = a & 0x7FFFFFFFu;
    if (f32_isnan(a)) return a | 0x00400000u;
    if (mag == 0) return a | 0x7F800000u;            /* +-0 -> +-inf */
    if (a >> 31) return F32_DEFAULT_NAN;             /* negative */
    if (mag == 0x7F800000u) return 0;
    int e;
    uint64_t f;
    yw4_rsqrte_core((int)(mag >> 23), (uint64_t)(mag & 0x7FFFFF) << 29, 127, &e, &f);
    return (uint32_t)(e & 0xFF) << 23 | (uint32_t)(f >> 29);
}
static inline uint64_t f64_rsqrte(uint64_t a) {
    uint64_t mag = a & 0x7FFFFFFFFFFFFFFFull;
    if (f64_isnan(a)) return a | 0x0008000000000000ull;
    if (mag == 0) return a | 0x7FF0000000000000ull;
    if (a >> 63) return F64_DEFAULT_NAN;
    if (mag == 0x7FF0000000000000ull) return 0;
    int e;
    uint64_t f;
    yw4_rsqrte_core((int)(mag >> 52), mag & YW4_M52, 1023, &e, &f);
    return (uint64_t)(e & 0x7FF) << 52 | f;
}

/* FRINT*: round to an integral value in the same format (signs of zero kept, NaNs quietened). */
static inline uint32_t f32_rint(uint32_t a, char mode) {
    if (f32_isnan(a)) return a | 0x00400000u;
    return bits_f32((float)yw4_round((double)f32_of(a), mode));  /* exact both ways */
}
static inline uint64_t f64_rint(uint64_t a, char mode) {
    if (f64_isnan(a)) return a | 0x0008000000000000ull;
    return bits_f64(yw4_round(f64_of(a), mode));
}

/* Float -> integer with a rounding mode (see yw4_round), saturating; NaN converts to 0
 * (FPToFixed). */
static inline int64_t yw4_to_s64(double x, char mode) {
    if (x != x) return 0;
    x = yw4_round(x, mode);
    if (x >= 9223372036854775808.0) return INT64_MAX;
    if (x < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}
static inline uint64_t yw4_to_u64(double x, char mode) {
    if (x != x) return 0;
    x = yw4_round(x, mode);
    if (x >= 18446744073709551616.0) return UINT64_MAX;
    if (x <= 0) return 0;
    return (uint64_t)x;
}
static inline int32_t yw4_to_s32(double x, char mode) {
    if (x != x) return 0;
    x = yw4_round(x, mode);
    if (x >= 2147483648.0) return INT32_MAX;
    if (x < -2147483648.0) return INT32_MIN;
    return (int32_t)x;
}
static inline uint32_t yw4_to_u32(double x, char mode) {
    if (x != x) return 0;
    x = yw4_round(x, mode);
    if (x >= 4294967296.0) return UINT32_MAX;
    if (x <= 0) return 0;
    return (uint32_t)x;
}

#endif
