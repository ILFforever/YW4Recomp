"""Side by side and difference of the same frames from two screenshot folders.

    python -m tools.dev.shot_compare DIR_A DIR_B FRAME [FRAME ...] [--out DIR] [--labels A B]

Writes <out>/compare_<frame>.png (A, B, difference: white where a channel differs by more than 24) and
prints the share of pixels that differ. Across separate runs the replay drifts (two runs of the same
settings already differ a lot), so for small effects compare frames of one run instead (frame_ab.py).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("frames", nargs="+", type=int)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--labels", nargs=2, default=("A", "B"))
    a = ap.parse_args(argv)
    out_dir = a.out or a.b
    out_dir.mkdir(parents=True, exist_ok=True)
    for fr in a.frames:
        ia = Image.open(a.a / f"frame_{fr:06d}.bmp").convert("RGB")
        ib = Image.open(a.b / f"frame_{fr:06d}.bmp").convert("RGB")
        d = np.abs(np.asarray(ia, int) - np.asarray(ib, int)).max(axis=2)
        share = 100.0 * float((d > 24).mean())
        amp = Image.fromarray(np.where(d > 24, 255, np.minimum(d * 6, 255)).astype(np.uint8)).convert("RGB")
        w, h = ia.size
        sheet = Image.new("RGB", (w * 3, h + 28), "white")
        for i, im in enumerate((ia, ib, amp)):
            sheet.paste(im, (i * w, 28))
        draw = ImageDraw.Draw(sheet)
        for i, t in enumerate((a.labels[0], a.labels[1], f"difference: {share:.2f}% of pixels")):
            draw.text((i * w + 8, 7), t, fill="black")
        sheet.save(out_dir / f"compare_{fr:06d}.png")
        print(fr, f"{share:.3f}% differ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
