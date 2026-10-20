"""Tests for the CRI @UTF table reader on a hand-built table (no game files)."""

from __future__ import annotations

import struct
import unittest

from tools.yw4 import cpk


def make_utf(rows: list[tuple[str, int]], const_name: str, const_value: int) -> bytes:
    # Columns: FileName (per-row string), FileSize (per-row u32), Kind (constant u16).
    strings = b"<NULL>\0"
    def s(text: str) -> int:
        nonlocal strings
        off = len(strings)
        strings += text.encode() + b"\0"
        return off

    cols = struct.pack(">BI", 0x5A, s("FileName"))
    cols += struct.pack(">BI", 0x54, s("FileSize"))
    cols += struct.pack(">BIH", 0x32, s(const_name), const_value)
    row_len = 8
    row_data = b"".join(struct.pack(">II", s(name), size) for name, size in rows)

    rows_off = 0x18 + len(cols)          # relative to offset 8
    str_off = rows_off + len(row_data)
    data_off = str_off + len(strings)
    body = struct.pack(">HHIIIHHI", 1, rows_off, str_off, data_off, 0, 3, row_len, len(rows))
    body += cols + row_data + strings
    return b"@UTF" + struct.pack(">I", len(body)) + body


class UtfTest(unittest.TestCase):
    def test_rows_and_constant(self):
        buf = make_utf([("a.g4tx", 10), ("b.bin", 20)], "Kind", 7)
        rows = cpk.read_utf(buf)
        self.assertEqual(rows, [
            {"FileName": "a.g4tx", "FileSize": 10, "Kind": 7},
            {"FileName": "b.bin", "FileSize": 20, "Kind": 7},
        ])

    def test_not_utf(self):
        with self.assertRaises(cpk.CpkError):
            cpk.read_utf(b"\x00" * 0x40)


if __name__ == "__main__":
    unittest.main()
