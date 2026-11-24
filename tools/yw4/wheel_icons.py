"""The shortcut wheel's icons (decision 0075) from the player's own game files, as <work_dir>/wheel/icons.bin.

    python -m tools.yw4.wheel_icons

The wheel (runtime/src/shortcutwheel.c) shows the game's own pictures for its eight field shortcuts, the ones the
field HUD's shortcut cross draws. They are game data (rule 1): never in the repo. As the exe's icon (decision 0048),
this takes them from the player's RomFS at build time; recomp.build turns the file into wheel_icons.c and links it
into the exe. Without it the wheel draws arrows.

Where they are: data/nx/menu/01_rpg/01_rpg00/rpg00_01/rpg00_01.g4tx in menu.cpk, the HUD's "guide" sheet (a
564 x 528 BC7 texture). Six have their own record in the sheet (rpg00_01_guide_icon_*); the bike and the walking
Yo-kai companion sit in the sheet without one, so their places are fixed below (the game is version-locked).
Decoded without Pillow (tools.yw4.bc7), only the blocks the icons cover.

icons.bin: b"YW4WHEEL", then for shortcuts 1 to 8: width (u16), height (u16), width * height * 4 bytes of straight
RGBA, rows top to bottom.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

from . import bc7, config, cpk

SHEET = "data/nx/menu/01_rpg/01_rpg00/rpg00_01/rpg00_01.g4tx"
SHEET_SIZE = (564, 528)

# shortcut number -> the sheet's record name, or a fixed (x, y, w, h) for the two without a record. The numbers are
# the D-pad's (1 up, 2 right, 3 down, 4 left) and the hold-L set's (5 up ... 8 left), matched to the HUD in play
# (2026-11-15: the owner's screenshots with and without L held).
ICONS = {
    1: "rpg00_01_guide_icon_r02",   # Auto-Navi
    2: "rpg00_01_guide_icon_l02",   # Diary
    3: (428, 240, 76, 76),          # Bike
    4: "rpg00_01_guide_icon_d01",   # Naviwoof Guide
    5: "rpg00_01_guide_icon_u01",   # Change Watchers
    6: "rpg00_01_guide_icon_r01",   # Mirapo
    7: (413, 452, 72, 72),          # Change Yo-kai Companion
    8: "rpg00_01_guide_icon_d02",   # Change moves and Equipment
}


class WheelIconError(Exception):
    pass


def sheet_records(data: bytes) -> tuple[dict[str, tuple[int, int, int, int]], int, int, bytes]:
    """The G4TX file's sub-image records by name, and its first texture: width, height, linear BC7 blocks."""
    from tools.yw4.g4tx import parse_g4tx

    try:
        sheet = parse_g4tx(data)
    except ValueError as e:
        raise WheelIconError(str(e)) from e
    return sheet.records, sheet.width, sheet.height, sheet.linear_bc7


def icons_from(data: bytes) -> dict[int, tuple[int, int, bytes]]:
    rects, w, h, linear = sheet_records(data)
    if (w, h) != SHEET_SIZE:
        raise WheelIconError(f"the sheet is {w} x {h}, not {SHEET_SIZE[0]} x {SHEET_SIZE[1]}")
    out = {}
    for n, where in ICONS.items():
        rect = rects.get(where) if isinstance(where, str) else where
        if not rect:
            raise WheelIconError(f"no {where} in the sheet")
        x, y, rw, rh = rect
        out[n] = (rw, rh, bc7.decode_rect(linear, w, x, y, rw, rh))
    return out


def write(icons: dict[int, tuple[int, int, bytes]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    blob = bytearray(b"YW4WHEEL")
    for n in range(1, 9):
        w, h, px = icons[n]
        blob += struct.pack("<HH", w, h) + px
    path.write_bytes(bytes(blob))


def main(argv: list[str] | None = None) -> int:
    cfg = config.load()
    menu = cfg.romfs_dir / "data" / "common" / "menu" / "menu.cpk"
    if not menu.is_file():
        print(f"wheel icons: {menu} not found; the shortcut wheel will draw arrows", file=sys.stderr)
        return 1
    entry = next((f for f in cpk.list_files(menu).files if f.path == SHEET), None)
    if not entry:
        print(f"wheel icons: {SHEET} not in {menu}", file=sys.stderr)
        return 1
    try:
        icons = icons_from(cpk.read(menu, entry))
    except WheelIconError as e:
        print(f"wheel icons: {e}", file=sys.stderr)
        return 1
    out = cfg.work_dir / "wheel" / "icons.bin"
    write(icons, out)
    print(f"wheel icons: 8 shortcut icons -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
