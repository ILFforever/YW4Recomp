"""Tests for the PFS0 reader on a synthetic archive (no game files)."""

from __future__ import annotations

import io
import struct
import tempfile
import unittest
from pathlib import Path

from tools.yw4 import pfs0


def make_pfs0(files: dict[str, bytes]) -> bytes:
    names = b""
    entries = b""
    data = b""
    for name, body in files.items():
        entries += struct.pack("<QQII", len(data), len(body), len(names), 0)
        names += name.encode() + b"\0"
        data += body
    names += b"\0" * (-len(names) % 0x20)  # real archives pad the string table
    return b"PFS0" + struct.pack("<III", len(files), len(names), 0) + entries + names + data


class Pfs0Test(unittest.TestCase):
    FILES = {"a.nca": b"hello", "abc.tik": b"\x01" * 0x40, "x.cnmt.nca": b""}

    def test_entries_and_extract(self):
        f = io.BytesIO(make_pfs0(self.FILES))
        entries = pfs0.read_entries(f)
        self.assertEqual([e.name for e in entries], list(self.FILES))
        with tempfile.TemporaryDirectory() as tmp:
            for e in entries:
                dest = Path(tmp) / e.name
                pfs0.extract(f, e, dest)
                self.assertEqual(dest.read_bytes(), self.FILES[e.name])

    def test_bad_magic(self):
        with self.assertRaises(pfs0.Pfs0Error):
            pfs0.read_entries(io.BytesIO(b"HFS0" + bytes(12)))

    def test_truncated_data(self):
        blob = make_pfs0({"a.nca": b"hello"})[:-2]
        f = io.BytesIO(blob)
        (entry,) = pfs0.read_entries(f)
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(pfs0.Pfs0Error):
            pfs0.extract(f, entry, Path(tmp) / "a.nca")


if __name__ == "__main__":
    unittest.main()
