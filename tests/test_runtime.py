"""Tests for runtime C code that don't need the game: compiled with the host compiler."""

from __future__ import annotations

import hashlib
import random
import subprocess
import tempfile
import unittest
from pathlib import Path

from recomp import check

RUNTIME = check.REPO / "runtime"

SHA_DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "yw4load.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) return 1;
    char out[65];
    yw4_sha256_hex(b, (size_t)n, out);
    puts(out);
    return 0;
}
"""


class Sha256Test(unittest.TestCase):
    """The runtime's version lock hashes main.bin itself; it must agree with hashlib."""

    def test_matches_hashlib(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(SHA_DRIVER)
            exe = t / "sha.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", f"-I{RUNTIME / 'include'}", "-o", str(exe),
                            str(t / "drv.c"), str(RUNTIME / "src" / "sha256.c")], check=True, capture_output=True)
            rng = random.Random(1)
            # lengths around the 55/56/64-byte padding boundaries, plus a big one
            for n in (0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 1000, 100_003):
                data = bytes(rng.getrandbits(8) for _ in range(n))
                (t / "in.bin").write_bytes(data)
                got = subprocess.run([str(exe), str(t / "in.bin")], capture_output=True, text=True, check=True)
                self.assertEqual(got.stdout.strip(), hashlib.sha256(data).hexdigest(), f"length {n}")


HEAP_DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#define REGION (64u << 20)
#define SLOTS 4000
static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }
static struct { unsigned char *p; size_t n; unsigned char fill; } s[SLOTS];
#define FAIL(...) do { printf(__VA_ARGS__); puts(""); return 1; } while (0)
static int check_live(void) {  /* contents intact, no overlaps (sorted sweep) */
    static int idx[SLOTS]; int k = 0;
    for (int i = 0; i < SLOTS; i++) if (s[i].p) {
        for (size_t j = 0; j < s[i].n; j++) if (s[i].p[j] != s[i].fill) return -1;
        idx[k++] = i;
    }
    for (int a = 1; a < k; a++) for (int b = a; b > 0 && s[idx[b-1]].p > s[idx[b]].p; b--) { int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t; }
    for (int a = 1; a < k; a++) if (s[idx[a-1]].p + s[idx[a-1]].n > s[idx[a]].p) return -2;
    return 0;
}
int main(void) {
    unsigned char *region = malloc(REGION + 64);
    Yw4Heap *h = yw4_heap_create(region + 3, REGION);  /* unaligned on purpose */
    if (!h) FAIL("create failed");
    size_t empty = yw4_heap_total_free(h);
    if (yw4_heap_largest_free(h) != empty) FAIL("fresh heap: largest %zu != free %zu", yw4_heap_largest_free(h), empty);
    for (int step = 0; step < 300000; step++) {
        int i = rnd() % SLOTS;
        unsigned op = rnd() % 8;
        size_t n = (rnd() % 4 == 0) ? rnd() % 200000 : rnd() % 300;
        if (!s[i].p && op < 6) {
            size_t align = (op == 5) ? (size_t)1 << (rnd() % 13) : 16;
            unsigned char *q = yw4_heap_alloc(h, n, align);
            if (!q) continue;
            if ((size_t)q % (align < 16 ? 16 : align)) FAIL("misaligned %p align %zu", (void *)q, align);
            s[i].p = q; s[i].n = n; s[i].fill = (unsigned char)rnd();
            memset(q, s[i].fill, n);
        } else if (s[i].p && op < 4) {
            if (yw4_heap_free(h, s[i].p)) FAIL("free of live block refused");
            if (yw4_heap_free(h, s[i].p) == 0) FAIL("double free accepted");
            s[i].p = NULL;
        } else if (s[i].p) {
            unsigned char *q = yw4_heap_realloc(h, s[i].p, n);
            if (!q) continue;
            for (size_t j = 0; j < (n < s[i].n ? n : s[i].n); j++) if (q[j] != s[i].fill) FAIL("realloc lost data");
            s[i].p = q; s[i].n = n; memset(q, s[i].fill, n);
        }
        if (step % 20000 == 0) {
            int r = check_live();
            if (r) FAIL("step %d: %s", step, r == -1 ? "contents corrupted" : "blocks overlap");
            if (yw4_heap_largest_free(h) > yw4_heap_total_free(h)) FAIL("largest > total free");
        }
    }
    if (check_live()) FAIL("final check failed");
    if (yw4_heap_free(h, region + 4096 + 16) == 0) FAIL("stray pointer accepted");
    for (int i = 0; i < SLOTS; i++) if (s[i].p && yw4_heap_free(h, s[i].p)) FAIL("final free refused");
    if (yw4_heap_total_free(h) != empty || yw4_heap_largest_free(h) != empty)
        FAIL("not fully merged: free %zu largest %zu, expected %zu", yw4_heap_total_free(h), yw4_heap_largest_free(h), empty);
    if (yw4_heap_alloc(h, empty, 16) == NULL) FAIL("whole heap not allocatable");
    if (yw4_heap_alloc(h, 1, 16) != NULL) FAIL("allocation from a full heap");
    puts("ok");
    return 0;
}
"""


class HeapTest(unittest.TestCase):
    """The allocator behind nn::mem::StandardAllocator (runtime/src/heap.c, decision 0011)."""

    def test_random_operations(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(HEAP_DRIVER)
            exe = t / "heap.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", f"-I{RUNTIME / 'include'}",
                            f"-I{RUNTIME / 'src'}", "-o", str(exe), str(t / "drv.c"), str(RUNTIME / "src" / "heap.c")],
                           check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(got.stdout.strip(), "ok")


STRING_DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "hle.h"
static CPUState c;
static long long call(GuestFn f, uint64_t a, uint64_t b, uint64_t n) {
    c.x[0] = a; c.x[1] = b; c.x[2] = n; f(&c); return (long long)c.x[0];
}
#define S(p) ((uint64_t)(uintptr_t)(p))
#define EXPECT(e, want) do { long long g_ = (e); if (g_ != (want)) { printf("%s = %lld, want %lld\n", #e, g_, (long long)(want)); return 1; } } while (0)
int main(void) {
    /* musl returns the byte difference, as unsigned chars */
    EXPECT(call(hle_libc_strcmp, S("abc"), S("abd"), 0), 'c' - 'd');
    EXPECT(call(hle_libc_strcmp, S("\xff"), S("a"), 0), 0xff - 'a');
    EXPECT(call(hle_libc_strcmp, S("ab"), S("abc"), 0), -'c');
    EXPECT(call(hle_libc_memcmp, S("a\x80"), S("a\x01"), 2), 0x7f);
    EXPECT(call(hle_libc_memcmp, S("xyz"), S("abc"), 0), 0);
    EXPECT(call(hle_libc_strncmp, S("abcx"), S("abcy"), 3), 0);
    EXPECT(call(hle_libc_strncmp, S("abcx"), S("abcy"), 4), 'x' - 'y');
    EXPECT(call(hle_libc_strncmp, S("a"), S("b"), 0), 0);
    EXPECT(call(hle_libc_strcasecmp, S("HeLLo"), S("hello"), 0), 0);
    EXPECT(call(hle_libc_strcasecmp, S("A"), S("b"), 0), 'a' - 'b');
    /* "C" locale ctype: nothing above 127, EOF in no class */
    EXPECT(call(hle_libc_isspace, 0xa0, 0, 0), 0);
    EXPECT(call(hle_libc_isspace, '\v', 0, 0), 1);
    EXPECT(call(hle_libc_isalnum, '@', 0, 0), 0);
    EXPECT(call(hle_libc_isalnum, 'Z', 0, 0), 1);
    EXPECT(call(hle_libc_ispunct, '~', 0, 0), 1);
    EXPECT(call(hle_libc_ispunct, ' ', 0, 0), 0);
    EXPECT(call(hle_libc_isxdigit, 'g', 0, 0), 0);
    EXPECT(call(hle_libc_isdigit, (uint32_t)-1, 0, 0), 0);
    EXPECT(call(hle_libc_toupper, 0xe9, 0, 0), 0xe9);
    EXPECT(call(hle_libc_tolower, 'Q', 0, 0), 'q');
    /* integers: guest long is 64-bit; atoi wraps like musl */
    EXPECT(call(hle_libc_atoi, S("  -42z"), 0, 0), -42);
    EXPECT(call(hle_libc_atoi, S("2147483648"), 0, 0), -2147483647 - 1);
    EXPECT(call(hle_libc_abs, (uint32_t)-7, 0, 0), 7);
    char *end = 0;
    EXPECT(call(hle_libc_strtol, S("0x7fffffffff!"), S(&end), 0), 0x7fffffffffLL);
    EXPECT(*end, '!');
    EXPECT(call(hle_libc_strtoul, S("-1"), 0, 10), -1);
    EXPECT(call(hle_libc_strtol, S("99999999999999999999"), 0, 10), 0x7fffffffffffffffLL);
    call(hle_libc_errno_location, 0, 0, 0);
    EXPECT(*(int *)(uintptr_t)c.x[0], 34);
    char buf[] = "a,,b;c";
    EXPECT(call(hle_libc_strtok, S(buf), S(",;"), 0), S(buf));
    EXPECT(call(hle_libc_strtok, 0, S(",;"), 0), S(buf + 3));
    EXPECT(call(hle_libc_strtok, 0, S(",;"), 0), S(buf + 5));
    EXPECT(call(hle_libc_strtok, 0, S(",;"), 0), 0);
    EXPECT(call(hle_libc_strnlen, S("abcdef"), 4, 0), 4);
    unsigned char m[8] = {0};
    EXPECT(call(hle_libc_memset_s, S(m), 4, 0x55) , 0);  /* x3 = 0 here: n = 0 */
    c.x[0] = S(m); c.x[1] = 4; c.x[2] = 0x55; c.x[3] = 8; hle_libc_memset_s(&c);
    EXPECT((long long)c.x[0], 22);
    EXPECT(m[3] == 0x55 && m[4] == 0, 1);
    puts("ok");
    return 0;
}
"""


class StringTest(unittest.TestCase):
    """C library functions where musl's exact result is reproduced (hle_string.c, decision 0012)."""

    def test_musl_conventions(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(STRING_DRIVER)
            exe = t / "str.exe"
            subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", f"-I{RUNTIME / 'include'}",
                            f"-I{RUNTIME / 'src'}", "-o", str(exe), str(t / "drv.c"),
                            str(RUNTIME / "src" / "hle_string.c")], check=True, capture_output=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(got.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
