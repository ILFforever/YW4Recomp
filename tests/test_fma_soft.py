"""musl's software fma (third_party/musl/src/math/fma.c), the FMADD of CPUs without FMA3 (runtime/include/yw4fp.h),
against the FMA3 instruction, which the Pi's golden results already check (recomp.check fp_dp3). Bit for bit on
millions of inputs aimed at the hard cases: every exponent, subnormals, and x*y close to -z (cancellation).
Skipped on a PC without FMA3 (nothing to compare with) or not x86-64."""

from __future__ import annotations

import platform
import subprocess
import tempfile
import unittest
from pathlib import Path

from recomp import check

DRIVER = r"""
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

double musl_fma(double, double, double);
__attribute__((target("fma"))) static double hw(double a, double b, double c) {
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
}
static uint64_t s = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static double d_of(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t u_of(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double any(void) {  /* every class: random bits, small exponents (subnormal results), near 1 */
    uint64_t r = rnd();
    switch (r & 3) {
    case 0: return d_of(rnd());
    case 1: return d_of((rnd() & 0x800fffffffffffffull) | (uint64_t)(rnd() % 120) << 52);  /* tiny */
    case 2: return d_of((rnd() & 0x800fffffffffffffull) | (uint64_t)(1023 - 30 + rnd() % 60) << 52);
    default: return d_of((rnd() & 0x800fffffffffffffull) | (uint64_t)(2047 - 1 - rnd() % 60) << 52);  /* huge */
    }
}
int main(void) {
    if (!__builtin_cpu_supports("fma")) { puts("skip"); return 0; }
    long bad = 0, n = 0;
    for (long i = 0; i < 4000000; i++) {
        double a = any(), b = any(), c;
        if (i & 1) c = any();
        else {  /* cancellation: c is -(a*b) moved by a few units in the last place */
            c = -(a * b);
            c = d_of(u_of(c) + (int64_t)(rnd() % 9) - 4);
        }
        double h = hw(a, b, c), m = musl_fma(a, b, c);
        n++;
        if (h != h && m != m) continue;  /* both NaN: yw4fp.h picks the NaN itself */
        if (u_of(h) != u_of(m) && bad++ < 10)
            printf("fma(%016llx, %016llx, %016llx): FMA3 %016llx, musl %016llx\n", (unsigned long long)u_of(a),
                   (unsigned long long)u_of(b), (unsigned long long)u_of(c), (unsigned long long)u_of(h),
                   (unsigned long long)u_of(m));
    }
    printf("%ld of %ld differ\n", bad, n);
    return bad != 0;
}
"""


@unittest.skipUnless(platform.machine().lower() in ("amd64", "x86_64"), "x86-64 only")
class SoftFmaTest(unittest.TestCase):
    def test_musl_fma_matches_fma3(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(DRIVER)
            objs = check.musl_fma_objects(t)
            exe = t / "fma.exe"
            # -ffp-contract=off: the a*b in the cancellation inputs must stay a plain multiply
            subprocess.run([check.host_cc(), "-std=c11", "-O2", "-ffp-contract=off", "-o", str(exe), str(t / "drv.c"),
                            *objs], check=True)
            got = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
            if got.stdout.strip() == "skip":
                self.skipTest("this CPU has no FMA3")
            self.assertEqual(got.returncode, 0, got.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
