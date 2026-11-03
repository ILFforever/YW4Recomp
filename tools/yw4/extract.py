"""Extract ExeFS + RomFS (update applied) from the user's own base game (NSP or XCI) and update.

    python -m tools.yw4.extract

Reads [input] base_nsp / update_nsp and [tools] hactool_exe / keys_dir from
config.local.toml, writes into [input] exefs_dir / romfs_dir. base_nsp may be an
XCI (a game-card image); update_nsp may be left empty when that XCI already holds
the 2.2.0 update.

Why the steps look like this:
- An NSP is a PFS0 archive of NCAs, an XCI keeps them in its secure partition
  (HFS0, xci.py). We pull out the NCAs ourselves into work/nca/ because hactool
  needs them as separate files.
- Which NCA is the base game's program and which the update's comes from what
  hactool reports, not from the file it was in (a game card can hold both): both
  carry the application's title ID (an update's program NCA does too, not the
  update's + 0x800), and the update's has a "Patch RomFS" section, the base's a
  plain RomFS.
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

import tomllib

from . import config, pfs0, progress, xci

TARGET = Path(__file__).resolve().parents[2] / "target.toml"


class ExtractError(Exception):
    pass


def unpack_ncas(path: Path, out_dir: Path, tag: str) -> tuple[list[Path], set[str]]:
    """Copy every content NCA out of an NSP or XCI. Returns NCA paths and the rights IDs of its tickets
    (a game card's NCAs have none: they need no title key)."""
    ncas, rights_ids = [], set()
    with path.open("rb") as f:
        entries = xci.secure_entries(f) if xci.is_xci(f) else pfs0.read_entries(f)
        wanted = [e for e in entries if e.name.endswith(".nca") and not e.name.endswith(".cnmt.nca")]
        total, done = sum(e.size for e in wanted), 0
        rights_ids = {e.name[:-4].lower() for e in entries if e.name.endswith(".tik")}

        def moved(n: int) -> None:
            nonlocal done
            done += n
            progress.report("unpack", done, total, path.name, every=0.25)

        for entry in wanted:
            dest = out_dir / f"{tag}-{entry.name}"
            if not (dest.is_file() and dest.stat().st_size == entry.size):
                pfs0.extract(f, entry, dest, moved)
            else:
                moved(entry.size)  # already there from an earlier, interrupted run
            ncas.append(dest)
    return ncas, rights_ids


def write_temp_keys(keys_dir: Path, rights_ids: set[str], home: Path) -> None:
    switch = home / ".switch"
    switch.mkdir()
    shutil.copyfile(keys_dir / "prod.keys", switch / "prod.keys")
    if not rights_ids:  # game-card NCAs only: no title keys needed, title.keys may not exist
        return
    if not (keys_dir / "title.keys").is_file():
        raise ExtractError("the NSP needs title keys, but keys_dir has no title.keys")
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


def folder_bytes(dirs: list[Path]) -> int:
    total = 0
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                try:
                    total += os.path.getsize(os.path.join(root, f))
                except OSError:
                    pass  # a file hactool is creating right now
    return total


def run_hactool(hactool: Path, home: Path, args: list[str], watch: tuple[str, list[Path], int] | None = None) -> str:
    """Run hactool with only our keys (home). `watch` = (progress step, output folders, their final size in bytes):
    hactool reports nothing while it extracts, so once a second the folders it writes into are measured and a
    progress line says how far it is (YW4_PROGRESS=1)."""
    env = dict(os.environ, USERPROFILE=str(home), HOME=str(home))
    cmd = [str(hactool), "--disablekeywarns", "--suppresskeys", *args]
    if watch is None:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace")
        out, code = proc.stdout + proc.stderr, proc.returncode
    else:
        step, dirs, total = watch
        with tempfile.TemporaryFile("w+", errors="replace") as log:  # a file, not a pipe: it never fills up
            proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, text=True)
            while True:
                try:
                    code = proc.wait(timeout=1.0)
                    break
                except subprocess.TimeoutExpired:
                    progress.report(step, min(folder_bytes(dirs), total - 1), total)
            log.seek(0)
            out = log.read()
    if code != 0 or re.search(r"(?m)^(Error|\[WARN\] Unable)", out):
        raise ExtractError(f"hactool failed ({code}):\n{out.strip()[-2000:]}")
    return out


def find_programs(hactool: Path, home: Path, ncas: list[Path], app_id: int) -> tuple[Path | None, Path | None]:
    """The game's base Program NCA and its update's (a Patch RomFS), each or None."""
    found: dict[str, Path] = {}
    for n in ncas:
        info = run_hactool(hactool, home, ["-i", str(n)])
        m = re.search(r"Title ID:\s+([0-9a-fA-F]{16})", info)
        if not re.search(r"Content Type:\s+Program", info) or not m or int(m.group(1), 16) != app_id:
            continue
        kind = "update" if re.search(r"Partition Type:\s+Patch RomFS", info) else "base"
        if kind in found:
            raise ExtractError(f"two {kind} Program NCAs: {found[kind].name}, {n.name}")
        found[kind] = n
    return found.get("base"), found.get("update")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep-nca", action="store_true", help="keep the unpacked NCAs in work/nca/")
    args = ap.parse_args(argv)

    try:
        cfg = config.load(require_exefs=False)
        for name in ("base_nsp", "hactool_exe"):
            if getattr(cfg, name) is None or not getattr(cfg, name).is_file():
                raise config.ConfigError(f"{name} is not set or not a file")
        if cfg.update_nsp is not None and not cfg.update_nsp.is_file():
            raise config.ConfigError("update_nsp is set but not a file")
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
        app_id = int(tomllib.loads(TARGET.read_text())["title"]["application_id"], 16)
        print("unpacking the game's files ...")
        ncas, rids = unpack_ncas(cfg.base_nsp, nca_dir, "base")
        if cfg.update_nsp is not None:
            upd_ncas, upd_rids = unpack_ncas(cfg.update_nsp, nca_dir, "upd")
            ncas, rids = ncas + upd_ncas, rids | upd_rids

        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            write_temp_keys(cfg.keys_dir, rids, home)
            base_prog, upd_prog = find_programs(cfg.hactool_exe, home, ncas, app_id)
            if base_prog is None:
                raise ExtractError(f"no base game ({app_id:016X}) in base_nsp" +
                                   (" or update_nsp" if cfg.update_nsp else ""))
            if upd_prog is None:
                raise ExtractError("no update for this game: set update_nsp to the 2.2.0 update")
            print(f"base program NCA:   {base_prog.name}\nupdate program NCA: {upd_prog.name}")
            print("extracting ExeFS + RomFS with update applied (takes a few minutes) ...")
            # hactool says nothing while it works: its output folders are measured against this build's size
            size = int(tomllib.loads(TARGET.read_text()).get("extracted", {}).get("bytes", 0)) or 1
            progress.report("extract", 0, size)
            run_hactool(cfg.hactool_exe, home, [
                f"--basenca={base_prog}",
                f"--exefsdir={cfg.exefs_dir}",
                f"--romfsdir={cfg.romfs_dir}",
                str(upd_prog),
            ], watch=("extract", [cfg.exefs_dir, cfg.romfs_dir], size))
            progress.report("extract", size, size)
            # The game's icon for the exe, from the NCAs already here (decision 0048); not having one is no error
            from . import icon
            progress.report("icon", 0, 1)
            try:
                print(f"icon: {icon.make(cfg, home, sorted(ncas, key=lambda n: not n.name.startswith('upd-')))}")
            except (icon.IconError, ExtractError) as e:
                print(f"icon: none ({e})")
            progress.report("icon", 1, 1)
    except (ExtractError, pfs0.Pfs0Error, xci.XciError) as e:
        print(f"extract failed: {e}", file=sys.stderr)
        return 1

    if not args.keep_nca:
        shutil.rmtree(nca_dir)
    if not (cfg.exefs_dir / "main").is_file():
        print("extract finished but exefs_dir has no 'main'", file=sys.stderr)
        return 1
    print("done. Next: python -m tools.yw4.ingest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
