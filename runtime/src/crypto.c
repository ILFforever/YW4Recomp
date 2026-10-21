/* ARMv8 Crypto extension: AESE/AESD/AESMC/AESIMC, SHA1*, SHA256* (decision 0014).
 *
 * Written from the Arm ARM pseudocode (DDI 0487, "Shared/functions/crypto"). Used by the
 * sdk's nn::crypto. Checked like every other instruction: golden results from the Pi.
 *
 * AES state: register byte i is state[row i % 4][column i / 4], as in FIPS-197's input
 * order. The S-box is computed from its definition (inverse in GF(2^8), then the affine
 * map) on first use rather than typed in.
 */
#include "yw4cpu.h"

static uint8_t sbox[256], inv_sbox[256];

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = (uint8_t)(a << 1) ^ (a & 0x80 ? 0x1B : 0);
        b >>= 1;
    }
    return r;
}

static void init_sbox(void) {
    if (sbox[0]) return;  /* sbox[0] = 0x63 once built */
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        for (int y = 1; y < 256 && x; y++)
            if (gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        uint8_t s = inv;
        for (int k = 1; k < 5; k++) s ^= (uint8_t)((inv << k) | (inv >> (8 - k)));
        s ^= 0x63;
        sbox[x] = s;
        inv_sbox[s] = (uint8_t)x;
    }
}

static void mix_columns(uint8_t *out, const uint8_t *in, int inverse) {
    static const uint8_t fwd[4] = {2, 3, 1, 1}, inv[4] = {14, 11, 13, 9};
    const uint8_t *k = inverse ? inv : fwd;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            uint8_t v = 0;
            for (int j = 0; j < 4; j++) v ^= gmul(k[(j - r + 4) % 4], in[4 * c + j]);
            out[4 * c + r] = v;
        }
}

void yw4_aes(Yw4AesOp op, v128 *d, const v128 *n) {
    init_sbox();
    uint8_t s[16], t[16];
    if (op == YW4_AESMC || op == YW4_AESIMC) {
        memcpy(s, n->b, 16);
        mix_columns(t, s, op == YW4_AESIMC);
        memcpy(d->b, t, 16);
        return;
    }
    for (int i = 0; i < 16; i++) s[i] = d->b[i] ^ n->b[i];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            if (op == YW4_AESE)  /* ShiftRows, then SubBytes */
                t[r + 4 * c] = sbox[s[r + 4 * ((c + r) % 4)]];
            else                 /* InvShiftRows, then InvSubBytes */
                t[r + 4 * ((c + r) % 4)] = inv_sbox[s[r + 4 * c]];
        }
    memcpy(d->b, t, 16);
}

static uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static uint32_t ror32c(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }
static uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t sha_majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }

/* SHA1C/P/M: X = abcd (Qd), Y = e (Sn), W = 4 words (Vm). */
static void sha1_hash(v128 *d, const v128 *n, const v128 *m, uint32_t (*f)(uint32_t, uint32_t, uint32_t)) {
    uint32_t x[4] = {d->s[0], d->s[1], d->s[2], d->s[3]}, y = n->s[0];
    for (int e = 0; e < 4; e++) {
        uint32_t t = f(x[1], x[2], x[3]);
        y = y + rol32(x[0], 5) + t + m->s[e];
        x[1] = rol32(x[1], 30);
        /* <Y, X> = ROL(Y:X, 32) */
        uint32_t top = x[3];
        x[3] = x[2], x[2] = x[1], x[1] = x[0], x[0] = y;
        y = top;
    }
    for (int i = 0; i < 4; i++) d->s[i] = x[i];
}

/* SHA256hash(X, Y, W, part1): returns X if part1, else Y. */
static void sha256_hash(uint32_t *out, const uint32_t *xin, const uint32_t *yin, const uint32_t *w, int part1) {
    uint32_t x[4], y[4];
    memcpy(x, xin, 16);
    memcpy(y, yin, 16);
    for (int e = 0; e < 4; e++) {
        uint32_t chs = sha_choose(y[0], y[1], y[2]);
        uint32_t maj = sha_majority(x[0], x[1], x[2]);
        uint32_t t = y[3] + (ror32c(y[0], 6) ^ ror32c(y[0], 11) ^ ror32c(y[0], 25)) + chs + w[e];
        x[3] = t + x[3];
        y[3] = t + (ror32c(x[0], 2) ^ ror32c(x[0], 13) ^ ror32c(x[0], 22)) + maj;
        /* <Y, X> = ROL(Y:X, 32): X's top word moves into Y's bottom, Y's top into X's bottom */
        uint32_t xt = x[3], yt = y[3];
        x[3] = x[2], x[2] = x[1], x[1] = x[0], x[0] = yt;
        y[3] = y[2], y[2] = y[1], y[1] = y[0], y[0] = xt;
    }
    memcpy(out, part1 ? x : y, 16);
}

void yw4_sha(Yw4ShaOp op, v128 *d, const v128 *n, const v128 *m) {
    v128 r = {{0}};
    switch (op) {
    case YW4_SHA1C: sha1_hash(d, n, m, sha_choose); return;
    case YW4_SHA1P: sha1_hash(d, n, m, sha_parity); return;
    case YW4_SHA1M: sha1_hash(d, n, m, sha_majority); return;
    case YW4_SHA1SU0:  /* (Vn<63:0> : Vd<127:64>) ^ Vd ^ Vm */
        r.d[0] = d->d[1], r.d[1] = n->d[0];
        r.d[0] ^= d->d[0] ^ m->d[0], r.d[1] ^= d->d[1] ^ m->d[1];
        break;
    case YW4_SHA1SU1: {  /* T = Vd ^ (Vn >> 32) */
        uint32_t t[4] = {d->s[0] ^ n->s[1], d->s[1] ^ n->s[2], d->s[2] ^ n->s[3], d->s[3]};
        r.s[0] = rol32(t[0], 1), r.s[1] = rol32(t[1], 1), r.s[2] = rol32(t[2], 1);
        r.s[3] = rol32(t[3], 1) ^ rol32(t[0], 2);
        break;
    }
    case YW4_SHA1H: r.s[0] = rol32(n->s[0], 30); break;
    case YW4_SHA256H: sha256_hash(r.s, d->s, n->s, m->s, 1); break;
    case YW4_SHA256H2: sha256_hash(r.s, n->s, d->s, m->s, 0); break;
    case YW4_SHA256SU0: {  /* T = Vn<31:0> : Vd<127:32> */
        uint32_t t[4] = {d->s[1], d->s[2], d->s[3], n->s[0]};
        for (int e = 0; e < 4; e++)
            r.s[e] = (ror32c(t[e], 7) ^ ror32c(t[e], 18) ^ (t[e] >> 3)) + d->s[e];
        break;
    }
    case YW4_SHA256SU1: {  /* T0 = Vm<31:0> : Vn<127:32>, T1 = Vm<127:64> */
        uint32_t t0[4] = {n->s[1], n->s[2], n->s[3], m->s[0]};
        uint32_t t1[2] = {m->s[2], m->s[3]};
        for (int e = 0; e < 4; e++) {
            uint32_t elt = e < 2 ? t1[e] : r.s[e - 2];
            elt = ror32c(elt, 17) ^ ror32c(elt, 19) ^ (elt >> 10);
            r.s[e] = elt + d->s[e] + t0[e];
        }
        break;
    }
    }
    *d = r;
}
