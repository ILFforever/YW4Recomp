/* Whole-function tests for the recompiler (recomp/functest.py).
 *
 * Built on the ARM64 reference machine into a flat image (flat.ld), run there natively
 * and, translated by recomp/function.py, on the host; results must match.
 *
 * Every test is `uint64_t t_<name>(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c)`.
 * buf points at 512 bytes of the shared scratch buffer (pi/testcase.h). Tests must be
 * deterministic for any input (bound loops by masking), must not return or store
 * addresses (they differ between machines), and must not call outside this image.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))
#define N 64 /* buf has 64 uint64_t */

typedef uint64_t (*binop)(uint64_t, uint64_t);

static const uint32_t primes[16] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53};

/* Loop with a data-dependent branch. */
uint64_t t_sum_odd(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t s = b;
    for (unsigned i = 0; i < (a & 63); i++)
        if (buf[i] & 1) s += buf[i] ^ c;
    return s;
}

/* Recursion through BL, stack frames. */
NOINLINE uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
uint64_t t_fib(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf; (void)b; (void)c;
    return fib(a & 15);
}

/* Division and remainder in a loop. */
uint64_t t_gcd(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf; (void)c;
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Nested loops, loads and stores. */
uint64_t t_sort(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)b; (void)c;
    unsigned n = (unsigned)(a & 31) + 1;
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j + 1 < n - i; j++)
            if ((int64_t)buf[j] > (int64_t)buf[j + 1]) {
                uint64_t t = buf[j];
                buf[j] = buf[j + 1];
                buf[j + 1] = t;
            }
    return buf[0];
}

/* Loop with an exit condition known only at run time. */
uint64_t t_collatz(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf; (void)b; (void)c;
    uint64_t n = (a & 0xFFFF) + 1, steps = 0;
    while (n != 1 && steps < 1000) {
        n = n & 1 ? 3 * n + 1 : n / 2;
        steps++;
    }
    return steps;
}

/* Bit tests (TBZ/TBNZ). */
uint64_t t_bits(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf;
    uint64_t r = 0;
    if (a & (1ull << 40)) r += b;
    if (!(a & 4)) r ^= c;
    if (a & 0x80000000u) r -= 7;
    for (unsigned i = 0; i < 64; i++)
        if (b >> i & 1) r += i;
    return r;
}

/* Indirect calls (BLR) through addresses computed PC-relatively. */
NOINLINE uint64_t op_add(uint64_t x, uint64_t y) { return x + y; }
NOINLINE uint64_t op_mul(uint64_t x, uint64_t y) { return x * y; }
NOINLINE uint64_t op_xor(uint64_t x, uint64_t y) { return x ^ y; }
NOINLINE uint64_t apply(binop f, uint64_t x, uint64_t y) { return f(x, y) + 1; }
/* Picks with branches, not a table: a table of pointers would need relocations. */
NOINLINE binop pick(uint64_t k) { return k == 0 ? op_add : k == 1 ? op_mul : op_xor; }
uint64_t t_indirect(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r = 0;
    for (unsigned i = 0; i < (a & 7); i++) {
        r = apply(pick((b + i) % 3), r, c + i);
        buf[i] = r;
    }
    return r;
}

/* Tail calls: at -O2 these become `b other` (a tail call into another function). */
NOINLINE uint64_t tail_target(uint64_t x, uint64_t y) { return x * 3 + y; }
NOINLINE uint64_t tail_caller(uint64_t x, uint64_t y) { return tail_target(x + 1, y ^ 5); }
uint64_t t_tailcall(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf; (void)c;
    return tail_caller(a, b) + 2;
}

/* Many arguments: some go on the stack. */
NOINLINE uint64_t many(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                       uint64_t g, uint64_t h, uint64_t i, uint64_t j, uint32_t k, uint8_t l) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + k * 11u + l;
}
uint64_t t_stack_args(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    return many(a, b, c, buf[0], buf[1], a ^ b, b ^ c, a + c, buf[2], buf[3], (uint32_t)(a >> 7), (uint8_t)c);
}

/* Compare chains (the build disables jump tables; jumptable.S tests those). */
uint64_t t_switch(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf;
    switch (a & 15) {
    case 0: return b;
    case 1: return b + c;
    case 3: return b * c;
    case 7: return b - c;
    case 8: return b >> (c & 63);
    case 12: return ~b;
    default: return a;
    }
}

/* Byte and halfword access, signed and unsigned, post-increment loops. */
uint64_t t_bytes(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    uint8_t *p = (uint8_t *)buf;
    int16_t *h = (int16_t *)buf;
    int64_t s = 0;
    for (unsigned i = 0; i < (a & 255); i++) s += (int8_t)p[i] == (int8_t)b ? 1000 : (int8_t)p[i];
    for (unsigned i = 0; i < (c & 127); i++) s += h[i];
    for (unsigned i = 0; i < 16; i++) p[256 + i] = (uint8_t)(s >> i);
    return (uint64_t)s;
}

/* Read-only data (ADRP + LDR). */
uint64_t t_rodata(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf; (void)c;
    uint64_t r = 1;
    for (unsigned i = 0; i < (a & 15); i++) r = r * primes[(b + i) & 15] % 1000003;
    return r;
}

/* Wide multiplies (UMULH/SMULH) and 32-bit sign extension. */
uint64_t t_wide(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf;
    unsigned __int128 u = (unsigned __int128)a * b;
    __int128 s = (__int128)(int64_t)b * (int64_t)c;
    int32_t w = (int32_t)a * (int32_t)c;
    return (uint64_t)(u >> 64) ^ (uint64_t)(s >> 64) ^ (uint64_t)(int64_t)w;
}

/* Mutual recursion. */
NOINLINE uint64_t is_odd(uint64_t n);
NOINLINE uint64_t is_even(uint64_t n) { return n == 0 ? 1 : is_odd(n - 1); }
NOINLINE uint64_t is_odd(uint64_t n) { return n == 0 ? 0 : is_even(n - 1); }
uint64_t t_mutual(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    (void)buf; (void)b; (void)c;
    return is_even(a & 255) * 2 + is_odd((a >> 8) & 255);
}

/* Struct copy through the stack, conditional selects. */
typedef struct { uint64_t x, y; uint32_t z; uint16_t w; } Item;
NOINLINE Item make_item(uint64_t a, uint64_t b) {
    Item it = {a, b, (uint32_t)(a ^ b), (uint16_t)(a + b)};
    return it;
}
uint64_t t_struct(uint64_t *buf, uint64_t a, uint64_t b, uint64_t c) {
    Item best = make_item(a, b);
    for (unsigned i = 0; i < (c & 7); i++) {
        Item it = make_item(buf[i], buf[i + 1]);
        if (it.z > best.z) best = it;
    }
    return best.x + best.y + best.z + best.w;
}
