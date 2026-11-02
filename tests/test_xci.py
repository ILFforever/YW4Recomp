"""Tests for the XCI reader on synthetic game-card images (no game files)."""

from __future__ import annotations

import io
import struct
import tempfile
import unittest
from pathlib import Path

from tools.yw4 import extract, xci


def make_hfs0(files: dict[str, bytes]) -> bytes:
    names, entries, data = b"", b"", b""
    for name, body in files.items():
        entries += struct.pack("<QQIIQ", len(data), len(body), len(names), min(len(body), 0x200), 0) + b"\0" * 0x20
        names += name.encode() + b"\0"
        data += body
    names += b"\0" * (-len(names) % 0x20)
    return b"HFS0" + struct.pack("<III", len(files), len(names), 0) + entries + names + data


def make_xci(secure: dict[str, bytes] | None, key_area: bool = False) -> bytes:
    parts = {"update": make_hfs0({"sys.nca": b"system"}), "normal": make_hfs0({})}
    if secure is not None:
        parts["secure"] = make_hfs0(secure)
    root = make_hfs0(parts)
    root_offset = 0xF000  # real cards put it here; anything past the header works
    head = bytearray(0x200)
    head[0x100:0x104] = b"HEAD"
    struct.pack_into("<QQ", head, 0x130, root_offset, 0x200)
    card = bytes(head) + b"\0" * (root_offset - len(head)) + root
    return (b"\xAA" * 0x1000 if key_area else b"") + card


class XciTest(unittest.TestCase):
    SECURE = {"0123.nca": b"program", "4567.cnmt.nca": b"meta", "89ab.tik": b"\1" * 0x40}

    def check(self, raw: bytes):
        f = io.BytesIO(raw)
        self.assertTrue(xci.is_xci(f))
        entries = {e.name: e for e in xci.secure_entries(f)}
        self.assertEqual(set(entries), set(self.SECURE))
        for name, body in self.SECURE.items():
            f.seek(entries[name].offset)
            self.assertEqual(f.read(entries[name].size), body)

    def test_secure_partition(self):
        self.check(make_xci(self.SECURE))

    def test_key_area_in_front(self):
        self.check(make_xci(self.SECURE, key_area=True))

    def test_not_an_xci(self):
        f = io.BytesIO(b"PFS0" + b"\0" * 0x2000)
        self.assertFalse(xci.is_xci(f))
        with self.assertRaises(xci.XciError):
            xci.secure_entries(f)

    def test_no_secure_partition(self):
        with self.assertRaisesRegex(xci.XciError, "no secure partition"):
            xci.secure_entries(io.BytesIO(make_xci(None)))

    def test_unpack_ncas_from_xci(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = Path(tmp) / "game.xci"
            card.write_bytes(make_xci(self.SECURE))
            out = Path(tmp) / "nca"
            out.mkdir()
            ncas, rids = extract.unpack_ncas(card, out, "base")
            self.assertEqual([p.name for p in ncas], ["base-0123.nca"])  # not the system update, not the cnmt
            self.assertEqual(ncas[0].read_bytes(), b"program")
            self.assertEqual(rids, {"89ab"})


if __name__ == "__main__":
    unittest.main()
