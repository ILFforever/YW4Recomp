"""List files inside CRI CPK archives (read-only, no decompression).

A CPK starts with "CPK " and a header table. CRI stores every table in its
"@UTF" format (big-endian):

    "@UTF" u32 size, then relative to offset 8:
        u16 version, u16 rows_offset, u32 strings_offset, u32 data_offset,
        u32 table_name (string offset), u16 column count, u16 row length, u32 row count
    column descriptors: u8 flags, u32 name (string offset), [constant value]
        flags high nibble = storage: 0x10 zero, 0x30 constant (value stored in
        the descriptor), 0x50 per-row (value stored in each row)
        flags low nibble = type: 0 u8, 1 s8, 2 u16, 3 s16, 4 u32, 5 s32,
        6 u64, 7 s64, 8 f32, 9 f64, 0xA string (u32 offset), 0xB data (u32 offset, u32 size)

The header's TocOffset points at a "TOC " chunk whose @UTF table lists
DirName, FileName, FileSize (stored), ExtractSize (uncompressed), FileOffset.
Some archives have only an ID-indexed ITOC with no names; we report a count.
File offsets in the TOC are relative to the TOC chunk start.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

_FMT = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f", 9: ">d"}


class CpkError(Exception):
    pass


def read_utf(buf: bytes) -> list[dict]:
    if buf[:4] != b"@UTF":
        raise CpkError("not a @UTF table (encrypted or unknown format)")
    _, rows_off, str_off, data_off, _name, ncols, row_len, nrows = struct.unpack_from(">HHIIIHHI", buf, 8)
    rows_off += 8
    str_off += 8
    data_off += 8

    def string(off: int) -> str:
        s = str_off + off
        return buf[s:buf.index(b"\0", s)].decode("utf-8", "replace")

    def value(typ: int, pos: int):
        if typ == 0xA:
            return string(struct.unpack_from(">I", buf, pos)[0]), 4
        if typ == 0xB:
            off, size = struct.unpack_from(">II", buf, pos)
            return (data_off + off, size), 8
        fmt = _FMT.get(typ)
        if fmt is None:
            raise CpkError(f"unknown @UTF column type {typ:#x}")
        return struct.unpack_from(fmt, buf, pos)[0], struct.calcsize(fmt)

    columns = []
    pos = 0x20
    for _ in range(ncols):
        flags = buf[pos]
        name = string(struct.unpack_from(">I", buf, pos + 1)[0])
        pos += 5
        storage, typ = flags & 0xF0, flags & 0x0F
        const = None
        if storage == 0x30:
            const, n = value(typ, pos)
            pos += n
        columns.append((name, storage, typ, const))

    rows = []
    for r in range(nrows):
        pos = rows_off + r * row_len
        row = {}
        for name, storage, typ, const in columns:
            if storage == 0x50:
                row[name], n = value(typ, pos)
                pos += n
            elif storage == 0x30:
                row[name] = const
            else:
                row[name] = None
        rows.append(row)
    return rows


def _chunk_table(f: BinaryIO, offset: int, magic: bytes) -> list[dict]:
    f.seek(offset)
    head = f.read(0x10)
    if head[:4] != magic:
        raise CpkError(f"expected {magic!r} at {offset:#x}")
    size = struct.unpack_from("<Q", head, 8)[0]
    return read_utf(f.read(size))


@dataclass(frozen=True)
class CpkFile:
    path: str
    stored_size: int
    extract_size: int
    offset: int  # absolute offset in the .cpk

    @property
    def compressed(self) -> bool:
        return self.extract_size != self.stored_size


@dataclass(frozen=True)
class CpkInfo:
    files: list[CpkFile]
    id_only_count: int  # files in an ITOC with no names (0 if the archive has a named TOC)


def list_files(path: Path) -> CpkInfo:
    with path.open("rb") as f:
        (header,) = _chunk_table(f, 0, b"CPK ")
        toc = header.get("TocOffset") or 0
        if not toc:
            return CpkInfo([], header.get("Files") or 0)
        files = []
        for row in _chunk_table(f, toc, b"TOC "):
            name = row.get("FileName") or ""
            d = row.get("DirName") or ""
            files.append(CpkFile(
                path=f"{d}/{name}" if d else name,
                stored_size=row.get("FileSize") or 0,
                extract_size=row.get("ExtractSize") or 0,
                offset=toc + (row.get("FileOffset") or 0),
            ))
        return CpkInfo(files, 0)


def peek(path: Path, entry: CpkFile, n: int = 8) -> bytes:
    with path.open("rb") as f:
        f.seek(entry.offset)
        return f.read(n)


def crilayla_decompress(src: bytes) -> bytes:
    """Decompress CRI's CRILAYLA format.

    Layout: "CRILAYLA", u32 LE uncompressed size (excluding the 0x100 prefix),
    u32 LE offset of the stored prefix (relative to 0x10). The first 0x100
    output bytes are stored raw at 0x10 + that offset. Everything else is an
    LZ stream read *backwards*: bits come from the end of the compressed data
    toward the start (MSB first), and output is written from the end toward
    the start. Each item is a flag bit: 0 = 8-bit literal; 1 = back-reference
    with a 13-bit offset (+3) and a length of 3 + a chain of 2,3,5,8,8,...-bit
    fields that continues while a field is all ones.
    """
    if src[:8] != b"CRILAYLA":
        raise CpkError("not CRILAYLA data")
    size, prefix_off = struct.unpack_from("<II", src, 8)
    out = bytearray(size + 0x100)
    out[:0x100] = src[0x10 + prefix_off:0x10 + prefix_off + 0x100]

    pos = 0x10 + prefix_off - 1  # next byte to read, moving backwards
    cur = 0
    left = 0

    def bits(n: int) -> int:
        nonlocal pos, cur, left
        v = 0
        while n:
            if left == 0:
                if pos < 0x10:
                    raise CpkError("CRILAYLA stream ran out")
                cur = src[pos]
                pos -= 1
                left = 8
            take = min(n, left)
            v = (v << take) | ((cur >> (left - take)) & ((1 << take) - 1))
            left -= take
            n -= take
        return v

    w = len(out) - 1
    while w >= 0x100:
        if bits(1):
            back = bits(13) + 3
            length = 3
            for width in (2, 3, 5):
                v = bits(width)
                length += v
                if v != (1 << width) - 1:
                    break
            else:
                while True:
                    v = bits(8)
                    length += v
                    if v != 0xFF:
                        break
            for _ in range(length):
                if w < 0x100:
                    break
                out[w] = out[w + back]
                w -= 1
        else:
            out[w] = bits(8)
            w -= 1
    return bytes(out)


def read(path: Path, entry: CpkFile) -> bytes:
    """Return a file's contents, decompressed if needed."""
    with path.open("rb") as f:
        f.seek(entry.offset)
        data = f.read(entry.stored_size)
    if entry.compressed:
        data = crilayla_decompress(data)
    if len(data) != entry.extract_size:
        raise CpkError(f"{entry.path}: got {len(data)} bytes, expected {entry.extract_size}")
    return data
