"""AMD FSR 3.1 upscaling for Linux, built from AMD's FidelityFX SDK v1.1.4 source (decision 0073).

    python -m tools.yw4.fsr_linux        (recomp/build.py runs it on Linux unless --no-fsr)

AMD ships the SDK for Windows only (SDK 2.x has no Vulkan backend at all), so the Linux library is made here, once
per SDK and patch set, into <work_dir>/deps/fsr-linux-1.1.4/:

    lib/libamd_fidelityfx_vk.so   the five ffx-api functions the game calls (runtime/src/gpu_fsr.c), the FSR 3.1.5
                                  upscaler, AMD's Vulkan backend, and yw4FfxHostSet
    include/ffx_api/              the matching headers, for gpu_fsr.c

Steps: the SDK's source archive (pinned by SHA-256, its sdk/ and ffx-api/ folders only); xplane-fsr3's Linux patches
and ours (third_party/fsr3-linux, README.md there says which is whose); AMD's shader compiler, ported, built with the
system's C++ compiler; the upscaler's shader permutations generated with it and glslang; the library linked with
its C++ runtime inside, so it needs nothing but the C library. About a minute on 8 threads. A stamp of the inputs
skips all of it when nothing changed.
"""

from __future__ import annotations

import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

from . import config, deps, progress

SDK_VERSION = "1.1.4"
SDK_NAME = f"FidelityFX-SDK-{SDK_VERSION}"
SDK_URL = f"https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v{SDK_VERSION}.tar.gz"
SDK_SHA256 = "25c46398a656150397597f78d44bf7cb445e9e177dd116b309bbfdf50d50cc9f"
PORT = config.REPO_ROOT / "third_party" / "fsr3-linux"
LIB = "libamd_fidelityfx_vk.so"

# the upscaler's library sources (xplane-fsr3's CMakeLists.txt, FidelityFX SDK v1.1.4 layout) and the ffx-api layer
SDK_SOURCES = [
    "sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp",
    "sdk/src/backends/vk/ffx_vk.cpp",
    "sdk/src/backends/shared/ffx_shader_blobs.cpp",
    "sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp",
    "sdk/src/shared/ffx_assert.cpp",
    "sdk/src/shared/ffx_message.cpp",
    "sdk/src/shared/ffx_breadcrumbs_list.cpp",
    "sdk/src/shared/ffx_object_management.cpp",
]
API_SOURCES = ["ffx-api/src/ffx_api.cpp", "ffx-api/src/ffx_provider.cpp", "ffx-api/src/ffx_provider_fsr3upscale.cpp",
               "ffx-api/src/backends.cpp", "ffx-api/src/validation.cpp"]
PORT_SOURCES = ["ffx_host_vk.cpp", "ffx_fg_stub.cpp", "yw4_ffx_host.cpp"]
# the backend's direct Vulkan calls, routed to the game's through the host shim (ffx_host_vk.h)
WRAPPED = ["vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceFeatures",
           "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceMemoryProperties", "vkEnumerateDeviceExtensionProperties",
           "vkGetDeviceProcAddr", "vkCreateBuffer"]
# the shader compiler's permutations (xplane-fsr3's gen_shaders.sh, after the SDK's CMakeCompileFSR3UpscalerShaders.txt)
SHADER_BASE = ["-reflection", "-deps=gcc", "-DFFX_GPU=1",
               "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
               "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
               "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
               "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
               "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
               "-compiler=glslang", "-e", "CS", "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-DFFX_GLSL=1"]
SHADER_PERMUTATIONS = [f"-DFFX_FSR3UPSCALER_OPTION_{o}={{0,1}}" for o in
                       ("REPROJECT_USE_LANCZOS_TYPE", "HDR_COLOR_INPUT", "LOW_RESOLUTION_MOTION_VECTORS",
                        "JITTERED_MOTION_VECTORS", "INVERTED_DEPTH", "APPLY_SHARPENING")]
SHADER_VARIANTS = [("", 0), ("_wave64", 0), ("_16bit", 1), ("_wave64_16bit", 1)]


def out_dir(cfg: config.Config) -> Path:
    return cfg.work_dir / "deps" / f"fsr-linux-{SDK_VERSION}"


def compilers() -> tuple[str, str]:
    """(C, C++) compilers: the build's (CC, set by tools/yw4/system.py) and its C++ sibling."""
    cc = os.environ.get("CC") or shutil.which("cc") or "cc"
    name = Path(cc).name
    for c, cxx in (("clang", "clang++"), ("gcc", "g++")):
        if name.startswith(c):
            sibling = str(Path(cc).with_name(name.replace(c, cxx, 1))) if os.sep in cc else name.replace(c, cxx, 1)
            if shutil.which(sibling):
                return cc, sibling
    return cc, shutil.which("c++") or "c++"


def stamp_of() -> str:
    """What the library is made from: the SDK's hash, our patches and port, this file."""
    h = hashlib.sha256(SDK_SHA256.encode())
    for p in sorted(PORT.rglob("*")) + [Path(__file__)]:
        if p.is_file():
            h.update(p.relative_to(config.REPO_ROOT).as_posix().encode() + b"\0" + p.read_bytes())
    return h.hexdigest()


def run(args: list[str], cwd: Path | None = None) -> None:
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{Path(args[0]).name} failed:\n{(r.stdout + r.stderr)[-3000:]}")


def unpack(archive: Path, src: Path) -> None:
    """The SDK's sdk/ and ffx-api/ folders, without its top folder."""
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    with tarfile.open(archive) as t:
        members = []
        for m in t.getmembers():
            parts = Path(m.name).parts
            if len(parts) > 1 and parts[0] == SDK_NAME and parts[1] in ("sdk", "ffx-api"):
                m.name = str(Path(*parts[1:]))
                members.append(m)
        t.extractall(src, members=members, filter="data")


def apply_patches(src: Path) -> None:
    for patch in ("fidelityfx-sdk-v1.1.4-linux.patch", "fsr-3.1.5-backport.patch"):
        p = PORT / "patches" / patch
        if shutil.which("git"):  # git apply works outside a repository too, and refuses a patch that doesn't fit
            run(["git", "apply", "--whitespace=nowarn", str(p)], cwd=src)
        else:
            run(["patch", "-p1", "--forward", "-i", str(p)], cwd=src)
    run([sys.executable, str(PORT / "patches" / "yw4_ffx_api_linux.py"), str(src / "ffx-api")])


def build_shader_compiler(src: Path, build: Path, cc: str, cxx: str) -> Path:
    libs = src / "sdk" / "tools" / "ffx_shader_compiler" / "libs"
    inc = [f"-I{PORT / 'ffx_sc'}", f"-I{libs / 'MD5'}", f"-I{libs / 'SPIRV-Reflect'}", f"-I{libs / 'tiny-process-library'}"]
    objs = []
    for s in [PORT / "ffx_sc" / f for f in ("ffx_sc.cpp", "glsl_compiler.cpp", "utils.cpp")] + \
             [libs / "MD5" / "md5.cpp", libs / "tiny-process-library" / "process.cpp",
              libs / "tiny-process-library" / "process_unix.cpp", libs / "SPIRV-Reflect" / "spirv_reflect.c"]:
        o = build / "sc" / (s.stem + ".o")
        o.parent.mkdir(parents=True, exist_ok=True)
        if s.suffix == ".c":
            run([cc, "-O2", *inc, "-c", str(s), "-o", str(o)])
        else:
            run([cxx, "-std=c++17", "-O2", "-w", *inc, "-c", str(s), "-o", str(o)])
        objs.append(str(o))
    exe = build / "FidelityFX_SC"
    run([cxx, "-o", str(exe), *objs, "-pthread"])
    return exe


def generate_shaders(src: Path, sc: Path, glslang: str, shaders: Path) -> None:
    gpu = src / "sdk" / "include" / "FidelityFX" / "gpu"
    files = sorted((src / "sdk" / "src" / "backends" / "vk" / "shaders" / "fsr3upscaler").glob("*.glsl"))
    jobs = [(f, suffix, half) for f in files for suffix, half in SHADER_VARIANTS]
    shaders.mkdir(parents=True, exist_ok=True)
    for i, (f, suffix, half) in enumerate(jobs):  # each run uses every core for its permutations
        progress.report("fsrshaders", i, len(jobs))
        run([str(sc), *SHADER_BASE, *SHADER_PERMUTATIONS, f"-glslangexe={glslang}", f"-name={f.stem}{suffix}",
             f"-DFFX_HALF={half}", f"-I{gpu}", f"-I{gpu / 'fsr3upscaler'}", f"-output={shaders}", str(f)])
    progress.report("fsrshaders", len(jobs), len(jobs))


def build_library(src: Path, build: Path, shaders: Path, cxx: str, out: Path) -> None:
    sdk = src / "sdk"
    common = ["-std=c++17", "-O2", "-fPIC", "-fvisibility=hidden", "-w", "-include", str(PORT / "ffx_linux_compat.h"),
              "-DFFX_GCC", "-DFFX_SDK_DEFAULT_CONTEXT_SIZE=(1024 * 256)",
              f"-I{config.REPO_ROOT / 'third_party' / 'vulkan' / 'include'}", f"-I{sdk / 'include'}",
              f"-I{sdk / 'include' / 'FidelityFX' / 'host'}", f"-I{PORT}"]
    sdk_flags = ["-DFFX_FSR3UPSCALER", f"-I{sdk / 'src' / 'shared'}", f"-I{sdk / 'src' / 'backends' / 'shared'}",
                 f"-I{sdk / 'src' / 'components'}", f"-I{shaders}", f"-I{sdk / 'include' / 'FidelityFX' / 'gpu'}",
                 f"-I{sdk / 'include' / 'FidelityFX' / 'gpu' / 'fsr3upscaler'}"]
    api_flags = ["-DFFX_BACKEND_VK", "-DFFX_UPSCALE_ONLY", "-DFFXDLL_VERSION_MAJOR=1", "-DFFXDLL_VERSION_MINOR=0",
                 "-DFFXDLL_VERSION_PATCH=1", "-DFFXDLL_VERSION_API=VK", "-DCI_PIPELINE_ID=0",
                 f"-I{src / 'ffx-api' / 'include'}", f"-I{src / 'ffx-api' / 'src'}"]
    jobs = [(src / s, sdk_flags) for s in SDK_SOURCES] + [(src / s, api_flags) for s in API_SOURCES] + \
           [(PORT / s, []) for s in PORT_SOURCES]
    (build / "lib").mkdir(parents=True, exist_ok=True)

    def one(job: tuple[Path, list[str]]) -> str:
        s, flags = job
        o = build / "lib" / (s.stem + ".o")
        run([cxx, *common, *flags, "-c", str(s), "-o", str(o)])
        return str(o)

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        objs = []
        for i, o in enumerate(pool.map(one, jobs)):
            objs.append(o)
            progress.report("fsrlib", i + 1, len(jobs))
    (out / "lib").mkdir(parents=True, exist_ok=True)
    run([cxx, "-shared", "-o", str(out / "lib" / LIB), *objs, *(f"-Wl,--wrap={f}" for f in WRAPPED),
         "-static-libstdc++", "-static-libgcc", "-Wl,--exclude-libs,ALL",  # the C++ runtime inside, its names hidden
         "-Wl,--no-undefined", "-pthread", "-lm"])
    shutil.rmtree(out / "include", ignore_errors=True)
    shutil.copytree(src / "ffx-api" / "include", out / "include")


def fetch_fsr_linux(cfg: config.Config) -> Path:
    """The Linux FSR library and its headers (see the module text), built unless already there for these inputs."""
    out = out_dir(cfg)
    stamp = out / ".stamp"
    want = stamp_of()
    if stamp.is_file() and stamp.read_text().strip() == want and (out / "lib" / LIB).is_file():
        return out
    from recomp import shader  # glslang: [tools] glslang or the PATH's, as the game's own shaders
    glslang = shader.glslang(cfg)
    if not glslang:
        raise RuntimeError("FSR for Linux needs glslang (glslangValidator)")
    cc, cxx = compilers()
    dl = cfg.work_dir / "deps" / f"fsr-linux-{SDK_VERSION}-dl"  # the archive alone in its own folder
    dl.mkdir(parents=True, exist_ok=True)
    archive = dl / f"{SDK_NAME}.tar.gz"
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != SDK_SHA256:
        deps.fetch_pinned([deps.MIRROR + archive.name, SDK_URL], archive, SDK_SHA256)  # ~200 MB
    shutil.rmtree(out, ignore_errors=True)
    src, build = out / "src", out / "build"
    print(f"fsr: building AMD FSR {SDK_VERSION} for Linux with {cxx} (about a minute)", flush=True)
    unpack(archive, src)
    apply_patches(src)
    sc = build_shader_compiler(src, build, cc, cxx)
    generate_shaders(src, sc, glslang, build / "shaders")
    build_library(src, build, build / "shaders", cxx, out)
    shutil.rmtree(src, ignore_errors=True)  # ~100 MB of sources and objects: only the library and headers stay
    shutil.rmtree(build, ignore_errors=True)
    stamp.write_text(want + "\n")
    return out


def main() -> int:
    if sys.platform == "win32":
        print("Windows: the build uses AMD's own signed DLLs (tools/yw4/deps.py)")
        return 0
    try:
        print(f"fsr: {fetch_fsr_linux(config.load(require_exefs=False)) / 'lib' / LIB}")
    except (RuntimeError, OSError, config.ConfigError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
