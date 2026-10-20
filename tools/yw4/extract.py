"""Extract ExeFS + RomFS (update applied) from the user's own base and update NSPs.

    python -m tools.yw4.extract

Reads [input] base_nsp / update_nsp and [tools] hactool_exe / keys_dir from
config.local.toml, writes into [input] exefs_dir / romfs_dir.

Why the steps look like this:
- An NSP is a PFS0 archive of NCAs. We pull out the NCAs ourselves (pfs0.py)
  into work/nca/ because hactool needs them as separate files.
- The NCAs are ticket-encrypted. The base and the update have different
  rights IDs, so they need different title keys, but hactool's --titlekey
  takes only one. hactool also loads ~/.switch/title.keys on its own and
  picks the right key per NCA, so we give it a temporary home folder holding
  prod.keys and only the title keys for these two rights IDs, then delete it.
- The update's RomFS is a patch (BKTR) on top of the base RomFS; --basenca
  makes hactool apply it, so romfs_dir gets the final, patched files.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from . import config, pfs0


class ExtractError(Exception):
    pass


def unpack_ncas(nsp: Path, out_dir: Path, tag: str) -> tuple[list[Path], set[str]]:
    """Copy every content NCA out of an NSP. Returns NCA paths and the rights IDs of its tickets."""
    ncas, rights_ids = [], set()
    with nsp.open("rb") as f:
        for entry in pfs0.read_entries(f):
            if entry.name.endswith(".tik"):
                rights_ids.add(entry.name[:-4].lower())
            elif entry.name.endswith(".nca") and not entry.name.endswith(".cnmt.nca"):
                dest = out_dir / f"{tag}-{entry.name}"
                if not (dest.is_file() and dest.stat().st_size == entry.size):
                    pfs0.extract(f, entry, dest)
                ncas.append(dest)
    return ncas, rights_ids


def write_temp_keys(keys_dir: Path, rights_ids: set[str], home: Path) -> None:
    switch = home / ".switch"
    switch.mkdir()
    shutil.copyfile(keys_dir / "prod.keys", switch / "prod.keys")
    # Only the keys we need, de-duplicated: hactool 1.4 aborts on repeated rights IDs.
    found: dict[str, str] = {}
    with (keys_dir / "title.keys").open(encoding="utf-8") as f:
        for line in f:
            rid, sep, key = line.partition("=")
            rid = rid.strip().lower()
            if sep and rid in rights_ids:
                found[rid] = key.strip()
    missing = rights_ids - found.keys()
    if missing:
        raise ExtractError(f"title.keys has no key for rights ID(s): {', '.join(sorted(missing))}")
    (switch / "title.keys").write_text("".join(f"{r} = {k}\n" for r, k in found.items()))


def run_hactool(hactool: Path, home: Path, args: list[str]) -> str:
    env = dict(os.environ, USERPROFILE=str(home), HOME=str(home))
    proc = subprocess.run([str(hactool), "--disablekeywarns", "--suppresskeys", *args],
                          env=env, capture_output=True, text=True, errors="replace")
    out = proc.stdout + proc.stderr
    if proc.returncode != 0 or re.search(r"(?m)^(Error|\[WARN\] Unable)", out):
        raise ExtractError(f"hactool failed ({proc.returncode}):\n{out.strip()[-2000:]}")
    return out


def find_program_nca(hactool: Path, home: Path, ncas: list[Path]) -> Path:
    programs = [n for n in ncas
                if re.search(r"Content Type:\s+Program", run_hactool(hactool, home, ["-i", str(n)]))]
    if len(programs) != 1:
        raise ExtractError(f"expected one Program NCA, found {len(programs)}: {programs}")
    return programs[0]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep-nca", action="store_true", help="keep the unpacked NCAs in work/nca/")
    args = ap.parse_args(argv)

    try:
        cfg = config.load(require_exefs=False)
        for name in ("base_nsp", "update_nsp", "hactool_exe"):
            if getattr(cfg, name) is None or not getattr(cfg, name).is_file():
                raise config.ConfigError(f"{name} is not set or not a file")
        if cfg.keys_dir is None or not (cfg.keys_dir / "prod.keys").is_file():
            raise config.ConfigError("keys_dir is not set or has no prod.keys")
    except config.ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return 1

    if any(cfg.exefs_dir.iterdir()) or any(cfg.romfs_dir.iterdir()):
        print(f"exefs_dir or romfs_dir is not empty; empty them first:\n  {cfg.exefs_dir}\n  {cfg.romfs_dir}",
              file=sys.stderr)
        return 1

    nca_dir = cfg.work_dir / "nca"
    nca_dir.mkdir(parents=True, exist_ok=True)
    try:
        print("unpacking NSPs ...")
        base_ncas, base_rids = unpack_ncas(cfg.base_nsp, nca_dir, "base")
        upd_ncas, upd_rids = unpack_ncas(cfg.update_nsp, nca_dir, "upd")

        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            write_temp_keys(cfg.keys_dir, base_rids | upd_rids, home)
            base_prog = find_program_nca(cfg.hactool_exe, home, base_ncas)
            upd_prog = find_program_nca(cfg.hactool_exe, home, upd_ncas)
            print(f"base program NCA:   {base_prog.name}\nupdate program NCA: {upd_prog.name}")
            print("extracting ExeFS + RomFS with update applied (takes a few minutes) ...")
            run_hactool(cfg.hactool_exe, home, [
                f"--basenca={base_prog}",
                f"--exefsdir={cfg.exefs_dir}",
                f"--romfsdir={cfg.romfs_dir}",
                str(upd_prog),
            ])
    except (ExtractError, pfs0.Pfs0Error) as e:
        print(f"extract failed: {e}", file=sys.stderr)
        return 1

    if not args.keep_nca:
        shutil.rmtree(nca_dir)
    if not (cfg.exefs_dir / "main").is_file():
        print("extract finished but exefs_dir has no 'main'", file=sys.stderr)
        return 1
    print(f"done. Next: python -m tools.yw4.ingest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
