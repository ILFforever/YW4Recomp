"""Fetch the build's binary dependencies into <work_dir>/deps/ (decision 0019).

    python -m tools.yw4.deps

SDL3 (zlib licence): on Windows the official MinGW development release, pinned by version
and SHA-256. Linux builds use the system's SDL3 (pkg-config sdl3) instead.
AMD FSR (decision 0039): the FSR SDK's source archive, pinned the same way; only the MIT
headers the runtime compiles against and the two signed Vulkan DLLs (loader and upscaler,
MIT too, listed in its Kits/FidelityFX/docs/license.md) are taken out. Windows only.
NVIDIA DLSS (decision 0040): single files of NVIDIA's DLSS SDK repository at a pinned tag, each
checked against its SHA-256: the headers, the static NGX library and nvngx_dlss.dll. They are
under NVIDIA's own licence (LICENSE.txt, fetched with them), never committed. Windows only.
Nothing here is committed: each archive is checked against its pinned hash and unpacked
into its own folder.
"""

from __future__ import annotations

import hashlib
import shutil
import sys
import tarfile
import zipfile
import urllib.request
from pathlib import Path

from . import config

SDL_VERSION = "3.4.18"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-devel-{SDL_VERSION}-mingw.tar.gz"
SDL_SHA256 = "049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf"


def sdl_dir(cfg: config.Config) -> Path:
    """The unpacked SDL3 for x86-64 MinGW: include/, lib/, bin/SDL3.dll."""
    return cfg.work_dir / "deps" / f"SDL3-{SDL_VERSION}" / "x86_64-w64-mingw32"


def fetch_sdl(cfg: config.Config) -> Path:
    out = sdl_dir(cfg)
    if (out / "bin" / "SDL3.dll").is_file():
        return out
    deps = cfg.work_dir / "deps"
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / Path(SDL_URL).name
    if not archive.is_file():
        print(f"downloading {SDL_URL}")
        with urllib.request.urlopen(SDL_URL) as r, open(archive, "wb") as f:
            shutil.copyfileobj(r, f)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SDL_SHA256:
        archive.unlink()
        raise RuntimeError(f"{archive.name}: SHA-256 {digest}, expected {SDL_SHA256}")
    with tarfile.open(archive) as t:
        t.extractall(deps, filter="data")  # "data": no absolute paths, links or devices
    if not (out / "bin" / "SDL3.dll").is_file():
        raise RuntimeError(f"{archive.name} unpacked without {out / 'bin' / 'SDL3.dll'}")
    return out


FSR_VERSION = "2.4.0"
FSR_URL = f"https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v{FSR_VERSION}.zip"
FSR_SHA256 = "27120e3bbe0d0b30213825ad40f19077563f04f04572e252109a392d5fb522cb"
FSR_DLLS = ("amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll")
FSR_TAKE = ("Kits/FidelityFX/api/include/", "Kits/FidelityFX/upscalers/include/", "Kits/FidelityFX/docs/license.md",
            *(f"Kits/FidelityFX/signedbin/{d}" for d in FSR_DLLS))


def fsr_dir(cfg: config.Config) -> Path:
    """The FSR files: Kits/FidelityFX/api/include, .../upscalers/include, .../signedbin/*.dll, the licence."""
    return cfg.work_dir / "deps" / f"fsr-{FSR_VERSION}"


def fetch_fsr(cfg: config.Config) -> Path:
    out = fsr_dir(cfg)
    dlls = [out / "Kits" / "FidelityFX" / "signedbin" / d for d in FSR_DLLS]
    if all(d.is_file() for d in dlls) and (out / "Kits" / "FidelityFX" / "api" / "include" / "ffx_api.h").is_file():
        return out
    deps = cfg.work_dir / "deps" / f"fsr-{FSR_VERSION}-dl"  # the archive alone in its own folder
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / f"FidelityFX-SDK-v{FSR_VERSION}.zip"
    if not archive.is_file():
        print(f"downloading {FSR_URL} (~250 MB)")
        with urllib.request.urlopen(FSR_URL) as r, open(archive, "wb") as f:
            shutil.copyfileobj(r, f)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != FSR_SHA256:
        archive.unlink()
        raise RuntimeError(f"{archive.name}: SHA-256 {digest}, expected {FSR_SHA256}")
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            rel = name.split("/", 1)[1] if "/" in name else ""
            if name.endswith("/") or not any(rel.startswith(t) for t in FSR_TAKE) or ".." in rel:
                continue
            dst = out / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(z.read(name))
    if not all(d.is_file() for d in dlls):
        raise RuntimeError(f"{archive.name} unpacked without the FSR Vulkan DLLs")
    return out


DLSS_VERSION = "310.9.16"
DLSS_URL = f"https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/"
DLSS_FILES = {
    "LICENSE.txt": "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
    "include/nvsdk_ngx.h": "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848",
    "include/nvsdk_ngx_defs.h": "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e",
    "include/nvsdk_ngx_defs_vk.h": "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031",
    "include/nvsdk_ngx_params.h": "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc",
    "include/nvsdk_ngx_helpers_vk.h": "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623",
    "include/nvsdk_ngx_vk.h": "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d",
    "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib": "bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c",
    "lib/Windows_x86_64/rel/nvngx_dlss.dll": "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
}


def dlss_dir(cfg: config.Config) -> Path:
    """The DLSS files at their repository paths: include/, lib/Windows_x86_64/..., LICENSE.txt."""
    return cfg.work_dir / "deps" / f"dlss-{DLSS_VERSION}"


def fetch_dlss(cfg: config.Config) -> Path:
    out = dlss_dir(cfg)
    for rel, want in DLSS_FILES.items():
        dst = out / rel
        if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() == want:
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {DLSS_URL}{rel}")
        with urllib.request.urlopen(DLSS_URL + rel) as r, open(dst, "wb") as f:
            shutil.copyfileobj(r, f)
        digest = hashlib.sha256(dst.read_bytes()).hexdigest()
        if digest != want:
            dst.unlink()
            raise RuntimeError(f"{rel}: SHA-256 {digest}, expected {want}")
    return out


def main() -> int:
    try:
        cfg = config.load()
        if sys.platform != "win32":
            print("not Windows: use the system's SDL3 (pkg-config sdl3)")
            return 0
        print(f"SDL3 {SDL_VERSION}: {fetch_sdl(cfg)}")
        print(f"FSR SDK {FSR_VERSION}: {fetch_fsr(cfg)}")
        print(f"DLSS SDK {DLSS_VERSION}: {fetch_dlss(cfg)}")
    except (config.ConfigError, RuntimeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
