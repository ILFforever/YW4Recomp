/* Guest CPU state and helpers shared by generated code and the runtime.
 *
 * Semantics follow the Arm ARM (DDI 0487) pseudocode for ARMv8.0.
 * Needs a compiler with unsigned __int128 (GCC/Clang on 64-bit hosts).
 */
#ifndef YW4CPU_H
#define YW4CPU_H

#include <stdint.h>
#include <string.h>

/* One SIMD/FP register. Lane 0 is the lowest-addressed (little-endian), as on ARM. */
typedef union {
    uint8_t b[16];
    uint16_t h[8];
    uint32_t s[4];
    uint64_t d[2];
    float f[4];
    double df[2];
} v128;

typedef struct {
    uint64_t x[31];  /* x0..x30 */
    uint64_t sp;
    uint64_t nzcv;   /* N=bit31 Z=bit30 C=bit29 V=bit28, as in the NZCV register */
    v128 v[32];      /* v0..v31 (b/h/s/d/q views of the same register) */
    uint32_t fpcr;   /* not used yet: main never changes it (docs/decisions/0008) */
    uint32_t fpsr;   /* cumulative FP exception flags: not modelled, main never reads them */
    /* Exclusive monitor (LDXR/STXR, decision 0009): address and value of the last exclusive
     * load; excl_size = 0 means no reservation. */
    uint64_t excl_addr;
    uint64_t excl_val;
    uint32_t excl_size;
} CPUState;

#define NZCV_N 0x80000000u
#define NZCV_Z 0x40000000u
#define NZCV_C 0x20000000u
#define NZCV_V 0x10000000u

/* AddWithCarry(x, y, carry_in): returns the sum, writes NZCV to *flags. */
static inline uint64_t awc64(uint64_t a, uint64_t b, unsigned c, uint64_t *flags) {
    unsigned __int128 us = (unsigned __int128)a + b + c;
    __int128 ss = (__int128)(int64_t)a + (int64_t)b + c;
    uint64_t r = (uint64_t)us;
    *flags = (r >> 63) << 31 | (uint64_t)(r == 0) << 30 | (uint64_t)((us >> 64) & 1) << 29 |
             (uint64_t)((__int128)(int64_t)r != ss) << 28;
    return r;
}

static inline uint32_t awc32(uint32_t a, uint32_t b, unsigned c, uint64_t *flags) {
    uint64_t us = (uint64_t)a + b + c;
    int64_t ss = (int64_t)(int32_t)a + (int32_t)b + c;
    uint32_t r = (uint32_t)us;
    *flags = (uint64_t)(r >> 31) << 31 | (uint64_t)(r == 0) << 30 | ((us >> 32) & 1) << 29 |
             (uint64_t)((int64_t)(int32_t)r != ss) << 28;
    return r;
}

/* NZ from a result, C and V cleared (logical ops with S). */
static inline uint64_t nz64(uint64_t r) { return (r >> 63) << 31 | (uint64_t)(r == 0) << 30; }
static inline uint64_t nz32(uint32_t r) { return (uint64_t)(r >> 31) << 31 | (uint64_t)(r == 0) << 30; }

/* ConditionHolds(cond) for cond = 0..15 (EQ NE CS CC MI PL VS VC HI LS GE LT GT LE AL NV). */
static inline int cond_holds(uint64_t nzcv, unsigned cond) {
    int n = (nzcv >> 31) & 1, z = (nzcv >> 30) & 1, c = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
    int r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static inline uint64_t ror64(uint64_t v, unsigned s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }
static inline uint32_t ror32(uint32_t v, unsigned s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline uint64_t asr64(uint64_t v, unsigned s) { return (uint64_t)((int64_t)v >> s); }
static inline uint32_t asr32(uint32_t v, unsigned s) { return (uint32_t)((int32_t)v >> s); }

static inline uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
/* CountLeadingSignBits: CLZ of x<N-1:1> EOR x<N-2:0>, an (N-1)-bit value. */
static inline uint64_t cls64(uint64_t v) { return clz64((v ^ (v >> 1)) & 0x7FFFFFFFFFFFFFFFull) - 1; }
static inline uint32_t cls32(uint32_t v) { return clz32((v ^ (v >> 1)) & 0x7FFFFFFFu) - 1; }

static inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return __builtin_bswap64(v);
}
static inline uint32_t rbit32(uint32_t v) { return (uint32_t)(rbit64(v) >> 32); }

static inline uint64_t rev16_64(uint64_t v) {
    return ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
}
static inline uint32_t rev16_32(uint32_t v) { return ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8); }
static inline uint64_t rev32_64(uint64_t v) {
    return (uint64_t)__builtin_bswap32((uint32_t)v) | (uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32;
}

static inline uint64_t udiv64(uint64_t n, uint64_t m) { return m ? n / m : 0; }
static inline uint32_t udiv32(uint32_t n, uint32_t m) { return m ? n / m : 0; }
static inline uint64_t sdiv64(uint64_t n, uint64_t m) {
    if (m == 0) return 0;
    if ((int64_t)m == -1) return 0 - n; /* avoids INT64_MIN / -1 overflow in C */
    return (uint64_t)((int64_t)n / (int64_t)m);
}
static inline uint32_t sdiv32(uint32_t n, uint32_t m) {
    if (m == 0) return 0;
    if ((int32_t)m == -1) return 0 - n;
    return (uint32_t)((int32_t)n / (int32_t)m);
}

static inline uint64_t umulh64(uint64_t a, uint64_t b) {
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
}
static inline uint64_t smulh64(uint64_t a, uint64_t b) {
    return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64);
}

/* Guest memory. Guest addresses are host addresses (decision 0005), so these are plain
 * host accesses. memcpy keeps them legal for unaligned addresses and under strict aliasing;
 * compilers turn each into one load/store instruction. Assumes a little-endian host. */
static inline uint8_t ld8(uint64_t a) { return *(const uint8_t *)(uintptr_t)a; }
static inline uint16_t ld16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static inline uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline void st8(uint64_t a, uint8_t v) { *(uint8_t *)(uintptr_t)a = v; }
static inline void st16(uint64_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, 2); }
static inline void st32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void st64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

#include "yw4fp.h"

#endif
