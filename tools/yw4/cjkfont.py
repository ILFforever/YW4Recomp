"""Make third_party/fonts/NotoSansSC-SemiBold-yw4.ttf: the overlay's font for Chinese and Japanese letters.

The overlay's own fonts hold Western letters only. The game's prompts (name entry) and names can be Japanese or
Chinese, so the overlay needs a CJK font of its own (issue #3: on a Linux PC without one, every letter was a box).
Noto Sans SC is close to the game's own gothic (compared against the game's letter sheet, data/nx/font): weight
600 sits between the game's strokes and ours. The variable font is pinned to weight 600 and cut down to the
letters of GB2312 (simplified Chinese), Big5 level 1 (traditional Chinese), JIS X 0208 (Japanese, via cp932),
kana, CJK and full-width punctuation: ~11,600 letters, ~3.8 MB. Letters outside it come from the system's fonts
(runtime/src/ui.c).

    python -m tools.yw4.cjkfont "NotoSansSC[wght].ttf"

The source: github.com/google/fonts at commit bbda7253fc363238b05d97515ca71f8ae49c4fa9, ofl/notosanssc/
(third_party/fonts/README.md).
"""

from __future__ import annotations

import argparse
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "third_party" / "fonts" / "NotoSansSC-SemiBold-yw4.ttf"
WEIGHT = 600


def _decoded(enc: str, leads, trails) -> set[int]:
    """Every character a double-byte encoding can spell."""
    cps: set[int] = set()
    for a in leads:
        for b in trails:
            try:
                cps.update(ord(c) for c in bytes([a, b]).decode(enc))
            except UnicodeDecodeError:
                pass
    return cps


def wanted() -> set[int]:
    cps = _decoded("gb2312", range(0xA1, 0xF8), range(0xA1, 0xFF))
    cps |= _decoded("big5", range(0xA1, 0xC7), [*range(0x40, 0x7F), *range(0xA1, 0xFF)])
    cps |= _decoded("cp932", [*range(0x81, 0xA0), *range(0xE0, 0xF0)], [*range(0x40, 0x7F), *range(0x80, 0xFD)])
    cps |= set(range(0x2000, 0x2070)) | set(range(0x3000, 0x3100)) | set(range(0xFF00, 0xFFF0))
    return {c for c in cps if c > 0x7F}  # the built-in Latin fonts draw ASCII


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", type=Path, help="NotoSansSC[wght].ttf (the variable font)")
    ap.add_argument("--out", type=Path, default=OUT)
    a = ap.parse_args(argv)
    font = instancer.instantiateVariableFont(TTFont(a.source), {"wght": WEIGHT})
    cmap = font.getBestCmap()
    want = wanted()
    have = sorted(c for c in want if c in cmap)
    opt = subset.Options()
    opt.layout_features = ["kern", "palt"]
    opt.hinting = False
    opt.desubroutinize = True
    opt.name_IDs = ["*"]  # keeps the copyright and licence names (the OFL asks for them)
    opt.notdef_outline = True
    s = subset.Subsetter(opt)
    s.populate(unicodes=have)
    s.subset(font)
    font.save(a.out)
    print(f"{a.out}: {len(have)} letters ({len(want) - len(have)} wanted but not in the font), "
          f"{a.out.stat().st_size / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
