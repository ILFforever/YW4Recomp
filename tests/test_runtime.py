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


if __name__ == "__main__":
    unittest.main()
