# FSR 3.1 upscaler on Linux / Vulkan

AMD ships FidelityFX SDK v1.1.4 (FSR 3.1.4) for Windows only. This directory builds the
FSR 3.1 upscaler and its Vulkan backend as a Linux static library, `libffx_fsr3upscaler_vk.a`,
and tests it against ground truth. The upscaler is 3.1.5: 2.x's one PC change is backported
(below). Frame generation is not built.

SDK 2.x (2.3.0, June 2026) can't replace v1.1.4 here. Its readme lists "Vulkan is currently not
supported in SDK", its only backend is `Kits/FidelityFX/backend/dx12/`, and its FSR 3 shaders
are HLSL only. v1.1.4 is the last release with the Vulkan backend and GLSL shaders.

## Build and test

```bash
../scripts/fetch-sdk.sh                              # once: AMD's SDK v1.1.4 + the patches below
cd fsr3-linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFSR3_TEST_GPU=9070   # GPU test optional
cmake --build build -j8
ctest --test-dir build --output-on-failure          # Lavapipe (software Vulkan)
# A GPU, with the Khronos validation layer (installed, or unpacked into ../third_party/validation-layers/layer):
FSR_DEVICE=9070 FSR_VALIDATION=1 FSR_OUT=/tmp/out \
  VK_ADD_LAYER_PATH=$PWD/../third_party/validation-layers/layer ./build/fsr3_selftest
```

Sources: `../third_party/FidelityFX-SDK`, a sparse clone of tag v1.1.4 (commit c6efa6b, `sdk/`
only) with `patches/fidelityfx-sdk-v1.1.4-linux.patch` and `patches/fsr-3.1.5-backport.patch`
applied, both done by `../scripts/fetch-sdk.sh`. MIT licence.

`fsr-3.1.5-backport.patch` brings the upscaler from 3.1.4 to 3.1.5 (SDK 2.0.0 note: "Fix for
possible negative rcas output"). It changes:
- the RCAS lower limiter in `gpu/fsr1/ffx_fsr1.h`, in its fp32, fp16 and paired-fp16 variants;
- `FFX_FSR3UPSCALER_VERSION_PATCH` from 4 to 5.

Everything else that differs between 3.1.4 and 2.3.0's 3.1.5 has no effect on this Vulkan build:
- DirectX 12 register macros and HLSL-only core helpers;
- `__XBOX_SCARLETT` blocks in the entry points;
- a host refactor: resource descriptions moved into functions, shader blobs looked up by the
  effect. The internal and shared resource tables have the same formats, sizes, flags and usages;
- validation messages, the watermark hook, include paths and copyright years.

The layer runs with `enableSharpening = false` (X-Plane's own RCAS follows), so its output is
byte-identical before and after the backport. fake_xplane replace output was compared with `cmp`
on RX 9070 XT fp32 and fp16 and on Lavapipe. The fixed RCAS shaders are compiled but not run by
any test.
Requires glslangValidator, Vulkan headers/loader, CMake 3.20, GCC with C++17.

## What `fsr3_selftest` shows

The scene is analytic: a zone plate and a rotated 7-pixel checkerboard, rendered on the CPU at
640x360 with FSR's jitter sequence. The camera pans 0.37, 0.11 display pixels per frame.
The upscaler gets exact motion vectors and produces 1280x720 output. After 48 frames that output
is compared with an 8x8-supersampled ground truth (PSNR, 16-pixel border excluded).
Measured 2026-10-17:

| Device / features | bilinear | FSR 3.1 panning | FSR, flipped MVs | bilinear static | FSR static | validation errors |
|---|---|---|---|---|---|---|
| Lavapipe, min | 13.81 | 16.62 | 13.11 | 13.79 | 17.66 | 0 |
| RX 9070 XT, min | 13.81 | 16.62 | 13.10 | 13.79 | 17.65 | 0 |
| RX 9070 XT, fp16 | 13.81 | 16.63 | 13.11 | 13.79 | 17.66 | 0 |
| RX 9070 XT, full (wave64 hidden) | 13.81 | 16.63 | 13.11 | 13.79 | 17.66 | 0 |

The flipped-vector control scoring below bilinear confirms the motion-vector convention:
`prevUV = currentUV + mv` (`ffx_fsr3upscaler_reproject.h:58`). Jitter: the render pixel centred
at s shows the scene point at s - jitter (`ffx_fsr3upscaler_accumulate.h:110`).
Crops viewed: FSR rebuilds the checkerboard that bilinear aliases. The finest zone-plate rings,
near the display's own limit, come out attenuated.

## Changes to AMD's code, and why

Shader compiler (`ffx_sc/`, patched copies of `sdk/tools/ffx_shader_compiler/src`):
- GLSL path only. The HLSL/DXC/FXC compilers and DXBC checksum need Windows DLLs.
- `pch.hpp`, `utils.cpp`: no Windows.h. UTF-8 <-> wchar_t conversion written out by hand.
- `ffx_sc.cpp`: the `PathAlloc*`/`CreateDirectoryW`/`_wfopen_s` calls are replaced with
  std::filesystem and fopen, and `wmain` with a `main` that converts argv.
- `glsl_compiler.cpp`: `snprintf(out, 32, ...)` while writing into a shrinking 33-byte buffer.
  glibc's fortify check aborts ("buffer overflow detected"). The size is now 3.
- `gen_shaders.sh` replaces the SDK's CMake shader rules: the same arguments and the same
  four header sets per pass.

SDK sources (`patches/fidelityfx-sdk-v1.1.4-linux.patch`):
1. `ffx_types.h`: `FFX_SDK_DEFAULT_CONTEXT_SIZE` is now overridable, and the build sets 1024*256.
   wchar_t is 4 bytes on Linux, so the private upscaler context (837,848 bytes, measured) no
   longer fits the Windows-sized public blob (524,288). Callers must use the same value; it is a
   PUBLIC compile definition of the library.
2. `ffx_vk.cpp` scratch layout: the sub-arrays were aligned to 4 bytes. GCC writes those
   structs with `movaps` (16-byte aligned), which segfaulted at `ffx_vk.cpp:1957` (gdb:
   `movaps %xmm0,0x20(%rbx)`, with rbx+0x20 only 8-byte aligned). Now 64.
3. `ffx_vk.cpp` descriptor pool: 1,024,000 descriptors per type took 125.0 MiB of GPU heap on
   RX 9070 XT (VK_EXT_memory_budget). It is now 2000 sets x 16 per type = 32,000 (3.9 MiB). 16
   covers the measured per-set maxima (9 sampled images, 7 storage images, 2 UBOs, 16 static
   samplers). `poolSizeCount` was 5 for a 6-entry array; fixed.
4. `ffx_vk.cpp` `findMemoryTypeIndex`: for device-local requests it skipped every host-visible
   type, so it found nothing on unified-memory devices (Lavapipe, iGPUs). It now falls back to
   a host-visible device-local type.

Linux glue (this directory):
- `ffx_linux_compat.h` (force-included): `_countof`, `wcscpy_s`, `strcpy_s`, `sprintf_s`,
  `swprintf_s`, `FFX_UNUSED`, and the missing standard headers.
- `ffx_fg_stub.cpp`: `ffxSetFrameGenerationConfigToSwapchainVK`. The frame-interpolation
  swapchain is not built.
- `ffx_host_vk.{h,cpp}`: the backend calls eight Vulkan functions directly (physical-device
  queries, `vkEnumerateDeviceExtensionProperties`, `vkGetDeviceProcAddr`, and one
  `vkCreateBuffer` in breadcrumbs). The library's link options `--wrap` them, so a layer can
  route them to its next-layer dispatch without linking libvulkan. The shim also reports only
  extensions and features the host enabled, because the backend chooses fp16, wave64 and AMD
  coherent memory from what the GPU *supports*. Inside another application that is invalid.

## Rules for a caller (learned from failures)

- **Zero the scratch memory.** `ffxGetInterfaceVK` reads `refCount` from it before initialising
  (`ffx_vk.cpp`, `FFX_RETURN_ON_ERROR(!backendContext->refCount, ...)`). Reused heap memory
  failed the second context creation with FFX_ERROR_BACKEND_API_ERROR.
- **Provide the three shared resources** (`dilatedDepth` R32F, `dilatedMotionVectors` RG16F,
  `reconstructedPrevNearestDepth` R32UI, at max render size) with STORAGE, SAMPLED and
  TRANSFER_DST usage. The upscaler clears one of them each frame. Leaving them null gives
  unwritten descriptors and a crash.
- **fp16 needs shaderInt16 too.** The 16-bit variants declare the Int16 capability. The shim
  reports shaderFloat16 only when both are enabled.
- **Wave64 is never offered.** Forcing subgroup size 64 gives blurred output on RADV GFX1201
  (wave64 run: 13.66 dB panning vs 16.63 dB). The cause is not investigated. The shim hides
  VK_EXT_subgroup_size_control.
- Imported images go back to the state you declared once the dispatch ends (`ffx_vk.cpp`, "Walk back
  all the resources that don't belong to us"). COMPUTE_READ maps to SHADER_READ_ONLY_OPTIMAL,
  UNORDERED_ACCESS to GENERAL.
- Line-number breakpoints in the optimised build land on the next statement. Two "failures"
  at the descriptor pool were false. Debug with `build-debug` (CMAKE_BUILD_TYPE=Debug).
