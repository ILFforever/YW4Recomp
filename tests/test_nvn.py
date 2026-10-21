"""The Tegra texture layout behind NVN storage sizes and WriteTexels (runtime/src/tegra.c, decision 0017).

The game sizes its texture memory from nvnTextureBuilderGetStorageSize, so a wrong size
silently overlaps textures. Checked against an independent Python model of the
block-linear layout and against sizes worked out by hand.
"""

from __future__ import annotations

import random
import subprocess
import tempfile
import unittest
from pathlib import Path

from recomp import check

RUNTIME = check.REPO / "runtime"

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "tegra.h"
int main(int argc, char **argv) {
    if (argc == 7) {  /* size <format> <width> <height> <depth> <layers> <levels> */
        TexLayout t = {0};
        t.fmt = nvn_format_info((uint32_t)strtoul(argv[1], 0, 0));
        t.width = atoi(argv[2]), t.height = atoi(argv[3]), t.depth = atoi(argv[4]);
        t.layers = atoi(argv[5]), t.levels = atoi(argv[6]);
        tegra_layout_init(&t);
        printf("%llu %u\n", (unsigned long long)tegra_total_size(&t), t.block_height_log2);
        return 0;
    }
    /* swizzle <wb> <hb> <bpb> <bh>: linear bytes 0,1,2... -> block-linear, printed as hex */
    uint32_t wb = atoi(argv[1]), hb = atoi(argv[2]), bpb = atoi(argv[3]), bh = atoi(argv[4]);
    size_t n = (size_t)wb * hb * bpb, out = 1 << 20;
    unsigned char *src = malloc(n), *dst = calloc(1, out);
    for (size_t i = 0; i < n; i++) src[i] = (unsigned char)(i * 7 + 3);
    tegra_swizzle(src, dst, wb, hb, 1, bpb, bh, 0);
    for (size_t i = 0; i < out; i++) printf("%02x", dst[i]);
    return 0;
}
"""


def gob_offset(x: int, y: int, width_bytes: int, bh: int) -> int:
    """Byte (x, y) of a 2D block-linear surface, from the Tegra X1 layout: 64x8-byte GOBs of
    four 16x2 sectors pairs, 2^bh GOBs stacked per block, blocks in rows."""
    gobs_wide = (width_bytes + 63) // 64
    block_rows = 8 << bh
    block = (y // block_rows) * gobs_wide + x // 64
    gob_in_block = (y % block_rows) // 8
    xi, yi = x % 64, y % 8
    in_gob = (xi // 32) * 256 + (yi // 2) * 64 + ((xi % 32) // 16) * 32 + (yi % 2) * 16 + xi % 16
    return block * (512 << bh) + gob_in_block * 512 + in_gob


class TegraTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        (t / "drv.c").write_text(DRIVER)
        cls.exe = t / "tegra.exe"
        subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", "-Werror", f"-I{RUNTIME / 'src'}", "-o",
                        str(cls.exe), str(t / "drv.c"), str(RUNTIME / "src" / "tegra.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def size(self, fmt: int, w: int, h: int, d: int = 1, layers: int = 1, levels: int = 1) -> tuple[int, int]:
        out = subprocess.run([str(self.exe), hex(fmt), str(w), str(h), str(d), str(layers), str(levels)],
                             capture_output=True, text=True, check=True).stdout.split()
        return int(out[0]), int(out[1])

    def test_sizes_by_hand(self):
        RGBA8, BC1 = 0x25, 0x41
        # 1920x1080 RGBA8: 7680 bytes = 120 GOBs wide; 135 GOBs high -> 16-GOB blocks, 9 block rows
        self.assertEqual(self.size(RGBA8, 1920, 1080), (120 * 512 * 16 * 9, 4))
        # 1x1 RGBA8: one GOB
        self.assertEqual(self.size(RGBA8, 1, 1), (512, 0))
        # 256x256 BC1: 64x64 blocks of 8 bytes = 512 bytes = 8 GOBs wide, 8 GOBs high -> one 8-GOB block row
        self.assertEqual(self.size(BC1, 256, 256), (8 * 512 * 8, 3))
        # mip chain 64x64 RGBA8, levels 64..1: each level is whole GOBs wide and whole blocks
        # high, and a level's block height shrinks while half a block still covers it
        lv = [(64, 64), (32, 32), (16, 16), (8, 8), (4, 4), (2, 2), (1, 1)]
        want = 0
        for w, h in lv:
            gw = (w * 4 + 63) // 64
            gh = (h + 7) // 8
            bh = 3  # level 0's block height, shrunk while a half-block still covers the level
            while bh > 0 and (8 << (bh - 1)) >= h:
                bh -= 1
            want += gw * 512 * (1 << bh) * -(-gh // (1 << bh))
        self.assertEqual(self.size(RGBA8, 64, 64, levels=7)[0], want)
        # 2D array: layers start on a block boundary
        single = self.size(RGBA8, 100, 20)[0]
        bh = self.size(RGBA8, 100, 20)[1]
        block = 512 << bh
        self.assertEqual(self.size(RGBA8, 100, 20, layers=3)[0], 3 * (-(-single // block) * block))

    def test_swizzle_matches_layout(self):
        rng = random.Random(4)
        for _ in range(12):
            wb, hb, bpb = rng.randint(1, 90), rng.randint(1, 70), rng.choice([1, 2, 4, 8, 16])
            if wb * bpb > 1000:
                continue
            bh = rng.randint(0, 4)
            out = bytes.fromhex(subprocess.run([str(self.exe), str(wb), str(hb), str(bpb), str(bh)],
                                               capture_output=True, text=True, check=True).stdout)
            for y in range(hb):
                for xb in range(wb * bpb):
                    i = y * wb * bpb + xb
                    self.assertEqual(out[gob_offset(xb, y, wb * bpb, bh)], (i * 7 + 3) & 0xFF,
                                     f"wb {wb} hb {hb} bpb {bpb} bh {bh} at ({xb}, {y})")


if __name__ == "__main__":
    unittest.main()
