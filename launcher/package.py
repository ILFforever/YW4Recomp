"""Make the launcher's download (decision 0052): the NSIS installer and a portable zip of the same files.

    python launcher/package.py               everything, into work/package/dist/
    python launcher/package.py --stage-only  only work/package/builder/ (no Rust build)

What ships next to the launcher, in a folder called builder/:

    python/    the official embeddable Python, its ._pth pointing at src/
    src/       this repo's build code at one commit (git archive: committed files only, never the working tree)
    tools/     glslang.exe and hactool.exe, from their official releases
    licenses/  the licence of each

Every download is pinned to an exact version and SHA-256. Nothing from the game is ever in here (rule 1): src/ holds
only committed files, and the stage is refused if a game-data file type turns up. The compiler is not shipped: the
launcher downloads it on the first build (tools/yw4/deps.py fetch_toolchain).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "work" / "package"
CACHE = OUT / "cache"
STAGE = OUT / "builder"
DIST = OUT / "dist"

PYTHON_ZIP = ("https://www.python.org/ftp/python/3.13.5/python-3.13.5-embed-amd64.zip",
              "7d2650fd9d1b9d002d4a315d5f354247fd6a44f30517c7ef577b08f57a0fb6d9")
GLSLANG_ZIP = ("https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip",
               "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b")
HACTOOL_ZIP = ("https://github.com/SciresM/hactool/releases/download/1.4.0/hactool-1.4.0-win.zip",
               "36e9a221c8a7949c86ada9388eb703c90663aedfe9f65b6032429614c5e1abe8")
GLSLANG_LICENSE = ("https://raw.githubusercontent.com/KhronosGroup/glslang/16.6.0/LICENSE.txt",
                   "17e70c676e1521ff3e4686f04a2053d93a7e28a33be8de7ec37ab0ff72feb677")
HACTOOL_LICENSE = ("https://raw.githubusercontent.com/SciresM/hactool/1.4.0/LICENSE",
                   "bde8a8c9f1fdcecaec2dd6f4733ed939a5ff2317a72126668e61ecae049eb5ab")

# what the build steps read: everything else in the repo (docs, tests, dev tools, the launcher itself) stays out
SOURCES = ["VERSION", "LICENSE", "target.toml", "recomp", "runtime", "third_party", "tools/yw4"]
GAME_DATA = {".nso", ".nca", ".nsp", ".xci", ".nsz", ".keys", ".ncz", ".istorage"}


def fetch(pin: tuple[str, str], name: str) -> bytes:
    url, want = pin
    path = CACHE / name
    if not path.is_file():
        CACHE.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        with urllib.request.urlopen(url) as r, open(path, "wb") as f:
            shutil.copyfileobj(r, f)
    data = path.read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        path.unlink()
        raise SystemExit(f"{name}: SHA-256 {got}, expected {want}")
    return data


def git(*args: str) -> bytes:
    return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, check=True).stdout


def stage(ref: str) -> None:
    shutil.rmtree(STAGE, ignore_errors=True)
    (STAGE / "tools").mkdir(parents=True)
    (STAGE / "licenses").mkdir()

    py = STAGE / "python"
    with zipfile.ZipFile(io.BytesIO(fetch(PYTHON_ZIP, "python.zip"))) as z:
        z.extractall(py)
    # The ._pth decides Python's whole import path (no PATH, no registry, no current folder): add our code.
    pth = next(py.glob("python*._pth"))
    pth.write_text(pth.read_text().replace("\n.\n", "\n.\n..\\src\n", 1))
    if "..\\src" not in pth.read_text():
        raise SystemExit(f"{pth.name}: unexpected contents")
    shutil.copy(py / "LICENSE.txt", STAGE / "licenses" / "python.txt")

    with zipfile.ZipFile(io.BytesIO(fetch(GLSLANG_ZIP, "glslang.zip"))) as z:
        (STAGE / "tools" / "glslang.exe").write_bytes(z.read("bin/glslang.exe"))
    with zipfile.ZipFile(io.BytesIO(fetch(HACTOOL_ZIP, "hactool.zip"))) as z:
        (STAGE / "tools" / "hactool.exe").write_bytes(z.read("hactool.exe"))
    (STAGE / "licenses" / "glslang.txt").write_bytes(fetch(GLSLANG_LICENSE, "glslang-license.txt"))
    (STAGE / "licenses" / "hactool.txt").write_bytes(fetch(HACTOOL_LICENSE, "hactool-license.txt"))

    src = STAGE / "src"
    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", ref, "--", *SOURCES))) as t:
        t.extractall(src, filter="data")
    commit = git("rev-parse", "--short=7", ref).decode().strip()
    (src / "COMMIT").write_text(commit + "\n")  # recomp/build.py's build stamp: a player's PC has no git
    shutil.copy(src / "LICENSE", STAGE / "licenses" / "yw4recomp.txt")

    bad = [p for p in STAGE.rglob("*") if p.suffix.lower() in GAME_DATA]
    if bad:
        raise SystemExit(f"game data in the stage, refusing: {bad[:5]}")
    files = [p for p in STAGE.rglob("*") if p.is_file()]
    print(f"stage: {len(files)} files, {sum(p.stat().st_size for p in files) / 1e6:.0f} MB, {ref} = {commit}")


def build() -> None:
    launcher = REPO / "launcher"
    version = (REPO / "launcher" / "VERSION").read_text().strip()  # the launcher's own number (0054)
    # only for this build: a plain `cargo build` has no stage and must keep working without one
    overrides = OUT / "tauri.package.json"
    overrides.write_text(json.dumps({
        "version": version,
        "mainBinaryName": "YW4 Recomp",
        "bundle": {"resources": {STAGE.as_posix() + "/": "builder/"}},
    }, indent=2))
    npx = shutil.which("npx")
    if not npx:
        raise SystemExit("npx not found: install Node.js")
    subprocess.run([npx, "tauri", "build", "--config", str(overrides)], cwd=launcher, check=True)

    release = launcher / "src-tauri" / "target" / "release"
    shutil.rmtree(DIST, ignore_errors=True)
    DIST.mkdir(parents=True)
    installers = list((release / "bundle" / "nsis").glob(f"*{version}*.exe"))
    if len(installers) != 1:
        raise SystemExit(f"expected one installer, found {installers}")
    shutil.copy(installers[0], DIST / f"YW4-Recomp-Launcher-{version}-setup.exe")

    with zipfile.ZipFile(DIST / f"YW4-Recomp-Launcher-{version}-portable.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(release / "YW4 Recomp.exe", "YW4 Recomp/YW4 Recomp.exe")
        for p in (release / "builder").rglob("*"):
            if p.is_file():
                z.write(p, Path("YW4 Recomp") / p.relative_to(release))
    for p in DIST.iterdir():
        print(f"{p.name}: {p.stat().st_size / 1e6:.0f} MB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage-only", action="store_true")
    ap.add_argument("--ref", default="HEAD", help="the commit whose build code ships (default: HEAD)")
    args = ap.parse_args()
    if sys.platform != "win32":
        raise SystemExit("the launcher's download is Windows only for now")
    stage(args.ref)
    if not args.stage_only:
        if git("status", "--porcelain", "--", "launcher"):
            print("note: launcher/ has uncommitted changes, and they will be in this build", file=sys.stderr)
        build()
    return 0


if __name__ == "__main__":
    sys.exit(main())
