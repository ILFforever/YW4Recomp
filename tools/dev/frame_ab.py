"""Compare two renderings within one run: image dumps of alternating frames (A on some, B on others).

    python -m tools.dev.frame_ab --addr 7269783000 --a 1801 1803 1805 --b 1802 1804 [--noise 7268f13000]

Reads work/dump/image_<addr>_f<frame>_0.rgbaf (YW4_DUMP_IMAGE with a list in YW4_DUMP_IMAGE_AT). Pixels that
are identical in all the A frames count as stable; for each B frame it reports how many stable pixels differ
from the first A frame. Things that move between frames still show up, so give --noise an image the B
setting does not change: its count is the motion baseline, and B is right when its count is no larger.

How the G-buffer programs were checked (decision 0026): YW4_GBUFFER_ALTERNATE=1800 (odd GPU frames ours),
YW4_DUMP_IMAGE=<the targets> YW4_DUMP_IMAGE_AT=1801,...,1806 (a frame dumped as F was drawn as F-1).
"""

from __future__ import annotations

import argparse
import struct
import sys

import numpy as np

from tools.yw4 import config


def load(cfg: config.Config, addr: str, frame: int) -> np.ndarray:
    b = (cfg.work_dir / "dump" / f"image_{addr}_f{frame}_0.rgbaf").read_bytes()
    w, h = struct.unpack("<II", b[:8])
    return np.frombuffer(b[8:8 + w * h * 16], dtype="<f4").reshape(h, w, 4)


def report(cfg: config.Config, addr: str, a_frames: list[int], b_frames: list[int], label: str) -> None:
    a = [load(cfg, addr, f) for f in a_frames]
    stable = np.ones(a[0].shape[:2], bool)
    for x in a[1:]:
        stable &= np.all(x == a[0], axis=2)
    for f in b_frames:
        d = np.abs(load(cfg, addr, f) - a[0]).max(axis=2)
        n = int((stable & (d > 0)).sum())
        mx = float(d[stable].max()) if stable.any() else 0.0
        print(f"{label} {addr} frame {f}: {int(stable.sum())} stable pixels, {n} differ (max {mx:.4f})")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--addr", nargs="+", required=True, help="dumped image addresses (hex, as in the file names)")
    ap.add_argument("--a", nargs="+", type=int, required=True, help="frames with the reference setting")
    ap.add_argument("--b", nargs="+", type=int, required=True, help="frames with the setting under test")
    ap.add_argument("--noise", nargs="*", default=[], help="images the tested setting does not change")
    a = ap.parse_args(argv)
    cfg = config.load(require_exefs=False)
    for addr in a.addr:
        report(cfg, addr, a.a, a.b, "tested")
    for addr in a.noise:
        report(cfg, addr, a.a, a.b, "noise ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
