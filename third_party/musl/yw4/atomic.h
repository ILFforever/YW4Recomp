/* musl's atomic.h, the one part src/math/fma.c uses: count leading zeros of a 64-bit value (fma.c calls it only
 * with one that isn't 0). Our own file, as glue.h and endian.h (third_party/musl/README.md). */
#ifndef YW4_MUSL_ATOMIC_H
#define YW4_MUSL_ATOMIC_H
#include <stdint.h>
static inline int a_clz_64(uint64_t x) { return __builtin_clzll(x); }
#endif
