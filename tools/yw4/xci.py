"""Read XCI files (game-card images): the NCAs in their secure partition.

An XCI holds the card's partitions in HFS0 archives:
    0x100 "HEAD", the card header; at 0x130 u64 offset of the root HFS0, 0x138 u64 its header size.
    The root HFS0 lists the partitions ("update", "normal", "secure", sometimes "logo"), each an HFS0 too.
    The game's NCAs are in "secure" ("update" holds a system update, not the game's).
Some dumps keep the card's 0x1000-byte key area in front (then "HEAD" is at 0x1100); every offset in the
header is from the end of that area.

HFS0 (little-endian) is PFS0 with longer entries:
    0x00 "HFS0"   0x04 u32 file count   0x08 u32 string table size   0x0C reserved
    then per file (0x40 bytes): u64 data offset, u64 size, u32 name offset, u32 hashed size,
    u64 reserved, 0x20 SHA-256 of the first hashed-size bytes
    then the string table; data offsets are relative to the end of the string table.
Game-card NCAs are not ticket-encrypted: they need prod.keys, no title key.
"""

from __future__ import annotations

import struct
from typing import BinaryIO

from .pfs0 import Entry


class XciError(Exception):
    pass


def is_xci(f: BinaryIO) -> bool:
    return _card_start(f) is not None


def _card_start(f: BinaryIO) -> int | None:
    """Where the card image starts in the file: 0, or 0x1000 after a key area; None if not an XCI."""
    for start in (0, 0x1000):
        f.seek(start + 0x100)
        if f.read(4) == b"HEAD":
            return start
    return None


def read_hfs0(f: BinaryIO, at: int) -> list[Entry]:
    f.seek(at)
    head = f.read(0x10)
    if len(head) < 0x10 or head[:4] != b"HFS0":
        raise XciError(f"no HFS0 archive at {at:#x}")
    count, strtab_size = struct.unpack_from("<II", head, 4)
    raw = f.read(count * 0x40)
    strtab = f.read(strtab_size)
    if len(raw) != count * 0x40 or len(strtab) != strtab_size:
        raise XciError(f"truncated HFS0 header at {at:#x}")
    data_start = at + 0x10 + count * 0x40 + strtab_size
    entries = []
    for i in range(count):
        offset, size, name_off = struct.unpack_from("<QQI", raw, i * 0x40)
        end = strtab.find(b"\0", name_off)
        entries.append(Entry(strtab[name_off:end if end >= 0 else None].decode("utf-8"), data_start + offset, size))
    return entries


def secure_entries(f: BinaryIO) -> list[Entry]:
    """The files of the secure partition (the game's NCAs; absolute offsets in the file)."""
    start = _card_start(f)
    if start is None:
        raise XciError("not an XCI (no card header)")
    f.seek(start + 0x130)
    root_offset, = struct.unpack("<Q", f.read(8))
    partitions = {e.name: e for e in read_hfs0(f, start + root_offset)}
    if "secure" not in partitions:
        raise XciError(f"no secure partition (partitions: {', '.join(sorted(partitions)) or 'none'})")
    return read_hfs0(f, partitions["secure"].offset)
