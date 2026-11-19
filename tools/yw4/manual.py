"""The game's electronic manual from the user's own NSP or XCI, as <work_dir>/manual/ (the title screen's Manual
button opens its index.html in the PC's browser: runtime/src/web.c).

    python -m tools.yw4.manual

The manual is game data (rule 1): it is never in the repo. On a Switch it is a set of HTML pages in the game's
"HtmlDocument" NCA, shown by the system's offline web applet (nn::web::ShowOfflineHtmlPage with a path such as
"manual.htdocs/index.html", relative to the NCA's html-document/ folder). The NSP holds that NCA next to the program;
extracted ExeFS/RomFS folders don't. tools.yw4.extract makes it too, from the NCAs it already unpacked.

- Only the small NCAs are unpacked (icon.unpack_small: a manual is a few megabytes). hactool calls both the
  HtmlDocument and the LegalInformation NCA "Manual"; the one whose RomFS has html-document/ is the manual.
- The update's is preferred over the base game's, as for the icon: it would carry 4++'s pages if it has its own.
"""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
from pathlib import Path

from . import config, extract, icon, pfs0, xci


class ManualError(Exception):
    pass


def make(cfg: config.Config, home: Path, ncas: list[Path]) -> Path:
    """<work_dir>/manual/ (the html-document folder's contents) from the first HtmlDocument NCA in `ncas` (preferred
    first; hactool's keys already in `home`). Returns the folder."""
    out = cfg.work_dir / "manual"
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        for n in ncas:
            if not re.search(r"Content Type:\s+Manual", extract.run_hactool(cfg.hactool_exe, home, ["-i", str(n)])):
                continue
            romfs = tmp / n.stem
            extract.run_hactool(cfg.hactool_exe, home, [f"--romfsdir={romfs}", str(n)])
            doc = romfs / "html-document"
            if not doc.is_dir():
                continue  # the legal information
            if out.exists():
                shutil.rmtree(out)
            shutil.copytree(doc, out)
            return out
    raise ManualError("no HtmlDocument NCA (the game's manual) in these files")


def main(argv: list[str] | None = None) -> int:
    try:
        cfg = config.load(require_exefs=False)
        files = [p for p in (cfg.update_nsp, cfg.base_nsp) if p is not None]  # the update's first
        if not files or cfg.hactool_exe is None or cfg.keys_dir is None:
            raise config.ConfigError("needs base_nsp (or update_nsp), hactool_exe and keys_dir: the manual is only "
                                     "in an NSP or XCI, not in extracted folders")
        nca_dir = cfg.work_dir / "manual-nca"
        nca_dir.mkdir(parents=True, exist_ok=True)
        ncas, rids, tkeys = [], set(), {}
        for tag, p in zip(("upd", "base") if cfg.update_nsp else ("base",), files):
            n, r = icon.unpack_small(p, nca_dir, tag)
            ncas, rids = ncas + n, rids | r
            tkeys |= extract.ticket_keys_of(p)
        try:
            with tempfile.TemporaryDirectory() as t:
                home = Path(t)
                extract.write_temp_keys(cfg.keys_dir, rids, home, tkeys)
                out = make(cfg, home, ncas)
        finally:
            shutil.rmtree(nca_dir, ignore_errors=True)
    except (config.ConfigError, ManualError, extract.ExtractError, pfs0.Pfs0Error, xci.XciError) as e:
        print(f"manual: {e}", file=sys.stderr)
        return 1
    pages = sum(1 for _ in out.rglob("*.htm*"))
    print(f"wrote {out} ({pages} pages)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
