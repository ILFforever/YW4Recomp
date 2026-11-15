# 0073 — DLSS and FSR upscaling on Linux

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: tools/yw4/fsr_linux.py, third_party/fsr3-linux, tools/yw4/deps.py, recomp/build.py, runtime/src/gpu_fsr.c, runtime/src/gpu_fg.c, runtime/src/gpu_dlss.c, runtime/src/settings.c, runtime/src/window.c, runtime/src/host.c, runtime/src/c11_keywords.h, launcher/ui/app.js
-->

Status: accepted, experimental on Linux. FSR upscaling checked in WSL on Lavapipe (software Vulkan) against AMD's own Windows library; FSR
frame generation checked there too (added later on 2026-11-14, below); DLSS only builds and links. None has run on a
real Linux GPU yet.

## Context

Upscaling and frame generation were Windows only (decisions 0039-0041): the build fetched AMD's signed Windows DLLs
and NVIDIA's Windows libraries. The owner asked for both upscalers on Linux.

## DLSS

NVIDIA's DLSS SDK at our pinned 310.9.16 already has Linux files: the static `libnvsdk_ngx.a` (built with GCC) and
`libnvidia-ngx-dlss.so.310.9.16`. `tools/yw4/deps.py` fetches them on Linux (pinned by SHA-256, NVIDIA's own address
only, as on Windows); `recomp/build.py` links the static library with the C++ runtime and `dlopen`, and copies the
DLSS library next to `yw4`. `gpu_dlss.c` names yw4's folder to NGX as the place to find it (NGX's default is the
application folder too). `msvc_shim.c` is Windows only: it stands in for the MSVC runtime the Windows library expects.

No DLSS frame generation or Reflex: they come through Streamline, which is Windows only.

## FSR

AMD ships no Linux build, and SDK 2.x has no Vulkan backend at all. SDK v1.1.4 (FSR 3.1.4) is the last with a Vulkan
backend and GLSL shaders. Building it on Linux needs AMD's shader compiler (Windows only) ported, and fixes to the
backend. A search found several projects doing this; the one we use is
**[xplane-fsr3](https://github.com/JonathanOrr/xplane-fsr3)** (MIT), whose `fsr3-linux/` holds exactly those parts.
It is a new project with no other users, so the owner asked for it to be checked before use, and it was:

- Its patches were read against AMD's source: the shader compiler changes are Windows file and text calls only;
  the four backend fixes are real (one is an AMD bug, `poolSizeCount` 5 for 6 pool sizes).
- Its smaller descriptor pool (16 per type per set) was checked against the generated shaders: at most 9 sampled
  images, 7 storage images, 2 constant buffers, 1 sampler.
- Its self-test reproduced its published numbers on Lavapipe: FSR 16.62 dB against bilinear 13.81 (panning), and
  13.11 with the motion vectors flipped (so the motion is used).
- In the game (late-game save, Mystic Grove, frame 2650, 1067x600 to 1600x900): our Linux build against AMD's own
  Windows library 3.1.6 on the RTX laptop, 31.7 dB on the 3D (mean difference 3.9 of 255, 0.6% of pixels more than
  30 apart: a software rasteriser against NVIDIA's); for scale, FSR against TAA is 18.5 dB.

The owner chose to use it with credit (option 1 of the two offered: theirs credited, or ours rewritten).

What is built (`tools/yw4/fsr_linux.py`, on the player's PC, about a minute):

1. AMD's SDK v1.1.4 source archive, pinned by SHA-256 (~200 MB; our deps-mirror first, GitHub after), its `sdk/` and
   `ffx-api/` folders.
2. xplane-fsr3's two patches (`git apply`, else `patch`), then ours, `third_party/fsr3-linux/patches/yw4_ffx_api_linux.py`:
   the SDK's `ffx-api` (the five functions the game calls) on Linux with only the FSR 3 provider, no DX12 driver hook,
   case-correct includes, standard integer literals, and headers usable from C.
3. AMD's shader compiler (xplane-fsr3's port) built with the system's C++ compiler; the upscaler's shader permutations
   generated with it and glslang.
4. `libamd_fidelityfx_vk.so`: the upscaler, AMD's Vulkan backend, the ffx-api layer, xplane-fsr3's host shim and our
   `yw4FfxHostSet`, with its C++ runtime inside (it needs only the C library). It exports exactly the five ffx-api
   functions and `yw4FfxHostSet`.

`gpu_fsr.c` opens it with `dlopen` (Windows: `LoadLibrary`) and calls `yw4FfxHostSet` with the device's extensions
and its 16-bit and synchronization2 features first: AMD's backend picks fp16, wave64 and other paths from what the GPU
*supports*, and the shim limits it to what the game *enabled*. It compiles against v1.1.4's headers on Linux, never
2.4.0's: the two use different values for the Vulkan backend's description type.

If the FSR build fails on a player's PC (an unusual compiler), the game is built without it and says why: FSR is an
optional extra, and TAA takes its place.

## FSR frame generation (added 2026-11-14)

First the Linux build was the upscaler only. Frame generation is now in the same library:

- `tools/yw4/fsr_linux.py` also builds AMD's frame interpolation, optical flow and frame generation provider, with the
  sources and shader options AMD's own CMake files give them. Which ones they are follows
  **[NX2navi](https://github.com/tangerineturtle1/NX2navi)**'s pull request #455 (MIT), which builds on the same
  xplane-fsr3 port; its `%s` to `%ls` fix in AMD's provider is in our patch, credited in
  `third_party/fsr3-linux/README.md`. No file of theirs is copied.
- Our ffx-api patch now takes in the frame generation provider and leaves out FSR 2 and AMD's frame generation
  swapchain: the game presents and paces frames itself, as on Windows (decision 0041).
- `gpu_fsr.c` opens the library once for both (`gpu_ffx_library()`); `gpu_fg.c` finds the functions with `dlsym`.
  v1.1.4 has no `PrepareV2`: Linux sends `Prepare` with a chained `PrepareCameraInfo`, and has no reset field (unused
  in v1.1.4). Its backend description has the three Vulkan fields flat, not in a `VkDeviceContext`.
- Linux thread stacks now get at least 8 MiB (glibc's default; was 1 MiB, `host.c`). AMD's frame interpolation puts
  a 142 KiB frame on the GPU worker's stack, which overflowed at the first generated frame (signal 11).
- The settings lock only what comes through Streamline on Linux: DLSS frame generation and Reflex.

Checked in WSL on Lavapipe (late-game save, frame 2600): a generated frame against the real one next to it, mean
difference 1 of 255, no pixel more than 64 apart, no holes or speckles; only soft edges differ. An earlier run had
black holes on characters and white speckles; they were gone after fixes in the same session, and which change fixed
them was not recorded. Not compared against Windows frame generation, no speed numbers.

## Issue #12: FSR built unpatched inside a clone; C11 keywords in C++ (2026-11-15)

A Linux player (GCC and Clang, AOSC) reported FSR failing to build with both compilers:
`sizeof(FfxFsr3UpscalerContext) >= sizeof(FfxFsr3UpscalerContext_Private)`, 524288 >= 837848. 524288 bytes is AMD's
default context size, `(1024 * 128)` uint32s, so our `-DFFX_SDK_DEFAULT_CONTEXT_SIZE=(1024 * 256)` never took effect:
stock `ffx_types.h` defines it without `#ifndef`, and only xplane-fsr3's Linux patch makes it overridable (wchar_t is 4
bytes on Linux, so the private contexts are larger).

The patches had not been applied at all. `apply_patches` ran `git apply` in the unpacked sources. Outside a git
repository that works like `patch`. Inside one, git reads the patch's paths from the repository's top, finds them outside
the folder it runs in, and skips them, printing nothing and exiting 0. The default work folder is `<clone>/work`, inside
the player's clone of this repository. Ours in WSL (`~/yw4linux/work`) is not, which is why it built here: reproduced in
WSL with the work folder inside a fresh `git init`, the same error with the same numbers, with Clang 19 which had just
built it fine outside one.

Fix (`tools/yw4/fsr_linux.py`): `git apply` runs with `GIT_CEILING_DIRECTORIES` set to the folder above the sources,
so it never looks for a repository there, and its errors are shown. After the patches, a line the Linux patch adds
(`#ifndef FFX_SDK_DEFAULT_CONTEXT_SIZE` in `ffx_types.h`) must be in the sources, or the build stops with that reason:
a skipped patch can't pass quietly again, whichever tool applied it. Checked in WSL with the work folder inside a
repository: GCC 13 and Clang 19 both build the library.

The same report: g++ stopped on `_Noreturn` in `host.h`, which `gpu_dlssg.cpp` (C++) includes. `runtime/src/c11_keywords.h`
gives `YW4_NORETURN` (`[[noreturn]]` in C++, `_Noreturn` in C), used by every runtime header that had `_Noreturn` (host.h,
kernel.h, hle.h, nvn.h, gpu_internal.h). In C++ only, it also defines `_Noreturn` and `_Thread_local` as the C++ words,
for the headers that keep the C11 spelling: `runtime/include/yw4rt.h` (the generated code's header; a change there makes
every player's build recompile all the generated code) and the thread-locals in nvn.h and gpu_internal.h, which g++
stopped on next. A test compiles those five headers as C++ with each compiler at hand, clang++ with
`-Werror=c11-extensions` (it otherwise takes C11 keywords as extensions): `tests/test_runtime.py`, CxxHeadersTest.
`recomp.ci` compiles all 131 runtime files with Clang 19 and with GCC 13 except `ui.c` (`#embed`: GCC 15 or newer, as
the setup page says).

## The launcher

Its Linux screens (decision 0071) now offer DLSS (RTX cards, upscaling only, about 60 MB) and FSR (upscaling and
frame generation, about 200 MB). The frame generation slide and chip show on Linux too.

## Not done

- No real Linux GPU yet: AMD, Intel or NVIDIA. No speed numbers.
- FSR frame generation on Linux not compared against the same shot on Windows.
- FSR's wave64 and 16-bit shader variants are built but only used where the game enabled the features they need.
- The SDK archive is not on our deps-mirror yet (uploading is the owner's): until then GitHub's generated archive is
  the only source, and GitHub has changed such archives' bytes before.
