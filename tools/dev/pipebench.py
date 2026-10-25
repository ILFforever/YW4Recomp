"""Build pipebench (tools/dev/pipebench.c) and time cold Vulkan pipeline builds of translated shaders.

    python -m tools.dev.pipebench KEY [KEY ...] [--dir work/shaders/glsl] [ENV=value ...]

For each fragment shader key: a pipeline with a stub vertex shader, built cold (a random string is
added to the SPIR-V, so the driver's shader cache can't answer), and how long vkCreateGraphicsPipelines
took. The game's own shaders build in ~60-75 ms; a change that makes them much slower stalls loading
(decision 0025). ENV: PB_NOSALT=1, PB_SALT=<n>, PB_FORMAT=<VkFormat>, PB_BLEND=1 (see the C file).
Needs [tools] cc and the Vulkan SDK (VULKAN_SDK).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from recomp import shader
from tools.yw4 import config

HERE = Path(__file__).resolve().parent


def build(cfg: config.Config) -> tuple[Path, Path]:
    out = cfg.work_dir / "build" / "pipebench"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("pipebench.exe" if os.name == "nt" else "pipebench")
    stub = out / "stub.vert.spv"
    glslang = shader.glslang(cfg)
    if not glslang:
        sys.exit("glslang not found: install the Vulkan SDK or set [tools] glslang")
    if not stub.exists():
        src = out / "stub.vert"
        src.write_text("#version 460\nvoid main() { gl_Position = vec4(0.0); }\n")
        subprocess.run([glslang, "-V", "-o", str(stub), str(src)], check=True, capture_output=True)
    if not exe.exists() or exe.stat().st_mtime < (HERE / "pipebench.c").stat().st_mtime:
        sdk = os.environ.get("VULKAN_SDK")
        if not sdk:
            sys.exit("VULKAN_SDK is not set")
        lib = "vulkan-1" if os.name == "nt" else "vulkan"
        subprocess.run([cfg.cc or "clang", "-O1", "-o", str(exe), str(HERE / "pipebench.c"), f"-I{sdk}/Include",
                        f"-I{sdk}/include", f"-L{sdk}/Lib", f"-L{sdk}/lib", f"-l{lib}"], check=True)
    return exe, stub


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("keys", nargs="+", help="fragment shader keys (and ENV=value)")
    ap.add_argument("--dir", type=Path, help="translated shaders (default: work/shaders/glsl)")
    a = ap.parse_args(argv)
    cfg = config.load(require_exefs=False)
    env = dict(os.environ)
    keys = []
    for k in a.keys:
        if "=" in k:
            name, _, value = k.partition("=")
            env[name] = value
        else:
            keys.append(k)
    exe, stub = build(cfg)
    glsl = a.dir or cfg.work_dir / "shaders" / "glsl"
    return subprocess.run([str(exe), str(stub), str(glsl), *keys], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
