"""Unit tests for whole-function translation that don't need the Pi
(hardware checks: python -m recomp.functest)."""

from __future__ import annotations

import struct
import unittest

from recomp import function


def code(*words: int) -> function.Code:
    return function.Code(struct.pack(f"<{len(words)}I", *words), 4 * len(words))


class DecodeFlowTest(unittest.TestCase):
    def test_branch_targets(self):
        self.assertEqual(function.decode_flow(0x14000002, 0x100).target, 0x108)   # b .+8
        self.assertEqual(function.decode_flow(0x97FFFFFF, 0x100).target, 0xFC)    # bl .-4
        self.assertEqual(function.decode_flow(0x97FFFFFF, 0x100).kind, "bl")
        self.assertEqual(function.decode_flow(0x54000040, 0x100).target, 0x108)   # b.eq .+8
        self.assertEqual(function.decode_flow(0xB4000040, 0x100).target, 0x108)   # cbz x0, .+8
        self.assertEqual(function.decode_flow(0x37F80040, 0x100).target, 0x108)   # tbnz w0, #31, .+8

    def test_conditions(self):
        self.assertEqual(function.decode_flow(0xB4000040, 0).cond, "((uint64_t)ctx->x[0]) == 0")
        self.assertEqual(function.decode_flow(0x37F80040, 0).cond, "(((((uint64_t)ctx->x[0])) >> 31) & 1) != 0")

    def test_register_branches(self):
        self.assertEqual(function.decode_flow(0xD65F03C0, 0).kind, "ret")
        self.assertEqual(function.decode_flow(0xD63F0100, 0), function.Flow("blr", reg=8))
        self.assertEqual(function.decode_flow(0xD61F0220, 0), function.Flow("br", reg=17))
        self.assertIsNone(function.decode_flow(0x8B020020, 0))                  # add x0, x1, x2


class PcRelativeTest(unittest.TestCase):
    def test_adrp_uses_the_page_of_the_instruction(self):
        # adrp x0, .+0x1000 at 0x1234 -> page 0x1000 + 0x1000
        self.assertEqual(function.emit_pc_relative(0xB0000000, 0x1234),
                         "ctx->x[0] = (uint64_t)(uint64_t)((yw4_image_base + 0x2000ull));")

    def test_adr_is_exact(self):
        self.assertIn("0x1238", function.emit_pc_relative(0x10000020, 0x1234))  # adr x0, .+4


class DiscoverTest(unittest.TestCase):
    def test_branch_to_other_entry_is_a_tail_call(self):
        # 0: cbz x0, 8 ; 4: b 12 (another entry) ; 8: ret ; 12: ret
        c = code(0xB4000040, 0x14000002, 0xD65F03C0, 0xD65F03C0)
        fn = function.discover(c, 0, {0, 12})
        self.assertEqual(fn.body, {0, 4, 8})
        src = function.emit_function(c, fn, {0, 12})
        self.assertIn("TAILCALL(f_c);", src)
        self.assertIn("goto L_8;", src)


if __name__ == "__main__":
    unittest.main()
