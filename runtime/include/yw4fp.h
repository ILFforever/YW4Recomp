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
    uint32_t r = bits_f32(sqrtf(f32_of(a)));
    return f32_isnan(r) ? F32_DEFAULT_NAN : r;
}
static inline uint64_t f64_sqrt(uint64_t a) {
    if (f64_isnan(a)) return f64_issnan(a) ? a | 0x0008000000000000ull : a;
    uint64_t r = bits_f64(sqrt(f64_of(a)));
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

/* Float -> integer with a rounding mode, saturating; NaN converts to 0 (FPToFixed).
 * mode: 'n' nearest even, 'a' nearest away from zero, 'p' toward +inf, 'm' toward -inf,
 * 'z' toward zero. */
static inline double yw4_round(double x, char mode) {
    switch (mode) {
    case 'n': return nearbyint(x);  /* the host default rounding mode is nearest even */
    case 'a': return round(x);
    case 'p': return ceil(x);
    case 'm': return floor(x);
    default: return trunc(x);
    }
}
/* FRINT*: round to an integral value in the same format (signs of zero kept, NaNs quietened). */
static inline uint32_t f32_rint(uint32_t a, char mode) {
    if (f32_isnan(a)) return a | 0x00400000u;
    float x = f32_of(a), r;
    switch (mode) {
    case 'n': r = nearbyintf(x); break;
    case 'a': r = roundf(x); break;
    case 'p': r = ceilf(x); break;
    case 'm': r = floorf(x); break;
    default: r = truncf(x); break;
    }
    return bits_f32(r);
}
static inline uint64_t f64_rint(uint64_t a, char mode) {
    if (f64_isnan(a)) return a | 0x0008000000000000ull;
    return bits_f64(yw4_round(f64_of(a), mode));
}

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
