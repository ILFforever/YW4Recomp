/* Test-case I/O shared by the Pi harness (harness.c) and the host checker (recomp/check.py).
 *
 * Input line, all hex:   <enc> <x0> ... <x30> <sp> <nzcv> <v0> ... <v31> <fpcr> <inmask> <outmask>
 * Output line, all hex:  <enc> <x0> ... <x30> <sp> <nzcv> <v0> ... <v31> <memory changes>
 *
 * Each v register is 32 hex digits, high half first. v31 is printed as 0: the harness
 * uses it to park its own state, so SIMD tests never use v31. FPSR (cumulative exception
 * flags) is not compared; main never reads it.
 *
 * Memory: both machines have a TEST_MEM_SIZE scratch buffer filled with the same byte
 * pattern. Registers set in inmask (bit n = xn, bit 31 = SP) hold offsets into the buffer;
 * the buffer's address is added to them before the instruction runs and subtracted from
 * the registers in outmask afterwards. That way the results don't depend on where each
 * machine put its buffer. Memory changes are "<offset>:<byte>" pairs joined by commas,
 * or "-" for none; the buffer is reset after each case.
 */
#ifndef TESTCASE_H
#define TESTCASE_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MEM_SIZE 0x20000  /* must match MEM_SIZE in recomp/golden.py */

/* Layout must match trampoline.S. */
typedef struct {
    uint64_t x[31];     /* 0x000 */
    uint64_t sp;        /* 0x0F8 */
    uint64_t nzcv;      /* 0x100 */
    uint64_t pad;       /* 0x108 */
    uint64_t v[32][2];  /* 0x110  [lo, hi] */
    uint64_t fpcr;      /* 0x310 */
    uint64_t fpsr;      /* 0x318 */
    uint64_t host_sp;   /* 0x320  harness use only */
} TestState;

static uint8_t test_mem[TEST_MEM_SIZE] __attribute__((aligned(64)));
static uint8_t test_mem_ref[TEST_MEM_SIZE];

static void test_mem_init(void) {
    for (uint32_t i = 0; i < TEST_MEM_SIZE; i++) {
        uint32_t h = i * 2654435761u;
        test_mem_ref[i] = (uint8_t)(h >> 24) ^ (uint8_t)i;
    }
    memcpy(test_mem, test_mem_ref, TEST_MEM_SIZE);
}

__attribute__((unused)) static void test_rebase(TestState *s, uint64_t mask, uint64_t delta) {
    for (int i = 0; i < 31; i++)
        if (mask >> i & 1) s->x[i] += delta;
    if (mask >> 31 & 1) s->sp += delta;
}

/* Returns 0 at end of input. */
__attribute__((unused)) static int test_read(FILE *f, uint32_t *enc, TestState *s, uint64_t *inmask, uint64_t *outmask) {
    char line[4096];
    if (!fgets(line, sizeof line, f)) return 0;
    memset(s, 0, sizeof *s);
    char *p = line;
    *enc = (uint32_t)strtoul(p, &p, 16);
    for (int i = 0; i < 31; i++) s->x[i] = strtoull(p, &p, 16);
    s->sp = strtoull(p, &p, 16);
    s->nzcv = strtoull(p, &p, 16);
    for (int i = 0; i < 32; i++) {
        while (*p == ' ') p++;
        char hi[17] = {0};
        memcpy(hi, p, 16);
        s->v[i][1] = strtoull(hi, NULL, 16);
        s->v[i][0] = strtoull(p + 16, &p, 16);
    }
    s->fpcr = strtoull(p, &p, 16);
    *inmask = strtoull(p, &p, 16);
    *outmask = strtoull(p, &p, 16);
    return 1;
}

/* Prints the changed buffer bytes ("-" if none) and resets them. */
static void test_write_mem(FILE *f) {
    int any = 0;
    for (uint32_t c = 0; c < TEST_MEM_SIZE; c += 64) {
        if (!memcmp(test_mem + c, test_mem_ref + c, 64)) continue;
        for (uint32_t i = c; i < c + 64; i++) {
            if (test_mem[i] == test_mem_ref[i]) continue;
            fprintf(f, "%s%" PRIx32 ":%02x", any ? "," : "", i, test_mem[i]);
            test_mem[i] = test_mem_ref[i];
            any = 1;
        }
    }
    fprintf(f, "%s\n", any ? "" : "-");
}

__attribute__((unused)) static void test_write(FILE *f, uint32_t enc, const TestState *s) {
    fprintf(f, "%08" PRIx32, enc);
    for (int i = 0; i < 31; i++) fprintf(f, " %" PRIx64, s->x[i]);
    fprintf(f, " %" PRIx64 " %" PRIx64, s->sp, s->nzcv);
    for (int i = 0; i < 31; i++) fprintf(f, " %016" PRIx64 "%016" PRIx64, s->v[i][1], s->v[i][0]);
    fprintf(f, " %032x ", 0);
    test_write_mem(f);
}

#endif
