"""tools.yw4.bc7 (the shortcut wheel's icons, decision 0075) against Pillow's BC7 decoder, pixel for pixel."""

from __future__ import annotations

import random
import unittest

from tools.yw4 import bc7

try:
    from tools.yw4.gaiji import decode_bc7_to_image  # needs Pillow (a developer's tool; players' builds have none)
except ImportError:
    decode_bc7_to_image = None


@unittest.skipIf(decode_bc7_to_image is None, "Pillow not installed")
class Bc7AgainstPillow(unittest.TestCase):
    def test_every_mode(self) -> None:
        rng = random.Random(75)
        blocks = []
        for mode in range(8):  # random blocks of each mode: the mode is the lowest set bit of the first byte
            for _ in range(32):
                b = bytearray(rng.randbytes(16))
                b[0] = (b[0] & ~((2 << mode) - 1) & 0xff) | (1 << mode)
                blocks.append(bytes(b))
        blocks.append(bytes(16))  # no mode bit
        w = 4 * len(blocks)  # one row of blocks
        linear = b"".join(blocks)
        want = decode_bc7_to_image(linear, w, 4).convert("RGBA").tobytes()
        got = bc7.decode_rect(linear, w, 0, 0, w, 4)
        bad = [i // 4 for i in range(0, len(want), 4) if want[i:i + 4] != got[i:i + 4]]
        self.assertFalse(bad, f"{len(bad)} pixels differ, first in block {bad[0] % w // 4 if bad else 0}")


if __name__ == "__main__":
    unittest.main()
