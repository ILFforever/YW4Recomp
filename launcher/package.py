"""Make the downloads of a release (decisions 0052, 0054).

    python launcher/package.py [launcher]    the launcher: the NSIS installer and a portable zip, into work/package/dist/
    python launcher/package.py --stage-only  only work/package/builder/ (no Rust build)
    python launcher/package.py game          the game's build code alone (~3 MB), into work/package/game/: a game
                                             update the launcher downloads (tag vX.Y.Z)

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
import re
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
GAME_DIST = OUT / "game"

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


def game_tree(ref: str, dest: Path) -> str:
    """The game's build code at `ref` into `dest`: committed files only (git archive), the commit (recomp/build.py's
    build stamp: a player's PC has no git) and release.toml, which the launcher reads before using it."""
    shutil.rmtree(dest, ignore_errors=True)
    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", ref, "--", *SOURCES))) as t:
        t.extractall(dest, filter="data")
    commit = git("rev-parse", "--short=7", ref).decode().strip()
    (dest / "COMMIT").write_text(commit + "\n")
    code = (dest / "tools" / "yw4" / "build_install.py").read_text()
    protocol = re.search(r"^PROTOCOL = (\d+)", code, re.M)
    min_launcher = re.search(r'^MIN_LAUNCHER = "([\d.]+)"', code, re.M)
    if not (protocol and min_launcher):
        raise SystemExit("tools/yw4/build_install.py: no PROTOCOL or MIN_LAUNCHER")
    version = (dest / "VERSION").read_text().strip()
    (dest / "release.toml").write_text(
        "# Written by launcher/package.py (decision 0054): what the launcher checks before using this game code.\n"
        f'version = "{version}"\ncommit = "{commit}"\nprotocol = {protocol[1]}\nmin_launcher = "{min_launcher[1]}"\n')
    bad = [p for p in dest.rglob("*") if p.suffix.lower() in GAME_DATA]
    if bad:
        raise SystemExit(f"game data in the game code, refusing: {bad[:5]}")
    return commit


def game(ref: str) -> None:
    """The game update: the game code as one zip, files at its top level."""
    tree = OUT / "game-tree"
    commit = game_tree(ref, tree)
    version = (tree / "VERSION").read_text().strip()
    GAME_DIST.mkdir(parents=True, exist_ok=True)
    out = GAME_DIST / f"yw4recomp-game-{version}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(tree.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(tree).as_posix())
    shutil.rmtree(tree)
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"{out.name}: {out.stat().st_size / 1e6:.1f} MB, {ref} = {commit}, SHA-256 {digest}")
    print(f"publish: gh release create v{version} --target {commit} --title \"YW4 Recomp {version}\" \"{out}\"")


def stage(ref: str) -> None:
    shutil.rmtree(STAGE, ignore_errors=True)
    (STAGE / "tools").mkdir(parents=True)
    (STAGE / "licenses").mkdir()

    py = STAGE / "python"
    with zipfile.ZipFile(io.BytesIO(fetch(PYTHON_ZIP, "python.zip"))) as z:
        z.extractall(py)
    # The ._pth decides Python's whole import path (no PATH, no registry, no current folder). It stays as shipped:
    # the launcher names the game code's folder on each run (its own copy or a game update, decision 0054).
    if not next(py.glob("python*._pth"), None):
        raise SystemExit("the embeddable Python has no ._pth file")
    shutil.copy(py / "LICENSE.txt", STAGE / "licenses" / "python.txt")

    with zipfile.ZipFile(io.BytesIO(fetch(GLSLANG_ZIP, "glslang.zip"))) as z:
        (STAGE / "tools" / "glslang.exe").write_bytes(z.read("bin/glslang.exe"))
    with zipfile.ZipFile(io.BytesIO(fetch(HACTOOL_ZIP, "hactool.zip"))) as z:
        (STAGE / "tools" / "hactool.exe").write_bytes(z.read("hactool.exe"))
    (STAGE / "licenses" / "glslang.txt").write_bytes(fetch(GLSLANG_LICENSE, "glslang-license.txt"))
    (STAGE / "licenses" / "hactool.txt").write_bytes(fetch(HACTOOL_LICENSE, "hactool-license.txt"))

    commit = game_tree(ref, STAGE / "src")
    shutil.copy(STAGE / "src" / "LICENSE", STAGE / "licenses" / "yw4recomp.txt")

    bad = [p for p in STAGE.rglob("*") if p.suffix.lower() in GAME_DATA]
    if bad:
        raise SystemExit(f"game data in the stage, refusing: {bad[:5]}")
    files = [p for p in STAGE.rglob("*") if p.is_file()]
    print(f"stage: {len(files)} files, {sum(p.stat().st_size for p in files) / 1e6:.0f} MB, {ref} = {commit}")


def rust_host() -> str:
    """The Rust compiler's own target ("x86_64-pc-windows-msvc", "x86_64-pc-windows-gnu", ...); "" without rustc."""
    try:
        out = subprocess.run(["rustc", "-vV"], capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""
    return next((line.split(":", 1)[1].strip() for line in out.splitlines() if line.startswith("host:")), "")


def build() -> None:
    launcher = REPO / "launcher"
    version = (REPO / "launcher" / "VERSION").read_text().strip()  # the launcher's own number (0054)
    # only for this build: a plain `cargo build` has no stage and must keep working without one
    overrides = OUT / "tauri.package.json"
    # the GNU Rust build links WebView2 through WebView2Loader.dll (an MSVC build links it in): it has to sit next to
    # the launcher, or Windows refuses to start it (2026-11-09: a friend's fresh install said the DLL was missing).
    # Cargo's build puts it in target/release; a first build makes it, so build once before bundling.
    # Only that build: an MSVC Rust (GitHub's build check, rustup's default on Windows) has neither DLL to ship.
    release = launcher / "src-tauri" / "target" / "release"
    loader = release / "WebView2Loader.dll"
    dlls: dict[str, str] = {}
    if rust_host().endswith(("-windows-gnu", "-windows-gnullvm")):
        if not loader.is_file():
            subprocess.run(["cargo", "build", "--release"], cwd=launcher / "src-tauri", check=True)
        shutil.copy(loader, OUT / "WebView2Loader.dll")
        # the same build links Rust's unwinder from llvm-mingw's libunwind.dll (the toolchain's bin, on the PATH for
        # the build): it ships next to the launcher as well
        clang = shutil.which("clang")
        unwind = Path(clang).parent / "libunwind.dll" if clang else None
        if not unwind or not unwind.is_file():
            raise SystemExit("libunwind.dll not found: put llvm-mingw's bin on the PATH")
        shutil.copy(unwind, OUT / "libunwind.dll")
        # the bundle copies both into target/release; the launcher's own build script runs from there with
        # libunwind.dll loaded, so a copy left from the last build can't be overwritten (os error 32): start without them
        for dll in ("WebView2Loader.dll", "libunwind.dll"):
            (release / dll).unlink(missing_ok=True)
            dlls[(OUT / dll).as_posix()] = dll
    # the installer's own pictures and icon (installer_art.py), drawn for this version; only here, like the stage
    sys.path.insert(0, str(launcher))
    import installer_art
    sidebar, header = installer_art.draw(OUT / "installer", version)
    overrides.write_text(json.dumps({
        "version": version,
        "mainBinaryName": "YW4 Recomp",
        "bundle": {
            # the launcher's themes (themes.rs): files anyone can copy into this folder, so they ship next to the launcher
            "resources": {**dlls, STAGE.as_posix() + "/": "builder/", (REPO / "launcher" / "ui" / "themes").as_posix() + "/": "themes/"},
            "windows": {"nsis": {
                "installerIcon": (launcher / "src-tauri" / "icons" / "icon.ico").as_posix(),
                "sidebarImage": sidebar.as_posix(),
                "headerImage": header.as_posix(),
            }},
        },
    }, indent=2))
    npx = shutil.which("npx")
    if not npx:
        raise SystemExit("npx not found: install Node.js")
    subprocess.run([npx, "tauri", "build", "--config", str(overrides)], cwd=launcher, check=True)

    shutil.rmtree(DIST, ignore_errors=True)
    DIST.mkdir(parents=True)
    installers = list((release / "bundle" / "nsis").glob(f"*{version}*.exe"))
    if len(installers) != 1:
        raise SystemExit(f"expected one installer, found {installers}")
    shutil.copy(installers[0], DIST / f"YW4-Recomp-Launcher-{version}-setup.exe")

    with zipfile.ZipFile(DIST / f"YW4-Recomp-Launcher-{version}-portable.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(release / "YW4 Recomp.exe", "YW4 Recomp/YW4 Recomp.exe")
        for dll in dlls.values():  # the GNU Rust build's (above); none with MSVC
            z.write(OUT / dll, f"YW4 Recomp/{dll}")
        for p in (release / "builder").rglob("*"):
            if p.is_file():
                z.write(p, Path("YW4 Recomp") / p.relative_to(release))
    for p in DIST.iterdir():
        print(f"{p.name}: {p.stat().st_size / 1e6:.0f} MB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="?", choices=("launcher", "game"), default="launcher")
    ap.add_argument("--stage-only", action="store_true")
    ap.add_argument("--ref", default="HEAD", help="the commit whose build code ships (default: HEAD)")
    args = ap.parse_args()
    if sys.platform != "win32":
        raise SystemExit("the launcher's download is Windows only for now")
    if args.what == "game":
        game(args.ref)
        return 0
    stage(args.ref)
    if not args.stage_only:
        if git("status", "--porcelain", "--", "launcher"):
            print("note: launcher/ has uncommitted changes, and they will be in this build", file=sys.stderr)
        build()
    return 0


if __name__ == "__main__":
    sys.exit(main())
