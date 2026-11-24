"""Tests for recomp/translate.py's pure parts, on made-up modules (no game files): symbol resolution,
weak symbols, override/hook/guest_symbol validation, PLT stub decoding, file writing."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from recomp import translate
from tools.yw4 import reloc

R = reloc.Reloc
STT_FUNC, STT_OBJECT = translate.STT_FUNC, 1


class Patched(unittest.TestCase):
    """raw_dynsym and read_relocs are replaced: a fake module's "image" is its list of dynsym rows
    (name, type, section index, value), and its relocations are looked up by that list's id."""

    def setUp(self):
        self._relocs = {}
        for p in (mock.patch.object(translate, "raw_dynsym", lambda image, info: image),
                  mock.patch.object(translate.reloc, "read_relocs", lambda image: self._relocs[id(image)])):
            p.start()
            self.addCleanup(p.stop)

    def make(self, name, syms=(), relocs=()):
        m = SimpleNamespace(name=name, image=list(syms), info={})
        self._relocs[id(m.image)] = list(relocs)
        return m


class ResolveRelocationsTest(Patched):
    def test_relative_symbolic_and_precedence(self):
        # main defines "dup" at 0x100; the sdk defines "dup" at 0x900 and "only_sdk" at 0x200.
        main = self.make("main", [("", 0, 0, 0), ("dup", STT_FUNC, 1, 0x100), ("only_sdk", STT_FUNC, 0, 0)],
                         [R(0x10, reloc.R_AARCH64_RELATIVE, 0, 0x40),
                          R(0x18, reloc.R_AARCH64_GLOB_DAT, 1, 0),
                          R(0x20, reloc.R_AARCH64_JUMP_SLOT, 2, 0)])
        sdk = self.make("sdk", [("", 0, 0, 0), ("dup", STT_FUNC, 1, 0x900), ("only_sdk", STT_FUNC, 1, 0x200)],
                        [R(0x30, reloc.R_AARCH64_ABS64, 1, 8)])
        rel, sym, unresolved = translate.resolve_relocations([main, sdk])
        self.assertEqual(rel, [[(0x10, 0x40)], []])
        # main's own definition wins (load order is search order); a symbol only the sdk defines goes to module 1
        self.assertEqual(sym[0], [(0x18, 0, 0x100), (0x20, 1, 0x200)])
        # in the sdk, "dup" binds to main (searched first), addend added
        self.assertEqual(sym[1], [(0x30, 0, 0x108)])
        self.assertEqual(sum(unresolved.values()), 0)

    def test_weak_undefined_becomes_zero(self):
        main = self.make("main", [("", 0, 0, 0), ("__start_marker", 0, 0, 0)],
                         [R(0x8, reloc.R_AARCH64_GLOB_DAT, 1, 0)])
        _, sym, unresolved = translate.resolve_relocations([main])
        self.assertEqual(sym[0], [(0x8, translate.NO_MODULE, 0)])
        self.assertEqual(unresolved["__start_marker"], 1)

    def test_addend_wraps_to_64_bits(self):
        main = self.make("main", [("", 0, 0, 0), ("f", STT_FUNC, 1, 0x10)],
                         [R(0x8, reloc.R_AARCH64_ABS64, 1, -0x20)])
        _, sym, _ = translate.resolve_relocations([main])
        self.assertEqual(sym[0][0][2], (0x10 - 0x20) & (2**64 - 1))

    def test_unsupported_relocation_type(self):
        main = self.make("main", [("", 0, 0, 0)], [R(0x8, 9999, 0, 0)])
        with self.assertRaisesRegex(RuntimeError, "unsupported relocation type 9999"):
            translate.resolve_relocations([main])


class OverrideTest(Patched):
    def setUp(self):
        super().setUp()
        self.mods = [self.make("main", [("memcpy", STT_FUNC, 1, 0x500), ("data", STT_OBJECT, 1, 0x600),
                                        ("undef", STT_FUNC, 0, 0)]),
                     self.make("sdk", [])]

    def toml(self, text):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        p = Path(d.name) / "overrides.toml"
        p.write_text(text)
        patcher = mock.patch.object(translate, "OVERRIDES", p)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_override_maps_offset_to_native(self):
        self.toml('[[override]]\nmodule="main"\nsymbol="memcpy"\nnative="my_memcpy"\n')
        self.assertEqual(translate.load_overrides(self.mods), {"main": {0x500: "my_memcpy"}, "sdk": {}})

    def test_override_unknown_module(self):
        self.toml('[[override]]\nmodule="nope"\nsymbol="memcpy"\nnative="x"\n')
        with self.assertRaisesRegex(RuntimeError, "no module 'nope'"):
            translate.load_overrides(self.mods)

    def test_override_symbol_must_be_a_defined_function(self):
        for sym in ("data", "undef", "missing"):  # an object, an import, and nothing
            self.toml(f'[[override]]\nmodule="main"\nsymbol="{sym}"\nnative="x"\n')
            with self.assertRaisesRegex(RuntimeError, "defines no function"):
                translate.load_overrides(self.mods)

    def test_hooks_entry_and_inside(self):
        self.toml('[[hook]]\nmodule="main"\noffset="0x1000"\nnative="h1"\n'
                  '[[hook]]\nmodule="main"\noffset="0x1000"\nat="0x1010"\nnative="h2"\n'
                  '[[hook]]\nmodule="sdk"\noffset="0x20"\nat="0x24"\nnative="h3"\n')
        entry, inside = translate.load_hooks(self.mods)
        self.assertEqual(entry, {"main": {0x1000: "h1"}, "sdk": {}})
        self.assertEqual(inside, {"main": {0x1000: {0x1010: "h2"}}, "sdk": {0x20: {0x24: "h3"}}})

    def test_hook_unknown_module(self):
        self.toml('[[hook]]\nmodule="x"\noffset="0x1"\nnative="h"\n')
        with self.assertRaisesRegex(RuntimeError, "no module 'x'"):
            translate.load_hooks(self.mods)

    def test_guest_symbols(self):
        self.toml('[[guest_symbol]]\nmodule="main"\nsymbol="memcpy"\n')
        self.assertEqual(translate.guest_symbols(self.mods), [("memcpy", 0, 0x500)])
        self.toml('[[guest_symbol]]\nmodule="main"\nsymbol="data"\n')
        with self.assertRaisesRegex(RuntimeError, "defines no function"):
            translate.guest_symbols(self.mods)
        self.toml('[[guest_symbol]]\nmodule="zzz"\nsymbol="memcpy"\n')
        with self.assertRaisesRegex(RuntimeError, "no module"):
            translate.guest_symbols(self.mods)

    def test_empty_file_gives_empty_tables(self):
        self.toml("")
        self.assertEqual(translate.load_overrides(self.mods), {"main": {}, "sdk": {}})
        self.assertEqual(translate.guest_symbols(self.mods), [])


class PltSlotTest(unittest.TestCase):
    STUB = [0x90000010,   # adrp x16, page
            0xF9400211,   # ldr  x17, [x16, #imm]
            0x91000210,   # add  x16, x16, #imm
            0xD61F0220]   # br   x17

    @staticmethod
    def code(words, text_end=None):
        data = b"".join(w.to_bytes(4, "little") for w in words)
        return SimpleNamespace(word=lambda off: int.from_bytes(data[off:off + 4], "little"),
                               text_end=len(data) if text_end is None else text_end)

    def test_decodes_slot(self):
        w = list(self.STUB)
        w[0] |= 1 << 29        # immlo=1 -> page +0x1000
        w[1] |= 3 << 10        # imm12=3 -> +24 bytes
        w[2] |= 3 << 10
        self.assertEqual(translate.plt_slot(self.code(w), 0), 0x1000 + 24)

    def test_not_a_stub(self):
        w = list(self.STUB)
        w[3] = 0xD503201F      # nop instead of br x17
        self.assertIsNone(translate.plt_slot(self.code(w), 0))

    def test_stub_past_text_end(self):
        self.assertIsNone(translate.plt_slot(self.code(self.STUB, text_end=8), 0))


class MiscTest(Patched):
    def test_setjmp_definition_is_found(self):
        m = self.make("main", [("_setjmp", STT_FUNC, 1, 0x40), ("other", STT_FUNC, 1, 0x80)])
        m.code = SimpleNamespace(text_end=0)
        self.assertEqual(translate.setjmp_entries([m], [[]], [set()]), [frozenset({0x40})])

    def test_write_if_changed_keeps_timestamp(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.c"
            translate.write_if_changed(p, "a")
            m1 = p.stat().st_mtime_ns
            translate.write_if_changed(p, "a")
            self.assertEqual(p.stat().st_mtime_ns, m1)
            translate.write_if_changed(p, "b")
            self.assertEqual(p.read_text(), "b")


if __name__ == "__main__":
    unittest.main()
