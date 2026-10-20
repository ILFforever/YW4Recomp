"""Tests for the LZ4 decoder and NSO parser, on synthetic data only (no game files).

Run from the repo root:  python -m unittest discover -s tests -t .
The optional `lz4` package, if installed, is used only to make test input.
"""

from __future__ import annotations

import hashlib
import os
import struct
import unittest

from tools.yw4 import lz4_block, nso

try:
    import lz4.block as lz4ref
except ImportError:
    lz4ref = None


class Lz4BlockTest(unittest.TestCase):
    def test_literals_only(self):
        # token 0x50: 5 literals, no match (last sequence)
        self.assertEqual(lz4_block.decompress(b"\x50hello", 5), b"hello")

    def test_overlapping_match(self):
        # 1 literal 'a', then match offset 1 length 4+3=7 -> 'a' * 8
        self.assertEqual(lz4_block.decompress(b"\x13a\x01\x00", 8), b"a" * 8)

    def test_extended_lengths(self):
        # 15 + 5 = 20 literals
        src = b"\xf0\x05" + b"x" * 20
        self.assertEqual(lz4_block.decompress(src, 20), b"x" * 20)

    def test_bad_offset(self):
        with self.assertRaises(ValueError):
            lz4_block.decompress(b"\x10a\x05\x00", 5)

    def test_truncated(self):
        with self.assertRaises(ValueError):
            lz4_block.decompress(b"\x50hel", 5)

    @unittest.skipIf(lz4ref is None, "lz4 package not installed")
    def test_matches_reference(self):
        rng = os.urandom(4096)
        data = rng + b"ABCD" * 5000 + rng[:100] + bytes(3000) + rng
        comp = lz4ref.compress(data, store_size=False)
        self.assertEqual(lz4_block.decompress(comp, len(data)), data)


def make_nso(text: bytes, ro: bytes, data: bytes, compress: bool) -> bytes:
    segs = [text, ro, data]
    payloads = [lz4ref.compress(s, store_size=False) if compress else s for s in segs]
    mem = [0, 0x1000, 0x2000]
    header = bytearray(nso.HEADER_SIZE)
    header[:4] = b"NSO0"
    flags = 0b111000 | (0b111 if compress else 0)
    struct.pack_into("<I", header, 0x0C, flags)
    off = nso.HEADER_SIZE
    for i, (seg, payload) in enumerate(zip(segs, payloads)):
        struct.pack_into("<3I", header, 0x10 + i * 0x10, off, mem[i], len(seg))
        struct.pack_into("<I", header, 0x60 + i * 4, len(payload))
        header[0xA0 + i * 0x20:0xC0 + i * 0x20] = hashlib.sha256(seg).digest()
        off += len(payload)
    header[0x40:0x54] = bytes(range(1, 21))
    return bytes(header) + b"".join(payloads)


class NsoTest(unittest.TestCase):
    TEXT, RO, DATA = b"\x1f\x20\x03\xd5" * 64, b"rodata" * 10, b"\x01\x02"

    def check_image(self, blob: bytes):
        img = nso.flat_image(blob)
        self.assertEqual(len(img), 0x2000 + len(self.DATA))
        self.assertEqual(img[:len(self.TEXT)], self.TEXT)
        self.assertEqual(img[0x1000:0x1000 + len(self.RO)], self.RO)
        self.assertEqual(img[len(self.TEXT):0x1000], bytes(0x1000 - len(self.TEXT)))

    def test_uncompressed(self):
        self.check_image(make_nso(self.TEXT, self.RO, self.DATA, compress=False))

    @unittest.skipIf(lz4ref is None, "lz4 package not installed")
    def test_compressed_same_image(self):
        a = nso.flat_image(make_nso(self.TEXT, self.RO, self.DATA, compress=False))
        b = nso.flat_image(make_nso(self.TEXT, self.RO, self.DATA, compress=True))
        self.assertEqual(a, b)

    def test_build_id(self):
        h = nso.parse_header(make_nso(self.TEXT, self.RO, self.DATA, compress=False))
        self.assertEqual(nso.build_id_hex(h.build_id), bytes(range(1, 21)).hex().upper())

    def test_hash_mismatch(self):
        blob = bytearray(make_nso(self.TEXT, self.RO, self.DATA, compress=False))
        blob[nso.HEADER_SIZE] ^= 0xFF
        with self.assertRaises(nso.NsoError):
            nso.flat_image(bytes(blob))

    def test_bad_magic(self):
        with self.assertRaises(nso.NsoError):
            nso.parse_header(b"XXXX" + bytes(0xFC))


if __name__ == "__main__":
    unittest.main()
