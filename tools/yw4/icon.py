"""The game's icon from the user's own NSP or XCI, as <work_dir>/icon/yw4.ico for the exe (decision 0048), and
the original JPEG beside it as icon.jpg for the launcher's Ready screen.

    python -m tools.yw4.icon

The icon is game data (rule 1): it is never in the repo. It lives in the game's Control NCA (with control.nacp),
which an NSP or XCI holds but extracted ExeFS/RomFS folders don't; without one the exe simply has no icon.
tools.yw4.extract makes it too, from the NCAs it already unpacked.

- Only the small NCAs are unpacked (a Control NCA is under a megabyte; the program is gigabytes), then hactool
  says which is the Control one. The update's is preferred over the base game's: it carries the 4++ icon.
- The icon is a 256x256 JPEG (icon_AmericanEnglish.dat, else another language's). An .ico can't hold a JPEG, so
  Windows' own image library (System.Drawing, through PowerShell) draws it as PNGs at 256, 48, 32 and 16 pixels,
  and this script packs them into the .ico (PNG entries, Windows Vista and later). No Python image library needed.
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from . import config, extract, pfs0, xci

SMALL = 64 << 20  # NCAs larger than this are never a Control NCA
SIZES = (256, 48, 32, 16)
LANGS = ("AmericanEnglish", "BritishEnglish", "Japanese")


class IconError(Exception):
    pass


def unpack_small(path: Path, out_dir: Path, tag: str) -> tuple[list[Path], set[str]]:
    """The NCAs under SMALL bytes from an NSP or XCI, and its tickets' rights IDs."""
    ncas, rights_ids = [], set()
    with path.open("rb") as f:
        entries = xci.secure_entries(f) if xci.is_xci(f) else pfs0.read_entries(f)
        for e in entries:
            if e.name.endswith(".tik"):
                rights_ids.add(e.name[:-4].lower())
            elif e.name.endswith(".nca") and not e.name.endswith(".cnmt.nca") and e.size < SMALL:
                dest = out_dir / f"{tag}-{e.name}"
                if not (dest.is_file() and dest.stat().st_size == e.size):
                    pfs0.extract(f, e, dest)
                ncas.append(dest)
    return ncas, rights_ids


def icon_jpeg(hactool: Path, home: Path, ncas: list[Path], tmp: Path) -> Path:
    """The icon JPEG from the first Control NCA in `ncas` (put the preferred ones first)."""
    for n in ncas:
        if not re.search(r"Content Type:\s+Control", extract.run_hactool(hactool, home, ["-i", str(n)])):
            continue
        out = tmp / n.stem
        extract.run_hactool(hactool, home, [f"--romfsdir={out}", str(n)])
        for lang in LANGS:
            if (out / f"icon_{lang}.dat").is_file():
                return out / f"icon_{lang}.dat"
        found = sorted(out.glob("icon_*.dat"))
        if found:
            return found[0]
    raise IconError("no Control NCA with an icon")


def jpeg_to_ico(jpeg: Path, ico: Path, tmp: Path) -> None:
    if sys.platform != "win32":
        raise IconError("making the .ico needs Windows (System.Drawing)")
    script = ("Add-Type -AssemblyName System.Drawing; $src = [System.Drawing.Image]::FromFile($env:YW4_ICON_IN); "
              "foreach ($s in " + ",".join(map(str, SIZES)) + ") { "
              "$b = New-Object System.Drawing.Bitmap $s, $s; $g = [System.Drawing.Graphics]::FromImage($b); "
              "$g.InterpolationMode = 'HighQualityBicubic'; $g.DrawImage($src, 0, 0, $s, $s); "
              "$b.Save((Join-Path $env:YW4_ICON_OUT \"$s.png\"), [System.Drawing.Imaging.ImageFormat]::Png); "
              "$g.Dispose(); $b.Dispose() }; $src.Dispose()")
    env = dict(os.environ, YW4_ICON_IN=str(jpeg), YW4_ICON_OUT=str(tmp))
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                       env=env, capture_output=True, text=True)
    pngs = [(s, tmp / f"{s}.png") for s in SIZES]
    if r.returncode or not all(p.is_file() for _, p in pngs):
        raise IconError(f"PowerShell could not convert the icon: {(r.stderr or r.stdout).strip()[-500:]}")
    data = [p.read_bytes() for _, p in pngs]
    head = struct.pack("<HHH", 0, 1, len(pngs))
    offset = len(head) + 16 * len(pngs)
    entries = b""
    for (s, _), d in zip(pngs, data):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(d), offset)
        offset += len(d)
    ico.parent.mkdir(parents=True, exist_ok=True)
    ico.write_bytes(head + entries + b"".join(data))


def make(cfg: config.Config, home: Path, ncas: list[Path]) -> Path:
    """<work_dir>/icon/yw4.ico from these unpacked NCAs (preferred first); hactool's keys already in `home`."""
    ico = cfg.work_dir / "icon" / "yw4.ico"
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        jpeg = icon_jpeg(cfg.hactool_exe, home, ncas, tmp)
        jpeg_to_ico(jpeg, ico, tmp)
        shutil.copyfile(jpeg, ico.with_name("icon.jpg"))  # the launcher's Ready screen shows it (the .ico is for Windows)
    return ico


def main(argv: list[str] | None = None) -> int:
    try:
        cfg = config.load(require_exefs=False)
        files = [p for p in (cfg.update_nsp, cfg.base_nsp) if p is not None]  # the update's icon first
        if not files and cfg.dlc_dir and cfg.dlc_dir.is_dir():
            for p in sorted(cfg.dlc_dir.glob("*.nsp")):
                name = p.name.upper()
                if "010086C00AF7C800" in name or "[UPD]" in name:
                    files.insert(0, p)
                elif "010086C00AF7C000" in name or "[BASE]" in name:
                    files.append(p)
        if not files or cfg.hactool_exe is None or cfg.keys_dir is None:
            raise config.ConfigError("needs base_nsp (or update_nsp), hactool_exe and keys_dir: the icon is only "
                                     "in an NSP or XCI, not in extracted folders")
        nca_dir = cfg.work_dir / "icon" / "nca"
        nca_dir.mkdir(parents=True, exist_ok=True)
        ncas, rids = [], set()
        for tag, p in zip(("upd", "base"), files):
            n, r = unpack_small(p, nca_dir, tag)
            ncas, rids = ncas + n, rids | r
        with tempfile.TemporaryDirectory() as t:
            home = Path(t)
            extract.write_temp_keys(cfg.keys_dir, rids, home)
            ico = make(cfg, home, ncas)
        shutil.rmtree(nca_dir)
    except (config.ConfigError, IconError, extract.ExtractError, pfs0.Pfs0Error, xci.XciError) as e:
        print(f"icon: {e}", file=sys.stderr)
        return 1
    print(f"wrote {ico}; the next python -m recomp.build puts it in yw4.exe")
    return 0


if __name__ == "__main__":
    sys.exit(main())
