"""Level-5 G4TX texture container parser (NXTCH000 BC7 texture + sub-image atlas records)."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from tools.yw4.bc7 import deswizzle_bc7

# G4TX header offsets (all table offsets are 4-byte word indices relative to BASE_WORDS).
G4TX_MAGIC = b"G4TX"
G4TX_BASE_WORDS_OFF = 0x0A
G4TX_NAME_COUNT_OFF = 0x22
G4TX_TEX_WORDS_OFF = 0x3E
G4TX_REC_WORDS_OFF = 0x42
G4TX_STR_WORDS_OFF = 0x46
G4TX_REC_STRIDE = 24
G4TX_REC_RECT_OFF = 4

# NXTCH000 texture header offsets.
NXTCH_MAGIC = b"NXTCH000"
NXTCH_WIDTH_OFF = 0x14
NXTCH_DATA_OFF = 0x100


@dataclass(frozen=True)
class G4txSheet:
    name: str
    width: int
    height: int
    linear_bc7: bytes
    records: dict[str, tuple[int, int, int, int]]


def parse_g4tx(data: bytes) -> G4txSheet:
    """Parse a G4TX container into its texture dimensions, linear BC7 blocks, and sub-image rects."""
    if data[:4] != G4TX_MAGIC:
        raise ValueError("not a G4TX file")
    u16 = lambda off: struct.unpack_from("<H", data, off)[0]
    base = u16(G4TX_BASE_WORDS_OFF)
    count = u16(G4TX_NAME_COUNT_OFF)

    strings = 4 * (base + u16(G4TX_STR_WORDS_OFF))
    names = []
    for i in range(count):
        off = strings + u16(strings + 2 * i)
        names.append(data[off : data.index(b"\0", off)].decode("ascii"))

    tex = 4 * (base + u16(G4TX_TEX_WORDS_OFF))
    if data[tex : tex + 8] != NXTCH_MAGIC:
        raise ValueError(f"expected NXTCH000 at offset {tex:#x}")
    w, h = struct.unpack_from("<II", data, tex + NXTCH_WIDTH_OFF)

    rec_tab = 4 * (base + u16(G4TX_REC_WORDS_OFF))
    records: dict[str, tuple[int, int, int, int]] = {}
    for i in range(1, count):
        x, y, rw, rh = struct.unpack_from("<4H", data, rec_tab + (i - 1) * G4TX_REC_STRIDE + G4TX_REC_RECT_OFF)
        records[names[i]] = (x, y, rw, rh)

    linear = bytes(deswizzle_bc7(data[tex + NXTCH_DATA_OFF :], w, h))
    return G4txSheet(name=names[0] if names else "sheet", width=w, height=h, linear_bc7=linear, records=records)
