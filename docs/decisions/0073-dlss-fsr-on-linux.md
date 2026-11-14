# 0073 — DLSS and FSR upscaling on Linux

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: tools/yw4/fsr_linux.py, third_party/fsr3-linux, tools/yw4/deps.py, recomp/build.py, runtime/src/gpu_fsr.c, runtime/src/gpu_fg.c, runtime/src/gpu_dlss.c, runtime/src/settings.c, runtime/src/window.c, runtime/src/host.c, launcher/ui/app.js
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

## The launcher

Its Linux screens (decision 0071) now offer DLSS (RTX cards, upscaling only, about 60 MB) and FSR (upscaling and
frame generation, about 200 MB). The frame generation slide and chip show on Linux too.

## Not done

- No real Linux GPU yet: AMD, Intel or NVIDIA. No speed numbers.
- FSR frame generation on Linux not compared against the same shot on Windows.
- FSR's wave64 and 16-bit shader variants are built but only used where the game enabled the features they need.
- The SDK archive is not on our deps-mirror yet (uploading is the owner's): until then GitHub's generated archive is
  the only source, and GitHub has changed such archives' bytes before.
