"""Unit tests for the lifter that don't need the Pi (hardware checks: python -m recomp.check)."""

from __future__ import annotations

import unittest

from recomp import lift


class BitMaskTest(unittest.TestCase):
    def test_known_immediates(self):
        # Values from the Arm ARM logical-immediate encoding (N, imms, immr) -> wmask
        cases = [
            ((0, 0b111100, 0), 32, 0x55555555),          # 01 repeated (2-bit element)
            ((0, 0b000111, 0), 32, 0xFF),                # 8 ones in a 32-bit element
            ((1, 0b000000, 0), 64, 0x1),                 # 1 one in a 64-bit element
            ((1, 0b111110, 1), 64, 0xBFFFFFFFFFFFFFFF),  # 63 ones rotated right by 1
        ]
        for (n, imms, immr), width, want in cases:
            self.assertEqual(lift.decode_bit_masks(n, imms, immr, True, width)[0], want)

    def test_reserved(self):
        self.assertIsNone(lift.decode_bit_masks(0, 0b111111, 0, True, 32))  # all-ones element
        self.assertIsNone(lift.decode_bit_masks(1, 0, 0, True, 32))         # 64-bit element in 32-bit op


class FormLookupTest(unittest.TestCase):
    KNOWN = {
        0x8B020020: "addsub_shift",   # add x0, x1, x2
        0xEB02003F: "addsub_shift",   # cmp x1, x2
        0x91000420: "addsub_imm",     # add x0, x1, #1
        0xD2800020: "movewide",       # mov x0, #1
        0xAA0103E0: "logical_shift",  # mov x0, x1
        0x9AC20C20: "dp2",            # sdiv x0, x1, x2
        0x9B027C20: "dp3",            # mul x0, x1, x2
        0x9A821020: "csel",           # csel x0, x1, x2, ne
        0xD3401C20: "bitfield",       # ubfx x0, x1, #0, #8 (uxtb)
    }

    def test_known_encodings(self):
        for enc, name in self.KNOWN.items():
            f = lift.find_form(enc)
            self.assertIsNotNone(f, hex(enc))
            self.assertEqual(f.name, name, hex(enc))
            self.assertTrue(f.valid(enc), hex(enc))

    def test_write_to_zero_register_is_dropped(self):
        # cmp x1, x2 writes only flags
        self.assertNotIn("ctx->x[31]", lift.lift(0xEB02003F))
        self.assertIn("ctx->nzcv", lift.lift(0xEB02003F))

    def test_unknown(self):
        with self.assertRaises(lift.LiftError):
            lift.lift(0x14000000)  # b . (branches are not single-instruction forms)


if __name__ == "__main__":
    unittest.main()
