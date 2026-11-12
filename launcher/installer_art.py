"""The installer's pictures: the tall one on its first and last pages, and the small one in the corner of the others.

    python launcher/installer_art.py OUT_DIR    draws sidebar.bmp + header.bmp for launcher/VERSION into OUT_DIR

The package script (package.py) calls draw() before the installer build, so the version on the picture is always the
one being built. Only our own icon (launcher/icon-source.png) is drawn: purple with faint ghosts behind the icon, the
name and the version (the owner's pick "B" of three mockups, 2026-11-13). NSIS wants plain 24-bit BMPs at exactly
164x314 (sidebar) and 150x57 (header); drawing three times larger and shrinking gives smooth edges.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[1]
ICON = REPO / "launcher" / "icon-source.png"

PURPLE = (69, 42, 122)   # the icon's own background
GOLD = (229, 186, 80)    # the icon's ring
CREAM = (248, 244, 250)  # the icon's ghost
S = 3                    # drawn this many times larger, then shrunk


def _font(bold: bool, size: int) -> ImageFont.FreeTypeFont:
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    try:
        return ImageFont.truetype(name, size)  # Windows' own UI font (the installer is Windows only)
    except OSError:
        return ImageFont.load_default(size)


def _ghost(icon: Image.Image) -> Image.Image:
    """The ghost alone: the icon's near-white pixels."""
    rgba = icon.convert("RGBA")
    r, g, b, a = rgba.split()
    white = Image.merge("RGB", (r, g, b)).convert("L").point(lambda v: 255 if v > 200 else 0)
    mask = Image.composite(a, Image.new("L", a.size, 0), white)
    ghost = Image.new("RGBA", rgba.size, (255, 255, 255, 0))
    ghost.putalpha(mask)
    return ghost.crop(mask.getbbox())


def _pattern(im: Image.Image, ghost: Image.Image, size: int, step_x: int, step_y: int) -> None:
    """Faint ghosts tiled over the whole picture, every other row shifted half a step."""
    g = ghost.resize((size, round(size * ghost.height / ghost.width)), Image.LANCZOS)
    g.putalpha(g.split()[3].point(lambda v: round(v * 0.10)))
    for row, y in enumerate(range(-step_y // 3, im.height, step_y)):
        for x in range(-step_x // 3 + (step_x // 2 if row % 2 else 0), im.width, step_x):
            im.paste(g, (x, y), g)


def _centred(d: ImageDraw.ImageDraw, width: int, y: int, text: str, font, fill) -> None:
    d.text(((width - d.textlength(text, font=font)) / 2, y), text, font=font, fill=fill)


def sidebar(icon: Image.Image, version: str) -> Image.Image:
    w, h = 164 * S, 314 * S
    im = Image.new("RGB", (w, h), PURPLE)
    _pattern(im, _ghost(icon), 34 * S, 82 * S, 58 * S)
    ic = icon.convert("RGBA").resize((104 * S, 104 * S), Image.LANCZOS)
    im.paste(ic, ((w - ic.width) // 2, 92 * S), ic)
    d = ImageDraw.Draw(im)
    _centred(d, w, 214 * S, "YW4 Recomp", _font(True, 20 * S), CREAM)
    _centred(d, w, 242 * S, version, _font(True, 11 * S), GOLD)
    return im.resize((164, 314), Image.LANCZOS)


def header(icon: Image.Image) -> Image.Image:
    w, h = 150 * S, 57 * S
    im = Image.new("RGB", (w, h), PURPLE)
    _pattern(im, _ghost(icon), 16 * S, 40 * S, 28 * S)
    ic = icon.convert("RGBA").resize((45 * S, 45 * S), Image.LANCZOS)
    im.paste(ic, ((w - ic.width) // 2, (h - ic.height) // 2), ic)
    return im.resize((150, 57), Image.LANCZOS)


def draw(out: Path, version: str) -> tuple[Path, Path]:
    out.mkdir(parents=True, exist_ok=True)
    icon = Image.open(ICON)
    side, head = out / "sidebar.bmp", out / "header.bmp"
    sidebar(icon, version).save(side, "BMP")
    header(icon).save(head, "BMP")
    return side, head


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    print(*draw(Path(sys.argv[1]), (REPO / "launcher" / "VERSION").read_text().strip()), sep="\n")
